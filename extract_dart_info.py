import io
import os
import re
import requests
import sys
import zipfile
import zlib
from struct import unpack

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_E_MACHINE 
from elftools.elf.sections import SymbolTableSection

# TODO: support both ELF and Mach-O file

# ---------------------------------------------------------------------------
# minimal Mach-O (iOS App.framework) support
# ---------------------------------------------------------------------------
MH_MAGIC_64 = 0xfeedfacf
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007


class MachOImage:
    """tiny Mach-O<64,little-endian> reader: load commands + symbol table"""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.buf = f.read()
        b = self.buf
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, _ = unpack('<8I', b[:32])
        if magic != MH_MAGIC_64:
            raise ValueError(f'not a Mach-O 64 image: magic={magic:#x}')
        self.cputype = cputype
        self.segments = []      # (vmaddr, vmsize, fileoff, filesize)
        self.symtab = None
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = unpack('<II', b[off:off + 8])
            if cmdsize == 0:
                break
            if cmd == LC_SEGMENT_64:
                segname = b[off + 8:off + 24].rstrip(b'\x00').decode()
                vmaddr, vmsize, fileoff, filesize = unpack('<4Q', b[off + 24:off + 56])
                self.segments.append((segname, vmaddr, vmsize, fileoff, filesize))
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = unpack('<4I', b[off + 8:off + 24])
                self.symtab = (symoff, nsyms, stroff, strsize)
            off += cmdsize

    def vm_to_offset(self, addr):
        for segname, vmaddr, vmsize, fileoff, filesize in self.segments:
            if vmaddr <= addr < vmaddr + vmsize:
                return fileoff + (addr - vmaddr)
        raise ValueError(f'address {addr:#x} is not backed by any segment')

    def symbols(self):
        if self.symtab is None:
            return {}
        symoff, nsyms, stroff, strsize = self.symtab
        strtab = self.buf[stroff:stroff + strsize]
        out = {}
        for i in range(nsyms):
            base = symoff + i * 16
            n_strx, n_type, n_sect, n_desc, n_value = unpack('<IBBHQ', self.buf[base:base + 16])
            name = strtab[n_strx:strtab.index(b'\x00', n_strx)].decode(errors='replace')
            out[name] = n_value
        return out

    @property
    def arch(self):
        return {CPU_TYPE_ARM64: 'arm64', CPU_TYPE_X86_64: 'x64'}.get(self.cputype, 'unknown')


def find_dart_snapshot_offsets(libapp_file):
    """returns (snapshot_data_offset, snapshot_instructions_offset)"""
    img = MachOImage(libapp_file)
    syms = img.symbols()
    data_addr = syms.get('_kDartSnapshotData')
    text_addr = syms.get('_kDartSnapshotText')
    if data_addr is None or text_addr is None:
        # pre-2.19 style: exported snapshot symbols
        for alt in ('_kDartVmSnapshotData', '_kDartIsolateSnapshotData'):
            if alt in syms:
                data_addr = syms[alt]
                break
        for alt in ('_kDartVmSnapshotText', '_kDartIsolateSnapshotText'):
            if alt in syms:
                text_addr = syms[alt]
                break
    if data_addr is None or text_addr is None:
        raise ValueError('cannot locate Dart snapshot symbols in Mach-O')
    return img.vm_to_offset(data_addr), img.vm_to_offset(text_addr)


def extract_snapshot_hash_flags_macho(libapp_file):
    data_off, _ = find_dart_snapshot_offsets(libapp_file)
    with open(libapp_file, 'rb') as f:
        f.seek(data_off + 20)          # header: magic(4) length(4) kind(4) pad(8)
        snapshot_hash = f.read(32).decode()
        data = f.read(512)
    flags = data[:data.index(b'\x00')].decode().strip().split(' ')
    return snapshot_hash, flags

def extract_snapshot_hash_flags(libapp_file):
    with open(libapp_file, 'rb') as f:
        magic = unpack('<I', f.read(4))[0]
    if magic == MH_MAGIC_64:
        return extract_snapshot_hash_flags_macho(libapp_file)
    with open(libapp_file, 'rb') as f:
        elf = ELFFile(f)
        # find "_kDartVmSnapshotData" symbol
        dynsym = elf.get_section_by_name('.dynsym')
        sym = dynsym.get_symbol_by_name('_kDartVmSnapshotData')[0]
        #section = elf.get_section(sym['st_shndx'])
        assert sym['st_size'] > 128
        f.seek(sym['st_value']+20)
        snapshot_hash = f.read(32).decode()
        data = f.read(256) # should be enough
        flags = data[:data.index(b'\0')].decode().strip().split(' ')
    
    return snapshot_hash, flags

def extract_libflutter_info(libflutter_file):
    with open(libflutter_file, 'rb') as f:
        magic = unpack('<I', f.read(4))[0]
    if magic == MH_MAGIC_64:
        img = MachOImage(libflutter_file)
        data = img.buf
        engine_ids = [h.decode() for h in re.findall(b'\x00([a-f\\d]{40})(?=\x00)', data)]
        m = re.search(br'\x00([\d\w\.-]+) \((stable|beta|dev)\)', data)
        dart_version = None if m is None else m.group(1).decode()
        return engine_ids, dart_version, img.arch, 'ios'
    with open(libflutter_file, 'rb') as f:
        elf = ELFFile(f)
        if elf.header.e_machine == 'EM_AARCH64': # 183
            arch = 'arm64'
        elif elf.header.e_machine == 'EM_IA_64': # 50
            arch = 'x64'
        else:
            assert False, f"Unsupport architecture: {elf.header.e_machine}"

        section = elf.get_section_by_name('.rodata')
        data = section.data()
        
        sha_hashes = re.findall(b'\x00([a-f\\d]{40})(?=\x00)', data)
        #print(sha_hashes)
        # all possible engine ids
        engine_ids = [ h.decode() for h in sha_hashes ]
        assert len(engine_ids) == 2, f'found hashes {", ".join(engine_ids)}'
        
        # beta/dev version of flutter might not use stable dart version (we can get dart version from sdk with found engine_id)
        # support stable, beta and dev channels
        m = re.search(br'\x00([\d\w\.-]+) \((stable|beta|dev)\)', data)
        if m is None:
            dart_version = None
        else:
            dart_version = m.group(1).decode()
        
    return engine_ids, dart_version, arch, 'android'

def get_dart_sdk_url_size(engine_ids):
    #url = f'https://storage.googleapis.com/dart-archive/channels/stable/release/3.0.3/sdk/dartsdk-windows-x64-release.zip'
    for engine_id in engine_ids:
        url = f'https://storage.googleapis.com/flutter_infra_release/flutter/{engine_id}/dart-sdk-windows-x64.zip'
        resp = requests.head(url)
        if resp.status_code == 200:
           sdk_size = int(resp.headers['Content-Length'])
           return engine_id, url, sdk_size
    
    return None, None, None

def get_dart_commit(url):
    # in downloaded zip
    # * dart-sdk/revision - the dart commit id of https://github.com/dart-lang/sdk/
    # * dart-sdk/version  - the dart version
    # revision and version zip file records should be in first 4096 bytes
    # using stream in case a server does not support range
    commit_id = None
    dart_version = None
    fp = None
    with requests.get(url, headers={"Range": "bytes=0-4096"}, stream=True) as r:
        if r.status_code // 10 == 20:
            x = next(r.iter_content(chunk_size=4096))
            fp = io.BytesIO(x)
    
    if fp is not None:
        while fp.tell() < 4096-30 and (commit_id is None or dart_version is None):
            #sig, ver, flags, compression, filetime, filedate, crc, compressSize, uncompressSize, filenameLen, extraLen = unpack(fp, '<IHHHHHIIIHH')
            _, _, _, compMethod, _, _, _, compressSize, _, filenameLen, extraLen = unpack('<IHHHHHIIIHH', fp.read(30))
            filename = fp.read(filenameLen)
            #print(filename)
            if extraLen > 0:
                fp.seek(extraLen, io.SEEK_CUR)
            data = fp.read(compressSize)
            
            # expect compression method to be zipfile.ZIP_DEFLATED
            assert compMethod == zipfile.ZIP_DEFLATED, 'Unexpected compression method'
            if filename == b'dart-sdk/revision':
                commit_id = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
            elif filename == b'dart-sdk/version':
                dart_version = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
    
    # TODO: if no revision and version in first 4096 bytes, get the file location from the first zip dir entries at the end of file (less than 256KB)
    return commit_id, dart_version

def extract_dart_info(libapp_file: str, libflutter_file: str):
    snapshot_hash, flags = extract_snapshot_hash_flags(libapp_file)
    #print('snapshot hash', snapshot_hash)
    #print(flags)

    engine_ids, dart_version, arch, os_name = extract_libflutter_info(libflutter_file)
    # print('possible engine ids', engine_ids)
    # print('dart version', dart_version)

    if dart_version is None:
        engine_id, sdk_url, sdk_size = get_dart_sdk_url_size(engine_ids)
        # print(engine_id)
        # print(sdk_url)
        # print(sdk_size)

        commit_id, dart_version = get_dart_commit(sdk_url)
        # print(commit_id)
        # print(dart_version)
        #assert dart_version == dart_version_sdk
    
    # TODO: os (android or ios) and architecture (arm64 or x64)
    return dart_version, snapshot_hash, flags, arch, os_name


if __name__ == "__main__":
    libdir = sys.argv[1]
    libapp_file = os.path.join(libdir, 'libapp.so')
    libflutter_file = os.path.join(libdir, 'libflutter.so')

    print(extract_dart_info(libapp_file, libflutter_file))

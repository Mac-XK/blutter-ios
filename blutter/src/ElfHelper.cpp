#include "pch.h"
#include "ElfHelper.h"
PRAGMA_WARNING(push, 0)
#include <platform/elf.h>
#if defined(DART_TARGET_OS_MACOS)
// old dart version has no mach_o.h
//#include <platform/mach_o.h>
#endif
PRAGMA_WARNING(pop)
#include <algorithm>
#include <vector>
#include <stdexcept>
#if defined(_WIN32) || defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
//#include <dlfcn.h>
#include <sys/mman.h>
#endif // #if defined(_WIN32) || defined(WIN32)

struct ElfIdent {
	uint8_t ei_magic[4];
	uint8_t ei_class;
	uint8_t ei_data;
	uint8_t ei_version;
	uint8_t ei_osabi;
	uint8_t ei_abiversion;
	uint8_t pad1[7];
};

using namespace dart::elf;

// Dart >= 3.13 dropped these from platform/elf.h; values are the real ELF symbol names
static constexpr const char* kVmSnapshotDataAsmSymbol = "_kDartVmSnapshotData";
static constexpr const char* kVmSnapshotInstructionsAsmSymbol = "_kDartVmSnapshotInstructions";
static constexpr const char* kIsolateSnapshotDataAsmSymbol = "_kDartIsolateSnapshotData";
static constexpr const char* kIsolateSnapshotInstructionsAsmSymbol = "_kDartIsolateSnapshotInstructions";


#ifdef _WIN32
static void* load_map_file(const char* path)
{
	HANDLE hFile = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		printf("\nCannot find %s\n", path);
		return NULL;
	}

	// because Dart API requires only snapshot buffer addresses (no relative access across snapshot),
	//   so we can just mapping a whole file and find address of snapshots
	HANDLE hMapFile = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hMapFile == INVALID_HANDLE_VALUE)
		return NULL;

	// need RW because dart initialization need writing data in BSS
	void* mem = MapViewOfFile(hMapFile, FILE_MAP_COPY, 0, 0, 0);
	CloseHandle(hMapFile);

	CloseHandle(hFile);
	return mem;
}
#else
static void* load_map_file(const char* path)
{
	// need RW because dart initialization need writing data in BSS
	int fd = open(path, O_RDONLY);
	struct stat st;

	fstat(fd, &st);
	void* mem = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

	close(fd);
	return mem;
}
#endif


// ---------------------------------------------------------------------------
// Minimal Mach-O (iOS App.framework) support added for pure-static iOS apps.
// iOS builds ship a *fused* AOT snapshot: only _kDartSnapshotData (data) and
// _kDartSnapshotText (code) exist, unlike Android's 4 separate symbols.
// ---------------------------------------------------------------------------
namespace macho {

struct mach_header_64 {
	uint32_t magic;
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};

struct load_command {
	uint32_t cmd;
	uint32_t cmdsize;
};

struct segment_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	uint32_t maxprot;
	uint32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct symtab_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t symoff;
	uint32_t nsyms;
	uint32_t stroff;
	uint32_t strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type;
	uint8_t n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

static constexpr uint32_t MH_MAGIC_64 = 0xfeedfacfu;
static constexpr uint32_t LC_SEGMENT_64 = 0x19u;
static constexpr uint32_t LC_SYMTAB = 0x2u;

struct VmToFile {
	const segment_command_64* seg;
	uintptr_t off(const uint64_t addr) const {
		return (uintptr_t)(addr - seg->vmaddr + seg->fileoff);
	}
	bool contains(uint64_t addr) const { return addr >= seg->vmaddr && addr < seg->vmaddr + seg->vmsize; }
};

static const uint8_t* resolve(const uint8_t* base, const std::vector<VmToFile>& segs, uint64_t addr)
{
	for (const auto& s : segs) {
		if (s.contains(addr)) {
			return base + s.off(addr);
		}
	}
	return nullptr;
}

} // namespace macho

static LibAppInfo findSnapshotsMachO(const uint8_t* base)
{
	using namespace macho;
	const auto* hdr = (const mach_header_64*)base;
	if (hdr->magic != MH_MAGIC_64)
		throw std::invalid_argument("Mach-O: invalid or non 64-bit magic");
	if (hdr->cputype != 0x0100000c) // CPU_TYPE_ARM64
		throw std::invalid_argument("Mach-O: expected arm64 (only arm64 AOT is supported)");

	std::vector<VmToFile> segs;
	const symtab_command* symtab = nullptr;

	const uint8_t* p = base + sizeof(mach_header_64);
	for (uint32_t i = 0; i < hdr->ncmds; i++) {
		const auto* lc = (const load_command*)p;
		if (lc->cmdsize == 0)
			break;
		if (lc->cmd == LC_SEGMENT_64) {
			segs.push_back({ (const segment_command_64*)lc });
		}
		else if (lc->cmd == LC_SYMTAB) {
			symtab = (const symtab_command*)lc;
		}
		p += lc->cmdsize;
	}

	if (symtab == nullptr)
		throw std::invalid_argument("Mach-O: no LC_SYMTAB (binary is stripped?)");

	const char* strtab = (const char*)base + symtab->stroff;
	const auto* syms = (const nlist_64*)(base + symtab->symoff);

	const uint8_t* snap_data = nullptr;
	const uint8_t* snap_text = nullptr;
	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		const char* name = strtab + syms[i].n_strx;
		if (strcmp(name, "_kDartSnapshotData") == 0)
			snap_data = resolve(base, segs, syms[i].n_value);
		else if (strcmp(name, "_kDartSnapshotText") == 0)
			snap_text = resolve(base, segs, syms[i].n_value);
	}

	if (snap_data == nullptr)
		throw std::invalid_argument("Mach-O: cannot find _kDartSnapshotData");
	if (snap_text == nullptr)
		throw std::invalid_argument("Mach-O: cannot find _kDartSnapshotText");

	// fused snapshot: every slot points at the same buffer pair
	return LibAppInfo{
		.lib = base,
		.vm_snapshot_data = snap_data,
		.vm_snapshot_instructions = snap_text,
		.isolate_snapshot_data = snap_data,
		.isolate_snapshot_instructions = snap_text,
	};
}

LibAppInfo ElfHelper::findSnapshots(const uint8_t* elf)
{
	const auto* hdr = (const ElfHeader*)elf;
	if (hdr->section_table_entry_size != sizeof(SectionHeader))
		throw std::invalid_argument("ELF: Invalid section entry size");

	const auto* section = (SectionHeader*)(elf + hdr->section_table_offset);
	const auto sh_num = hdr->num_section_headers;

	// find .dynstr and .dynsym sections, so we can map the section names
	const char* dynstr = nullptr;
	const Symbol* dynsym = nullptr;
	const Symbol* dynsym_end = nullptr;
	for (uint16_t i = 0; i < sh_num; i++, section++) {
		if (section->type == SectionHeaderType::SHT_STRTAB && dynstr == nullptr) {
			// we want only .dynstr for .dynsym
			const char* strtab = (const char*)elf + section->file_offset;
			const char* last = strtab + section->file_size;
			const char* s_first = kVmSnapshotDataAsmSymbol;
			const char* s_last = s_first + strlen(kVmSnapshotDataAsmSymbol) + 1;
			//if (memmem(strtab, section->s_size, kVmSnapshotDataAsmSymbol, strlen(kVmSnapshotDataAsmSymbol))) {
			if (std::search(strtab, last, s_first, s_last) != last) {
				// found it
				dynstr = strtab;
			}
		}
		if (section->type == SectionHeaderType::SHT_DYNSYM) {
			if (section->entry_size != sizeof(Symbol))
				throw std::invalid_argument("ELF: Invalid DYNSYM entry size");
			dynsym = (Symbol*)(elf + section->file_offset);
			dynsym_end = (Symbol*)(elf + section->file_offset + section->file_size);
		}
		if (dynsym != nullptr && dynstr != nullptr)
			break;
	}

	// find the required symbol addresses
	const uint8_t* vm_snapshot_data = nullptr;
	const uint8_t* vm_snapshot_instructions = nullptr;
	const uint8_t* isolate_snapshot_data = nullptr;
	const uint8_t* isolate_snapshot_instructions = nullptr;
	for (; dynsym < dynsym_end; dynsym++) {
		if (dynsym->info == 0)
			continue;

		const char* name = dynstr + dynsym->name;
		// Note: sym_size is no needed for dart VM (its blob contains size)
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) {
			vm_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) {
			vm_snapshot_instructions = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) {
			isolate_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) {
			isolate_snapshot_instructions = elf + dynsym->value;
		}
	}

	if (vm_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Data");
	if (vm_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Instructions");
	if (isolate_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Data");
	if (isolate_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Instructions");

	return LibAppInfo{
		.lib = elf,
		.vm_snapshot_data = vm_snapshot_data,
		.vm_snapshot_instructions = vm_snapshot_instructions,
		.isolate_snapshot_data = isolate_snapshot_data,
		.isolate_snapshot_instructions = isolate_snapshot_instructions,
	};
}

LibAppInfo ElfHelper::MapLibAppSo(const char* path)
{
	void* lib = load_map_file(path);
	uint8_t* img = (uint8_t*)(lib);
	if (img == nullptr)
		throw std::invalid_argument("Cannot map input file");

	// sniff container format instead of trusting the host OS
	const uint32_t magic = *(const uint32_t*)img;
	if (magic == macho::MH_MAGIC_64 || magic == 0xcffaedfe /* MH_CIGAM_64 */) {
		if (magic == 0xcffaedfe)
			throw std::invalid_argument("Mach-O: byte-swapped image is not supported");
		return findSnapshotsMachO(img);
	}

	const auto* hdr = (ElfHeader*)img;
	const auto* ident = (ElfIdent*)hdr->ident;
	if (memcmp(ident->ei_magic, "\x7f" "ELF", 4) != 0)
		throw std::invalid_argument("Unknown container format (neither ELF nor Mach-O)");
	if (ident->ei_data != 1)
		throw std::invalid_argument("ELF: Support only little endian");
	if (ident->ei_class != ELFCLASS64)
		throw std::invalid_argument("ELF: Support only 64 bits");

	return findSnapshots(img);
}

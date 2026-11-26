#include "../runtime/pe32_image.h"
#include "../runtime/guest_address_space.h"
#include "../runtime/import_binder.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using br::pe32::Image;

static bool readFile(const fs::path& path, std::vector<uint8_t>& bytes)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream) return false;
	bytes.assign(std::istreambuf_iterator<char>(stream), {});
	return stream.good() || stream.eof();
}

static uint16_t u16(const std::vector<uint8_t>& b, size_t p)
{
	return static_cast<uint16_t>(uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8));
}

static uint32_t u32(const std::vector<uint8_t>& b, size_t p)
{
	return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) |
		(uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}

static uint32_t u32(const uint8_t* b)
{
	return uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
		(uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

static bool sameNoCase(std::string a, std::string b)
{
	std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return a == b;
}

static bool checkRelocations(const Image& preferred, const Image& rebased, std::string& error)
{
	const uint32_t pe = u32(preferred.bytes, 0x3c);
	const uint16_t optionalSize = u16(preferred.bytes, pe + 20);
	const size_t directories = size_t(pe) + 24 + 96;
	const uint32_t relocRva = u32(preferred.bytes, directories + 5 * 8);
	const uint32_t relocSize = u32(preferred.bytes, directories + 5 * 8 + 4);
	const uint64_t end = uint64_t(relocRva) + relocSize;
	if (optionalSize < 96 + 6 * 8 || !relocRva || end > preferred.bytes.size()) {
		error = "missing or invalid relocation directory in test image";
		return false;
	}
	uint32_t checked = 0;
	for (uint64_t block = relocRva; block < end;) {
		const uint32_t page = u32(preferred.bytes, static_cast<size_t>(block));
		const uint32_t blockSize = u32(preferred.bytes, static_cast<size_t>(block + 4));
		if (blockSize < 8 || block + blockSize > end || ((blockSize - 8) & 1)) {
			error = "malformed relocation block in test oracle";
			return false;
		}
		for (uint64_t p = block + 8; p < block + blockSize; p += 2) {
			const uint16_t item = u16(preferred.bytes, static_cast<size_t>(p));
			const uint16_t type = item >> 12;
			if (type == 0) continue;
			if (type != 3) { error = "unexpected non-HIGHLOW relocation in test image"; return false; }
			const uint64_t target = uint64_t(page) + (item & 0x0fff);
			if (target + 4 > preferred.bytes.size()) { error = "relocation target outside image"; return false; }
			const uint32_t before = u32(preferred.bytes, static_cast<size_t>(target));
			const uint32_t after = u32(rebased.bytes, static_cast<size_t>(target));
			if (after != before + (rebased.loadBase - preferred.loadBase)) {
				error = "rebased HIGHLOW target did not receive the image delta";
				return false;
			}
			++checked;
		}
		block += blockSize;
	}
	if (checked != rebased.appliedRelocationCount) {
		error = "loader relocation count disagrees with independent oracle";
		return false;
	}
	return true;
}

static bool loadImage(const fs::path& path, uint32_t base, Image& image, std::string& error)
{
	std::vector<uint8_t> file;
	if (!readFile(path, file)) { error = "cannot read " + path.string(); return false; }
	return br::pe32::load(file, base, image, error);
}

int main(int argc, char** argv)
{
	if (argc != 3) {
		std::cerr << "usage: pe32_loader_smoke <game-exe> <scripts-directory>\n";
		return 2;
	}
	std::string error;
	Image mainImage;
	if (!loadImage(argv[1], 0x00400000, mainImage, error)) {
		std::cerr << "main PE load failed: " << error << '\n'; return 1;
	}
	if (mainImage.preferredBase != 0x00400000 || mainImage.loadBase != mainImage.preferredBase ||
		mainImage.entryAddress() != mainImage.loadBase + mainImage.entryRva || mainImage.imports.empty() || mainImage.exports.empty()) {
		std::cerr << "main PE metadata invariant failed\n"; return 1;
	}
	std::unordered_map<std::string, const br::pe32::Export*> gameExportsByName;
	std::unordered_map<uint32_t, const br::pe32::Export*> gameExportsByOrdinal;
	for (const auto& item : mainImage.exports) {
		gameExportsByOrdinal.emplace(item.ordinal, &item);
		for (const auto& name : item.names) gameExportsByName.emplace(name, &item);
	}
	br::runtime::GuestAddressSpace guest;
	if (!guest.reserve(error) || !guest.mapImage(mainImage, error)) {
		std::cerr << "guest address-space main image mapping failed: " << error << '\n'; return 1;
	}
	const uint8_t* guestMain = guest.host(mainImage.loadBase, 2);
	if (!guestMain || guestMain[0] != 'M' || guestMain[1] != 'Z' ||
		std::memcmp(guestMain, mainImage.bytes.data(), mainImage.bytes.size()) != 0) {
		std::cerr << "guest main image bytes differ after mapping\n"; return 1;
	}
	if (guest.mapImage(mainImage, error)) {
		std::cerr << "guest address-space accepted an overlapping image\n"; return 1;
	}
	br::runtime::ImportBindingTable importBindings;
	br::runtime::ImportBindStats mainBindStats;
	if (!importBindings.bindImageImports(mainImage, mainImage, "Battle_Realms_F.exe", guest, mainBindStats, error) ||
		mainBindStats.executableExportsResolved != 0 || mainBindStats.apiImportReferences != mainImage.imports.size()) {
		std::cerr << "main executable IAT binding failed: " << error << '\n'; return 1;
	}
	for (const auto& import : mainImage.imports) {
		const uint32_t iat = u32(mainImage.bytes, import.iatRva);
		const uint8_t* guestIat = guest.host(mainImage.loadBase + import.iatRva, 4);
		const auto* stub = importBindings.lookup(iat);
		if (!guestIat || u32(guestIat) != iat || !stub || !sameNoCase(stub->library, import.library) ||
			stub->byOrdinal != import.byOrdinal || (import.byOrdinal ? stub->ordinal != import.ordinal : stub->name != import.name)) {
			std::cerr << "main IAT does not contain a tracked API stub\n"; return 1;
		}
	}
	Image wronglyRebasedMain;
	if (loadImage(argv[1], mainImage.preferredBase + 0x10000000, wronglyRebasedMain, error)) {
		std::cerr << "main PE unexpectedly accepted a rebase without relocations\n"; return 1;
	}
	std::cout << "MAIN preferred=0x" << std::hex << mainImage.preferredBase << std::dec
		<< " sections=" << mainImage.sections.size() << " imports=" << mainImage.imports.size()
		<< " no-reloc-rebase=rejected\n";

	std::vector<fs::path> dlls;
	for (const auto& item : fs::directory_iterator(argv[2])) {
		std::string extension = item.path().extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		if (item.is_regular_file() && extension == ".dll") dlls.push_back(item.path());
	}
	std::sort(dlls.begin(), dlls.end());
	uint32_t nextBase = 0x20000000;
	size_t loaded = 0, skippedNonPe = 0, totalImports = 0;
	size_t gameExeImports = 0, resolvedGameExeImports = 0, unresolvedGameExeImports = 0;
	uint64_t totalApiImportReferences = mainBindStats.apiImportReferences;
	uint64_t totalFixups = 0;
	for (const auto& path : dlls) {
		std::vector<uint8_t> file;
		if (!readFile(path, file)) { std::cerr << "cannot read " << path << '\n'; return 1; }
		Image preferred;
		if (!br::pe32::load(file, 0x10000000, preferred, error)) {
			if (path.filename() == "Serpent 5E_F.dll") { ++skippedNonPe; continue; }
			std::cerr << "DLL preferred-base load failed for " << path << ": " << error << '\n'; return 1;
		}
		if (preferred.preferredBase != 0x10000000 || preferred.relocationCount != 0) {
			std::cerr << "DLL preferred-base invariant failed for " << path << '\n'; return 1;
		}
		const uint32_t base = nextBase;
		Image relocated;
		if (!br::pe32::load(file, base, relocated, error)) {
			std::cerr << "DLL relocation failed for " << path << ": " << error << '\n'; return 1;
		}
		if (relocated.preferredBase != 0x10000000 || relocated.loadBase != base ||
			relocated.entryAddress() != base + relocated.entryRva || relocated.appliedRelocationCount == 0 ||
			!checkRelocations(preferred, relocated, error)) {
			std::cerr << "DLL relocation check failed for " << path << ": " << error << '\n'; return 1;
		}
		for (const auto& import : relocated.imports) {
			std::string library = import.library;
			std::transform(library.begin(), library.end(), library.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			if (library != "battle_realms_f.exe") continue;
			++gameExeImports;
			const auto found = import.byOrdinal ? gameExportsByOrdinal.find(import.ordinal) != gameExportsByOrdinal.end()
				: gameExportsByName.find(import.name) != gameExportsByName.end();
			if (found) ++resolvedGameExeImports;
			else ++unresolvedGameExeImports;
		}
		if (!guest.mapImage(relocated, error)) {
			std::cerr << "guest address-space DLL mapping failed for " << path << ": " << error << '\n'; return 1;
		}
		br::runtime::ImportBindStats bindStats;
		if (!importBindings.bindImageImports(relocated, mainImage, "Battle_Realms_F.exe", guest, bindStats, error)) {
			std::cerr << "DLL IAT binding failed for " << path << ": " << error << '\n'; return 1;
		}
		totalApiImportReferences += bindStats.apiImportReferences;
		const uint8_t* guestDll = guest.host(base, 2);
		if (!guestDll || guestDll[0] != 'M' || guestDll[1] != 'Z' ||
			std::memcmp(guestDll, relocated.bytes.data(), relocated.bytes.size()) != 0) {
			std::cerr << "guest DLL bytes differ after mapping for " << path << '\n'; return 1;
		}
		for (const auto& import : relocated.imports) {
			const uint32_t iat = u32(relocated.bytes, import.iatRva);
			const uint8_t* guestIat = guest.host(base + import.iatRva, 4);
			if (!guestIat || u32(guestIat) != iat) {
				std::cerr << "guest IAT bytes differ after binding for " << path << '\n'; return 1;
			}
			std::string library = import.library;
			std::transform(library.begin(), library.end(), library.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			if (library == "battle_realms_f.exe") {
				const br::pe32::Export* symbol = nullptr;
				for (const auto& candidate : mainImage.exports) {
					if ((import.byOrdinal && candidate.ordinal == import.ordinal) ||
						(!import.byOrdinal && std::find(candidate.names.begin(), candidate.names.end(), import.name) != candidate.names.end())) {
						symbol = &candidate; break;
					}
				}
				if (!symbol || iat != mainImage.loadBase + symbol->rva) {
					std::cerr << "script IAT does not target the matching game export for " << path << '\n'; return 1;
				}
			} else {
				const auto* stub = importBindings.lookup(iat);
				if (!stub || stub->guestAddress != iat || !sameNoCase(stub->library, import.library) ||
					stub->byOrdinal != import.byOrdinal || (import.byOrdinal ? stub->ordinal != import.ordinal : stub->name != import.name)) {
					std::cerr << "OS import IAT does not target a tracked API stub for " << path << '\n'; return 1;
				}
			}
		}
		const uint64_t next = (uint64_t(base) + relocated.sizeOfImage + 0xffff) & ~uint64_t(0xffff);
		if (next > 0x70000000) { std::cerr << "test module address range exhausted\n"; return 1; }
		nextBase = static_cast<uint32_t>(next);
		totalImports += relocated.imports.size();
		totalFixups += relocated.appliedRelocationCount;
		++loaded;
	}
	if (loaded != 68 || skippedNonPe != 1) {
		std::cerr << "unexpected DLL catalog: valid=" << loaded << " invalid=" << skippedNonPe << '\n';
		return 1;
	}
	if (!gameExeImports || unresolvedGameExeImports) {
		std::cerr << "script imports against the main EXE did not fully resolve\n"; return 1;
	}
	std::cout << "SCRIPTS valid=" << loaded << " non_pe=" << skippedNonPe
		<< " imports=" << totalImports << " rebased_HIGHLOW=" << totalFixups
		<< " executable_exports=" << mainImage.exports.size()
		<< " game_EXE_imports=" << gameExeImports << " exports_resolved=" << resolvedGameExeImports
		<< " api_import_refs=" << totalApiImportReferences << " unique_api_stubs=" << importBindings.apiStubs().size()
		<< " distinct_logical_ranges=yes guest_window=4GiB images_mapped=69\n";

	std::vector<uint8_t> malformed(64, 0);
	Image rejected;
	if (br::pe32::load(malformed, 0x00400000, rejected, error)) {
		std::cerr << "malformed PE input was accepted\n"; return 1;
	}
	std::cout << "MALFORMED truncated_input=rejected\nPASS\n";
	return 0;
}

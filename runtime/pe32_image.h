#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace br::pe32 {

struct Section {
	std::string name;
	uint32_t rva = 0;
	uint32_t virtualSize = 0;
	uint32_t rawSize = 0;
	uint32_t characteristics = 0;
};

struct Import {
	std::string library;
	std::string name;
	uint16_t ordinal = 0;
	uint32_t iatRva = 0;
	bool byOrdinal = false;
};

struct Export {
	uint32_t ordinal = 0;
	uint32_t rva = 0;
	std::vector<std::string> names;
	std::string forwarder;
};

struct Image {
	uint32_t preferredBase = 0;
	uint32_t loadBase = 0;
	uint32_t entryRva = 0;
	uint32_t declaredSizeOfImage = 0;
	uint32_t sizeOfImage = 0;
	uint32_t sizeOfHeaders = 0;
	uint32_t stackReserveSize = 0;
	uint32_t stackCommitSize = 0;
	uint32_t relocationCount = 0;
	uint32_t appliedRelocationCount = 0;
	std::vector<Section> sections;
	std::vector<Import> imports;
	std::vector<Export> exports;
	std::vector<uint8_t> bytes;

	uint32_t entryAddress() const { return loadBase + entryRva; }
};

// Maps a PE32/I386 file into a zero-filled image and applies HIGHLOW base
// relocations when loadBase differs from the preferred base. It deliberately
// records imports without binding them to any host API implementation.
bool load(const std::vector<uint8_t>& file, uint32_t loadBase,
	Image& out, std::string& error);

} // namespace br::pe32

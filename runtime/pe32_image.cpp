#include "pe32_image.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace br::pe32 {
namespace {

bool range(size_t size, uint64_t offset, uint64_t length)
{
	return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

bool read16(const std::vector<uint8_t>& data, uint64_t offset, uint16_t& value)
{
	if (!range(data.size(), offset, 2)) return false;
	value = static_cast<uint16_t>(data[offset] | (uint16_t(data[offset + 1]) << 8));
	return true;
}

bool read32(const std::vector<uint8_t>& data, uint64_t offset, uint32_t& value)
{
	if (!range(data.size(), offset, 4)) return false;
	value = uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
		(uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
	return true;
}

bool write32(std::vector<uint8_t>& data, uint64_t offset, uint32_t value)
{
	if (!range(data.size(), offset, 4)) return false;
	for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (i * 8));
	return true;
}

bool imageRead16(const Image& image, uint64_t rva, uint16_t& value)
{
	return read16(image.bytes, rva, value);
}

bool imageRead32(const Image& image, uint64_t rva, uint32_t& value)
{
	return read32(image.bytes, rva, value);
}

bool cstring(const std::vector<uint8_t>& data, uint64_t rva, std::string& value)
{
	if (rva >= data.size()) return false;
	auto begin = data.begin() + static_cast<ptrdiff_t>(rva);
	auto end = std::find(begin, data.end(), uint8_t(0));
	if (end == data.end()) return false;
	value.assign(reinterpret_cast<const char*>(&*begin), static_cast<size_t>(end - begin));
	return true;
}

bool fail(std::string& error, const char* message)
{
	error = message;
	return false;
}

} // namespace

bool load(const std::vector<uint8_t>& file, uint32_t loadBase,
	Image& out, std::string& error)
{
	error.clear();
	Image image;
	uint16_t mz = 0;
	uint32_t peOffset = 0, signature = 0;
	if (!read16(file, 0, mz) || mz != 0x5a4d) return fail(error, "missing MZ header");
	if (!read32(file, 0x3c, peOffset) || !read32(file, peOffset, signature) || signature != 0x00004550)
		return fail(error, "invalid PE header offset or signature");

	uint16_t machine = 0, sectionCount = 0, optionalSize = 0;
	if (!read16(file, uint64_t(peOffset) + 4, machine) || machine != 0x014c)
		return fail(error, "PE machine is not I386");
	if (!read16(file, uint64_t(peOffset) + 6, sectionCount) || !sectionCount || sectionCount > 96 ||
		!read16(file, uint64_t(peOffset) + 20, optionalSize))
		return fail(error, "invalid COFF section count or optional-header size");

	const uint64_t optional = uint64_t(peOffset) + 24;
	uint16_t magic = 0;
	uint32_t numDirectories = 0;
	uint32_t sectionAlignment = 0;
	if (optionalSize < 96 || !read16(file, optional, magic) || magic != 0x010b ||
		!read32(file, optional + 16, image.entryRva) ||
		!read32(file, optional + 28, image.preferredBase) ||
		!read32(file, optional + 32, sectionAlignment) ||
		!read32(file, optional + 56, image.declaredSizeOfImage) ||
		!read32(file, optional + 60, image.sizeOfHeaders) ||
		!read32(file, optional + 72, image.stackReserveSize) ||
		!read32(file, optional + 76, image.stackCommitSize) ||
		!read32(file, optional + 92, numDirectories))
		return fail(error, "invalid PE32 optional header");
	if (!sectionAlignment || (sectionAlignment & (sectionAlignment - 1)) || sectionAlignment > 0x100000 ||
		!image.declaredSizeOfImage || image.declaredSizeOfImage > 0x7fffffff ||
		image.stackCommitSize > image.stackReserveSize ||
		image.sizeOfHeaders > file.size() || image.entryRva >= image.declaredSizeOfImage)
		return fail(error, "invalid image/header/entry size");
	const uint64_t mappedSize = (uint64_t(image.declaredSizeOfImage) + sectionAlignment - 1) & ~uint64_t(sectionAlignment - 1);
	if (!mappedSize || mappedSize > 0x7fffffff) return fail(error, "aligned image size is invalid");
	image.sizeOfImage = static_cast<uint32_t>(mappedSize);
	if (image.sizeOfHeaders > image.sizeOfImage) return fail(error, "headers exceed mapped image size");
	if (uint64_t(loadBase) + image.sizeOfImage > (uint64_t(1) << 32))
		return fail(error, "requested guest image range overflows 32-bit address space");
	image.loadBase = loadBase;
	if (!range(file.size(), optional, optionalSize)) return fail(error, "optional header extends past file");

	struct Directory { uint32_t rva = 0, size = 0; };
	auto directory = [&](uint32_t index, Directory& result) {
		if (index >= numDirectories) return true;
		if (uint64_t(96) + uint64_t(index + 1) * 8 > optionalSize) return false;
		return read32(file, optional + 96 + uint64_t(index) * 8, result.rva) &&
			read32(file, optional + 100 + uint64_t(index) * 8, result.size);
	};
	Directory exports, imports, relocations;
	if (!directory(0, exports) || !directory(1, imports) || !directory(5, relocations))
		return fail(error, "truncated PE data directories");
	if (exports.rva && !range(image.sizeOfImage, exports.rva, exports.size))
		return fail(error, "export directory is outside mapped image");
	if (imports.rva && !range(image.sizeOfImage, imports.rva, imports.size))
		return fail(error, "import directory is outside mapped image");
	if (relocations.rva && !range(image.sizeOfImage, relocations.rva, relocations.size))
		return fail(error, "relocation directory is outside mapped image");
	if (loadBase != image.preferredBase && (!relocations.rva || !relocations.size))
		return fail(error, "image has no base-relocation directory for requested rebase");

	image.bytes.assign(image.sizeOfImage, 0);
	std::copy_n(file.begin(), image.sizeOfHeaders, image.bytes.begin());
	const uint64_t sectionTable = optional + optionalSize;
	if (!range(file.size(), sectionTable, uint64_t(sectionCount) * 40))
		return fail(error, "section table extends past file");
	image.sections.reserve(sectionCount);
	for (uint16_t i = 0; i < sectionCount; ++i) {
		const uint64_t header = sectionTable + uint64_t(i) * 40;
		Section section;
		char name[9]{};
		std::memcpy(name, file.data() + header, 8);
		section.name = name;
		uint32_t span = 0, rawOffset = 0;
		if (!read32(file, header + 8, section.virtualSize) ||
			!read32(file, header + 12, section.rva) ||
			!read32(file, header + 16, section.rawSize) ||
			!read32(file, header + 20, rawOffset) ||
			!read32(file, header + 36, section.characteristics))
			return fail(error, "truncated section header");
		span = std::max(section.virtualSize, section.rawSize);
		if (!range(image.sizeOfImage, section.rva, span) || !range(file.size(), rawOffset, section.rawSize))
			return fail(error, "section range extends past image or file");
		if (section.rawSize)
			std::copy_n(file.begin() + rawOffset, section.rawSize, image.bytes.begin() + section.rva);
		image.sections.push_back(std::move(section));
	}

	if (exports.rva && exports.size) {
		if (exports.size < 40) return fail(error, "export directory is too small");
		uint32_t ordinalBase = 0, functionCount = 0, nameCount = 0;
		uint32_t functionsRva = 0, namesRva = 0, ordinalsRva = 0;
		if (!imageRead32(image, exports.rva + 16, ordinalBase) ||
			!imageRead32(image, exports.rva + 20, functionCount) ||
			!imageRead32(image, exports.rva + 24, nameCount) ||
			!imageRead32(image, exports.rva + 28, functionsRva) ||
			!imageRead32(image, exports.rva + 32, namesRva) ||
			!imageRead32(image, exports.rva + 36, ordinalsRva))
			return fail(error, "truncated export directory");
		if (functionCount > image.sizeOfImage / 4 || nameCount > functionCount ||
			!range(image.sizeOfImage, functionsRva, uint64_t(functionCount) * 4) ||
			!range(image.sizeOfImage, namesRva, uint64_t(nameCount) * 4) ||
			!range(image.sizeOfImage, ordinalsRva, uint64_t(nameCount) * 2))
			return fail(error, "export address/name/ordinal table is outside image");
		std::vector<Export> byIndex(functionCount);
		for (uint32_t i = 0; i < functionCount; ++i) {
			uint32_t functionRva = 0;
			if (!imageRead32(image, uint64_t(functionsRva) + uint64_t(i) * 4, functionRva))
				return fail(error, "truncated export address table");
			if (!functionRva) continue;
			if (uint64_t(ordinalBase) + i > std::numeric_limits<uint32_t>::max())
				return fail(error, "export ordinal overflows 32-bit range");
			byIndex[i].ordinal = ordinalBase + i;
			byIndex[i].rva = functionRva;
			if (functionRva >= exports.rva && uint64_t(functionRva) < uint64_t(exports.rva) + exports.size) {
				if (!cstring(image.bytes, functionRva, byIndex[i].forwarder))
					return fail(error, "invalid forwarded export string");
			} else if (functionRva >= image.sizeOfImage) {
				return fail(error, "export target RVA is outside image");
			}
		}
		for (uint32_t i = 0; i < nameCount; ++i) {
			uint32_t nameRva = 0;
			uint16_t ordinalIndex = 0;
			if (!imageRead32(image, uint64_t(namesRva) + uint64_t(i) * 4, nameRva) ||
				!imageRead16(image, uint64_t(ordinalsRva) + uint64_t(i) * 2, ordinalIndex) ||
				ordinalIndex >= byIndex.size())
				return fail(error, "invalid export name ordinal");
			std::string name;
			if (!cstring(image.bytes, nameRva, name)) return fail(error, "invalid export name string");
			if (!byIndex[ordinalIndex].rva) return fail(error, "named export has no function address");
			byIndex[ordinalIndex].names.push_back(std::move(name));
		}
		for (auto& item : byIndex) if (item.rva) image.exports.push_back(std::move(item));
	}

	const uint32_t delta = loadBase - image.preferredBase;
	if (delta) {
		uint64_t cursor = relocations.rva;
		const uint64_t end = uint64_t(relocations.rva) + relocations.size;
		while (cursor < end) {
			uint32_t pageRva = 0, blockSize = 0;
			if (!imageRead32(image, cursor, pageRva) || !imageRead32(image, cursor + 4, blockSize) ||
				blockSize < 8 || blockSize > end - cursor || ((blockSize - 8) & 1))
				return fail(error, "malformed base-relocation block");
			for (uint64_t p = cursor + 8; p < cursor + blockSize; p += 2) {
				uint16_t item = 0;
				if (!imageRead16(image, p, item)) return fail(error, "truncated base-relocation entry");
				const uint16_t type = item >> 12;
				const uint64_t target = uint64_t(pageRva) + (item & 0x0fff);
				if (type == 0) continue; // IMAGE_REL_BASED_ABSOLUTE padding
				if (type != 3) return fail(error, "unsupported PE32 relocation type (only HIGHLOW is supported)");
				uint32_t value = 0;
				if (!imageRead32(image, target, value) || !write32(image.bytes, target, value + delta))
					return fail(error, "HIGHLOW relocation target is outside image");
				++image.relocationCount;
				++image.appliedRelocationCount;
			}
			cursor += blockSize;
		}
	}

	if (imports.rva && imports.size) {
		const uint64_t importEnd = uint64_t(imports.rva) + imports.size;
		bool terminated = false;
		for (uint64_t descriptor = imports.rva; descriptor + 20 <= importEnd; descriptor += 20) {
			uint32_t lookupRva = 0, nameRva = 0, iatRva = 0;
			if (!imageRead32(image, descriptor, lookupRva) ||
				!imageRead32(image, descriptor + 12, nameRva) ||
				!imageRead32(image, descriptor + 16, iatRva))
				return fail(error, "truncated import descriptor");
			if (!lookupRva && !nameRva && !iatRva) { terminated = true; break; }
			if (!nameRva || !iatRva) return fail(error, "incomplete import descriptor");
			std::string library;
			if (!cstring(image.bytes, nameRva, library)) return fail(error, "invalid import library name");
			const uint32_t namesRva = lookupRva ? lookupRva : iatRva;
			bool thunkTerminated = false;
			for (uint32_t index = 0; index < image.sizeOfImage / 4; ++index) {
				uint32_t thunk = 0;
				const uint64_t lookup = uint64_t(namesRva) + uint64_t(index) * 4;
				const uint64_t iat = uint64_t(iatRva) + uint64_t(index) * 4;
				if (!imageRead32(image, lookup, thunk) || !range(image.sizeOfImage, iat, 4))
					return fail(error, "import thunk array extends past image");
				if (!thunk) { thunkTerminated = true; break; }
				Import item;
				item.library = library;
				item.iatRva = static_cast<uint32_t>(iat);
				item.byOrdinal = (thunk & 0x80000000u) != 0;
				if (item.byOrdinal) item.ordinal = static_cast<uint16_t>(thunk & 0xffffu);
				else if (!imageRead16(image, thunk, item.ordinal) || !cstring(image.bytes, uint64_t(thunk) + 2, item.name))
					return fail(error, "invalid import-by-name thunk");
				image.imports.push_back(std::move(item));
			}
			if (!thunkTerminated) return fail(error, "unterminated import thunk array");
		}
		if (!terminated) return fail(error, "unterminated import descriptor table");
	}

	out = std::move(image);
	return true;
}

} // namespace br::pe32

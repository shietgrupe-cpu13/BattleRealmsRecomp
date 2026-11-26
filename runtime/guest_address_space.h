#pragma once

#include "pe32_image.h"

#include <cstdint>
#include <map>
#include <string>

namespace br::runtime {

// 32-bit guest addresses are offsets into one reserved 4 GiB host window.
// Images are committed on demand and kept at their selected guest addresses.
class GuestAddressSpace {
public:
	GuestAddressSpace() = default;
	~GuestAddressSpace();
	GuestAddressSpace(const GuestAddressSpace&) = delete;
	GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;

	bool reserve(std::string& error);
	bool commitRange(uint32_t address, uint32_t size, std::string& error);
	bool decommitRange(uint32_t address, uint32_t size, std::string& error);
	bool mapImage(const pe32::Image& image, std::string& error);
	uint8_t* host(uint32_t address, uint32_t size = 1);
	const uint8_t* host(uint32_t address, uint32_t size = 1) const;
	bool containsMapped(uint32_t address, uint32_t size) const;
	bool reserved() const { return base_ != nullptr; }
	uint64_t hostPageSize() const { return pageSize_; }
	uint8_t* base() { return base_; }

private:
	bool commit(uint32_t address, uint32_t size, std::string& error);
	uint8_t* base_ = nullptr;
	uint64_t pageSize_ = 0;
	std::map<uint64_t, uint64_t> mappedImages_;
};

} // namespace br::runtime

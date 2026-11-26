#include "guest_address_space.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace br::runtime {
namespace {
constexpr uint64_t kGuestWindowSize = uint64_t(1) << 32;

bool rangesOverlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
	return a0 < b1 && b0 < a1;
}
}

GuestAddressSpace::~GuestAddressSpace()
{
	if (!base_) return;
#if defined(_WIN32)
	VirtualFree(base_, 0, MEM_RELEASE);
#else
	munmap(base_, kGuestWindowSize);
#endif
}

bool GuestAddressSpace::reserve(std::string& error)
{
	if (base_) return true;
	void* memory = nullptr;
#if defined(_WIN32)
	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	pageSize_ = info.dwPageSize;
	if (!pageSize_ || (pageSize_ & (pageSize_ - 1))) {
		error = "host page size is invalid";
		return false;
	}
	memory = VirtualAlloc(nullptr, static_cast<SIZE_T>(kGuestWindowSize), MEM_RESERVE, PAGE_NOACCESS);
	if (!memory) {
#else
	const long page = sysconf(_SC_PAGESIZE);
	if (page <= 0 || (page & (page - 1))) {
		error = "host page size is invalid";
		return false;
	}
	pageSize_ = static_cast<uint64_t>(page);
	memory = mmap(nullptr, kGuestWindowSize, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (memory == MAP_FAILED) {
#endif
		error = "cannot reserve contiguous 4 GiB guest address window";
		return false;
	}
	base_ = static_cast<uint8_t*>(memory);
	return true;
}

bool GuestAddressSpace::commit(uint32_t address, uint32_t size, std::string& error)
{
	if (!base_) { error = "guest window is not reserved"; return false; }
	if (!size) return true;
	const uint64_t end = uint64_t(address) + size;
	if (end > kGuestWindowSize) { error = "guest commit range overflows 32-bit address space"; return false; }
	const uint64_t lo = uint64_t(address) & ~(pageSize_ - 1);
	const uint64_t hi = (end + pageSize_ - 1) & ~(pageSize_ - 1);
	bool protectedPages = false;
#if defined(_WIN32)
	protectedPages = VirtualAlloc(base_ + lo, static_cast<SIZE_T>(hi - lo), MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
	protectedPages = mprotect(base_ + lo, hi - lo, PROT_READ | PROT_WRITE) == 0;
#endif
	if (hi > kGuestWindowSize || !protectedPages) {
		error = "cannot commit guest pages";
		return false;
	}
	return true;
}

bool GuestAddressSpace::commitRange(uint32_t address, uint32_t size, std::string& error)
{
	return commit(address, size, error);
}

bool GuestAddressSpace::decommitRange(uint32_t address, uint32_t size, std::string& error)
{
	if (!base_) { error = "guest window is not reserved"; return false; }
	if (!size) { error = "guest decommit size is zero"; return false; }
	const uint64_t end = uint64_t(address) + size;
	if (end > kGuestWindowSize) { error = "guest decommit range overflows 32-bit address space"; return false; }
	const uint64_t lo = uint64_t(address) & ~(pageSize_ - 1);
	const uint64_t hi = (end + pageSize_ - 1) & ~(pageSize_ - 1);
	if (hi > kGuestWindowSize) { error = "guest decommit range exceeds guest window"; return false; }
#if defined(_WIN32)
	if (!VirtualFree(base_ + lo, static_cast<SIZE_T>(hi - lo), MEM_DECOMMIT)) {
		error = "cannot decommit guest pages";
		return false;
	}
#else
	if (madvise(base_ + lo, hi - lo, MADV_DONTNEED) != 0 ||
		mprotect(base_ + lo, hi - lo, PROT_NONE) != 0) {
		error = "cannot decommit guest pages";
		return false;
	}
#endif
	return true;
}

bool GuestAddressSpace::mapImage(const pe32::Image& image, std::string& error)
{
	if (!base_) { error = "guest window is not reserved"; return false; }
	if (!image.sizeOfImage || image.bytes.size() != image.sizeOfImage ||
		uint64_t(image.loadBase) + image.sizeOfImage > kGuestWindowSize) {
		error = "PE image metadata does not fit guest address space";
		return false;
	}
	const uint64_t begin = image.loadBase;
	const uint64_t end = begin + image.sizeOfImage;
	auto next = mappedImages_.lower_bound(begin);
	if (next != mappedImages_.end() && rangesOverlap(begin, end, next->first, next->second)) {
		error = "PE image overlaps an already mapped image";
		return false;
	}
	if (next != mappedImages_.begin()) {
		auto previous = std::prev(next);
		if (rangesOverlap(begin, end, previous->first, previous->second)) {
			error = "PE image overlaps an already mapped image";
			return false;
		}
	}
	if (!commit(image.loadBase, image.sizeOfImage, error)) return false;
	std::memcpy(base_ + image.loadBase, image.bytes.data(), image.bytes.size());
	mappedImages_.emplace(begin, end);
	return true;
}

uint8_t* GuestAddressSpace::host(uint32_t address, uint32_t size)
{
	if (!base_ || uint64_t(address) + size > kGuestWindowSize) return nullptr;
	return base_ + address;
}

const uint8_t* GuestAddressSpace::host(uint32_t address, uint32_t size) const
{
	if (!base_ || uint64_t(address) + size > kGuestWindowSize) return nullptr;
	return base_ + address;
}

bool GuestAddressSpace::containsMapped(uint32_t address, uint32_t size) const
{
	if (!size) return false;
	const uint64_t begin = address;
	const uint64_t end = begin + size;
	auto next = mappedImages_.upper_bound(begin);
	if (next == mappedImages_.begin()) return false;
	--next;
	return next->first <= begin && end <= next->second;
}

} // namespace br::runtime

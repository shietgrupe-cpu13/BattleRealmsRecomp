#include "win32_memory.h"

#include "guest.h"
#include "ra3_guest_bridge.h"
#include "win32_base.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

namespace br::runtime {
namespace {
constexpr uint32_t kProcessHeap = 0x00050000u;
constexpr uint32_t kHeapBase = 0x30000000u;
constexpr uint32_t kHeapEnd = 0x60000000u;
constexpr uint32_t kVirtualBase = 0x60000000u;
constexpr uint32_t kVirtualEnd = 0x6f000000u;
constexpr uint32_t kAllocationGranularity = 0x10000u;
constexpr uint32_t kInvalidSize = 0xffffffffu;
constexpr uint32_t kHeapZeroMemory = 0x00000008u;
constexpr uint32_t kGlobalZeroInit = 0x00000040u;
constexpr uint32_t kInvalidParameter = 87u;
constexpr uint32_t kNotEnoughMemory = 8u;
constexpr uint32_t kNotLocked = 158u;
constexpr uint32_t kInvalidAddress = 487u;
constexpr uint32_t kMemCommit = 0x00001000u;
constexpr uint32_t kMemReserve = 0x00002000u;
constexpr uint32_t kMemDecommit = 0x00004000u;
constexpr uint32_t kMemRelease = 0x00008000u;

struct Allocation {
	uint32_t capacity = 0;
	uint32_t requested = 0;
	uint32_t globalLocks = 0;
};

class GuestHeap {
public:
	GuestHeap() { freeBlocks_.emplace(kHeapBase, kHeapEnd - kHeapBase); }

	uint32_t allocate(uint32_t requested, bool zero)
	{
		const uint64_t rounded64 = (std::max<uint64_t>(requested, 1u) + 7u) & ~uint64_t(7u);
		if (rounded64 > std::numeric_limits<uint32_t>::max()) return 0;
		const uint32_t capacity = static_cast<uint32_t>(rounded64);
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto block = freeBlocks_.begin(); block != freeBlocks_.end(); ++block) {
			if (block->second < capacity) continue;
			const uint32_t address = block->first;
			if (!guest::commit(address, capacity)) return 0;
			const uint32_t remaining = block->second - capacity;
			freeBlocks_.erase(block);
			if (remaining) freeBlocks_.emplace(address + capacity, remaining);
			allocated_.emplace(address, Allocation{capacity, requested, 0});
			if (zero) std::memset(guest::host(address), 0, requested);
			return address;
		}
		return 0;
	}

	bool release(uint32_t address)
	{
		if (!address) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		const auto allocation = allocated_.find(address);
		if (allocation == allocated_.end() || allocation->second.globalLocks) return false;
		uint32_t size = allocation->second.capacity;
		allocated_.erase(allocation);
		auto next = freeBlocks_.lower_bound(address);
		if (next != freeBlocks_.begin()) {
			auto previous = std::prev(next);
			if (uint64_t(previous->first) + previous->second == address) {
				address = previous->first;
				size += previous->second;
				freeBlocks_.erase(previous);
			}
		}
		next = freeBlocks_.lower_bound(address);
		if (next != freeBlocks_.end() && uint64_t(address) + size == next->first) {
			size += next->second;
			freeBlocks_.erase(next);
		}
		freeBlocks_.emplace(address, size);
		return true;
	}

	uint32_t size(uint32_t address) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = allocated_.find(address);
		return found == allocated_.end() ? kInvalidSize : found->second.capacity;
	}

	bool globalLock(uint32_t address)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = allocated_.find(address);
		if (found == allocated_.end()) return false;
		++found->second.globalLocks;
		return true;
	}

	int globalUnlock(uint32_t address)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = allocated_.find(address);
		if (found == allocated_.end() || !found->second.globalLocks) return -1;
		return --found->second.globalLocks ? 1 : 0;
	}

	uint32_t reallocate(uint32_t address, uint32_t requested, bool zero)
	{
		if (!address) return allocate(requested, zero);
		Allocation old;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto found = allocated_.find(address);
			if (found == allocated_.end() || found->second.globalLocks) return 0;
			old = found->second;
			if (requested <= old.capacity) {
				if (zero && requested > old.requested)
					std::memset(guest::host(address + old.requested), 0, requested - old.requested);
				found->second.requested = requested;
				return address;
			}
		}
		const uint32_t replacement = allocate(requested, false);
		if (!replacement) return 0;
		const uint32_t copySize = std::min(old.requested, requested);
		std::memcpy(guest::host(replacement), guest::host(address), copySize);
		if (zero && requested > copySize)
			std::memset(guest::host(replacement + copySize), 0, requested - copySize);
		(void)release(address);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto found = allocated_.find(replacement);
			if (found != allocated_.end()) found->second.requested = requested;
		}
		return replacement;
	}

private:
	mutable std::mutex mutex_;
	std::map<uint32_t, uint32_t> freeBlocks_;
	std::map<uint32_t, Allocation> allocated_;
};

class GuestVirtualMemory {
public:
	GuestVirtualMemory() { freeBlocks_.emplace(kVirtualBase, kVirtualEnd - kVirtualBase); }

	uint32_t allocate(uint32_t requestedAddress, uint32_t size, uint32_t type)
	{
		if (!size || !(type & (kMemReserve | kMemCommit)) ||
			(type & ~(kMemReserve | kMemCommit))) return 0;
		const bool reserve = (type & kMemReserve) != 0;
		const bool commit = (type & kMemCommit) != 0;
		if (!requestedAddress && !reserve) return 0;
		const uint64_t hostPage = br::runtime::sharedGuestAddressSpace().hostPageSize();
		if (!hostPage || hostPage > std::numeric_limits<uint32_t>::max() ||
			(hostPage & (hostPage - 1u))) return 0;
		const uint32_t pageSize = static_cast<uint32_t>(hostPage);
		uint32_t pageAddress = requestedAddress & ~(pageSize - 1u);
		uint32_t commitSize = 0;
		if (requestedAddress) {
			const uint64_t requestedEnd = uint64_t(requestedAddress) + size;
			const uint64_t commitEnd = (requestedEnd + pageSize - 1u) & ~uint64_t(pageSize - 1u);
			if (requestedEnd > kVirtualEnd || commitEnd > kVirtualEnd || pageAddress < kVirtualBase) return 0;
			commitSize = static_cast<uint32_t>(commitEnd - pageAddress);
		}

		std::lock_guard<std::mutex> lock(mutex_);
		uint32_t base = pageAddress;
		bool newReservation = false;
		if (reserve) {
			uint64_t roundedSize = (uint64_t(size) + kAllocationGranularity - 1u) &
				~uint64_t(kAllocationGranularity - 1u);
			if (roundedSize > std::numeric_limits<uint32_t>::max()) return 0;
			if (!requestedAddress) {
				const auto block = std::find_if(freeBlocks_.begin(), freeBlocks_.end(),
					[roundedSize](const auto& item) { return item.second >= roundedSize; });
				if (block == freeBlocks_.end()) return 0;
				base = block->first;
			} else {
				if ((requestedAddress & (kAllocationGranularity - 1u)) != 0) return 0;
				base = requestedAddress;
			}
			if (!reserveAt(base, static_cast<uint32_t>(roundedSize))) return 0;
			newReservation = true;
			pageAddress = base;
			const uint64_t commitEnd = (uint64_t(base) + size + pageSize - 1u) &
				~uint64_t(pageSize - 1u);
			commitSize = static_cast<uint32_t>(commitEnd - base);
		} else {
			if (!requestedAddress || !containsReservation(pageAddress, commitSize)) return 0;
		}

		if (commit) {
			if (!guest::commit(pageAddress, commitSize)) {
				if (newReservation) releaseReservation(base);
				return 0;
			}
			const uint32_t reservationBase = baseOrReservation(pageAddress);
			const auto reservation = reservations_.find(reservationBase);
			if (reservation == reservations_.end()) return 0;
			addCommitted(reservation->second, pageAddress, pageAddress + commitSize);
		}
		return reserve ? base : pageAddress;
	}

	bool release(uint32_t address, uint32_t size, uint32_t freeType)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (freeType == kMemRelease) {
			if (size != 0) return false;
			const auto found = reservations_.find(address);
			if (found == reservations_.end()) return false;
			for (const auto& range : found->second.committed) {
				if (!guest::decommit(range.first, range.second - range.first)) return false;
			}
			releaseReservation(address);
			return true;
		}
		if (freeType != kMemDecommit || !size) return false;
		const uint64_t hostPage = br::runtime::sharedGuestAddressSpace().hostPageSize();
		if (!hostPage || hostPage > std::numeric_limits<uint32_t>::max()) return false;
		const uint32_t pageSize = static_cast<uint32_t>(hostPage);
		const uint32_t pageAddress = address & ~(pageSize - 1u);
		const uint64_t end = (uint64_t(address) + size + pageSize - 1u) & ~uint64_t(pageSize - 1u);
		if (end > kVirtualEnd || !containsReservation(pageAddress,
			static_cast<uint32_t>(end - pageAddress))) return false;
		if (!guest::decommit(pageAddress, static_cast<uint32_t>(end - pageAddress))) return false;
		const uint32_t reservationBase = baseOrReservation(pageAddress);
		const auto reservation = reservations_.find(reservationBase);
		if (reservation == reservations_.end()) return false;
		removeCommitted(reservation->second, pageAddress, static_cast<uint32_t>(end));
		return true;
	}

private:
	struct Reservation {
		uint32_t size = 0;
		// Host-page aligned committed intervals [begin, end).
		std::map<uint32_t, uint32_t> committed;
	};

	static void addCommitted(Reservation& reservation, uint32_t begin, uint32_t end)
	{
		auto it = reservation.committed.lower_bound(begin);
		if (it != reservation.committed.begin()) {
			auto previous = std::prev(it);
			if (previous->second >= begin) {
				begin = previous->first;
				end = std::max(end, previous->second);
				it = reservation.committed.erase(previous);
			}
		}
		while (it != reservation.committed.end() && it->first <= end) {
			end = std::max(end, it->second);
			it = reservation.committed.erase(it);
		}
		reservation.committed.emplace(begin, end);
	}

	static void removeCommitted(Reservation& reservation, uint32_t begin, uint32_t end)
	{
		auto it = reservation.committed.lower_bound(begin);
		if (it != reservation.committed.begin()) --it;
		while (it != reservation.committed.end()) {
			const uint32_t oldBegin = it->first;
			const uint32_t oldEnd = it->second;
			if (oldBegin >= end) break;
			if (oldEnd <= begin) { ++it; continue; }
			it = reservation.committed.erase(it);
			if (oldBegin < begin) reservation.committed.emplace(oldBegin, begin);
			if (oldEnd > end) {
				reservation.committed.emplace(end, oldEnd);
				break;
			}
		}
	}

	uint32_t baseOrReservation(uint32_t address) const
	{
		const auto next = reservations_.upper_bound(address);
		return next == reservations_.begin() ? 0u : std::prev(next)->first;
	}

	bool containsReservation(uint32_t address, uint32_t size) const
	{
		const auto next = reservations_.upper_bound(address);
		if (next == reservations_.begin()) return false;
		const auto reservation = std::prev(next);
		return address >= reservation->first &&
			uint64_t(address) + size <= uint64_t(reservation->first) + reservation->second.size;
	}

	bool reserveAt(uint32_t address, uint32_t size)
	{
		const auto upper = freeBlocks_.upper_bound(address);
		if (upper == freeBlocks_.begin()) return false;
		const auto block = std::prev(upper);
		const uint64_t blockEnd = uint64_t(block->first) + block->second;
		const uint64_t reserveEnd = uint64_t(address) + size;
		if (address < block->first || reserveEnd > blockEnd) return false;
		const uint32_t oldAddress = block->first;
		freeBlocks_.erase(block);
		if (address > oldAddress) freeBlocks_.emplace(oldAddress, address - oldAddress);
		if (reserveEnd < blockEnd)
			freeBlocks_.emplace(static_cast<uint32_t>(reserveEnd), static_cast<uint32_t>(blockEnd - reserveEnd));
		reservations_.emplace(address, Reservation{size, {}});
		return true;
	}

	void releaseReservation(uint32_t address)
	{
		const auto found = reservations_.find(address);
		if (found == reservations_.end()) return;
		const uint32_t size = found->second.size;
		reservations_.erase(found);
		auto next = freeBlocks_.lower_bound(address);
		if (next != freeBlocks_.begin()) {
			const auto previous = std::prev(next);
			if (uint64_t(previous->first) + previous->second == address) {
				address = previous->first;
				const uint32_t combined = previous->second + size;
				freeBlocks_.erase(previous);
				next = freeBlocks_.lower_bound(address);
				if (next != freeBlocks_.end() && uint64_t(address) + combined == next->first) {
					freeBlocks_.emplace(address, combined + next->second);
					freeBlocks_.erase(next);
				} else freeBlocks_.emplace(address, combined);
				return;
			}
		}
		next = freeBlocks_.lower_bound(address);
		if (next != freeBlocks_.end() && uint64_t(address) + size == next->first) {
			const uint32_t combined = size + next->second;
			freeBlocks_.erase(next);
			freeBlocks_.emplace(address, combined);
			return;
		}
		freeBlocks_.emplace(address, size);
	}

	std::mutex mutex_;
	std::map<uint32_t, uint32_t> freeBlocks_;
	std::map<uint32_t, Reservation> reservations_;
};

GuestHeap& processHeap()
{
	static GuestHeap heap;
	return heap;
}

GuestVirtualMemory& virtualMemory()
{
	static GuestVirtualMemory memory;
	return memory;
}

uint32_t argument(CPU* cpu, uint32_t index)
{
	return guest::load<uint32_t>(cpu->r[ESP] + 4u + index * 4u);
}

bool validHeap(uint32_t heap)
{
	// HeapCreate currently returns the process-heap token. Distinct heap
	// arenas/destruction semantics are not implemented yet.
	return heap == 0 || heap == kProcessHeap;
}

void getProcessHeap(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = kProcessHeap;
}

void destroyHeap(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = validHeap(argument(cpu, 0)) ? 1u : 0u;
	if (!cpu->r[EAX]) setWin32LastError(kInvalidParameter);
}

void heapAlloc(CPU* cpu, const ApiImportStub&)
{
	if (!validHeap(argument(cpu, 0))) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	const uint32_t address = processHeap().allocate(argument(cpu, 2),
		(argument(cpu, 1) & kHeapZeroMemory) != 0);
	if (!address) setWin32LastError(kNotEnoughMemory);
	cpu->r[EAX] = address;
}

void heapFree(CPU* cpu, const ApiImportStub&)
{
	const bool released = validHeap(argument(cpu, 0)) && processHeap().release(argument(cpu, 2));
	if (!released) setWin32LastError(kInvalidParameter);
	cpu->r[EAX] = released ? 1u : 0u;
}

void heapReAlloc(CPU* cpu, const ApiImportStub&)
{
	if (!validHeap(argument(cpu, 0))) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	const uint32_t address = processHeap().reallocate(argument(cpu, 2), argument(cpu, 3),
		(argument(cpu, 1) & kHeapZeroMemory) != 0);
	if (!address) setWin32LastError(kNotEnoughMemory);
	cpu->r[EAX] = address;
}

void heapSize(CPU* cpu, const ApiImportStub&)
{
	if (!validHeap(argument(cpu, 0))) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = kInvalidSize;
		return;
	}
	cpu->r[EAX] = processHeap().size(argument(cpu, 2));
}

void heapCreate(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = kProcessHeap;
}

void globalAlloc(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = processHeap().allocate(argument(cpu, 1),
		(argument(cpu, 0) & kGlobalZeroInit) != 0);
	if (!address) setWin32LastError(kNotEnoughMemory);
	cpu->r[EAX] = address;
}

void globalFree(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = argument(cpu, 0);
	if (!address || processHeap().release(address)) cpu->r[EAX] = 0;
	else {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = address;
	}
}

void globalLock(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = argument(cpu, 0);
	cpu->r[EAX] = processHeap().globalLock(address) ? address : 0u;
	if (!cpu->r[EAX]) setWin32LastError(kInvalidParameter);
}

void globalUnlock(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = argument(cpu, 0);
	const int result = processHeap().globalUnlock(address);
	if (result < 0) {
		setWin32LastError(kNotLocked);
		cpu->r[EAX] = 0;
	} else {
		cpu->r[EAX] = static_cast<uint32_t>(result);
		if (!result) setWin32LastError(0);
	}
}

void globalSize(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = processHeap().size(argument(cpu, 0));
}

void virtualAlloc(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = virtualMemory().allocate(argument(cpu, 0), argument(cpu, 1), argument(cpu, 2));
	if (!address) setWin32LastError(kNotEnoughMemory);
	cpu->r[EAX] = address;
}

void virtualFree(CPU* cpu, const ApiImportStub&)
{
	const bool released = virtualMemory().release(argument(cpu, 0), argument(cpu, 1), argument(cpu, 2));
	if (!released) setWin32LastError(kInvalidAddress);
	cpu->r[EAX] = released ? 1u : 0u;
}
} // namespace

bool registerWin32MemoryHandlers(ImportDispatchRegistry& handlers)
{
	return handlers.registerName("kernel32.dll", "GetProcessHeap", getProcessHeap, 0) &&
		handlers.registerName("kernel32.dll", "VirtualAlloc", virtualAlloc, 16) &&
		handlers.registerName("kernel32.dll", "VirtualFree", virtualFree, 12) &&
		handlers.registerName("kernel32.dll", "HeapCreate", heapCreate, 12) &&
		handlers.registerName("kernel32.dll", "HeapDestroy", destroyHeap, 4) &&
		handlers.registerName("kernel32.dll", "HeapAlloc", heapAlloc, 12) &&
		handlers.registerName("kernel32.dll", "HeapFree", heapFree, 12) &&
		handlers.registerName("kernel32.dll", "HeapReAlloc", heapReAlloc, 16) &&
		handlers.registerName("kernel32.dll", "HeapSize", heapSize, 12) &&
		handlers.registerName("kernel32.dll", "GlobalAlloc", globalAlloc, 8) &&
		handlers.registerName("kernel32.dll", "GlobalFree", globalFree, 4) &&
		handlers.registerName("kernel32.dll", "GlobalLock", globalLock, 4) &&
		handlers.registerName("kernel32.dll", "GlobalUnlock", globalUnlock, 4) &&
		handlers.registerName("kernel32.dll", "GlobalSize", globalSize, 4);
}

} // namespace br::runtime

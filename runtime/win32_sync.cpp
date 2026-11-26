#include "win32_sync.h"
#include "win32_base.h"
#include "win32_file.h"
#include "ra3_guest_bridge.h"

#include "guest.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace br::runtime {
namespace {
constexpr uint32_t kWaitObject0 = 0x00000000u;
constexpr uint32_t kWaitTimeout = 0x00000102u;
constexpr uint32_t kWaitFailed = 0xffffffffu;
constexpr uint32_t kInfinite = 0xffffffffu;
constexpr uint32_t kInvalidHandleError = 6u;
constexpr uint32_t kFirstHandle = 0x6e000000u;

struct Event {
	std::mutex mutex;
	std::condition_variable changed;
	bool manualReset = false;
	bool signaled = false;
};

class RecursiveCriticalSection {
public:
	void lock()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		const std::thread::id current = std::this_thread::get_id();
		if (owner_ == current) {
			++depth_;
			return;
		}
		available_.wait(lock, [this] { return depth_ == 0; });
		owner_ = current;
		depth_ = 1;
	}

	bool try_lock()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const std::thread::id current = std::this_thread::get_id();
		if (depth_ != 0 && owner_ != current) return false;
		owner_ = current;
		++depth_;
		return true;
	}

	bool try_lock_for(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		const std::thread::id current = std::this_thread::get_id();
		if (depth_ != 0 && owner_ == current) {
			++depth_;
			return true;
		}
		if (!available_.wait_for(lock, timeout, [this] { return depth_ == 0; })) return false;
		owner_ = current;
		depth_ = 1;
		return true;
	}

	bool unlock()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!depth_ || owner_ != std::this_thread::get_id()) return false;
		if (--depth_ == 0) {
			owner_ = std::thread::id{};
			available_.notify_one();
		}
		return true;
	}

private:
	std::mutex mutex_;
	std::condition_variable available_;
	std::thread::id owner_{};
	uint32_t depth_ = 0;
};

std::mutex criticalSectionsMutex;
std::unordered_map<uint32_t, std::shared_ptr<RecursiveCriticalSection>> criticalSections;
std::mutex eventsMutex;
std::unordered_map<uint32_t, std::shared_ptr<Event>> events;
std::unordered_map<uint32_t, std::shared_ptr<RecursiveCriticalSection>> mutexes;
std::unordered_map<std::string, uint32_t> namedMutexes;
std::unordered_map<uint32_t, std::string> mutexNames;
uint32_t nextHandle = kFirstHandle;

uint32_t argument(CPU* cpu, uint32_t index)
{
	return guest::load<uint32_t>(cpu->r[ESP] + 4u + index * 4u);
}

void setLastError(uint32_t value)
{
	setWin32LastError(value);
}

std::shared_ptr<RecursiveCriticalSection> criticalSection(uint32_t address, bool create)
{
	std::lock_guard<std::mutex> lock(criticalSectionsMutex);
	const auto found = criticalSections.find(address);
	if (found != criticalSections.end()) return found->second;
	if (!create) return {};
	auto section = std::make_shared<RecursiveCriticalSection>();
	criticalSections.emplace(address, section);
	return section;
}

void initializeCriticalSection(CPU* cpu, const ApiImportStub&)
{
	const uint32_t address = argument(cpu, 0);
	(void)criticalSection(address, true);
	guest::store<uint32_t>(address, 1u);
}

void initializeCriticalSectionAndSpinCount(CPU* cpu, const ApiImportStub& import)
{
	initializeCriticalSection(cpu, import);
	cpu->r[EAX] = 1u;
}

void deleteCriticalSection(CPU* cpu, const ApiImportStub&)
{
	std::lock_guard<std::mutex> lock(criticalSectionsMutex);
	criticalSections.erase(argument(cpu, 0));
}

void enterCriticalSection(CPU* cpu, const ApiImportStub&)
{
	criticalSection(argument(cpu, 0), true)->lock();
}

void leaveCriticalSection(CPU* cpu, const ApiImportStub&)
{
	const auto section = criticalSection(argument(cpu, 0), false);
	if (!section) return;
	(void)section->unlock();
}

void tryEnterCriticalSection(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = criticalSection(argument(cpu, 0), true)->try_lock() ? 1u : 0u;
}

void createEventA(CPU* cpu, const ApiImportStub&)
{
	auto event = std::make_shared<Event>();
	event->manualReset = argument(cpu, 1) != 0;
	event->signaled = argument(cpu, 2) != 0;
	std::lock_guard<std::mutex> lock(eventsMutex);
	if (nextHandle < kFirstHandle || nextHandle > 0x6efffffcu) {
		cpu->r[EAX] = 0;
		setLastError(8u); // ERROR_NOT_ENOUGH_MEMORY
		return;
	}
	const uint32_t handle = nextHandle;
	nextHandle += 4u;
	events.emplace(handle, std::move(event));
	cpu->r[EAX] = handle;
}

std::string readGuestName(uint32_t address)
{
	std::string name;
	if (!address) return name;
	for (uint32_t i = 0; i < 1024u; ++i) {
		const auto* byte = br::runtime::sharedGuestAddressSpace().host(address + i);
		if (!byte || !*byte) break;
		name.push_back(static_cast<char>(*byte));
	}
	return name;
}

void createMutexA(CPU* cpu, const ApiImportStub&)
{
	const bool initialOwner = argument(cpu, 1) != 0;
	const std::string name = readGuestName(argument(cpu, 2));
	uint32_t handle = 0;
	{
		std::lock_guard<std::mutex> lock(eventsMutex);
		if (!name.empty()) {
			auto existing = namedMutexes.find(name);
			if (existing != namedMutexes.end() && mutexes.count(existing->second)) {
				cpu->r[EAX] = existing->second;
				setLastError(183u); // ERROR_ALREADY_EXISTS
				return;
			}
		}
		if (nextHandle < kFirstHandle || nextHandle > 0x6efffffcu) {
			cpu->r[EAX] = 0;
			setLastError(8u); // ERROR_NOT_ENOUGH_MEMORY
			return;
		}
		handle = nextHandle;
		nextHandle += 4u;
	}
	auto object = std::make_shared<RecursiveCriticalSection>();
	if (initialOwner) object->lock();
	{
		std::lock_guard<std::mutex> lock(eventsMutex);
		mutexes.emplace(handle, std::move(object));
		if (!name.empty()) {
			namedMutexes[name] = handle;
			mutexNames[handle] = name;
		}
	}
	cpu->r[EAX] = handle;
}

std::shared_ptr<RecursiveCriticalSection> findMutex(uint32_t handle)
{
	std::lock_guard<std::mutex> lock(eventsMutex);
	const auto found = mutexes.find(handle);
	return found == mutexes.end() ? std::shared_ptr<RecursiveCriticalSection>{} : found->second;
}

void releaseMutex(CPU* cpu, const ApiImportStub&)
{
	const auto object = findMutex(argument(cpu, 0));
	if (!object || !object->unlock()) {
		setLastError(288u); // ERROR_NOT_OWNER
		cpu->r[EAX] = 0;
		return;
	}
	cpu->r[EAX] = 1u;
}

std::shared_ptr<Event> findEvent(uint32_t handle)
{
	std::lock_guard<std::mutex> lock(eventsMutex);
	const auto found = events.find(handle);
	return found == events.end() ? std::shared_ptr<Event>{} : found->second;
}

void setEvent(CPU* cpu, const ApiImportStub&)
{
	const auto event = findEvent(argument(cpu, 0));
	if (!event) {
		setLastError(kInvalidHandleError);
		cpu->r[EAX] = 0;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(event->mutex);
		event->signaled = true;
	}
	if (event->manualReset) event->changed.notify_all();
	else event->changed.notify_one();
	cpu->r[EAX] = 1;
}

void resetEvent(CPU* cpu, const ApiImportStub&)
{
	const auto event = findEvent(argument(cpu, 0));
	if (!event) {
		setLastError(kInvalidHandleError);
		cpu->r[EAX] = 0;
		return;
	}
	std::lock_guard<std::mutex> lock(event->mutex);
	event->signaled = false;
	cpu->r[EAX] = 1;
}

void closeHandle(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	{
		std::lock_guard<std::mutex> lock(eventsMutex);
		if (events.erase(handle)) {
			cpu->r[EAX] = 1u;
			return;
		}
		if (mutexes.erase(handle)) {
			auto name = mutexNames.find(handle);
			if (name != mutexNames.end()) {
				auto named = namedMutexes.find(name->second);
				if (named != namedMutexes.end() && named->second == handle) namedMutexes.erase(named);
				mutexNames.erase(name);
			}
			cpu->r[EAX] = 1u;
			return;
		}
	}
	if (closeWin32FileHandle(handle)) {
		cpu->r[EAX] = 1u;
		return;
	}
	setLastError(kInvalidHandleError);
	cpu->r[EAX] = 0u;
}

void waitForSingleObject(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const uint32_t timeout = argument(cpu, 1);
	const auto event = findEvent(handle);
	if (event) {
		std::unique_lock<std::mutex> lock(event->mutex);
		const auto ready = [&event] { return event->signaled; };
		bool signaled = false;
		if (timeout == kInfinite) {
			event->changed.wait(lock, ready);
			signaled = true;
		} else if (timeout == 0) {
			signaled = ready();
		} else {
			signaled = event->changed.wait_for(lock, std::chrono::milliseconds(timeout), ready);
		}
		if (signaled && !event->manualReset) event->signaled = false;
		cpu->r[EAX] = signaled ? kWaitObject0 : kWaitTimeout;
		return;
	}
	const auto mutex = findMutex(handle);
	if (!mutex) {
		setLastError(kInvalidHandleError);
		cpu->r[EAX] = kWaitFailed;
		return;
	}
	const bool signaled = timeout == kInfinite ? (mutex->lock(), true) : timeout == 0
		? mutex->try_lock() : mutex->try_lock_for(std::chrono::milliseconds(timeout));
	cpu->r[EAX] = signaled ? kWaitObject0 : kWaitTimeout;
}

int32_t* atomicValue(CPU* cpu, uint32_t argumentIndex)
{
	const uint32_t address = argument(cpu, argumentIndex);
	return reinterpret_cast<int32_t*>(guest::host(address));
}

void interlockedIncrement(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = static_cast<uint32_t>(__atomic_add_fetch(atomicValue(cpu, 0), 1, __ATOMIC_SEQ_CST));
}

void interlockedDecrement(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = static_cast<uint32_t>(__atomic_sub_fetch(atomicValue(cpu, 0), 1, __ATOMIC_SEQ_CST));
}

void interlockedExchange(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = static_cast<uint32_t>(__atomic_exchange_n(
		atomicValue(cpu, 0), static_cast<int32_t>(argument(cpu, 1)), __ATOMIC_SEQ_CST));
}
} // namespace

bool registerWin32SyncHandlers(ImportDispatchRegistry& handlers)
{
	return handlers.registerName("kernel32.dll", "InitializeCriticalSection", initializeCriticalSection, 4) &&
		handlers.registerName("kernel32.dll", "InitializeCriticalSectionAndSpinCount", initializeCriticalSectionAndSpinCount, 8) &&
		handlers.registerName("kernel32.dll", "DeleteCriticalSection", deleteCriticalSection, 4) &&
		handlers.registerName("kernel32.dll", "EnterCriticalSection", enterCriticalSection, 4) &&
		handlers.registerName("kernel32.dll", "LeaveCriticalSection", leaveCriticalSection, 4) &&
		handlers.registerName("kernel32.dll", "TryEnterCriticalSection", tryEnterCriticalSection, 4) &&
		handlers.registerName("kernel32.dll", "CreateEventA", createEventA, 16) &&
		handlers.registerName("kernel32.dll", "CreateMutexA", createMutexA, 12) &&
		handlers.registerName("kernel32.dll", "ReleaseMutex", releaseMutex, 4) &&
		handlers.registerName("kernel32.dll", "SetEvent", setEvent, 4) &&
		handlers.registerName("kernel32.dll", "ResetEvent", resetEvent, 4) &&
		handlers.registerName("kernel32.dll", "WaitForSingleObject", waitForSingleObject, 8) &&
		handlers.registerName("kernel32.dll", "CloseHandle", closeHandle, 4) &&
		handlers.registerName("kernel32.dll", "InterlockedIncrement", interlockedIncrement, 4) &&
		handlers.registerName("kernel32.dll", "InterlockedDecrement", interlockedDecrement, 4) &&
		handlers.registerName("kernel32.dll", "InterlockedExchange", interlockedExchange, 8);
}

} // namespace br::runtime

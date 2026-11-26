#include "win32_base.h"

#include "guest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <ctime>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace br::runtime {
namespace {
thread_local uint32_t lastError = 0;
std::atomic<uint32_t> nextThreadId{1};
std::atomic<uint32_t> processErrorMode{0};
std::atomic<uint32_t> nextRegistryHandle{0x71000000u};
struct GuestRegistryValue {
	uint32_t type = 0u;
	std::vector<uint8_t> data;
};
std::mutex registryMutex;
std::map<uint32_t, std::string> registryHandlePaths;
std::map<std::string, std::map<std::string, GuestRegistryValue>> registryValues;
std::mutex multimediaTimerMutex;
std::map<uint32_t, uint32_t> multimediaTimerPeriods;
struct GuestMultimediaTimer {
	uint32_t id = 0;
	uint32_t delayMs = 0;
	uint32_t resolutionMs = 0;
	uint32_t callback = 0;
	uint32_t user = 0;
	uint32_t flags = 0;
	std::chrono::steady_clock::time_point due;
};
std::map<uint32_t, GuestMultimediaTimer> multimediaTimers;
uint32_t nextMultimediaTimerId = 1u;
thread_local uint32_t tracedTimerCallbackCount = 0u;
thread_local uint32_t tracedTimerLastState50 = 0xffffffffu;
thread_local uint32_t tracedTimerLastState60 = 0xffffffffu;
const auto processClockStart = std::chrono::steady_clock::now();
constexpr uint32_t kTlsSlots = 64u;
constexpr uint32_t kTlsOutOfIndexes = 0xffffffffu;
constexpr uint32_t kInvalidParameter = 87u;
constexpr uint32_t kNotEnoughMemory = 8u;

struct TlsValue {
	uint32_t generation = 0;
	uint32_t value = 0;
};

std::mutex tlsMutex;
std::array<bool, kTlsSlots> tlsAllocated{};
std::array<uint32_t, kTlsSlots> tlsGenerations{};
thread_local std::array<TlsValue, kTlsSlots> threadTlsValues{};

uint32_t argument(CPU* cpu, uint32_t index)
{
	return guest::load<uint32_t>(cpu->r[ESP] + 4u + index * 4u);
}

std::string lowerRegistryName(std::string value)
{
	for (char& ch : value) {
		if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
	}
	return value;
}

std::string registryPathForRoot(uint32_t root)
{
	if (root == 0x80000000u) return "hkey_classes_root";
	if (root == 0x80000001u) return "hkey_current_user";
	if (root == 0x80000002u) return "hkey_local_machine";
	if (root == 0x80000003u) return "hkey_users";
	if (root == 0x80000005u) return "hkey_current_config";
	return {};
}

std::string readRegistrySubkey(uint32_t address)
{
	std::string value;
	if (!address) return value;
	for (uint32_t offset = 0; offset < 4096u; ++offset) {
		const char ch = static_cast<char>(guest::load<uint8_t>(address + offset));
		if (!ch) break;
		value.push_back(ch);
	}
	return lowerRegistryName(std::move(value));
}

std::string registryPathForHandle(uint32_t handle)
{
	const std::string predefined = registryPathForRoot(handle);
	if (!predefined.empty()) return predefined;
	const auto found = registryHandlePaths.find(handle);
	return found == registryHandlePaths.end() ? std::string{} : found->second;
}

void getLastError(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = lastError;
}

[[noreturn]] void exitProcess(CPU* cpu, const ApiImportStub&)
{
	throw GuestProcessExit{argument(cpu, 0)};
}

void setErrorMode(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = processErrorMode.exchange(argument(cpu, 0), std::memory_order_relaxed);
}

void regCreateKeyExA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t root = argument(cpu, 0);
	const uint32_t subkey = argument(cpu, 1);
	const uint32_t output = argument(cpu, 7);
	const uint32_t disposition = argument(cpu, 8);
	if (!output) {
		cpu->r[EAX] = 87u; return;
	}
	std::lock_guard<std::mutex> lock(registryMutex);
	std::string path = registryPathForHandle(root);
	if (path.empty()) { cpu->r[EAX] = 6u; return; }
	const std::string child = readRegistrySubkey(subkey);
	if (!child.empty()) { path.push_back('\\'); path += child; }
	const bool existed = registryValues.find(path) != registryValues.end();
	registryValues.try_emplace(path);
	const uint32_t handle = nextRegistryHandle.fetch_add(4u, std::memory_order_relaxed);
	registryHandlePaths.emplace(handle, path);
	guest::store<uint32_t>(output, handle);
	if (disposition) guest::store<uint32_t>(disposition, existed ? 2u : 1u);
	cpu->r[EAX] = 0u; // ERROR_SUCCESS
}

void regSetValueExA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0u);
	const uint32_t valueNameAddress = argument(cpu, 1u);
	const uint32_t reserved = argument(cpu, 2u);
	const uint32_t type = argument(cpu, 3u);
	const uint32_t dataAddress = argument(cpu, 4u);
	const uint32_t byteCount = argument(cpu, 5u);
	if (reserved || byteCount > 16u * 1024u * 1024u || (!dataAddress && byteCount)) {
		cpu->r[EAX] = 87u; // ERROR_INVALID_PARAMETER.
		return;
	}
	std::lock_guard<std::mutex> lock(registryMutex);
	const std::string path = registryPathForHandle(handle);
	if (path.empty()) { cpu->r[EAX] = 6u; return; } // ERROR_INVALID_HANDLE.
	GuestRegistryValue value;
	value.type = type;
	value.data.resize(byteCount);
	for (uint32_t index = 0; index < byteCount; ++index)
		value.data[index] = guest::load<uint8_t>(dataAddress + index);
	registryValues[path][readRegistrySubkey(valueNameAddress)] = std::move(value);
	cpu->r[EAX] = 0u; // ERROR_SUCCESS.
}

void regCloseKey(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(registryMutex);
	if (!registryPathForRoot(handle).empty()) { cpu->r[EAX] = 0u; return; }
	cpu->r[EAX] = registryHandlePaths.erase(handle) ? 0u : 6u;
}

void timeBeginPeriod(CPU* cpu, const ApiImportStub&)
{
	const uint32_t period = argument(cpu, 0);
	if (!period) { cpu->r[EAX] = 97u; return; } // TIMERR_NOCANDO
	{ std::lock_guard<std::mutex> lock(multimediaTimerMutex); ++multimediaTimerPeriods[period]; }
	cpu->r[EAX] = 0u;
}

void timeEndPeriod(CPU* cpu, const ApiImportStub&)
{
	const uint32_t period = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(multimediaTimerMutex);
	auto found = multimediaTimerPeriods.find(period);
	if (!period || found == multimediaTimerPeriods.end()) { cpu->r[EAX] = 97u; return; }
	if (--found->second == 0u) multimediaTimerPeriods.erase(found);
	cpu->r[EAX] = 0u;
}

void timeSetEvent(CPU* cpu, const ApiImportStub&)
{
	const uint32_t delay = argument(cpu, 0);
	const uint32_t resolution = argument(cpu, 1);
	const uint32_t callback = argument(cpu, 2);
	const uint32_t user = argument(cpu, 3);
	const uint32_t flags = argument(cpu, 4);
	// This runtime currently supports TIME_CALLBACK_FUNCTION timers. The game
	// uses a periodic callback; event-object timer modes need native HANDLEs and
	// are rejected instead of returning a timer that can never signal.
	if (!delay || !callback || (flags & ~0x00000001u)) {
		cpu->r[EAX] = 0u;
		return;
	}
	uint32_t id = 0u;
	{
		std::lock_guard<std::mutex> lock(multimediaTimerMutex);
		id = nextMultimediaTimerId++;
		if (!id) id = nextMultimediaTimerId++;
		const auto now = std::chrono::steady_clock::now();
		multimediaTimers.emplace(id, GuestMultimediaTimer{
			id, delay, resolution, callback, user, flags, now + std::chrono::milliseconds(delay)});
	}
	cpu->r[EAX] = id;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-winmm] timeSetEvent id=%u delay=%u resolution=%u callback=%08x user=%08x flags=%08x\n",
			id, delay, resolution, callback, user, flags);
}

void timeKillEvent(CPU* cpu, const ApiImportStub&)
{
	const uint32_t id = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(multimediaTimerMutex);
	cpu->r[EAX] = multimediaTimers.erase(id) ? 0u : 97u; // TIMERR_NOERROR / TIMERR_NOCANDO.
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-winmm] timeKillEvent id=%u result=%u\n", id, cpu->r[EAX]);
}

void pumpMultimediaTimersInternal(CPU* cpu)
{
	static thread_local bool pumping = false;
	if (!cpu || pumping) return;
	pumping = true;
	for (;;) {
		GuestMultimediaTimer dueTimer{};
		bool foundDueTimer = false;
		const auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lock(multimediaTimerMutex);
			for (auto it = multimediaTimers.begin(); it != multimediaTimers.end(); ++it) {
				if (it->second.due > now) continue;
				dueTimer = it->second;
				if (dueTimer.flags & 0x00000001u)
					it->second.due = now + std::chrono::milliseconds(dueTimer.delayMs);
				else
					multimediaTimers.erase(it);
				foundDueTimer = true;
				break;
			}
		}
		if (!foundDueTimer) break;
		const uint32_t arguments[5] = {dueTimer.id, 0u, dueTimer.user, 0u, 0u};
		const CPU savedCpu = *cpu;
		const uint32_t savedHighFlags = g_eflags_hi;
		uint32_t callbackResult = 0u;
		std::string error;
		const bool traceGameState = std::getenv("BR_TRACE_GAME_STATE") != nullptr;
		const uint32_t state50Before = traceGameState ? guest::load<uint32_t>(0x006de340u) : 0u;
		const uint32_t state60Before = traceGameState ? guest::load<uint32_t>(0x006de350u) : 0u;
		const uint32_t callbackNumber = tracedTimerCallbackCount++;
		const bool traceCallback = traceGameState &&
			(callbackNumber < 4u || callbackNumber % 64u == 0u ||
			 state50Before != tracedTimerLastState50 || state60Before != tracedTimerLastState60);
		tracedTimerLastState50 = state50Before;
		tracedTimerLastState60 = state60Before;
		if (traceCallback) {
			const auto threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
			std::fprintf(stderr, "[br-state] timer-enter callback=%08x id=%u host-thread=%zx guest-esp=%08x fsbase=%08x fields=%08x/%08x\n",
				dueTimer.callback, dueTimer.id, static_cast<size_t>(threadId), cpu->r[ESP], cpu->fsbase,
				state50Before, state60Before);
		}
		const bool callbackOk = callGuestStdcall(cpu, dueTimer.callback, arguments, 5u, callbackResult, error);
		if (traceCallback) {
			const uint32_t state50After = guest::load<uint32_t>(0x006de340u);
			const uint32_t state60After = guest::load<uint32_t>(0x006de350u);
			const auto threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
			std::fprintf(stderr, "[br-state] timer-exit callback=%08x id=%u host-thread=%zx guest-esp=%08x fsbase=%08x fields=%08x/%08x result=%08x\n",
				dueTimer.callback, dueTimer.id, static_cast<size_t>(threadId), cpu->r[ESP], cpu->fsbase,
				state50After, state60After, callbackResult);
		}
		*cpu = savedCpu;
		g_eflags_hi = savedHighFlags;
		if (!callbackOk) {
			{
				std::lock_guard<std::mutex> lock(multimediaTimerMutex);
				multimediaTimers.erase(dueTimer.id);
			}
			pumping = false;
			dispatchFailure(cpu, dueTimer.callback, error.c_str());
		}
		if (std::getenv("BR_TRACE_DDRAW"))
			std::fprintf(stderr, "[br-winmm] delivered timeSetEvent id=%u callback=%08x\n",
				dueTimer.id, dueTimer.callback);
	}
	pumping = false;
}

void getVersion(CPU* cpu, const ApiImportStub&)
{
	// Report the Windows XP 5.1 compatibility profile used for this legacy game.
	// GetVersion packs major/minor in the low word and the NT build in the high word.
	cpu->r[EAX] = 0x0a280105u; // 5.1, build 2600
}

void setLastError(CPU* cpu, const ApiImportStub&)
{
	lastError = argument(cpu, 0);
}

void getTickCount(CPU* cpu, const ApiImportStub&)
{
	const auto elapsed = std::chrono::steady_clock::now() - processClockStart;
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	cpu->r[EAX] = static_cast<uint32_t>(static_cast<uint64_t>(milliseconds));
}

void queryPerformanceCounter(CPU* cpu, const ApiImportStub&)
{
	const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	guest::store<int64_t>(argument(cpu, 0), static_cast<int64_t>(ticks));
	cpu->r[EAX] = 1;
}

void queryPerformanceFrequency(CPU* cpu, const ApiImportStub&)
{
	guest::store<int64_t>(argument(cpu, 0), 1000000000ll);
	cpu->r[EAX] = 1;
}

void sleepMilliseconds(CPU* cpu, const ApiImportStub&)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(argument(cpu, 0)));
}

void sleepEx(CPU* cpu, const ApiImportStub&)
{
	const uint32_t milliseconds = argument(cpu, 0);
	const bool alertable = argument(cpu, 1) != 0;
	(void)alertable; // QueueUserAPC is imported but still unimplemented; no guest APC can be queued yet.

	if (milliseconds == 0) {
		// SleepEx(0, ...) yields the rest of this time slice; sleep_for(0) need not.
		std::this_thread::yield();
	} else if (milliseconds == std::numeric_limits<uint32_t>::max()) {
		// INFINITE does not time out. There is currently no guest APC queue to wake this wait.
		std::mutex waitMutex;
		static std::condition_variable neverSignaled;
		std::unique_lock<std::mutex> lock(waitMutex);
		neverSignaled.wait(lock, [] { return false; });
	} else {
		std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	}

	// No I/O completion/APC ran, so the timeout/yield completed normally.
	cpu->r[EAX] = 0; // WAIT_IO_COMPLETION is returned only after running a guest APC.
}

void getCurrentThreadId(CPU* cpu, const ApiImportStub&)
{
	thread_local const uint32_t threadId = nextThreadId.fetch_add(1, std::memory_order_relaxed);
	cpu->r[EAX] = threadId;
}

void getCurrentProcessId(CPU* cpu, const ApiImportStub&)
{
#if defined(_WIN32)
	cpu->r[EAX] = static_cast<uint32_t>(_getpid());
#else
	cpu->r[EAX] = static_cast<uint32_t>(getpid());
#endif
}

void getSystemTimeAsFileTime(CPU* cpu, const ApiImportStub&)
{
	constexpr int64_t kWindowsEpochOffsetSeconds = 11644473600ll;
	const auto unix100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const int64_t fileTime = unix100ns + kWindowsEpochOffsetSeconds * 10000000ll;
	guest::store<uint64_t>(argument(cpu, 0), static_cast<uint64_t>(fileTime));
}

int64_t daysFromCivil(int32_t year, uint32_t month, uint32_t day)
{
	year -= month <= 2u ? 1 : 0;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const uint32_t yearOfEra = static_cast<uint32_t>(year - era * 400);
	const uint32_t monthPrime = month > 2u ? month - 3u : month + 9u;
	const uint32_t dayOfYear = (153u * monthPrime + 2u) / 5u + day - 1u;
	const uint32_t dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
	return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

void civilFromDays(int64_t unixDays, uint16_t& year, uint16_t& month, uint16_t& day)
{
	const int64_t z = unixDays + 719468ll;
	const int64_t era = (z >= 0 ? z : z - 146096ll) / 146097ll;
	const uint32_t dayOfEra = static_cast<uint32_t>(z - era * 146097ll);
	const uint32_t yearOfEra = (dayOfEra - dayOfEra / 1460u + dayOfEra / 36524u -
		dayOfEra / 146096u) / 365u;
	int32_t civilYear = static_cast<int32_t>(yearOfEra) + static_cast<int32_t>(era * 400ll);
	const uint32_t dayOfYear = dayOfEra - (365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u);
	const uint32_t monthPrime = (5u * dayOfYear + 2u) / 153u;
	const uint32_t civilDay = dayOfYear - (153u * monthPrime + 2u) / 5u + 1u;
	const uint32_t civilMonth = monthPrime < 10u ? monthPrime + 3u : monthPrime - 9u;
	civilYear += civilMonth <= 2u ? 1 : 0;
	year = static_cast<uint16_t>(civilYear);
	month = static_cast<uint16_t>(civilMonth);
	day = static_cast<uint16_t>(civilDay);
}

bool systemTimeFieldsToFileTime(const uint16_t fields[8], uint64_t& fileTime)
{
	const uint32_t year = fields[0];
	const uint32_t month = fields[1];
	const uint32_t day = fields[3];
	const uint32_t hour = fields[4];
	const uint32_t minute = fields[5];
	const uint32_t second = fields[6];
	const uint32_t millisecond = fields[7];
	if (year < 1601u || year > 30827u || month < 1u || month > 12u ||
		hour > 23u || minute > 59u || second > 59u || millisecond > 999u) return false;
	const auto isLeapYear = [](uint32_t value) {
		return (value % 4u == 0u) && ((value % 100u != 0u) || (value % 400u == 0u));
	};
	static constexpr uint8_t monthDays[] = {31u, 28u, 31u, 30u, 31u, 30u,
		31u, 31u, 30u, 31u, 30u, 31u};
	uint32_t daysInMonth = monthDays[month - 1u];
	if (month == 2u && isLeapYear(year)) ++daysInMonth;
	if (day < 1u || day > daysInMonth) return false;
	const int64_t unixDays = daysFromCivil(static_cast<int32_t>(year), month, day);
	const uint64_t daysSinceWindowsEpoch = static_cast<uint64_t>(unixDays + 134774ll);
	constexpr uint64_t ticksPerSecond = 10000000ull;
	const uint64_t seconds = ((daysSinceWindowsEpoch * 24u + hour) * 60u + minute) * 60u + second;
	fileTime = seconds * ticksPerSecond + uint64_t(millisecond) * 10000u;
	return true;
}

void systemTimeToFileTime(CPU* cpu, const ApiImportStub&)
{
	// SYSTEMTIME is eight little-endian WORDs. SystemTimeToFileTime treats its
	// input as UTC and ignores wDayOfWeek, matching the Win32 API contract.
	const uint32_t input = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	uint16_t fields[8]{};
	for (uint32_t index = 0; index < 8u; ++index)
		fields[index] = guest::load<uint16_t>(input + index * 2u);
	uint64_t fileTime = 0u;
	if (!systemTimeFieldsToFileTime(fields, fileTime)) {
		cpu->r[EAX] = 0u;
		lastError = kInvalidParameter;
		return;
	}
	guest::store<uint64_t>(output, fileTime);
	cpu->r[EAX] = 1u;
}

void fileTimeToSystemTime(CPU* cpu, const ApiImportStub&)
{
	const uint32_t input = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	const uint64_t fileTime = input ? guest::load<uint64_t>(input) : 0u;
	// Microsoft documents that values with the high bit set cannot be
	// represented by FileTimeToSystemTime.
	if (!input || !output || fileTime >= 0x8000000000000000ull) {
		cpu->r[EAX] = 0u;
		lastError = kInvalidParameter;
		return;
	}
	constexpr uint64_t kTicksPerMillisecond = 10000ull;
	constexpr uint64_t kTicksPerSecond = 10000000ull;
	constexpr uint64_t kSecondsPerDay = 86400ull;
	constexpr int64_t kWindowsEpochUnixDays = 134774ll;
	const uint64_t daysSinceWindowsEpoch = fileTime / (kSecondsPerDay * kTicksPerSecond);
	const uint64_t ticksWithinDay = fileTime % (kSecondsPerDay * kTicksPerSecond);
	const uint64_t secondsWithinDay = ticksWithinDay / kTicksPerSecond;
	const int64_t unixDays = static_cast<int64_t>(daysSinceWindowsEpoch) - kWindowsEpochUnixDays;
	uint16_t year = 0u, month = 0u, day = 0u;
	civilFromDays(static_cast<int64_t>(unixDays), year, month, day);
	if (!year) {
		cpu->r[EAX] = 0u;
		lastError = kInvalidParameter;
		return;
	}
	const int64_t weekdayValue = (unixDays + 4ll) % 7ll;
	const uint16_t weekday = static_cast<uint16_t>(weekdayValue < 0 ? weekdayValue + 7ll : weekdayValue);
	const uint16_t fields[8] = {
		year, month, weekday, day,
		static_cast<uint16_t>(secondsWithinDay / 3600ull),
		static_cast<uint16_t>((secondsWithinDay / 60ull) % 60ull),
		static_cast<uint16_t>(secondsWithinDay % 60ull),
		static_cast<uint16_t>((ticksWithinDay % kTicksPerSecond) / kTicksPerMillisecond)};
	for (uint32_t index = 0u; index < 8u; ++index)
		guest::store<uint16_t>(output + index * 2u, fields[index]);
	cpu->r[EAX] = 1u;
}

void fileTimeToLocalFileTime(CPU* cpu, const ApiImportStub&)
{
	const uint32_t input = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!input || !output || input == output) {
		cpu->r[EAX] = 0u;
		lastError = kInvalidParameter;
		return;
	}
	const uint64_t utcFileTime = guest::load<uint64_t>(input);
	const std::time_t now = std::time(nullptr);
	std::tm localNow{};
#if defined(_WIN32)
	if (localtime_s(&localNow, &now) != 0) {
#else
	if (!localtime_r(&now, &localNow)) {
#endif
		cpu->r[EAX] = 0u;
		lastError = kInvalidParameter;
		return;
	}
	const int32_t year = localNow.tm_year + 1900;
	const uint32_t month = static_cast<uint32_t>(localNow.tm_mon + 1);
	const int64_t localWallSeconds = daysFromCivil(year, month,
		static_cast<uint32_t>(localNow.tm_mday)) * 86400ll +
		static_cast<int64_t>(localNow.tm_hour) * 3600ll +
		static_cast<int64_t>(localNow.tm_min) * 60ll + localNow.tm_sec;
	const int64_t currentLocalBiasSeconds = localWallSeconds - static_cast<int64_t>(now);
	constexpr uint64_t ticksPerSecond = 10000000ull;
	uint64_t localFileTime = 0u;
	if (currentLocalBiasSeconds >= 0) {
		const uint64_t offset = static_cast<uint64_t>(currentLocalBiasSeconds) * ticksPerSecond;
		if (utcFileTime > std::numeric_limits<uint64_t>::max() - offset) {
			cpu->r[EAX] = 0u;
			lastError = kInvalidParameter;
			return;
		}
		localFileTime = utcFileTime + offset;
	} else {
		const uint64_t offset = static_cast<uint64_t>(-currentLocalBiasSeconds) * ticksPerSecond;
		if (utcFileTime < offset) {
			cpu->r[EAX] = 0u;
			lastError = kInvalidParameter;
			return;
		}
		localFileTime = utcFileTime - offset;
	}
	guest::store<uint64_t>(output, localFileTime);
	cpu->r[EAX] = 1u;
}

void getLocalTime(CPU* cpu, const ApiImportStub&)
{
	const auto now = std::chrono::system_clock::now();
	const auto wholeSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now - wholeSeconds).count();
	const std::time_t time = std::chrono::system_clock::to_time_t(wholeSeconds);
	std::tm local{};
#if defined(_WIN32)
	if (localtime_s(&local, &time) != 0) return;
#else
	if (!localtime_r(&time, &local)) return;
#endif
	const uint16_t fields[8] = {
		static_cast<uint16_t>(local.tm_year + 1900),
		static_cast<uint16_t>(local.tm_mon + 1),
		static_cast<uint16_t>(local.tm_wday),
		static_cast<uint16_t>(local.tm_mday),
		static_cast<uint16_t>(local.tm_hour),
		static_cast<uint16_t>(local.tm_min),
		static_cast<uint16_t>(local.tm_sec),
		static_cast<uint16_t>(milliseconds),
	};
	const uint32_t output = argument(cpu, 0);
	for (uint32_t index = 0; index < 8u; ++index)
		guest::store<uint16_t>(output + index * 2u, fields[index]);
}

void tlsAlloc(CPU* cpu, const ApiImportStub&)
{
	std::lock_guard<std::mutex> lock(tlsMutex);
	for (uint32_t index = 0; index < kTlsSlots; ++index) {
		if (tlsAllocated[index]) continue;
		tlsAllocated[index] = true;
		if (!tlsGenerations[index]) tlsGenerations[index] = 1;
		cpu->r[EAX] = index;
		return;
	}
	cpu->r[EAX] = kTlsOutOfIndexes;
	lastError = kNotEnoughMemory;
}

void tlsFree(CPU* cpu, const ApiImportStub&)
{
	const uint32_t index = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(tlsMutex);
	if (index >= kTlsSlots || !tlsAllocated[index]) {
		cpu->r[EAX] = 0;
		lastError = kInvalidParameter;
		return;
	}
	tlsAllocated[index] = false;
	++tlsGenerations[index];
	if (!tlsGenerations[index]) ++tlsGenerations[index];
	cpu->r[EAX] = 1;
}

void tlsGetValue(CPU* cpu, const ApiImportStub&)
{
	const uint32_t index = argument(cpu, 0);
	uint32_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(tlsMutex);
		if (index >= kTlsSlots || !tlsAllocated[index]) {
			cpu->r[EAX] = 0;
			lastError = kInvalidParameter;
			return;
		}
		generation = tlsGenerations[index];
	}
	const TlsValue value = threadTlsValues[index];
	cpu->r[EAX] = value.generation == generation ? value.value : 0u;
	// Win32 specifies that a successful TlsGetValue clears the calling thread's
	// last-error value, including when the stored TLS value itself is null.
	lastError = 0;
}

void tlsSetValue(CPU* cpu, const ApiImportStub&)
{
	const uint32_t index = argument(cpu, 0);
	uint32_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(tlsMutex);
		if (index >= kTlsSlots || !tlsAllocated[index]) {
			cpu->r[EAX] = 0;
			lastError = kInvalidParameter;
			return;
		}
		generation = tlsGenerations[index];
	}
	threadTlsValues[index] = TlsValue{generation, argument(cpu, 1)};
	cpu->r[EAX] = 1;
}
} // namespace

void pumpMultimediaTimers(CPU* cpu)
{
	pumpMultimediaTimersInternal(cpu);
}

uint32_t currentWin32LastError()
{
	return lastError;
}

void setWin32LastError(uint32_t value)
{
	lastError = value;
}

bool registerWin32BaseHandlers(ImportDispatchRegistry& handlers)
{
	return handlers.registerName("kernel32.dll", "GetLastError", getLastError, 0) &&
		handlers.registerName("kernel32.dll", "ExitProcess", exitProcess, 4) &&
		handlers.registerName("advapi32.dll", "RegCreateKeyExA", regCreateKeyExA, 36) &&
		handlers.registerName("advapi32.dll", "RegSetValueExA", regSetValueExA, 24) &&
		handlers.registerName("advapi32.dll", "RegCloseKey", regCloseKey, 4) &&
		handlers.registerName("kernel32.dll", "SetErrorMode", setErrorMode, 4) &&
		handlers.registerName("kernel32.dll", "GetVersion", getVersion, 0) &&
		handlers.registerName("kernel32.dll", "SetLastError", setLastError, 4) &&
		handlers.registerName("kernel32.dll", "GetTickCount", getTickCount, 0) &&
		handlers.registerName("kernel32.dll", "QueryPerformanceCounter", queryPerformanceCounter, 4) &&
		handlers.registerName("kernel32.dll", "QueryPerformanceFrequency", queryPerformanceFrequency, 4) &&
		handlers.registerName("kernel32.dll", "Sleep", sleepMilliseconds, 4) &&
		handlers.registerName("kernel32.dll", "SleepEx", sleepEx, 8) &&
		handlers.registerName("kernel32.dll", "GetCurrentThreadId", getCurrentThreadId, 0) &&
		handlers.registerName("kernel32.dll", "GetCurrentProcessId", getCurrentProcessId, 0) &&
		handlers.registerName("kernel32.dll", "GetSystemTimeAsFileTime", getSystemTimeAsFileTime, 4) &&
		handlers.registerName("kernel32.dll", "SystemTimeToFileTime", systemTimeToFileTime, 8) &&
		handlers.registerName("kernel32.dll", "FileTimeToSystemTime", fileTimeToSystemTime, 8) &&
		handlers.registerName("kernel32.dll", "FileTimeToLocalFileTime", fileTimeToLocalFileTime, 8) &&
		handlers.registerName("kernel32.dll", "GetLocalTime", getLocalTime, 4) &&
		handlers.registerName("kernel32.dll", "TlsAlloc", tlsAlloc, 0) &&
		handlers.registerName("kernel32.dll", "TlsFree", tlsFree, 4) &&
		handlers.registerName("kernel32.dll", "TlsGetValue", tlsGetValue, 4) &&
		handlers.registerName("kernel32.dll", "TlsSetValue", tlsSetValue, 8) &&
		handlers.registerName("winmm.dll", "timeGetTime", getTickCount, 0) &&
		handlers.registerName("winmm.dll", "timeBeginPeriod", timeBeginPeriod, 4) &&
		handlers.registerName("winmm.dll", "timeEndPeriod", timeEndPeriod, 4) &&
		handlers.registerName("winmm.dll", "timeSetEvent", timeSetEvent, 20) &&
		handlers.registerName("winmm.dll", "timeKillEvent", timeKillEvent, 4);
}

} // namespace br::runtime

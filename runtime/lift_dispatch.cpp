#include "lift_dispatch.h"
#include "guest.h"
#include "ra3_guest_bridge.h"
#include "win32_base.h"
#include "android_diagnostics.h"
#include "guest_dispatch_diagnostics.h"
#if defined(__ANDROID__) && defined(BR_ANDROID_GUEST_PROFILE)
#include "android_guest_profiler.h"
#endif
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <atomic>
#include <utility>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

extern const unsigned lifted_count;

namespace br::runtime {
namespace {
std::atomic<GuestImportLookup> gImportLookup{nullptr};
std::atomic<const void*> gImportContext{nullptr};
std::atomic<const ImportDispatchRegistry*> gHandlers{nullptr};
thread_local GuestDispatchTraceContext gGuestDispatchTrace{};
std::atomic<uint64_t> gGuestDispatchTraceDispatchCount{0u};
std::atomic<uint64_t> gGuestDispatchTraceLocalCount{0u};

uint64_t nextGuestDispatchTraceSample(std::atomic<uint64_t>& counter)
{
	const uint64_t occurrence = counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
	return br::runtime::shouldLogGuestDispatchTrace(occurrence) ? occurrence : 0u;
}

bool traceImportedCalls()
{
	static const bool enabled = [] {
		const char* value = std::getenv("BR_TRACE_IMPORTS");
		return value && value[0] && value[0] != '0';
	}();
	return enabled;
}

bool traceThisImport(const ApiImportStub& import)
{
	if (!traceImportedCalls()) return false;
	const char* mode = std::getenv("BR_TRACE_IMPORTS");
	if (!mode || std::string(mode) != "compact" || import.byOrdinal) return true;
	const std::string& name = import.name;
	return name.rfind("Heap", 0) != 0 && name != "EnterCriticalSection" &&
		name != "LeaveCriticalSection" && name != "DeleteCriticalSection" &&
		name != "ReadFile" && name != "SetErrorMode" && name != "SetLastError" &&
		name != "MultiByteToWideChar" &&
		name != "GetTickCount" && name != "QueryPerformanceCounter" &&
		name != "QueryPerformanceFrequency";
}

void enforceImportCallLimit(const ApiImportStub& import)
{
	static const uint64_t limit = [] {
		const char* value = std::getenv("BR_MAX_API_CALLS");
		if (!value || !*value) return uint64_t(0);
		char* end = nullptr;
		const unsigned long long parsed = std::strtoull(value, &end, 10);
		return end == value ? uint64_t(0) : static_cast<uint64_t>(parsed);
	}();
	if (!limit) return;
	static std::atomic<uint64_t> callCount{0};
	const uint64_t number = callCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
	if (number <= limit) return;
	std::string message = "diagnostic API-call limit ";
	message += std::to_string(limit);
	message += " reached before ";
	message += import.byOrdinal ? "#" + std::to_string(import.ordinal) : import.name;
	throw std::runtime_error(message);
}

#if defined(BR_HEADLESS_GUEST_DEADLINE_DIAGNOSTIC)
const ApiImportStub* resolveImport(uint32_t guestAddress);

void stopGuestAtDiagnosticDeadline(const CPU* cpu, uint32_t guestAddress)
{
	static const uint64_t deadlineMs = [] {
		const char* value = std::getenv("BR_STOP_GUEST_AFTER_MS");
		if (!value || !*value) return uint64_t(0);
		char* end = nullptr;
		const unsigned long long parsed = std::strtoull(value, &end, 10);
		return end != value && *end == '\0' && parsed > 0ull && parsed <= 3600000ull
			? static_cast<uint64_t>(parsed) : uint64_t(0);
	}();
	if (!deadlineMs) return;
	static const auto started = std::chrono::steady_clock::now();
	const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count());
	if (elapsedMs < deadlineMs) return;
	std::fprintf(stderr,
		"[br-host] diagnostic guest stop deadline elapsed=%llu ms target=%08x esp=%08x ebp=%08x "
		"eax=%08x ecx=%08x edx=%08x ebx=%08x fsbase=%08x\n",
		static_cast<unsigned long long>(elapsedMs), guestAddress,
		cpu ? cpu->r[ESP] : 0u, cpu ? cpu->r[EBP] : 0u,
		cpu ? cpu->r[EAX] : 0u, cpu ? cpu->r[ECX] : 0u,
		cpu ? cpu->r[EDX] : 0u, cpu ? cpu->r[EBX] : 0u,
		cpu ? cpu->fsbase : 0u);
	if (const auto* import = resolveImport(guestAddress)) {
		if (import->byOrdinal)
			std::fprintf(stderr, "[br-host] diagnostic deadline target import=%s!#%u\n",
				import->library.c_str(), import->ordinal);
		else
			std::fprintf(stderr, "[br-host] diagnostic deadline target import=%s!%s\n",
				import->library.c_str(), import->name.c_str());
	} else {
		std::fprintf(stderr, "[br-host] diagnostic deadline target is not a bound import\n");
	}
	throw std::runtime_error("diagnostic guest execution deadline reached");
}
#endif

std::string traceGuestString(uint32_t address)
{
	std::string value;
	if (!address) return value;
	for (uint32_t i = 0; i < 160u; ++i) {
		const uint8_t* byte = guest::host(address + i);
		if (!byte || !*byte) break;
		value.push_back(*byte >= 0x20u && *byte <= 0x7eu ? static_cast<char>(*byte) : '?');
	}
	return value;
}

std::string traceGuestWideString(uint32_t address)
{
	std::string value;
	if (!address) return value;
	for (uint32_t i = 0; i < 160u; ++i) {
		const uint16_t character = guest::load<uint16_t>(address + i * 2u);
		if (!character) break;
		value.push_back(character >= 0x20u && character <= 0x7eu
			? static_cast<char>(character) : '?');
	}
	return value;
}

void traceGuestFrames(const CPU* cpu)
{
	if (!cpu) return;
	uint32_t frame = cpu->r[EBP];
	for (unsigned depth = 0; depth < 8u && frame >= 0x7ff00000u &&
		uint64_t(frame) + 0x20u <= 0x7ff10000u; ++depth) {
		const uint32_t previous = guest::load<uint32_t>(frame);
		const uint32_t returnAddress = guest::load<uint32_t>(frame + 4u);
		std::fprintf(stderr, "[br-frame] depth=%u ebp=0x%08x return=0x%08x args=[%08x,%08x,%08x,%08x,%08x,%08x]\n",
			depth, frame, returnAddress,
			guest::load<uint32_t>(frame + 8u), guest::load<uint32_t>(frame + 12u),
			guest::load<uint32_t>(frame + 16u), guest::load<uint32_t>(frame + 20u),
			guest::load<uint32_t>(frame + 24u), guest::load<uint32_t>(frame + 28u));
		if (previous <= frame || previous >= 0x7ff10000u) break;
		frame = previous;
	}
}

std::string normalizedLibrary(const std::string& library)
{
	std::string result = library;
	for (char& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return result;
}

const ApiImportStub* boundImportLookup(const void* context, uint32_t guestAddress)
{
	return static_cast<const ImportBindingTable*>(context)->lookup(guestAddress);
}

std::string namedKey(const std::string& library, const std::string& name)
{
	return normalizedLibrary(library) + "!N:" + name;
}

std::string ordinalKey(const std::string& library, uint16_t ordinal)
{
	return normalizedLibrary(library) + "!O:" + std::to_string(ordinal);
}

const ApiImportStub* resolveImport(uint32_t guestAddress)
{
	const auto lookup = gImportLookup.load(std::memory_order_acquire);
	if (!lookup) return nullptr;
	return lookup(gImportContext.load(std::memory_order_acquire), guestAddress);
}

const LiftedEntry* findLifted(uint32_t guestAddress, unsigned* indexOut = nullptr)
{
	unsigned lo = 0;
	unsigned hi = lifted_count;
	while (lo < hi) {
		const unsigned mid = lo + (hi - lo) / 2;
		if (lifted_table[mid].addr < guestAddress) lo = mid + 1;
		else hi = mid;
	}
	if (!guestAddress || lo >= lifted_count || lifted_table[lo].addr != guestAddress ||
		!lifted_table[lo].fn) return nullptr;
	if (indexOut) *indexOut = lo;
	return &lifted_table[lo];
}

void logUnknownImportArguments(CPU* cpu, uint32_t stackPointer, const ApiImportStub* import)
{
	if (!cpu || !guest::base || stackPointer < 0x7ff00000u ||
		uint64_t(stackPointer) + 36u > 0x7ff10000u) return;
	const uint32_t ret = guest::load<uint32_t>(stackPointer);
	const uint32_t a0 = guest::load<uint32_t>(stackPointer + 4u);
	const uint32_t a1 = guest::load<uint32_t>(stackPointer + 8u);
	const uint32_t a2 = guest::load<uint32_t>(stackPointer + 12u);
	const uint32_t a3 = guest::load<uint32_t>(stackPointer + 16u);
	const uint32_t a4 = guest::load<uint32_t>(stackPointer + 20u);
	const uint32_t a5 = guest::load<uint32_t>(stackPointer + 24u);
	const uint32_t a6 = guest::load<uint32_t>(stackPointer + 28u);
	const uint32_t a7 = guest::load<uint32_t>(stackPointer + 32u);
	char path[161]{};
	if (import && !import->byOrdinal && import->name == "FindFirstFileA") {
		for (uint32_t i = 0; i + 1u < sizeof(path); ++i) {
			const auto* byte = guest::host(a0 + i);
			if (!byte || !*byte) break;
			path[i] = (*byte >= 0x20u && *byte <= 0x7eu) ? static_cast<char>(*byte) : '?';
		}
	}
	#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
		"unresolved API guest caller=0x%08x args=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]",
		ret, a0, a1, a2, a3, a4, a5, a6, a7);
	if (path[0]) br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative", "FindFirstFileA pattern='%s'", path);
	#endif
	std::fprintf(stderr,
		"Battle Realms unresolved API caller=0x%08x args=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
		ret, a0, a1, a2, a3, a4, a5, a6, a7);
}

} // namespace

GuestDispatchTraceContext enterGuestDispatchTrace(CPU* cpu, uint32_t target,
	uint32_t parent, uint32_t instruction, const char* mechanism, const char* route)
{
	const GuestDispatchTraceContext previous = gGuestDispatchTrace;
	if (target != 0x006a13a0u) return previous;
	gGuestDispatchTrace = {target, parent, instruction, mechanism, route};
	const uint64_t occurrence = nextGuestDispatchTraceSample(gGuestDispatchTraceDispatchCount);
	if (!occurrence) return previous;
	const uint32_t esp = cpu ? cpu->r[ESP] : 0u;
	const uint32_t ebp = cpu ? cpu->r[EBP] : 0u;
	std::fprintf(stderr,
		"[br-dispatch-site] occurrence=%llu target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=%s esp=0x%08x ebp=0x%08x\n",
		static_cast<unsigned long long>(occurrence), target, instruction, instruction, parent, mechanism ? mechanism : "unknown",
		route ? route : "unknown", esp, ebp);
#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"GUEST_DISPATCH_TRACE_SAMPLE",
		"occurrence=%llu target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=%s esp=0x%08x ebp=0x%08x",
		static_cast<unsigned long long>(occurrence), target, instruction, instruction, parent, mechanism ? mechanism : "unknown",
		route ? route : "unknown", esp, ebp);
#endif
	return previous;
}

void leaveGuestDispatchTrace(const GuestDispatchTraceContext& previous)
{
	gGuestDispatchTrace = previous;
}

GuestDispatchTraceContext currentGuestDispatchTrace()
{
	return gGuestDispatchTrace;
}

void traceGuestDispatchLocal(CPU* cpu, uint32_t target, uint32_t parent,
	uint32_t instruction, const char* mechanism)
{
	if (target != 0x006a13a0u) return;
	const uint64_t occurrence = nextGuestDispatchTraceSample(gGuestDispatchTraceLocalCount);
	if (!occurrence) return;
	const uint32_t esp = cpu ? cpu->r[ESP] : 0u;
	const uint32_t ebp = cpu ? cpu->r[EBP] : 0u;
	std::fprintf(stderr,
		"[br-dispatch-site] occurrence=%llu target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=local-block esp=0x%08x ebp=0x%08x\n",
		static_cast<unsigned long long>(occurrence), target, instruction, instruction, parent, mechanism ? mechanism : "unknown", esp, ebp);
#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"GUEST_DISPATCH_TRACE_SAMPLE",
		"occurrence=%llu target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=local-block esp=0x%08x ebp=0x%08x",
		static_cast<unsigned long long>(occurrence), target, instruction, instruction, parent, mechanism ? mechanism : "unknown", esp, ebp);
#endif
}

[[noreturn]] void dispatchFailure(CPU* cpu, uint32_t guestAddress, const char* reason)
{
	const uint32_t stackPointer = cpu ? cpu->r[ESP] : 0;
	if (const auto* import = resolveImport(guestAddress)) {
		logUnknownImportArguments(cpu, stackPointer, import);
		if (import->byOrdinal) {
			#if defined(__ANDROID__)
			br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_FATAL, "BattleRealmsNative",
				"unresolved import %s!#%u at 0x%08x (ESP=0x%08x): %s",
				import->library.c_str(), import->ordinal, guestAddress, stackPointer, reason);
			#endif
			std::fprintf(stderr, "Battle Realms unresolved import %s!#%u at 0x%08x (ESP=0x%08x): %s\n",
				import->library.c_str(), import->ordinal, guestAddress, stackPointer, reason);
		} else {
			#if defined(__ANDROID__)
			br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_FATAL, "BattleRealmsNative",
				"unresolved import %s!%s at 0x%08x (ESP=0x%08x): %s",
				import->library.c_str(), import->name.c_str(), guestAddress, stackPointer, reason);
			#endif
			std::fprintf(stderr, "Battle Realms unresolved import %s!%s at 0x%08x (ESP=0x%08x): %s\n",
				import->library.c_str(), import->name.c_str(), guestAddress, stackPointer, reason);
		}
		const char* throwFailures = std::getenv("BR_THROW_FAILURES");
		if (throwFailures && throwFailures[0] && throwFailures[0] != '0') {
			std::string message = "guest import failure ";
			message += import->library;
			message += import->byOrdinal ? "!#" + std::to_string(import->ordinal) : "!" + import->name;
			message += " at ";
			char address[16]{};
			std::snprintf(address, sizeof(address), "0x%08x", guestAddress);
			message += address;
			message += ": ";
			message += reason ? reason : "unknown failure";
			throw std::runtime_error(message);
		}
		std::abort();
	}
	#if defined(__ANDROID__)
	uint32_t returnAddress = 0, stack1 = 0, stack2 = 0, stack3 = 0;
	uint32_t frame = cpu ? cpu->r[EBP] : 0;
	uint32_t frame1 = 0, frame2 = 0, frame3 = 0;
	if (cpu && guest::base && stackPointer >= 0x7ff00000u &&
		uint64_t(stackPointer) + 16u <= 0x7ff10000u) {
		returnAddress = guest::load<uint32_t>(stackPointer);
		stack1 = guest::load<uint32_t>(stackPointer + 4u);
		stack2 = guest::load<uint32_t>(stackPointer + 8u);
		stack3 = guest::load<uint32_t>(stackPointer + 12u);
	}
	if (guest::base && frame >= 0x7ff00000u && uint64_t(frame) + 8u <= 0x7ff10000u) {
		frame1 = guest::load<uint32_t>(frame + 4u);
		const uint32_t parent = guest::load<uint32_t>(frame);
		if (parent >= 0x7ff00000u && uint64_t(parent) + 8u <= 0x7ff10000u) {
			frame2 = guest::load<uint32_t>(parent + 4u);
			const uint32_t grandparent = guest::load<uint32_t>(parent);
			if (grandparent >= 0x7ff00000u && uint64_t(grandparent) + 8u <= 0x7ff10000u)
				frame3 = guest::load<uint32_t>(grandparent + 4u);
		}
	}
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_FATAL, "BattleRealmsNative",
		"guest dispatch failure target=0x%08x return=0x%08x stack=[%08x,%08x,%08x] EBP=0x%08x frames=[%08x,%08x,%08x] (ESP=0x%08x): %s",
		guestAddress, returnAddress, stack1, stack2, stack3, frame, frame1, frame2, frame3, stackPointer, reason);
	{
	const auto dispatchTrace = br::runtime::currentGuestDispatchTrace();
	if (dispatchTrace.target == guestAddress)
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_FATAL, "BattleRealmsNative",
			"dispatch-failure target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=%s",
			dispatchTrace.target, dispatchTrace.instruction, dispatchTrace.instruction, dispatchTrace.parent,
			dispatchTrace.mechanism ? dispatchTrace.mechanism : "unknown",
			dispatchTrace.route ? dispatchTrace.route : "unknown");
	}
	#endif
	std::fprintf(stderr, "Battle Realms guest dispatch failure at 0x%08x (ESP=0x%08x): %s\n",
		guestAddress, stackPointer, reason);
	const auto dispatchTrace = br::runtime::currentGuestDispatchTrace();
	if (dispatchTrace.target == guestAddress)
		std::fprintf(stderr,
			"Battle Realms guest dispatch site target=0x%08x caller=0x%08x guest_ip=0x%08x parent=0x%08x mechanism=%s route=%s\n",
			dispatchTrace.target, dispatchTrace.instruction, dispatchTrace.instruction, dispatchTrace.parent,
			dispatchTrace.mechanism ? dispatchTrace.mechanism : "unknown",
			dispatchTrace.route ? dispatchTrace.route : "unknown");
	const char* throwFailures = std::getenv("BR_THROW_FAILURES");
	if (throwFailures && throwFailures[0] && throwFailures[0] != '0') {
		char message[160]{};
		std::snprintf(message, sizeof(message), "guest dispatch failure at 0x%08x (ESP=0x%08x): %s",
			guestAddress, stackPointer, reason ? reason : "unknown failure");
		throw std::runtime_error(message);
	}
	std::abort();
}

bool ImportDispatchRegistry::registerName(const std::string& library, const std::string& name,
	ImportedCallHandler handler, uint32_t stackPopBytes)
{
	if (!handler || (stackPopBytes & 3u)) return false;
	return handlers_.emplace(namedKey(library, name), ImportedCall{handler, stackPopBytes}).second;
}

bool ImportDispatchRegistry::registerOrdinal(const std::string& library, uint16_t ordinal,
	ImportedCallHandler handler, uint32_t stackPopBytes)
{
	if (!handler || (stackPopBytes & 3u)) return false;
	return handlers_.emplace(ordinalKey(library, ordinal), ImportedCall{handler, stackPopBytes}).second;
}

const ImportedCall* ImportDispatchRegistry::find(const ApiImportStub& import) const
{
	const auto key = import.byOrdinal ? ordinalKey(import.library, import.ordinal)
		: namedKey(import.library, import.name);
	const auto found = handlers_.find(key);
	return found == handlers_.end() ? nullptr : &found->second;
}

void configureLiftDispatch(GuestImportLookup importLookup, const void* importContext,
	const ImportDispatchRegistry* handlers)
{
	gImportContext.store(importContext, std::memory_order_release);
	gHandlers.store(handlers, std::memory_order_release);
	gImportLookup.store(importLookup, std::memory_order_release);
}

void configureLiftDispatch(const ImportBindingTable& imports,
	const ImportDispatchRegistry* handlers)
{
	configureLiftDispatch(boundImportLookup, &imports, handlers);
}

} // namespace br::runtime

void call_target(CPU* cpu, uint32_t guestAddress)
{
	if (!cpu) br::runtime::dispatchFailure(cpu, guestAddress, "null CPU state");
#if defined(BR_HEADLESS_GUEST_DEADLINE_DIAGNOSTIC)
	br::runtime::stopGuestAtDiagnosticDeadline(cpu, guestAddress);
#endif
	if (const auto* import = br::runtime::resolveImport(guestAddress)) {
		br::runtime::enforceImportCallLimit(*import);
		const auto* handlers = br::runtime::gHandlers.load(std::memory_order_acquire);
		const auto* call = handlers ? handlers->find(*import) : nullptr;
		if (!call) {
			if (br::runtime::traceImportedCalls() && !import->byOrdinal &&
				import->library == "MSACM32.dll" && import->name == "acmStreamOpen") {
				const uint32_t esp = cpu->r[ESP];
				const uint32_t streamOutput = guest::load<uint32_t>(esp + 4u);
				const uint32_t sourceFormat = guest::load<uint32_t>(esp + 12u);
				const uint32_t destinationFormat = guest::load<uint32_t>(esp + 16u);
				std::fprintf(stderr, "[br-api] unresolved acmStreamOpen args=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
					streamOutput, guest::load<uint32_t>(esp + 8u), sourceFormat, destinationFormat,
					guest::load<uint32_t>(esp + 20u), guest::load<uint32_t>(esp + 24u),
					guest::load<uint32_t>(esp + 28u), guest::load<uint32_t>(esp + 32u));
				for (const auto& format : {std::pair<const char*, uint32_t>{"source", sourceFormat},
					std::pair<const char*, uint32_t>{"destination", destinationFormat}}) {
					if (format.second)
						std::fprintf(stderr, "[br-api] acmStreamOpen %s WAVEFORMATEX tag=%u channels=%u rate=%u avg=%u align=%u bits=%u extra=%u\n",
							format.first, guest::load<uint16_t>(format.second),
							guest::load<uint16_t>(format.second + 2u),
							guest::load<uint32_t>(format.second + 4u),
							guest::load<uint32_t>(format.second + 8u),
							guest::load<uint16_t>(format.second + 12u),
							guest::load<uint16_t>(format.second + 14u),
							guest::load<uint16_t>(format.second + 16u));
					if (format.first[0] == 's' && format.second &&
						guest::load<uint16_t>(format.second) == 2u &&
						guest::load<uint16_t>(format.second + 16u) >= 4u) {
						const uint32_t samplesPerBlock = guest::load<uint16_t>(format.second + 18u);
						const uint32_t coefficientCount = guest::load<uint16_t>(format.second + 20u);
						std::fprintf(stderr, "[br-api] acmStreamOpen ADPCM samples-per-block=%u coefficients=%u", samplesPerBlock, coefficientCount);
						for (uint32_t i = 0; i < coefficientCount && i < 7u; ++i)
							std::fprintf(stderr, " (%d,%d)",
								guest::load<int16_t>(format.second + 22u + i * 4u),
								guest::load<int16_t>(format.second + 24u + i * 4u));
						std::fputc('\n', stderr);
					}
				}
			}
			if (br::runtime::traceImportedCalls() && !import->byOrdinal &&
				import->name == "LoadLibraryA") {
				const uint32_t module = guest::load<uint32_t>(cpu->r[ESP] + 4u);
				std::fprintf(stderr, "[br-api] unresolved LoadLibraryA module='%s'\n",
					br::runtime::traceGuestString(module).c_str());
			}
			br::runtime::dispatchFailure(cpu, guestAddress, "unimplemented imported API");
		}
		const uint32_t entryEsp = cpu->r[ESP];
		uint32_t traceA1 = 0;
		if (br::runtime::traceThisImport(*import)) {
			const uint32_t caller = guest::load<uint32_t>(entryEsp);
			const uint32_t a0 = guest::load<uint32_t>(entryEsp + 4u);
			const uint32_t a1 = guest::load<uint32_t>(entryEsp + 8u);
			traceA1 = a1;
			const uint32_t a2 = guest::load<uint32_t>(entryEsp + 12u);
			const uint32_t a3 = guest::load<uint32_t>(entryEsp + 16u);
			const uint32_t a4 = guest::load<uint32_t>(entryEsp + 20u);
			const uint32_t a5 = guest::load<uint32_t>(entryEsp + 24u);
			if (import->byOrdinal)
				std::fprintf(stderr, "[br-api] %s!#%u caller=0x%08x esp=0x%08x args=[%08x,%08x,%08x,%08x]\n",
					import->library.c_str(), import->ordinal, caller, entryEsp, a0, a1, a2, a3);
			else {
				std::fprintf(stderr, "[br-api] %s!%s caller=0x%08x esp=0x%08x args=[%08x,%08x,%08x,%08x]\n",
					import->library.c_str(), import->name.c_str(), caller, entryEsp, a0, a1, a2, a3);
				if (import->name == "MessageBoxA")
					std::fprintf(stderr, "[br-api] MessageBox text='%s' title='%s'\n",
						br::runtime::traceGuestString(a1).c_str(), br::runtime::traceGuestString(a2).c_str());
				else if (import->name == "CreateFileA")
					std::fprintf(stderr, "[br-api] CreateFile path='%s'\n",
						br::runtime::traceGuestString(a0).c_str());
				else if (import->name == "SetCurrentDirectoryA")
					std::fprintf(stderr, "[br-api] SetCurrentDirectory path='%s'\n",
						br::runtime::traceGuestString(a0).c_str());
				else if (import->name == "WideCharToMultiByte")
					std::fprintf(stderr, "[br-api] WideCharToMultiByte src='%s' sourceLength=%d dest=0x%08x capacity=%u\n",
						br::runtime::traceGuestWideString(a2).c_str(), static_cast<int32_t>(a3), a4, a5);
				else if (import->name == "GetPrivateProfileIntA" || import->name == "GetPrivateProfileStringA")
					std::fprintf(stderr, "[br-api] INI section='%s' key='%s' path='%s'\n",
						br::runtime::traceGuestString(a0).c_str(), br::runtime::traceGuestString(a1).c_str(),
						br::runtime::traceGuestString(import->name == "GetPrivateProfileIntA" ? a3 : a5).c_str());
			if (import->name == "GetPrivateProfileIntA") br::runtime::traceGuestFrames(cpu);
			}
		}
		bool traceLongM2w = false;
	if (br::runtime::traceImportedCalls() && !import->byOrdinal &&
		import->name == "MultiByteToWideChar") {
		const uint32_t caller = guest::load<uint32_t>(entryEsp);
		const uint32_t source = guest::load<uint32_t>(entryEsp + 12u);
		const int32_t sourceLength = static_cast<int32_t>(guest::load<uint32_t>(entryEsp + 16u));
		const uint32_t destination = guest::load<uint32_t>(entryEsp + 20u);
		const uint32_t destinationCapacity = guest::load<uint32_t>(entryEsp + 24u);
		const std::string sourceText = br::runtime::traceGuestString(source);
		static std::atomic<uint32_t> localeTraceCount{0u};
		const uint32_t localeTraceIndex = caller == 0x0066c579u
			? localeTraceCount.fetch_add(1u, std::memory_order_relaxed) + 1u : 0u;
		traceLongM2w = sourceText.find("Battle") != std::string::npos ||
			sourceText.find("Realm") != std::string::npos || destination == 0x302d6a10u ||
			(caller == 0x0066c579u && (localeTraceIndex <= 4u || localeTraceIndex % 1000u == 0u));
		if (traceLongM2w)
			std::fprintf(stderr, "[br-api] MultiByteToWideChar caller=0x%08x cp=%u flags=%u src='%s' length=%d dest=0x%08x capacity=%u\n",
				caller,
				guest::load<uint32_t>(entryEsp + 4u), guest::load<uint32_t>(entryEsp + 8u),
				sourceText.c_str(), sourceLength, destination, destinationCapacity);
	}
		{
#if defined(__ANDROID__) && defined(BR_ANDROID_GUEST_PROFILE)
			br::runtime::AndroidGuestHostCallScope hostCallScope;
#endif
			call->handler(cpu, *import);
		}
		if (br::runtime::traceThisImport(*import) && !import->byOrdinal &&
			(import->name == "GetModuleFileNameA" || import->name == "GetCurrentDirectoryA")) {
			std::fprintf(stderr, "[br-api] output-string='%s'\n",
				br::runtime::traceGuestString(traceA1).c_str());
		}
		if (br::runtime::traceThisImport(*import) && !import->byOrdinal &&
			import->name == "WideCharToMultiByte") {
			const uint32_t output = guest::load<uint32_t>(entryEsp + 20u);
			std::fprintf(stderr, "[br-api] WideCharToMultiByte output='%s' result=%u\n",
				br::runtime::traceGuestString(output).c_str(), cpu->r[EAX]);
		}
		if (traceLongM2w) {
			const uint32_t destination = guest::load<uint32_t>(entryEsp + 20u);
			if (cpu->r[EAX])
				std::fprintf(stderr, "[br-api] MultiByteToWideChar output='%s' result=%u\n",
					br::runtime::traceGuestWideString(destination).c_str(), cpu->r[EAX]);
			else
				std::fprintf(stderr, "[br-api] MultiByteToWideChar conversion failed last-error=%u\n",
					br::runtime::currentWin32LastError());
		}
		cpu->r[ESP] += 4u + call->stackPopBytes;
		br::runtime::pumpMultimediaTimers(cpu);
		if (br::runtime::traceThisImport(*import))
			std::fprintf(stderr, "[br-api] return eax=0x%08x esp=0x%08x\n", cpu->r[EAX], cpu->r[ESP]);
		return;
	}
	const auto* function = br::runtime::findLifted(guestAddress);
	if (!function) br::runtime::dispatchFailure(cpu, guestAddress, "unknown guest function address");
	function->fn(cpu);
}

void call_target_ic(CPU* cpu, uint32_t guestAddress, std::atomic<uint64_t>* inlineCache)
{
#if defined(BR_HEADLESS_GUEST_DEADLINE_DIAGNOSTIC)
	br::runtime::stopGuestAtDiagnosticDeadline(cpu, guestAddress);
#endif
	if (br::runtime::resolveImport(guestAddress)) {
		call_target(cpu, guestAddress);
		return;
	}
	unsigned index = 0;
	const auto* function = br::runtime::findLifted(guestAddress, &index);
	if (!function) {
		call_target(cpu, guestAddress);
		return;
	}
	if (inlineCache) {
		uint64_t expected = 0;
		const uint64_t cacheEntry = (uint64_t(index) << 32) | guestAddress;
		inlineCache->compare_exchange_strong(expected, cacheEntry, std::memory_order_relaxed);
	}
	function->fn(cpu);
}

bool callGuestStdcall(CPU* cpu, uint32_t guestAddress, const uint32_t* arguments,
	uint32_t argumentCount, uint32_t& result, std::string& error)
{
	if (!cpu || (argumentCount && !arguments)) {
		error = "invalid CPU or callback argument vector";
		return false;
	}
	if (!br::runtime::findLifted(guestAddress)) {
		error = "callback target is not a lifted guest function";
		return false;
	}
	const uint64_t bytes64 = 4ull * (uint64_t(argumentCount) + 1ull);
	const uint32_t oldEsp = cpu->r[ESP];
	if (bytes64 > oldEsp) {
		error = "callback stack underflow";
		return false;
	}
	const uint32_t bytes = static_cast<uint32_t>(bytes64);
	const uint32_t callbackEsp = oldEsp - bytes;
	auto* stack = br::runtime::sharedGuestAddressSpace().host(callbackEsp, bytes);
	if (!stack) {
		error = "callback stack range is not committed guest memory";
		return false;
	}
	(void)stack;
	guest::store<uint32_t>(callbackEsp, 0xffffffffu); // sentinel return slot; lifted RET returns to the C++ bridge
	for (uint32_t i = 0; i < argumentCount; ++i)
		guest::store<uint32_t>(callbackEsp + 4u + i * 4u, arguments[i]);
	cpu->r[ESP] = callbackEsp;
	call_target(cpu, guestAddress);
	result = cpu->r[EAX];
	const bool stdcallCleanupCorrect = cpu->r[ESP] == oldEsp;
	cpu->r[ESP] = oldEsp;
	if (!stdcallCleanupCorrect) {
		error = "guest callback returned with an unexpected stdcall stack depth";
		return false;
	}
	error.clear();
	return true;
}

[[noreturn]] void unsupported(CPU* cpu, uint32_t guestAddress, const char* instruction)
{
	br::runtime::dispatchFailure(cpu, guestAddress,
		instruction ? instruction : "unsupported x86 instruction");
}

void ra3_hook_after(CPU*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

bool ra3_gate(CPU*, uint32_t)
{
	return false;
}

uint32_t g_eflags_hi = 0;
uint32_t ra3_stub_tolower = 0xffffffffu;

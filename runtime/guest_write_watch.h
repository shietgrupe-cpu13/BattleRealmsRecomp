#pragma once

// Diagnostic-only store wrappers for generated guest code. This file is
// force-included only by the optional headless state-watch build. The vendor
// RA3 headers remain untouched.
#ifndef RA3_LIFTED_TU
#define RA3_LIFTED_TU 1
#endif
#include "lifted_prelude.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef RA3_TRACE
extern thread_local uint32_t ra3_ring[256];
extern thread_local uint32_t ra3_ring_pos;
#endif

namespace br::diagnostics {

inline bool gameStateWriteTraceEnabled()
{
	static const bool enabled = std::getenv("BR_TRACE_GAME_STATE_WRITES") != nullptr;
	return enabled;
}

inline uint32_t watchedGuestAddress()
{
	static const uint32_t address = [] {
		const char* text = std::getenv("BR_TRACE_GUEST_WRITE_ADDRESS");
		if (!text || !*text) return 0u;
		char* end = nullptr;
		const unsigned long parsed = std::strtoul(text, &end, 0);
		return end != text && *end == '\0' && parsed <= 0xfffffffful
			? static_cast<uint32_t>(parsed) : 0u;
	}();
	return address;
}

inline const std::vector<uint32_t>& watchedGuestFunctions()
{
	static const std::vector<uint32_t> functions = [] {
		std::vector<uint32_t> result;
		const char* text = std::getenv("BR_TRACE_GUEST_FUNCTIONS");
		if (!text || !*text) return result;
		std::string list(text);
		size_t begin = 0;
		while (begin < list.size()) {
			const size_t end = list.find_first_of(",; ", begin);
			const std::string token = list.substr(begin, end == std::string::npos
				? std::string::npos : end - begin);
			if (!token.empty()) {
				char* parsedEnd = nullptr;
				const unsigned long value = std::strtoul(token.c_str(), &parsedEnd, 0);
				if (parsedEnd != token.c_str() && *parsedEnd == '\0' && value <= 0xfffffffful)
					result.push_back(static_cast<uint32_t>(value));
			}
			if (end == std::string::npos) break;
			begin = end + 1;
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}();
	return functions;
}

inline void traceGuestFunction(uint32_t address, const CPU* cpu)
{
	const auto& functions = watchedGuestFunctions();
	if (functions.empty() || !std::binary_search(functions.begin(), functions.end(), address)) return;
	const uint32_t esp = cpu ? cpu->r[4] : 0u;
	const uint32_t returnAddress = esp ? G32(esp) : 0u;
	std::fprintf(stderr,
		"[br-function] enter address=%08x return=%08x esp=%08x fsbase=%08x "
		"eax=%08x ecx=%08x edx=%08x ebx=%08x ebp=%08x esi=%08x edi=%08x\n",
		address, returnAddress, esp, cpu ? cpu->fsbase : 0u,
		cpu ? cpu->r[0] : 0u, cpu ? cpu->r[1] : 0u, cpu ? cpu->r[2] : 0u,
		cpu ? cpu->r[3] : 0u, cpu ? cpu->r[5] : 0u, cpu ? cpu->r[6] : 0u,
		cpu ? cpu->r[7] : 0u);
}

template <class T>
inline void watchedGuestStore(uint32_t address, T value, const CPU* cpu,
	uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
	uint32_t esp, uint32_t ebp, uint32_t esi, uint32_t edi,
	const char* sourceFile, int sourceLine)
{
	constexpr uint32_t kState50 = 0x006de340u;
	constexpr uint32_t kState60 = 0x006de350u;
	const uint64_t writeBegin = address;
	const uint64_t writeEnd = writeBegin + sizeof(T);
	const bool touchesState50 = writeBegin < uint64_t(kState50) + sizeof(uint32_t) &&
		uint64_t(kState50) < writeEnd;
	const bool touchesState60 = writeBegin < uint64_t(kState60) + sizeof(uint32_t) &&
		uint64_t(kState60) < writeEnd;
	const uint32_t watchedAddress = watchedGuestAddress();
	const bool touchesExtraAddress = watchedAddress &&
		writeBegin < uint64_t(watchedAddress) + sizeof(uint32_t) &&
		uint64_t(watchedAddress) < writeEnd;
	if ((!gameStateWriteTraceEnabled() || (!touchesState50 && !touchesState60)) &&
		!touchesExtraAddress) {
		guest::store<T>(address, value);
		return;
	}

	// This lock affects only opt-in diagnostic builds and makes each captured
	// before/write/after record internally consistent if guest threads are added.
	static std::mutex traceMutex;
	std::lock_guard<std::mutex> lock(traceMutex);
	const uint32_t before50 = guest::load<uint32_t>(kState50);
	const uint32_t before60 = guest::load<uint32_t>(kState60);
	const uint32_t watchedBefore = touchesExtraAddress
		? guest::load<uint32_t>(watchedAddress) : 0u;
	guest::store<T>(address, value);
	const uint32_t after50 = guest::load<uint32_t>(kState50);
	const uint32_t after60 = guest::load<uint32_t>(kState60);
	const uint32_t watchedAfter = touchesExtraAddress
		? guest::load<uint32_t>(watchedAddress) : 0u;
	uint32_t guestFunction = 0u;
#ifdef RA3_TRACE
	if (ra3_ring_pos) guestFunction = ra3_ring[(ra3_ring_pos - 1u) & 255u];
#endif
	const auto hostThread = std::hash<std::thread::id>{}(std::this_thread::get_id());
	std::fprintf(stderr,
		"[br-state-write] guest-store address=%08x width=%zu value=%llx function=%08x "
		"fields=%08x/%08x->%08x/%08x site=%s:%d\n"
		"[br-state-context] host-thread=%zx fsbase=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x "
		"esp=%08x ebp=%08x esi=%08x edi=%08x\n",
		address, sizeof(T), static_cast<unsigned long long>(value), guestFunction,
		before50, before60, after50, after60, sourceFile, sourceLine,
		hostThread, cpu ? cpu->fsbase : 0u, eax, ecx, edx, ebx, esp, ebp, esi, edi);
	if (touchesExtraAddress)
		std::fprintf(stderr,
			"[br-guest-write-watch] address=%08x width=%zu value=%llx watched=%08x:%08x->%08x "
			"function=%08x site=%s:%d\n",
			address, sizeof(T), static_cast<unsigned long long>(value), watchedAddress,
			watchedBefore, watchedAfter, guestFunction, sourceFile, sourceLine);
}

} // namespace br::diagnostics

#undef TRACE_FN
#ifdef RA3_TRACE
#define TRACE_FN(a) do { \
	uint32_t i_ = ra3_ring_pos++ & 255u; \
	ra3_ring[i_] = (a); ra3_ring_ra[i_] = G32(c->r[4]); \
	::br::diagnostics::traceGuestFunction((a), c); \
} while (0)
#else
#define TRACE_FN(a) (::br::diagnostics::traceGuestFunction((a), c))
#endif

#undef S8
#undef S16
#undef S32
#define S8(a, v) (::br::diagnostics::watchedGuestStore<uint8_t>((a), (uint8_t)(v), c, eax, ecx, edx, ebx, esp, ebp, esi, edi, __FILE__, __LINE__))
#define S16(a, v) (::br::diagnostics::watchedGuestStore<uint16_t>((a), (uint16_t)(v), c, eax, ecx, edx, ebx, esp, ebp, esi, edi, __FILE__, __LINE__))
#define S32(a, v) (::br::diagnostics::watchedGuestStore<uint32_t>((a), (uint32_t)(v), c, eax, ecx, edx, ebx, esp, ebp, esi, edi, __FILE__, __LINE__))

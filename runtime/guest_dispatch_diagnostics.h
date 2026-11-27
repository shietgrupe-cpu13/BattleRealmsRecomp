#pragma once

#include "cpu.h"

#include <atomic>
#include <cstdint>

namespace br::runtime {

struct GuestDispatchTraceContext {
	uint32_t target = 0;
	uint32_t parent = 0;
	uint32_t instruction = 0;
	const char* mechanism = nullptr;
	const char* route = nullptr;
};

GuestDispatchTraceContext enterGuestDispatchTrace(CPU* cpu, uint32_t target,
	uint32_t parent, uint32_t instruction, const char* mechanism, const char* route);
void leaveGuestDispatchTrace(const GuestDispatchTraceContext& previous);
GuestDispatchTraceContext currentGuestDispatchTrace();
void traceGuestDispatchLocal(CPU* cpu, uint32_t target, uint32_t parent,
	uint32_t instruction, const char* mechanism);

class GuestDispatchTraceScope {
public:
	GuestDispatchTraceScope(CPU* cpu, uint32_t target, uint32_t parent,
		uint32_t instruction, const char* mechanism, const char* route)
		: previous_(enterGuestDispatchTrace(cpu, target, parent, instruction, mechanism, route)),
	  active_(target == 0x006a13a0u) {}
	~GuestDispatchTraceScope() { if (active_) leaveGuestDispatchTrace(previous_); }
	GuestDispatchTraceScope(const GuestDispatchTraceScope&) = delete;
	GuestDispatchTraceScope& operator=(const GuestDispatchTraceScope&) = delete;
private:
	GuestDispatchTraceContext previous_;
	bool active_;
};

} // namespace br::runtime

// This macro is emitted only into a diagnostic overlay TU. Keep the same
// per-site inline cache behavior as RA3-Native's ICALL while retaining the
// exact guest transfer site for the one target under investigation.
#define BR_ICALL_AT(t_, parent_, instruction_, mechanism_) do { \
	static std::atomic<uint64_t> br_ic_; \
	uint32_t br_target_ = (t_); \
	::br::runtime::GuestDispatchTraceScope br_trace_(c, br_target_, (parent_), \
		(instruction_), (mechanism_), "guest-dispatch"); \
	uint64_t br_entry_ = br_ic_.load(std::memory_order_relaxed); \
	if ((uint32_t)br_entry_ == br_target_ && br_target_) \
		lifted_table[br_entry_ >> 32].fn(c); \
	else call_target_ic(c, br_target_, &br_ic_); \
} while (0)

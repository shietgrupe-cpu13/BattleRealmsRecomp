#pragma once

#if defined(__ANDROID__)
#include <cstdint>

extern thread_local uint32_t ra3_ring[256];
extern thread_local uint32_t ra3_ring_ra[256];
extern thread_local uint32_t ra3_ring_pos;

namespace br::runtime {

extern thread_local uint32_t brActiveGuestFunction;
extern thread_local uint32_t brActiveGuestCaller;

// Used only by the dedicated traced Android objects. The scope restores the
// caller after a lifted function returns, so periodic samples describe the
// currently executing guest function rather than the last function entered.
class AndroidGuestFunctionScope {
public:
	AndroidGuestFunctionScope(uint32_t function, uint32_t caller) noexcept
		: previousFunction_(__atomic_load_n(&brActiveGuestFunction, __ATOMIC_RELAXED)),
		  previousCaller_(__atomic_load_n(&brActiveGuestCaller, __ATOMIC_RELAXED))
	{
		const uint32_t slot = ::ra3_ring_pos++ & 255u;
		::ra3_ring[slot] = function;
		::ra3_ring_ra[slot] = caller;
		__atomic_store_n(&brActiveGuestCaller, caller, __ATOMIC_RELAXED);
		__atomic_store_n(&brActiveGuestFunction, function, __ATOMIC_RELAXED);
	}
	~AndroidGuestFunctionScope()
	{
		__atomic_store_n(&brActiveGuestFunction, previousFunction_, __ATOMIC_RELAXED);
		__atomic_store_n(&brActiveGuestCaller, previousCaller_, __ATOMIC_RELAXED);
	}
	AndroidGuestFunctionScope(const AndroidGuestFunctionScope&) = delete;
	AndroidGuestFunctionScope& operator=(const AndroidGuestFunctionScope&) = delete;
private:
	uint32_t previousFunction_;
	uint32_t previousCaller_;
};

// Imported Win32 and COM handlers execute host code while a lifted caller's
// scope remains on the stack. Temporarily clear that scope so samples describe
// guest instruction residency rather than charging renderer/API time to the
// last guest caller.
class AndroidGuestHostCallScope {
public:
	AndroidGuestHostCallScope() noexcept
		: previousFunction_(__atomic_load_n(&brActiveGuestFunction, __ATOMIC_RELAXED)),
		  previousCaller_(__atomic_load_n(&brActiveGuestCaller, __ATOMIC_RELAXED))
	{
		__atomic_store_n(&brActiveGuestCaller, 0u, __ATOMIC_RELAXED);
		__atomic_store_n(&brActiveGuestFunction, 0u, __ATOMIC_RELAXED);
	}
	~AndroidGuestHostCallScope()
	{
		__atomic_store_n(&brActiveGuestFunction, previousFunction_, __ATOMIC_RELAXED);
		__atomic_store_n(&brActiveGuestCaller, previousCaller_, __ATOMIC_RELAXED);
	}
	AndroidGuestHostCallScope(const AndroidGuestHostCallScope&) = delete;
	AndroidGuestHostCallScope& operator=(const AndroidGuestHostCallScope&) = delete;
private:
	uint32_t previousFunction_;
	uint32_t previousCaller_;
};

// Register the calling guest thread's RA3-compatible lifted-function ring with
// the low-overhead sampler. Call begin/end on the same thread as the guest.
void beginAndroidGuestProfileThread();
void endAndroidGuestProfileThread();

} // namespace br::runtime
#endif

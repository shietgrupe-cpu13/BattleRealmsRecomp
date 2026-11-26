// Host-side regression for the profiling scope transitions used by the
// Android-only sampler. This does not require Android headers or a device.
#define __ANDROID__ 1
#include "android_guest_profiler.h"

#include <cstdint>

thread_local uint32_t ra3_ring[256]{};
thread_local uint32_t ra3_ring_ra[256]{};
thread_local uint32_t ra3_ring_pos{};

namespace br::runtime {
thread_local uint32_t brActiveGuestFunction{};
thread_local uint32_t brActiveGuestCaller{};
}

int main()
{
	using namespace br::runtime;
	brActiveGuestFunction = 0x12345678u;
	brActiveGuestCaller = 0x87654321u;
	{
		AndroidGuestHostCallScope host;
		if (brActiveGuestFunction != 0u || brActiveGuestCaller != 0u) return 1;
	}
	if (brActiveGuestFunction != 0x12345678u || brActiveGuestCaller != 0x87654321u) return 2;
	{
		AndroidGuestFunctionScope guest(0x00401000u, 0x00402000u);
		if (brActiveGuestFunction != 0x00401000u || brActiveGuestCaller != 0x00402000u) return 3;
		{
			AndroidGuestHostCallScope host;
			if (brActiveGuestFunction != 0u || brActiveGuestCaller != 0u) return 4;
		}
		if (brActiveGuestFunction != 0x00401000u || brActiveGuestCaller != 0x00402000u) return 5;
	}
	if (brActiveGuestFunction != 0x12345678u || brActiveGuestCaller != 0x87654321u) return 6;
	return 0;
}

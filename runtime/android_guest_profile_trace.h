#pragma once

// Force-included only by BR_PROFILE_GUEST_FUNCTION_SCOPE builds. Keep the
// upstream RA3 trace ring for crash diagnostics while adding scoped current-
// function tracking for accurate periodic PC samples.
#include "lifted_prelude.h"
#include "android_guest_profiler.h"

#if defined(RA3_TRACE) && defined(__ANDROID__)
#undef TRACE_FN
#define TRACE_FN(address) \
	::br::runtime::AndroidGuestFunctionScope brAndroidGuestFunctionScope( \
		(address), G32(c->r[ESP]))
#endif

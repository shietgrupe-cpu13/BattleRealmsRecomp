#pragma once

#include "lift_dispatch.h"

#include <cstdint>

namespace br::runtime {

// ExitProcess ends the guest process, not the Android host. The entrypoint
// boundary catches this control-flow signal and reports the guest exit code.
struct GuestProcessExit {
	uint32_t code;
};

// Installs handlers whose behavior is independent of the window, filesystem,
// allocator, and DLL-lifecycle adapters. Returns false if registration collides.
bool registerWin32BaseHandlers(ImportDispatchRegistry& handlers);
uint32_t currentWin32LastError();
void setWin32LastError(uint32_t value);
// Deliver due TIME_CALLBACK_FUNCTION multimedia timer callbacks at guest API
// dispatch boundaries, where the single emulated CPU state is safe to re-enter.
void pumpMultimediaTimers(CPU* cpu);

} // namespace br::runtime

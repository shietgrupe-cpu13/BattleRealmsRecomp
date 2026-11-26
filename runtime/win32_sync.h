#pragma once

#include "lift_dispatch.h"

namespace br::runtime {

// Registers the initial event and critical-section subset used by the game.
// This is not a complete Win32 synchronization implementation.
bool registerWin32SyncHandlers(ImportDispatchRegistry& handlers);

} // namespace br::runtime

#pragma once

#include "lift_dispatch.h"

namespace br::runtime {

// Registers the current guest-memory subset of Heap*, Global*, and Virtual*.
// HeapCreate still aliases the process heap; page protection semantics are not
// fully modeled by the shared guest address space.
bool registerWin32MemoryHandlers(ImportDispatchRegistry& handlers);

} // namespace br::runtime

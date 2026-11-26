#pragma once

#include "guest_address_space.h"

namespace br::runtime {

// Shared address space used by both the Battle Realms PE loader and the
// RA3-compatible lifted-code memory helpers.
GuestAddressSpace& sharedGuestAddressSpace();

} // namespace br::runtime

// Battle Realms extension used by VirtualFree(MEM_DECOMMIT). This is separate
// from the upstream RA3 guest API and leaves the reserved 4 GiB window intact.
namespace guest {
bool decommit(uint32_t address, uint32_t size);
}

#include "ra3_guest_bridge.h"

#include "guest.h"

namespace br::runtime {
GuestAddressSpace& sharedGuestAddressSpace()
{
	static GuestAddressSpace addressSpace;
	return addressSpace;
}
} // namespace br::runtime

namespace guest {

uint8_t* base = nullptr;

bool init()
{
	std::string error;
	auto& addressSpace = br::runtime::sharedGuestAddressSpace();
	if (!addressSpace.reserve(error)) return false;
	base = addressSpace.base();
	return true;
}

bool commit(uint32_t address, uint32_t size)
{
	if (!base) return false;
	std::string error;
	return br::runtime::sharedGuestAddressSpace().commitRange(address, size, error);
}

bool decommit(uint32_t address, uint32_t size)
{
	if (!base) return false;
	std::string error;
	return br::runtime::sharedGuestAddressSpace().decommitRange(address, size, error);
}

} // namespace guest

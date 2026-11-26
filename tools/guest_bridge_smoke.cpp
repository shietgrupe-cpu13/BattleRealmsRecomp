#include "ra3_guest_bridge.h"

#include "guest.h"

#include <cstdint>
#include <cstdio>

int main()
{
	if (!guest::init()) {
		std::fprintf(stderr, "guest::init failed\n");
		return 1;
	}
	auto& addressSpace = br::runtime::sharedGuestAddressSpace();
	if (guest::base != addressSpace.base() || !guest::commit(0x12345000u, 0x1000u)) {
		std::fprintf(stderr, "guest bridge did not expose/commit the loader address space\n");
		return 2;
	}
	guest::store<uint32_t>(0x12345010u, 0xBA771E5u);
	const uint32_t actual = guest::load<uint32_t>(0x12345010u);
	if (actual != 0xBA771E5u || addressSpace.host(0x12345010u) != guest::base + 0x12345010u) {
		std::fprintf(stderr, "guest bridge load/store mismatch: 0x%08x\n", actual);
		return 3;
	}
	std::printf("guest bridge ok: base=%p logical=0x12345010 value=0x%08x\n",
		static_cast<void*>(guest::base), actual);
	return 0;
}

#include "guest_write_watch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
void exerciseWatch(CPU* c)
{
	uint32_t eax = 0x55667788u;
	uint32_t ecx = 0x006de2f0u;
	uint32_t edx = 0u;
	uint32_t ebx = 0u;
	uint32_t esp = 0x7ff0f000u;
	uint32_t ebp = 0x7ff0f100u;
	uint32_t esi = 0x006de2f0u;
	uint32_t edi = 0u;
	S32(0x006de340u, eax);
	S16(0x006de352u, 0xa1b2u);
	S32(0x006de360u, 0x12345678u); // Uses the opt-in arbitrary-address watcher.
	S8(0x006de358u, 0x5au); // Outside the two watched fields; no watch record expected.
}
}

int main()
{
#if defined(_WIN32)
	if (_putenv_s("BR_TRACE_GAME_STATE_WRITES", "1") != 0) return 1;
	if (_putenv_s("BR_TRACE_GUEST_WRITE_ADDRESS", "0x006de360") != 0) return 1;
#else
	if (setenv("BR_TRACE_GAME_STATE_WRITES", "1", 1) != 0) return 1;
	if (setenv("BR_TRACE_GUEST_WRITE_ADDRESS", "0x006de360", 1) != 0) return 1;
#endif
	if (!guest::init()) return 2;
	if (!guest::commit(0x006de000u, 0x1000u)) return 3;

	guest::store<uint32_t>(0x006de340u, 0xaabbccddu);
	guest::store<uint32_t>(0x006de350u, 0x11223344u);
	CPU cpu{};
	exerciseWatch(&cpu);

	if (guest::load<uint32_t>(0x006de340u) != 0x55667788u) return 4;
	if (guest::load<uint32_t>(0x006de350u) != 0xa1b23344u) return 5;
	if (guest::load<uint8_t>(0x006de358u) != 0x5au) return 6;
	if (guest::load<uint32_t>(0x006de360u) != 0x12345678u) return 7;
	std::fprintf(stderr, "[br-state-write-test] PASS watched stores preserve guest memory values\n");
	return 0;
}

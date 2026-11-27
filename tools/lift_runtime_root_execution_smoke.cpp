#include "lifted_decl.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::array<uint8_t, 0x10000> guestMemory{};
}

namespace guest {
uint8_t* const base = guestMemory.data();
}

uint32_t g_eflags_hi = 0;
uint32_t ra3_stub_tolower = 0;

void ra3_hook_after(CPU*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
bool ra3_gate(CPU*, uint32_t) { return false; }
void call_target(CPU*, uint32_t) {}
void call_target_ic(CPU*, uint32_t, std::atomic<uint64_t>*) {}
[[noreturn]] void unsupported(CPU*, uint32_t, const char*) { std::abort(); }

int main()
{
	CPU cpu{};
	constexpr uint32_t source = 0x1000u;
	constexpr uint32_t destination = 0x1100u;
	constexpr uint32_t entryStack = 0x2000u;
	constexpr uint32_t frame = 0x2100u;
	constexpr uint32_t previousFrame = 0x2200u;
	constexpr uint32_t returnAddress = 0x12345678u;
	constexpr uint32_t argument = 0x89abcdefu;
	constexpr uint32_t savedEsi = 0x87654321u;
	constexpr uint32_t savedEdi = 0x76543210u;

	guest::store<uint8_t>(source, 0xa5u);
	guest::store<uint8_t>(destination, 0x5au);
	guest::store<uint32_t>(entryStack, savedEsi);
	guest::store<uint32_t>(entryStack + 4u, savedEdi);
	guest::store<uint32_t>(frame, previousFrame);
	guest::store<uint32_t>(frame + 4u, returnAddress);
	guest::store<uint32_t>(frame + 8u, argument);
	cpu.r[ESP] = entryStack;
	cpu.r[ECX] = 0x11111111u;
	cpu.r[EDX] = 0x22222222u;
	cpu.r[EBX] = 0x33333333u;
	cpu.r[EBP] = frame;
	cpu.r[ESI] = source;
	cpu.r[EDI] = destination;
	cpu.r[EAX] = 0xfeedfaceu;
	cpu.t_cf = 1;
	cpu.t_pf = 1;
	cpu.t_af = 1;
	cpu.t_zf = 1;
	cpu.t_sf = 1;
	cpu.t_of = 1;
	cpu.df = 1;
	const auto memoryBeforeCall = guestMemory;
	f_006a13a0(&cpu);

	if (guest::load<uint8_t>(destination) != 0xa5u || cpu.r[EAX] != argument ||
		cpu.r[ECX] != 0x11111111u || cpu.r[EDX] != 0x22222222u || cpu.r[EBX] != 0x33333333u ||
		cpu.r[ESI] != savedEsi || cpu.r[EDI] != savedEdi ||
		cpu.r[EBP] != previousFrame || cpu.r[ESP] != frame + 8u ||
		cpu.t_cf != 1 || cpu.t_pf != 1 || cpu.t_af != 1 || cpu.t_zf != 1 || cpu.t_sf != 1 ||
		cpu.t_of != 1 || cpu.df != 1) {
		std::fprintf(stderr,
			"0x006A13A0 execution mismatch: dst=%02x eax=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
			guest::load<uint8_t>(destination), cpu.r[EAX], cpu.r[ESI], cpu.r[EDI],
			cpu.r[EBP], cpu.r[ESP]);
		return 1;
	}
	std::printf("{\"registers\":{\"eax\":%u,\"ecx\":%u,\"edx\":%u,\"ebx\":%u,\"esp\":%u,\"ebp\":%u,\"esi\":%u,\"edi\":%u},"
		"\"flags\":{\"cf\":%u,\"pf\":%u,\"af\":%u,\"zf\":%u,\"sf\":%u,\"of\":%u,\"df\":%u},"
		"\"eip_after_return\":%u,\"memory_writes\":[",
		cpu.r[EAX], cpu.r[ECX], cpu.r[EDX], cpu.r[EBX], cpu.r[ESP], cpu.r[EBP], cpu.r[ESI], cpu.r[EDI],
		cpu.t_cf, cpu.t_pf, cpu.t_af, cpu.t_zf, cpu.t_sf, cpu.t_of, cpu.df,
		guest::load<uint32_t>(frame + 4u));
	bool firstWrite = true;
	for (uint32_t address = 0; address < guestMemory.size(); ++address) {
		if (guestMemory[address] == memoryBeforeCall[address]) continue;
		std::printf("%s{\"address\":%u,\"before\":%u,\"after\":%u}", firstWrite ? "" : ",",
			address, memoryBeforeCall[address], guestMemory[address]);
		firstWrite = false;
	}
	std::puts("]}");
	return 0;
}

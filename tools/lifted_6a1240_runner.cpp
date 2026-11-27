#include "lifted_decl.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
std::array<uint8_t, 0x01000000> guestMemory{};
std::array<uint8_t, 0x01000000> beforeMemory{};
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

namespace {
uint16_t read16(const std::vector<uint8_t>& file, size_t at)
{
	if (at + 2u > file.size()) std::abort();
	return static_cast<uint16_t>(file[at] | (static_cast<uint16_t>(file[at + 1u]) << 8));
}

uint32_t read32(const std::vector<uint8_t>& file, size_t at)
{
	if (at + 4u > file.size()) std::abort();
	return static_cast<uint32_t>(file[at]) | (static_cast<uint32_t>(file[at + 1u]) << 8) |
		(static_cast<uint32_t>(file[at + 2u]) << 16) | (static_cast<uint32_t>(file[at + 3u]) << 24);
}

bool map_pe32(const char* path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::vector<uint8_t> file(std::istreambuf_iterator<char>(input), {});
	const size_t pe = read32(file, 0x3cu);
	if (pe + 24u > file.size() || read32(file, pe) != 0x00004550u) return false;
	const uint16_t section_count = read16(file, pe + 6u);
	const uint16_t optional_size = read16(file, pe + 20u);
	const size_t opt = pe + 24u;
	if (read16(file, opt) != 0x10bu) return false;
	const uint32_t image_base = read32(file, opt + 28u);
	const uint32_t image_size = read32(file, opt + 56u);
	const uint32_t headers_size = read32(file, opt + 60u);
	if (image_base >= guestMemory.size() || image_size > guestMemory.size() - image_base) return false;
	if (headers_size > file.size() || headers_size > guestMemory.size() - image_base) return false;
	std::copy_n(file.begin(), headers_size, guestMemory.begin() + image_base);
	const size_t sections = opt + optional_size;
	for (uint16_t i = 0; i < section_count; ++i) {
		const size_t row = sections + static_cast<size_t>(i) * 40u;
		const uint32_t rva = read32(file, row + 12u);
		const uint32_t raw_size = read32(file, row + 16u);
		const uint32_t raw_offset = read32(file, row + 20u);
		const uint64_t destination = static_cast<uint64_t>(image_base) + rva;
		if (raw_offset > file.size() || raw_size > file.size() - raw_offset ||
			destination > guestMemory.size() || raw_size > guestMemory.size() - destination) return false;
		std::copy_n(file.begin() + raw_offset, raw_size,
			guestMemory.begin() + static_cast<std::ptrdiff_t>(destination));
	}
	return true;
}
}

int main(int argc, char** argv)
{
	if (argc != 3) return 2;
	const unsigned count = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0));
	if (count < 32 || count > 64) return 2;
	if (!map_pe32(argv[1])) return 3;
	constexpr uint32_t source = 0x1000u;
	constexpr uint32_t destination = 0x1100u;
	constexpr uint32_t stack = 0x2000u;
	constexpr uint32_t returnAddress = 0x12345678u;
	for (unsigned i = 0; i < 64; ++i) {
		guest::store<uint8_t>(source + i, static_cast<uint8_t>(i * 7u + 19u));
		guest::store<uint8_t>(destination + i, 0xccu);
	}
	guest::store<uint32_t>(stack, returnAddress);
	guest::store<uint32_t>(stack + 4u, destination);
	guest::store<uint32_t>(stack + 8u, source);
	guest::store<uint32_t>(stack + 12u, count);

	CPU cpu{};
	cpu.r[EAX] = 0xfeedfaceu;
	cpu.r[ECX] = 0x11111111u;
	cpu.r[EDX] = 0x22222222u;
	cpu.r[EBX] = 0x33333333u;
	cpu.r[ESP] = stack;
	cpu.r[EBP] = 0x44444444u;
	cpu.r[ESI] = 0x87654321u;
	cpu.r[EDI] = 0x76543210u;
	cpu.t_cf = cpu.t_pf = cpu.t_af = cpu.t_zf = cpu.t_sf = cpu.t_of = 1;
	beforeMemory = guestMemory;
	f_006a1240(&cpu);

	std::printf("{\"registers\":{\"eax\":%u,\"ecx\":%u,\"edx\":%u,\"ebx\":%u,\"esp\":%u,\"ebp\":%u,\"esi\":%u,\"edi\":%u},"
		"\"flags\":{\"cf\":%u,\"pf\":%u,\"af\":%u,\"zf\":%u,\"sf\":%u,\"of\":%u,\"df\":%u},"
		"\"eip_after_return\":%u,\"memory_writes\":[",
		cpu.r[EAX], cpu.r[ECX], cpu.r[EDX], cpu.r[EBX], cpu.r[ESP], cpu.r[EBP], cpu.r[ESI], cpu.r[EDI],
		cpu.t_cf, cpu.t_pf, cpu.t_af, cpu.t_zf, cpu.t_sf, cpu.t_of, cpu.df,
		guest::load<uint32_t>(stack));
	bool first = true;
	for (uint32_t address = 0; address < guestMemory.size(); ++address) {
		if (beforeMemory[address] == guestMemory[address]) continue;
		std::printf("%s{\"address\":%u,\"before\":%u,\"after\":%u}", first ? "" : ",",
			address, beforeMemory[address], guestMemory[address]);
		first = false;
	}
	std::puts("]}");
	return 0;
}

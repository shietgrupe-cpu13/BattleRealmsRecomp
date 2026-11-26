#include "game_runtime.h"
#include "br_x87.h"

#include "cpu.h"
#include "ra3_guest_bridge.h"
#include "win32_startup.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>

extern const unsigned lifted_count;

namespace br::runtime {
namespace {
constexpr uint32_t kMainImageBase = 0x00400000u;
constexpr uint32_t kStartupStackTop = 0x7ff10000u;
constexpr uint32_t kDefaultStackReserve = 0x00100000u;
constexpr uint32_t kStartupTebBase = 0x7fff0000u;
constexpr uint32_t kStartupTebSize = 0x00002000u;
constexpr uint32_t kStartupPebBase = kStartupTebBase + 0x1000u;
constexpr uint32_t kStartupCommandLineBase = 0x7ffe0000u;
constexpr char kStartupCommandLine[] = "Battle_Realms_F.exe";
constexpr char kExpectedExecutableSha256[] =
	"bc0e026e906faab266a58d2f02d5015f4a21937c6a0669d934b53d21836701f0";

constexpr std::array<uint32_t, 64> kSha256K = {{
	0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
	0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
	0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
	0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
	0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
	0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
	0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
	0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
}};

uint32_t rotateRight(uint32_t value, unsigned count)
{
	return (value >> count) | (value << (32u - count));
}

std::string sha256(const std::vector<uint8_t>& input)
{
	std::array<uint32_t, 8> state = {{
		0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
		0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
	}};
	const uint64_t bitLength = uint64_t(input.size()) * 8u;
	const size_t paddedSize = ((input.size() + 9u + 63u) / 64u) * 64u;
	std::vector<uint8_t> padded(paddedSize, 0u);
	for (size_t i = 0; i < input.size(); ++i) padded[i] = input[i];
	padded[input.size()] = 0x80u;
	for (unsigned i = 0; i < 8; ++i)
		padded[paddedSize - 1u - i] = static_cast<uint8_t>(bitLength >> (8u * i));

	for (size_t offset = 0; offset < padded.size(); offset += 64u) {
		std::array<uint32_t, 64> words{};
		for (unsigned i = 0; i < 16; ++i) {
			const size_t p = offset + size_t(i) * 4u;
			words[i] = (uint32_t(padded[p]) << 24) | (uint32_t(padded[p + 1]) << 16) |
				(uint32_t(padded[p + 2]) << 8) | uint32_t(padded[p + 3]);
		}
		for (unsigned i = 16; i < 64; ++i) {
			const uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
			const uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
			words[i] = words[i - 16] + s0 + words[i - 7] + s1;
		}
		auto [a0,b0,c0,d0,e0,f0,g0,h0] = state;
		uint32_t a=a0,b=b0,c=c0,d=d0,e=e0,f=f0,g=g0,h=h0;
		for (unsigned i = 0; i < 64; ++i) {
			const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
			const uint32_t choose = (e & f) ^ (~e & g);
			const uint32_t t1 = h + s1 + choose + kSha256K[i] + words[i];
			const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
			const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = s0 + majority;
			h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
		}
		state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
		state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
	}
	std::ostringstream result;
	result << std::hex << std::setfill('0');
	for (uint32_t word : state) result << std::setw(8) << word;
	return result.str();
}

bool hasLiftedEntry(uint32_t address)
{
	unsigned lo = 0;
	unsigned hi = lifted_count;
	while (lo < hi) {
		const unsigned mid = lo + (hi - lo) / 2u;
		if (lifted_table[mid].addr < address) lo = mid + 1u;
		else hi = mid;
	}
	return lo < lifted_count && lifted_table[lo].addr == address && lifted_table[lo].fn;
}

uint32_t readGuest32(const GuestAddressSpace& guest, uint32_t address)
{
	const uint8_t* p = guest.host(address, 4);
	if (!p) return 0;
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
} // namespace

bool MainExecutableRuntime::initialize(const std::vector<uint8_t>& file, GuestAddressSpace& guest,
	const ImportDispatchRegistry& handlers, std::string& error)
{
	if (initialized_) return true;
	if (attempted_) { error = "main executable initialization already failed"; return false; }
	if (sha256(file) != kExpectedExecutableSha256) {
		error = "selected Battle_Realms_F.exe SHA-256 does not match the lifted source executable";
		return false;
	}
	if (!pe32::load(file, kMainImageBase, image_, error)) return false;
	if (!hasLiftedEntry(image_.entryAddress())) {
		error = "generated table does not contain this executable entrypoint";
		return false;
	}
	if (&guest != &sharedGuestAddressSpace()) {
		error = "main executable must use the shared guest address space used by lifted functions";
		return false;
	}
	if (!::guest::base && !::guest::init()) {
		error = "cannot initialize the shared guest address space";
		return false;
	}
	if (!guest.reserved() && !guest.reserve(error)) return false;
	attempted_ = true;
	if (!guest.mapImage(image_, error)) return false;
	if (!imports_.bindImageImports(image_, image_, "Battle_Realms_F.exe", guest, stats_, error)) return false;

	constexpr std::array<const char*, 4> entrypointImports = {{
		"GetVersion", "GetCommandLineA", "GetStartupInfoA", "GetModuleHandleA"
	}};
	for (const char* requiredName : entrypointImports) {
		bool verified = false;
		for (const auto& import : image_.imports) {
			if (import.library != "KERNEL32.dll" || import.name != requiredName) continue;
			const auto* stub = imports_.lookup(readGuest32(guest, image_.loadBase + import.iatRva));
			verified = stub && handlers.find(*stub);
			break;
		}
		if (!verified) {
			error = std::string("entrypoint import KERNEL32!") + requiredName +
				" is missing or has no registered handler";
			return false;
		}
	}
	configureLiftDispatch(imports_, &handlers);
	initialized_ = true;
	return true;
}

bool MainExecutableRuntime::prepareInitialCpu(CPU& cpu, GuestAddressSpace& addressSpace,
	std::string& error)
{
	if (!initialized_) { error = "main executable must be mapped before creating its x86 CPU state"; return false; }
	if (&addressSpace != &sharedGuestAddressSpace() || !::guest::base) {
		error = "startup CPU must use the initialized shared guest address space";
		return false;
	}
	const uint64_t pageSize = addressSpace.hostPageSize();
	const uint32_t requestedReserve = image_.stackReserveSize ?
		image_.stackReserveSize : kDefaultStackReserve;
	if (!pageSize || pageSize > 0x10000u || requestedReserve > 0x10000000u) {
		error = "PE startup stack reserve or host page size is invalid";
		return false;
	}
	const uint64_t reserveSize64 = (uint64_t(requestedReserve) + pageSize - 1u) & ~(pageSize - 1u);
	if (!reserveSize64 || reserveSize64 >= kStartupStackTop || reserveSize64 > 0xffffffffu) {
		error = "PE startup stack reserve does not fit the guest stack window";
		return false;
	}
	const uint32_t stackReserve = static_cast<uint32_t>(reserveSize64);
	const uint32_t stackLimit = kStartupStackTop - stackReserve;
	// Commit the PE-declared reserve up front. This gives the lifted process the
	// same usable stack address range as Windows without depending on a host
	// guard-page handler, which differs between the Windows test runner and Android.
	if (!addressSpace.commitRange(stackLimit, stackReserve, error) ||
		!addressSpace.commitRange(kStartupTebBase, kStartupTebSize, error) ||
		!addressSpace.commitRange(kStartupCommandLineBase, 0x1000u, error)) return false;

	cpu = CPU{};
	br_x87_reset(&cpu);
	cpu.r[ESP] = kStartupStackTop - 4u;
	cpu.fsbase = kStartupTebBase;
	::guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	::guest::store<uint32_t>(cpu.fsbase + 0x00u, 0xffffffffu); // NT_TIB.ExceptionList
	::guest::store<uint32_t>(cpu.fsbase + 0x04u, kStartupStackTop); // NT_TIB.StackBase
	::guest::store<uint32_t>(cpu.fsbase + 0x08u, stackLimit); // NT_TIB.StackLimit
	::guest::store<uint32_t>(cpu.fsbase + 0x18u, cpu.fsbase); // NT_TIB.Self
	::guest::store<uint32_t>(cpu.fsbase + 0x20u, 1u); // ClientId.UniqueProcess placeholder
	::guest::store<uint32_t>(cpu.fsbase + 0x24u, 1u); // ClientId.UniqueThread placeholder
	::guest::store<uint32_t>(cpu.fsbase + 0x30u, kStartupPebBase); // TEB.ProcessEnvironmentBlock
	::guest::store<uint32_t>(cpu.fsbase + 0x34u, 0u); // LastErrorValue
	::guest::store<uint32_t>(cpu.fsbase + 0xe0u, stackLimit); // TEB.DeallocationStack
	for (uint32_t i = 0; i < sizeof(kStartupCommandLine); ++i)
		::guest::store<uint8_t>(kStartupCommandLineBase + i,
			static_cast<uint8_t>(kStartupCommandLine[i]));
	configureWin32StartupContext(image_.loadBase, kStartupCommandLineBase, kStartupCommandLine, &imports_);
	return true;
}

} // namespace br::runtime

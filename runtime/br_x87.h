#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "fpu.h"

// The upstream CPU structure models the x87 data registers and TOP, but not
// the x87 tag word. Keep the physical-register non-empty bits alongside each
// guest CPU context so FXAM can distinguish an empty register from +0.0.
namespace br_x87_detail {
inline std::mutex& tagMutex()
{
	static std::mutex mutex;
	return mutex;
}

inline std::unordered_map<const CPU*, uint8_t>& tags()
{
	static std::unordered_map<const CPU*, uint8_t> states;
	return states;
}

inline bool occupied(const CPU* cpu, uint32_t physicalIndex)
{
	std::lock_guard<std::mutex> lock(tagMutex());
	const auto it = tags().find(cpu);
	return it != tags().end() && (it->second & (1u << (physicalIndex & 7u))) != 0;
}

inline void setOccupied(const CPU* cpu, uint32_t physicalIndex, bool value)
{
	std::lock_guard<std::mutex> lock(tagMutex());
	uint8_t& mask = tags()[cpu];
	const uint8_t bit = static_cast<uint8_t>(1u << (physicalIndex & 7u));
	if (value) mask = static_cast<uint8_t>(mask | bit);
	else mask = static_cast<uint8_t>(mask & static_cast<uint8_t>(~bit));
}

inline void reset(const CPU* cpu)
{
	std::lock_guard<std::mutex> lock(tagMutex());
	tags().erase(cpu);
}

inline void swapTags(const CPU* cpu, uint32_t first, uint32_t second)
{
	std::lock_guard<std::mutex> lock(tagMutex());
	uint8_t& mask = tags()[cpu];
	const uint8_t a = static_cast<uint8_t>(1u << (first & 7u));
	const uint8_t b = static_cast<uint8_t>(1u << (second & 7u));
	if (((mask & a) != 0) != ((mask & b) != 0)) mask ^= static_cast<uint8_t>(a | b);
}
} // namespace br_x87_detail

inline void br_x87_reset(CPU* cpu)
{
	br_x87_detail::reset(cpu);
}

inline void br_fpush(CPU* cpu, double value)
{
	cpu->top = (cpu->top - 1u) & 7u;
	cpu->st[cpu->top] = value;
	br_x87_detail::setOccupied(cpu, cpu->top, true);
}

inline double br_fpop(CPU* cpu)
{
	const uint32_t oldTop = cpu->top & 7u;
	const double value = cpu->st[oldTop];
	br_x87_detail::setOccupied(cpu, oldTop, false);
	cpu->top = (oldTop + 1u) & 7u;
	return value;
}

inline void br_farith(CPU* cpu, int operation, int destination, double other,
	bool reverse, bool pop)
{
	const double a = ST(cpu, destination);
	const double result = reverse ? fapply(operation, other, a) : fapply(operation, a, other);
	ST(cpu, destination) = fround(cpu, result);
	if (pop) br_fpop(cpu);
}

inline void br_fxch(CPU* cpu, uint32_t stackIndex)
{
	const uint32_t first = cpu->top & 7u;
	const uint32_t second = (first + stackIndex) & 7u;
	const double value = cpu->st[first];
	cpu->st[first] = cpu->st[second];
	cpu->st[second] = value;
	br_x87_detail::swapTags(cpu, first, second);
}

// Store the current value in the guest's 10-byte x87 extended format and pop.
// Converting a binary64 value to extended precision is exact for finite input.
inline void br_fstp_ext80(CPU* cpu, uint32_t address)
{
	const double value = ST(cpu, 0);
	const bool negative = std::signbit(value);
	uint16_t signExponent = negative ? 0x8000u : 0u;
	uint64_t significand = 0;
	if (std::isnan(value)) {
		signExponent = static_cast<uint16_t>(signExponent | 0x7fffu);
		significand = 0xc000000000000000ull;
	} else if (std::isinf(value)) {
		signExponent = static_cast<uint16_t>(signExponent | 0x7fffu);
		significand = 0x8000000000000000ull;
	} else if (value != 0.0) {
		int exponent = 0;
		const double mantissa = std::frexp(std::fabs(value), &exponent);
		signExponent = static_cast<uint16_t>(signExponent | static_cast<uint16_t>(exponent - 1 + 16383));
		const long double scaled = std::ldexp(static_cast<long double>(mantissa), 64);
		significand = static_cast<uint64_t>(scaled);
	}
	guest::store<uint64_t>(address, significand);
	guest::store<uint16_t>(address + 8u, signExponent);
	br_fpop(cpu);
}

// Emulate FXAM condition codes and TOP. fcc contains the x87 condition-code
// bits; fstatus() combines them with TOP when the guest executes FNSTSW.
inline void br_fxam(CPU* cpu)
{
	const uint32_t physicalIndex = cpu->top & 7u;
	if (!br_x87_detail::occupied(cpu, physicalIndex)) {
		cpu->fcc = 0x4100u; // empty: C3:C2:C0 = 101, C1 = 0
		return;
	}
	const double value = cpu->st[physicalIndex];
	uint16_t condition = std::signbit(value) ? 0x0200u : 0u; // C1 = sign
	if (std::isnan(value)) condition |= 0x0100u;             // C3:C2:C0 = 001
	else if (std::isinf(value)) condition |= 0x0500u;        // C3:C2:C0 = 011
	else if (value == 0.0) condition |= 0x4000u;             // C3:C2:C0 = 100
	else if (std::fpclassify(value) == FP_SUBNORMAL) condition |= 0x4400u; // 110
	else condition |= 0x0400u;                               // normal: 010
	cpu->fcc = condition;
}

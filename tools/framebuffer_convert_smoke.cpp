#include "framebuffer_convert.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
bool sawColdScaleMap = false;

uint8_t referenceChannel(uint32_t packed, uint32_t mask)
{
	uint32_t shift = 0u;
	while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
	if (shift == 32u) return 0u;
	const uint32_t range = mask >> shift;
	const uint32_t value = (packed & mask) >> shift;
	return static_cast<uint8_t>((uint64_t(value) * 255u + range / 2u) / range);
}

void referenceConvert(const std::vector<uint8_t>& source, uint32_t width,
	uint32_t height, uint32_t pitch, uint32_t sourceBytes, uint32_t redMask,
	uint32_t greenMask, uint32_t blueMask, std::vector<uint8_t>& destination,
	uint32_t destWidth, uint32_t destHeight, uint32_t stride, uint32_t offsetX,
	uint32_t offsetY, uint32_t drawWidth, uint32_t drawHeight)
{
	for (uint32_t y = 0u; y < destHeight; ++y)
		std::fill_n(destination.data() + size_t(y) * stride * 4u, size_t(destWidth) * 4u, 0u);
	for (uint32_t y = 0u; y < drawHeight; ++y) {
		const uint32_t sy = std::min(height - 1u, static_cast<uint32_t>((uint64_t(y) * height) / drawHeight));
		for (uint32_t x = 0u; x < drawWidth; ++x) {
			const uint32_t sx = std::min(width - 1u, static_cast<uint32_t>((uint64_t(x) * width) / drawWidth));
			const uint8_t* p = source.data() + size_t(sy) * pitch + size_t(sx) * sourceBytes;
			uint32_t packed = 0u;
			for (uint32_t b = 0u; b < sourceBytes; ++b) packed |= uint32_t(p[b]) << (b * 8u);
			uint8_t* out = destination.data() + size_t(y + offsetY) * stride * 4u + size_t(x + offsetX) * 4u;
			out[0] = referenceChannel(packed, redMask);
			out[1] = referenceChannel(packed, greenMask);
			out[2] = referenceChannel(packed, blueMask);
			out[3] = 255u;
		}
	}
}

bool compare(uint32_t sourceBytes, uint32_t redMask, uint32_t greenMask,
	uint32_t blueMask)
{
	constexpr uint32_t width = 37u, height = 23u, pitch = width * 4u + 13u;
	constexpr uint32_t destWidth = 59u, destHeight = 41u, stride = 63u;
	constexpr uint32_t offsetX = 4u, offsetY = 3u, drawWidth = 51u, drawHeight = 35u;
	std::vector<uint8_t> source(size_t(pitch) * height);
	for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xffu);
	std::vector<uint8_t> expected(size_t(stride) * destHeight * 4u, 0xcdu);
	auto actual = expected;
	br::runtime::FramebufferConversionTimings firstTimings{};
	referenceConvert(source, width, height, pitch, sourceBytes, redMask, greenMask,
		blueMask, expected, destWidth, destHeight, stride, offsetX, offsetY,
		drawWidth, drawHeight);
	if (!br::runtime::convertFramebufferToRgba8888(source.data(), width, height,
		pitch, sourceBytes, redMask, greenMask, blueMask, actual.data(), destWidth,
		destHeight, stride, offsetX, offsetY, drawWidth, drawHeight, &firstTimings)) return false;
	if (actual != expected) {
		const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
		std::cerr << "frame mismatch source_bytes=" << sourceBytes << " offset=" <<
			std::distance(actual.begin(), mismatch.first) << " actual=" <<
			unsigned(*mismatch.first) << " expected=" << unsigned(*mismatch.second) << '\n';
		return false;
	}
	if (!sawColdScaleMap) {
		if (!firstTimings.temporaryAllocationBytes || !firstTimings.scaleMapRebuilt) {
			std::cerr << "cold scale map reported no allocation\n";
			return false;
		}
		sawColdScaleMap = true;
	} else if (firstTimings.temporaryAllocationBytes || firstTimings.scaleMapRebuilt) {
		std::cerr << "warm scale map rebuilt or allocated bytes=" << firstTimings.temporaryAllocationBytes << '\n';
		return false;
	}
	auto profiled = expected;
	br::runtime::FramebufferConversionTimings timings{};
	if (!br::runtime::convertFramebufferToRgba8888(source.data(), width, height,
		pitch, sourceBytes, redMask, greenMask, blueMask, profiled.data(), destWidth,
		destHeight, stride, offsetX, offsetY, drawWidth, drawHeight, &timings)) return false;
	if (profiled != expected || timings.temporaryAllocationBytes != 0u ||
		timings.scaleMapRebuilt || !timings.prepareNanoseconds || !timings.pixelLoopNanoseconds) {
		std::cerr << "profiled conversion mismatch alloc=" << timings.temporaryAllocationBytes <<
			" setup_ns=" << timings.prepareNanoseconds << " map_ns=" << timings.scaleMapNanoseconds <<
			" pixels_ns=" << timings.pixelLoopNanoseconds << '\n';
		return false;
	}
	return true;
}
} // namespace

int main()
{
	if (!compare(2u, 0xf800u, 0x07e0u, 0x001fu) ||
		!compare(3u, 0x0000ffu, 0x00ff00u, 0xff0000u) ||
		!compare(4u, 0x00ff0000u, 0x0000ff00u, 0x000000ffu)) {
		std::cerr << "FAIL framebuffer conversion differs from legacy output\n";
		return 1;
	}
	std::cout << "PASS framebuffer output matches legacy RGB565, 24-bit, and 32-bit paths\n";
	return 0;
}

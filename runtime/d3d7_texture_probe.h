#pragma once

#include <cstdint>

namespace br::runtime::d3d7 {

// Cheap statistics accumulated during the existing texture conversion loop.
// They let Android logs distinguish black source texels from conversion/upload
// problems without adding a second full-texture scan.
struct TextureContentProbe {
	uint64_t pixels = 0;
	uint64_t rgbBlackPixels = 0;
	uint64_t alphaZeroPixels = 0;
	uint64_t opaqueBlackPixels = 0;
	uint32_t sourceFnv1a = 2166136261u;
	uint32_t rgbaFnv1a = 2166136261u;

	void add(uint32_t packedSource, uint32_t sourceBytes,
		uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) noexcept
	{
		for (uint32_t byte = 0; byte < sourceBytes; ++byte) {
			sourceFnv1a = (sourceFnv1a ^ uint8_t(packedSource >> (byte * 8u))) * 16777619u;
		}
		const uint8_t rgba[] = {red, green, blue, alpha};
		for (const uint8_t channel : rgba) rgbaFnv1a = (rgbaFnv1a ^ channel) * 16777619u;
		++pixels;
		if (red == 0u && green == 0u && blue == 0u) {
			++rgbBlackPixels;
			if (alpha != 0u) ++opaqueBlackPixels;
		}
		if (alpha == 0u) ++alphaZeroPixels;
	}
};

} // namespace br::runtime::d3d7

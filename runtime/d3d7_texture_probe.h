#pragma once

#include "d3d7_gpu_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace br::runtime::d3d7 {

struct TextureUvProbe {
	float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
	uint32_t sampledPixels = 0u, blackPixels = 0u, alphaZeroPixels = 0u;
	bool valid = false, withinUnitRange = false;
};

inline uint8_t textureChannel(uint32_t pixel, uint32_t mask) noexcept
{
	if (!mask) return 0u;
	const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(mask));
	const uint32_t maximum = mask >> shift;
	return maximum ? static_cast<uint8_t>((((pixel & mask) >> shift) * 255u + maximum / 2u) / maximum) : 0u;
}

inline void sampleTextureUvBlackPixels(const GpuDrawState& state, TextureUvProbe& probe) noexcept
{
	if (!probe.valid || !probe.withinUnitRange || !state.activeTextureStages) return;
	const GpuSurface& texture = state.textures[0];
	if (!texture.pixels || !texture.width || !texture.height ||
		(texture.bitsPerPixel != 16u && texture.bitsPerPixel != 32u) ||
		uint64_t(texture.pitch) < uint64_t(texture.width) * (texture.bitsPerPixel / 8u)) return;
	const int32_t border = (state.textureStageState[0][16] == 2u ||
		state.textureStageState[0][17] == 2u) ? 1 : 0;
	const int32_t x0 = std::clamp(int32_t(std::floor(probe.minU * texture.width)) - border,
		0, int32_t(texture.width) - 1);
	const int32_t y0 = std::clamp(int32_t(std::floor(probe.minV * texture.height)) - border,
		0, int32_t(texture.height) - 1);
	const int32_t x1 = std::max(x0 + 1, std::clamp(int32_t(std::ceil(probe.maxU * texture.width)) + border,
		0, int32_t(texture.width)));
	const int32_t y1 = std::max(y0 + 1, std::clamp(int32_t(std::ceil(probe.maxV * texture.height)) + border,
		0, int32_t(texture.height)));
	const uint32_t bytesPerPixel = texture.bitsPerPixel / 8u;
	// This is bounded diagnostic sampling, not renderer work. Keep uploads and
	// frame latency from scaling with the entire texture footprint (up to 65k
	// pixels for one 256x256 draw).
	constexpr int32_t kMaxSamplesPerAxis = 64;
	const int32_t stepX = std::max(1, (x1 - x0 + kMaxSamplesPerAxis - 1) / kMaxSamplesPerAxis);
	const int32_t stepY = std::max(1, (y1 - y0 + kMaxSamplesPerAxis - 1) / kMaxSamplesPerAxis);
	for (int32_t y = y0; y < y1; y += stepY) for (int32_t x = x0; x < x1; x += stepX) {
		uint32_t packed = 0u;
		std::memcpy(&packed, texture.pixels + size_t(y) * texture.pitch + size_t(x) * bytesPerPixel,
			bytesPerPixel);
		if (textureChannel(packed, texture.redMask) == 0u &&
			textureChannel(packed, texture.greenMask) == 0u &&
			textureChannel(packed, texture.blueMask) == 0u) ++probe.blackPixels;
		if (texture.alphaMask && textureChannel(packed, texture.alphaMask) == 0u) ++probe.alphaZeroPixels;
		++probe.sampledPixels;
	}
}

inline TextureUvProbe probeTextureUv(const GpuDrawState& state, const GpuVertex* vertices,
	uint32_t vertexCount, bool samplePixels = true) noexcept
{
	TextureUvProbe probe{};
	if (!vertices || !vertexCount || !state.activeTextureStages) return probe;
	const uint32_t set = state.textureStageState[0][11] & 0xffffu;
	if (set >= vertices[0].texcoord.size()) return probe;
	probe.minU = probe.minV = std::numeric_limits<float>::infinity();
	probe.maxU = probe.maxV = -std::numeric_limits<float>::infinity();
	for (uint32_t i = 0u; i < vertexCount; ++i) {
		const float u = vertices[i].texcoord[set][0], v = vertices[i].texcoord[set][1];
		if (!std::isfinite(u) || !std::isfinite(v)) return TextureUvProbe{};
		probe.minU = std::min(probe.minU, u); probe.maxU = std::max(probe.maxU, u);
		probe.minV = std::min(probe.minV, v); probe.maxV = std::max(probe.maxV, v);
	}
	probe.valid = true;
	probe.withinUnitRange = probe.minU >= 0.0f && probe.minV >= 0.0f &&
		probe.maxU <= 1.0f && probe.maxV <= 1.0f;
	if (samplePixels) sampleTextureUvBlackPixels(state, probe);
	return probe;
}

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

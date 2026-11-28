#pragma once

#include "d3d7_gpu_backend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace br::runtime::d3d7 {

inline bool sameGpuSurface(const GpuSurface& a, const GpuSurface& b)
{
	return a.handle == b.handle && a.width == b.width && a.height == b.height &&
		a.pitch == b.pitch && a.bitsPerPixel == b.bitsPerPixel &&
		a.redMask == b.redMask && a.greenMask == b.greenMask &&
		a.blueMask == b.blueMask && a.alphaMask == b.alphaMask &&
		a.cpuRevision == b.cpuRevision && a.pixels == b.pixels && a.byteCount == b.byteCount;
}

// Only states consumed by flushPending()/fragmentShader() may split a queued
// GLES batch. Guest-only state is already reflected in transformed vertices.
inline bool sameGpuBatchState(const GpuDrawState& a, const GpuDrawState& b)
{
	if (!sameGpuSurface(a.target, b.target) || !sameGpuSurface(a.depth, b.depth) ||
		a.activeTextureStages != b.activeTextureStages ||
		a.viewportX != b.viewportX || a.viewportY != b.viewportY ||
		a.viewportWidth != b.viewportWidth || a.viewportHeight != b.viewportHeight)
		return false;

	constexpr uint32_t renderStateIndices[] = {
		7u, 14u, 15u, 19u, 20u, 22u, 23u, 24u, 25u, 27u, 28u, 29u, 34u, 140u
	};
	for (uint32_t index : renderStateIndices)
		if (a.renderState[index] != b.renderState[index]) return false;

	constexpr uint32_t textureStageStateIndices[] = {1u, 2u, 3u, 4u, 5u, 6u, 11u, 13u, 14u, 16u, 17u};
	for (uint32_t stage = 0; stage < a.activeTextureStages; ++stage) {
		if (!sameGpuSurface(a.textures[stage], b.textures[stage])) return false;
		for (uint32_t index : textureStageStateIndices)
			if (a.textureStageState[stage][index] != b.textureStageState[stage][index]) return false;
	}
	return true;
}

struct GpuFragmentShaderKey {
	// Active-stage count, five shader-level render states, then the seven
	// texture-stage values that change generated fragment-shader source.
	std::array<uint32_t, 62> values{};
	bool operator==(const GpuFragmentShaderKey& other) const { return values == other.values; }
};

inline GpuFragmentShaderKey makeGpuFragmentShaderKey(const GpuDrawState& state)
{
	GpuFragmentShaderKey key{};
	size_t out = 0;
	key.values[out++] = state.activeTextureStages;
	for (uint32_t index : {29u, 28u, 140u, 15u, 25u}) key.values[out++] = state.renderState[index];
	constexpr uint32_t textureStateIndices[] = {1u, 2u, 3u, 4u, 5u, 6u, 11u};
	for (uint32_t stage = 0; stage < 8u; ++stage)
		for (uint32_t index : textureStateIndices)
			key.values[out++] = state.textureStageState[stage][index];
	return key;
}

struct GpuFragmentShaderKeyHash {
	size_t operator()(const GpuFragmentShaderKey& key) const
	{
		uint64_t hash = 1469598103934665603ull;
		for (uint32_t value : key.values) {
			hash ^= uint64_t(value);
			hash *= 1099511628211ull;
		}
		return static_cast<size_t>(hash);
	}
};

} // namespace br::runtime::d3d7

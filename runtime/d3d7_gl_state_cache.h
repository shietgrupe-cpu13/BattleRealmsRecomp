#pragma once

#include <array>
#include <cstdint>

namespace br::runtime::d3d7 {

// Tracks only values owned by this EGL context. Callers invalidate a field
// whenever a helper/readback path changes the corresponding GL state.
template <typename T>
struct CachedGlValue {
	T value{};
	bool valid = false;

	bool update(T next) noexcept
	{
		if (valid && value == next) return false;
		value = next;
		valid = true;
		return true;
	}

	void invalidate() noexcept { valid = false; }
};

struct GlStateCache {
	CachedGlValue<uint32_t> framebuffer;
	CachedGlValue<std::array<int32_t, 4>> viewport;
	CachedGlValue<uint32_t> program;
	CachedGlValue<uint32_t> activeTexture;
	CachedGlValue<uint32_t> vertexArray;
	CachedGlValue<uint32_t> arrayBuffer;
	CachedGlValue<bool> depthTest;
	CachedGlValue<uint32_t> depthFunction;
	CachedGlValue<bool> depthWrite;
	CachedGlValue<bool> blend;
	CachedGlValue<std::array<uint32_t, 2>> blendFunction;
	CachedGlValue<bool> cull;
	CachedGlValue<uint32_t> cullFace;
	CachedGlValue<uint32_t> frontFace;
	std::array<CachedGlValue<uint32_t>, 8> textures{};
	std::array<CachedGlValue<uint32_t>, 8> samplers{};
	uint64_t skippedCalls = 0u;

	template <typename T>
	bool apply(CachedGlValue<T>& state, T value) noexcept
	{
		if (state.update(value)) return true;
		++skippedCalls;
		return false;
	}

	void invalidateTextureBindings() noexcept
	{
		for (auto& texture : textures) texture.invalidate();
	}

	void invalidateBindings() noexcept
	{
		framebuffer.invalidate();
		activeTexture.invalidate();
		vertexArray.invalidate();
		arrayBuffer.invalidate();
		invalidateTextureBindings();
		for (auto& sampler : samplers) sampler.invalidate();
	}

	void invalidateDrawState() noexcept
	{
		invalidateBindings();
		viewport.invalidate();
		program.invalidate();
		depthTest.invalidate();
		depthFunction.invalidate();
		depthWrite.invalidate();
		blend.invalidate();
		blendFunction.invalidate();
		cull.invalidate();
		cullFace.invalidate();
		frontFace.invalidate();
	}
};

} // namespace br::runtime::d3d7

#pragma once

#include <array>
#include <cstdint>

namespace br::runtime::d3d7 {

// A platform renderer receives already transformed and clipped vertices from
// the D3D7 compatibility layer. Keeping this packet independent of GLES lets
// the Windows software renderer remain the reference implementation.
struct GpuVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float reciprocalW = 1.0f;
	std::array<float, 4> diffuse{{1.0f, 1.0f, 1.0f, 1.0f}};
	std::array<float, 4> specular{};
	std::array<std::array<float, 2>, 8> texcoord{};
};

struct GpuSurface {
	uint32_t handle = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pitch = 0;
	uint32_t bitsPerPixel = 0;
	uint32_t redMask = 0;
	uint32_t greenMask = 0;
	uint32_t blueMask = 0;
	uint32_t alphaMask = 0;
	uint64_t cpuRevision = 0;
	uint8_t* pixels = nullptr;
	uint32_t byteCount = 0;
};

struct GpuDrawState {
	GpuSurface target{};
	GpuSurface depth{};
	std::array<GpuSurface, 8> textures{};
	uint32_t guestCaller = 0;
	uint32_t guestFvf = 0;
	std::array<uint32_t, 256> renderState{};
	std::array<std::array<uint32_t, 32>, 8> textureStageState{};
	uint32_t activeTextureStages = 0;
	uint32_t viewportX = 0;
	uint32_t viewportY = 0;
	uint32_t viewportWidth = 0;
	uint32_t viewportHeight = 0;
};

// The Android implementation owns its EGL context on the calling guest
// thread. It returns false before changing the guest surfaces if the draw is
// unsupported, allowing the software renderer to execute the same draw.
bool renderGpuTriangles(const GpuDrawState& state, const GpuVertex* vertices,
	uint32_t vertexCount) noexcept;
uint64_t consumeGpuBackendBatchSubmissions() noexcept;
uint64_t consumeGpuBackendSubmitNanoseconds() noexcept;
uint64_t consumeGpuBackendSkippedStateCalls() noexcept;
bool synchronizeGpuSurface(const GpuSurface& surface) noexcept;
void markGpuSurfaceCpuWrite(uint32_t handle) noexcept;
void invalidateGpuSurface(uint32_t handle) noexcept;
void resetGpuRenderer() noexcept;

} // namespace br::runtime::d3d7

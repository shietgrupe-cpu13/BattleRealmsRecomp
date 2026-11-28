#pragma once

#include "lift_dispatch.h"

#include <cstdint>
#include <string>

namespace br::runtime {

bool registerWin32StartupHandlers(ImportDispatchRegistry& handlers);
void configureWin32StartupContext(uint32_t mainModuleBase, uint32_t commandLineAddress,
	const std::string& mainModuleName, ImportBindingTable* imports = nullptr);
void configureWin32StartupDisplayMetrics(uint32_t width, uint32_t height);
void configureWin32StartupLocale(uint32_t userDefaultLcid, uint32_t ansiCodePage);
using Win32FramePresenter = void (*)(const uint8_t* pixels, uint32_t width, uint32_t height,
	uint32_t pitch, uint32_t bitsPerPixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask);
void configureWin32FramePresenter(Win32FramePresenter presenter);
struct Win32FrameRenderStats {
	uint64_t d3dDrawCalls = 0;
	uint64_t d3dRasterNanoseconds = 0;
	uint64_t directDrawFlips = 0;
	uint64_t gpuDrawCalls = 0;
	uint64_t softwareFallbackDrawCalls = 0;
	uint64_t gpuTriangles = 0;
	uint64_t softwareFallbackTriangles = 0;
	uint64_t gpuBackendBatches = 0;
	uint64_t d3dSetupNanoseconds = 0;
	uint64_t d3dGeometryNanoseconds = 0;
	uint64_t d3dGpuSubmitNanoseconds = 0;
	uint64_t d3dFallbackNanoseconds = 0;
	uint64_t d3dSyncReadbackNanoseconds = 0;
	uint64_t gpuBackendSubmitNanoseconds = 0;
	uint64_t gpuStateCallsSkipped = 0;
};
Win32FrameRenderStats consumeWin32FrameRenderStats();

// Mixes active guest DirectSound PCM buffers into host signed 16-bit PCM. The
// Android AAudio callback uses this without calling back into guest code.
struct Win32AudioMixStats {
	uint32_t activeBuffers = 0;
	uint64_t clippedSamples = 0;
	uint64_t silentSamples = 0;
	uint64_t sumSquaredSamples = 0;
	uint32_t peakAbsoluteSample = 0;
};
void renderWin32DirectSoundPcm(int16_t* output, uint32_t frames,
	uint32_t sampleRate, uint16_t channels, Win32AudioMixStats* stats = nullptr);

// Native host input bridge. Coordinates passed to postGuestMouseMessage are in
// guest client pixels; the event is delivered both to the guest window queue
// and to DirectInput state.
uint32_t activeGuestWindowHandle();
uint64_t guestPresentationCount();
uint64_t synthesizedPaintMessageCount();
void activeGuestWindowSize(uint32_t& width, uint32_t& height);
void postGuestKeyMessage(uint32_t virtualKey, bool down);
void postGuestMouseMessage(uint32_t message, int32_t x, int32_t y, uint32_t wParam);
void postGuestQuitMessage(uint32_t exitCode);

} // namespace br::runtime

#include <android/log.h>
#include <android/input.h>
#include <android/native_window.h>
#include <aaudio/AAudio.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include "win32_base.h"
#include "win32_memory.h"
#include "win32_sync.h"
#include "win32_file.h"
#include "win32_startup.h"
#include "game_runtime.h"
#include "ra3_guest_bridge.h"
#include "framebuffer_convert.h"
#include "android_diagnostics.h"
#include "android_touch_gestures.h"
#include "android_audio_monitor.h"
#if defined(BR_ANDROID_GUEST_PROFILE)
#include "android_guest_profiler.h"
#endif

#include <cerrno>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <exception>
#include <mutex>
#include <malloc.h>
#include <thread>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

extern const unsigned lifted_count;

namespace {
br::runtime::ImportDispatchRegistry gRuntimeImportHandlers;
std::once_flag gRuntimeInit;
bool gRuntimeHandlersReady = false;
br::runtime::MainExecutableRuntime gMainExecutableRuntime;
CPU gMainCpu{};
bool gMainCpuReady = false;
std::mutex gMainExecutableMutex;
std::atomic<bool> gGuestEntrypointRunning{false};
struct GuestEntrypointRunReset {
	~GuestEntrypointRunReset()
	{
		gGuestEntrypointRunning.store(false, std::memory_order_release);
	}
};
JavaVM* gJavaVm = nullptr;
jclass gSafProviderClass = nullptr;
jmethodID gOpenReadOnlyFd = nullptr;
jmethodID gGetFileAttributes = nullptr;
jmethodID gDeleteFile = nullptr;
jmethodID gCreateDirectory = nullptr;
jmethodID gFindFiles = nullptr;
std::mutex gSafProviderMutex;
br::input::TouchGestureRecognizer gTouchGestures;
std::atomic<bool> gNativeRightClickMode{false};
uint32_t gNativeTouchButtons = 0u;
std::mutex gAndroidTouchDispatchMutex;
std::atomic<int32_t> gNativeLastGuestX{0}, gNativeLastGuestY{0};
std::mutex gAndroidWindowMutex;
ANativeWindow* gAndroidWindow = nullptr;
ANativeWindow* gAndroidGeometryWindow = nullptr;
AAudioStream* gAndroidAudioStream = nullptr;
uint32_t gAndroidGeometryWidth = 0u;
uint32_t gAndroidGeometryHeight = 0u;
std::atomic<uint64_t> gAndroidPresentedFrames{0u};
std::atomic<uint64_t> gAndroidPresentErrors{0u};
br::runtime::AndroidAudioCallbackMonitor gAudioCallbackMonitor;
std::atomic<uint64_t> gAudioFrameCount{0u};
std::atomic<uint64_t> gAudioActiveBuffers{0u}, gAudioActiveCallbackCount{0u};
std::atomic<uint64_t> gAudioClippedSamples{0u}, gAudioCallbackTimeUs{0u};
std::atomic<uint64_t> gAudioSilentSamples{0u}, gAudioSumSquaredSamples{0u}, gAudioSampleCount{0u};
std::atomic<uint64_t> gAudioCallbackOverruns{0u}, gAudioXrunQueries{0u}, gAudioXrunQueryFailures{0u};
std::atomic<uint32_t> gAudioCallbackSerial{0u};
std::atomic<int32_t> gAudioXrunCount{-1};
std::atomic<uint32_t> gAudioPeakAbsoluteSample{0u}, gAudioMaxCallbackTimeUs{0u};
std::atomic<int32_t> gAndroidAudioErrorPending{AAUDIO_OK};
std::atomic<int32_t> gAndroidAudioObservedState{-1};
std::atomic<uint64_t> gAudioLastCallbackSteadyNs{0u};
std::atomic<uint64_t> gAndroidAudioStartedSteadyNs{0u};
std::atomic<uint32_t> gAndroidAudioRecoveryCount{0u};
std::atomic<bool> gAudioCallbackEverEntered{false};
std::chrono::steady_clock::time_point gAndroidAudioLastRecovery{};
std::atomic<bool> gAndroidForeground{false};
std::atomic<bool> gAndroidMetricsReset{true};
std::chrono::steady_clock::time_point gAndroidGuestStart{};
struct AndroidFrameSample {
	uint64_t intervalUs = 0, guestCpuUs = 0, lockUs = 0, convertUs = 0, postUs = 0;
	uint64_t convertSetupUs = 0, scaleMapUs = 0, convertPixelsUs = 0;
	uint64_t converterTempAllocBytes = 0;
	uint64_t d3dDraws = 0, d3dRasterNs = 0, ddrawFlips = 0;
	uint64_t gpuDraws = 0, softwareFallbackDraws = 0, gpuTriangles = 0, softwareFallbackTriangles = 0;
	uint64_t gpuBackendBatches = 0;
	uint64_t d3dSetupNs = 0, d3dGeometryNs = 0, d3dGpuSubmitNs = 0;
	uint64_t d3dFallbackNs = 0, d3dSyncReadbackNs = 0;
	uint64_t gpuBackendSubmitNs = 0;
};
std::array<AndroidFrameSample, 512> gAndroidFrameSamples{};
size_t gAndroidFrameSampleCount = 0;
size_t gAndroidFrameSampleNext = 0;
uint64_t gAndroidFramesInSummary = 0;
std::chrono::steady_clock::time_point gAndroidLastFrameStart{};
std::chrono::steady_clock::time_point gAndroidNextSummary{};
uint64_t gAndroidLastThreadCpuNs = 0;
bool gAndroidFirstFrameLogged = false;
bool gAndroidContentChangeLogged = false;
uint32_t gAndroidInitialContentHash = 0;

uint64_t threadCpuNowNs()
{
	timespec value{};
	return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0
		? uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec) : 0u;
}

uint64_t steadyNowNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t readMemoryKb(const char* field)
{
	FILE* file = std::fopen("/proc/self/status", "r");
	if (!file) return 0u;
	char line[256]{};
	uint64_t value = 0u;
	while (std::fgets(line, sizeof(line), file)) {
		if (std::strncmp(line, field, std::strlen(field)) == 0) {
			unsigned long long parsed = 0u;
			if (std::sscanf(line + std::strlen(field), "%llu", &parsed) == 1) value = parsed;
			break;
		}
	}
	std::fclose(file);
	return value;
}

uint64_t readMallocLiveBytes()
{
	const struct mallinfo info = mallinfo();
	return info.uordblks > 0 ? static_cast<uint64_t>(info.uordblks) : 0u;
}

uint32_t sampleFramebufferHash(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp)
{
	const uint32_t bytes = (bpp + 7u) / 8u;
	uint32_t hash = 2166136261u;
	if (!pixels || !width || !height || bytes < 2u || bytes > 4u) return hash;
	for (uint32_t y = 0; y < 8u; ++y) for (uint32_t x = 0; x < 8u; ++x) {
		const uint32_t sx = (width - 1u) * x / 7u, sy = (height - 1u) * y / 7u;
		const uint8_t* pixel = pixels + size_t(sy) * pitch + size_t(sx) * bytes;
		for (uint32_t byte = 0; byte < bytes; ++byte) hash = (hash ^ pixel[byte]) * 16777619u;
	}
	return hash;
}

void emitPerformanceSummary(std::chrono::steady_clock::time_point now)
{
	const double windowSeconds = std::chrono::duration<double>(now - gAndroidNextSummary + std::chrono::seconds(3)).count();
	std::array<uint64_t, 512> intervals{};
	uint64_t intervalCount = 0u, guestCpu = 0u, lock = 0u, convert = 0u, post = 0u;
	uint64_t convertSetup = 0u, scaleMap = 0u, convertPixels = 0u, converterTempAlloc = 0u;
	uint64_t draws = 0u, rasterNs = 0u, flips = 0u;
	uint64_t gpuDraws = 0u, softwareFallbackDraws = 0u, gpuTriangles = 0u, softwareFallbackTriangles = 0u;
	uint64_t gpuBackendBatches = 0u;
	uint64_t d3dSetupNs = 0u, d3dGeometryNs = 0u, d3dGpuSubmitNs = 0u;
	uint64_t d3dFallbackNs = 0u, d3dSyncReadbackNs = 0u;
	uint64_t gpuBackendSubmitNs = 0u;
	for (size_t i = 0; i < gAndroidFrameSampleCount; ++i) {
		const auto& sample = gAndroidFrameSamples[i];
		if (sample.intervalUs) intervals[intervalCount++] = sample.intervalUs;
		guestCpu += sample.guestCpuUs; lock += sample.lockUs; convert += sample.convertUs; post += sample.postUs;
		convertSetup += sample.convertSetupUs; scaleMap += sample.scaleMapUs; convertPixels += sample.convertPixelsUs;
		converterTempAlloc += sample.converterTempAllocBytes;
		draws += sample.d3dDraws; rasterNs += sample.d3dRasterNs; flips += sample.ddrawFlips;
		gpuDraws += sample.gpuDraws; softwareFallbackDraws += sample.softwareFallbackDraws;
		gpuTriangles += sample.gpuTriangles; softwareFallbackTriangles += sample.softwareFallbackTriangles;
		gpuBackendBatches += sample.gpuBackendBatches;
		d3dSetupNs += sample.d3dSetupNs; d3dGeometryNs += sample.d3dGeometryNs;
		d3dGpuSubmitNs += sample.d3dGpuSubmitNs; d3dFallbackNs += sample.d3dFallbackNs;
		d3dSyncReadbackNs += sample.d3dSyncReadbackNs;
		gpuBackendSubmitNs += sample.gpuBackendSubmitNs;
	}
	std::sort(intervals.begin(), intervals.begin() + intervalCount);
	const auto percentile = [&](double q) -> uint64_t {
		if (!intervalCount) return 0u;
		const size_t index = std::min(intervalCount - 1u, static_cast<size_t>(q * static_cast<double>(intervalCount - 1u)));
		return intervals[index];
	};
	uint64_t intervalSum = 0u;
	for (size_t i = 0; i < intervalCount; ++i) intervalSum += intervals[i];
	const uint64_t avgFrameUs = intervalCount ? intervalSum / intervalCount : 0u;
	const double fps = windowSeconds > 0.0 ? static_cast<double>(gAndroidFramesInSummary) / windowSeconds : 0.0;
	const auto audioCallbackActivity = gAudioCallbackMonitor.consumeWindow();
	const uint64_t audioFrames = gAudioFrameCount.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioActiveBuffers = gAudioActiveBuffers.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioActiveCallbacks = gAudioActiveCallbackCount.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioClippedSamples = gAudioClippedSamples.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioCallbackTimeUs = gAudioCallbackTimeUs.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioSilentSamples = gAudioSilentSamples.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioSumSquares = gAudioSumSquaredSamples.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioSampleCount = gAudioSampleCount.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioCallbackOverruns = gAudioCallbackOverruns.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioXrunQueries = gAudioXrunQueries.exchange(0u, std::memory_order_relaxed);
	const uint64_t audioXrunQueryFailures = gAudioXrunQueryFailures.exchange(0u, std::memory_order_relaxed);
	const int32_t audioXrunCount = gAudioXrunCount.load(std::memory_order_relaxed);
	static int32_t lastAudioXrunCount = -1;
	const int32_t audioXrunDelta = audioXrunCount >= 0 && lastAudioXrunCount >= 0
		? std::max(0, audioXrunCount - lastAudioXrunCount) : -1;
	if (audioXrunCount >= 0) lastAudioXrunCount = audioXrunCount;
	const uint32_t audioPeak = gAudioPeakAbsoluteSample.exchange(0u, std::memory_order_relaxed);
	const uint32_t audioMaxCallbackUs = gAudioMaxCallbackTimeUs.exchange(0u, std::memory_order_relaxed);
	const double audioRms = audioSampleCount ? std::sqrt(static_cast<double>(audioSumSquares) /
		static_cast<double>(audioSampleCount)) : 0.0;
	const uint64_t audioSilentPermille = audioSampleCount ? audioSilentSamples * 1000u / audioSampleCount : 0u;
	const uint64_t audioClippedPermille = audioSampleCount ? audioClippedSamples * 1000u / audioSampleCount : 0u;
	const uint64_t nowSteadyNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		now.time_since_epoch()).count());
	const uint64_t lastAudioCallbackNs = gAudioLastCallbackSteadyNs.load(std::memory_order_relaxed);
	const uint64_t audioCallbackGapMs = lastAudioCallbackNs && nowSteadyNs >= lastAudioCallbackNs
		? (nowSteadyNs - lastAudioCallbackNs) / 1000000u : 0u;
	const char* audioTransportStatus = audioXrunDelta > 0 ? "aaudio_xrun_detected" :
		audioCallbackOverruns ? "callback_deadline_overrun_detected" :
		!audioCallbackActivity.completed ? "no_audio_callbacks" :
		audioActiveCallbacks ? "callback_running_with_mixer_buffers" : "callbacks_running_no_active_buffers";
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsPerf", "PERFORMANCE_WINDOW",
		"window=%.2fs frames=%llu presented_fps=%.2f frame_us_avg=%llu p95=%llu p99=%llu guest_cpu_us_per_frame=%llu d3d_draw_calls=%llu d3d_raster_ms=%.2f d3d_setup_us_per_frame=%llu d3d_vertex_geometry_us_per_frame=%llu d3d_gpu_batch_queue_us_per_frame=%llu d3d_gpu_flush_submit_cpu_us_per_frame=%llu d3d_fallback_us_per_frame=%llu d3d_sync_readback_hash_us_per_frame=%llu d3d_gpu_draw_calls=%llu d3d_gl_batches=%llu d3d_software_fallback_draw_calls=%llu d3d_gpu_triangles=%llu d3d_software_fallback_triangles=%llu ddraw_flips=%llu convert_scale_us_per_frame=%llu scale_map_us_per_frame=%llu conversion_setup_us_per_frame=%llu pixel_convert_scale_us_per_frame=%llu native_lock_us_per_frame=%llu unlock_post_us_per_frame=%llu converter_temp_alloc_bytes_per_frame=%llu audio_stream_state=%d audio_recoveries=%u audio_callback_gap_ms=%llu audio_callback_entered=%llu audio_callbacks=%llu audio_callback_inflight=%u audio_frames=%llu audio_active_callbacks=%llu audio_active_buffers_per_callback=%.2f audio_transport_status=%s audio_xrun_count=%d audio_xrun_delta=%d audio_xrun_queries=%llu audio_xrun_query_failures=%llu audio_rms=%0.1f audio_silent_permille=%llu audio_peak_abs=%u audio_clipped_samples=%llu audio_clipped_permille=%llu audio_callback_overruns=%llu audio_callback_us_avg=%llu audio_callback_us_max=%u malloc_live_kb=%llu rss_kb=%llu vmhwm_kb=%llu present_errors_total=%llu",
		windowSeconds, static_cast<unsigned long long>(gAndroidFramesInSummary), fps,
		static_cast<unsigned long long>(avgFrameUs), static_cast<unsigned long long>(percentile(0.95)),
		static_cast<unsigned long long>(percentile(0.99)),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? guestCpu / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(draws), static_cast<double>(rasterNs) / 1000000.0,
		static_cast<unsigned long long>(gAndroidFramesInSummary ? d3dSetupNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? d3dGeometryNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? d3dGpuSubmitNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? gpuBackendSubmitNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? d3dFallbackNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? d3dSyncReadbackNs / gAndroidFramesInSummary / 1000u : 0u),
		static_cast<unsigned long long>(gpuDraws), static_cast<unsigned long long>(gpuBackendBatches),
		static_cast<unsigned long long>(softwareFallbackDraws),
		static_cast<unsigned long long>(gpuTriangles), static_cast<unsigned long long>(softwareFallbackTriangles),
		static_cast<unsigned long long>(flips), static_cast<unsigned long long>(gAndroidFramesInSummary ? convert / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? scaleMap / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? convertSetup / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? convertPixels / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? lock / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? post / gAndroidFramesInSummary : 0u),
		static_cast<unsigned long long>(gAndroidFramesInSummary ? converterTempAlloc / gAndroidFramesInSummary : 0u),
		gAndroidAudioObservedState.load(std::memory_order_relaxed),
		gAndroidAudioRecoveryCount.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(audioCallbackGapMs),
		static_cast<unsigned long long>(audioCallbackActivity.entered),
		static_cast<unsigned long long>(audioCallbackActivity.completed), audioCallbackActivity.inFlight,
		static_cast<unsigned long long>(audioFrames),
		static_cast<unsigned long long>(audioActiveCallbacks),
		audioCallbackActivity.completed ? static_cast<double>(audioActiveBuffers) /
			static_cast<double>(audioCallbackActivity.completed) : 0.0,
		audioTransportStatus, audioXrunCount, audioXrunDelta,
		static_cast<unsigned long long>(audioXrunQueries), static_cast<unsigned long long>(audioXrunQueryFailures),
		audioRms, static_cast<unsigned long long>(audioSilentPermille), audioPeak,
		static_cast<unsigned long long>(audioClippedSamples), static_cast<unsigned long long>(audioClippedPermille),
		static_cast<unsigned long long>(audioCallbackOverruns),
		static_cast<unsigned long long>(audioCallbackActivity.completed
			? audioCallbackTimeUs / audioCallbackActivity.completed : 0u),
		audioMaxCallbackUs,
		static_cast<unsigned long long>(readMallocLiveBytes() / 1024u),
		static_cast<unsigned long long>(readMemoryKb("VmRSS:")),
		static_cast<unsigned long long>(readMemoryKb("VmHWM:")),
		static_cast<unsigned long long>(gAndroidPresentErrors.load(std::memory_order_relaxed)));
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsPerf", "AUDIO_SIGNAL_WINDOW",
		"frames=%llu active_callbacks=%llu active_buffers_per_callback=%.2f rms=%.1f silence_permille=%llu peak_abs=%u clipped_samples=%llu clipped_permille=%llu callback_deadline_overruns=%llu callback_us_avg=%llu callback_us_max=%u xrun_count=%d xrun_delta=%d",
		static_cast<unsigned long long>(audioFrames),
		static_cast<unsigned long long>(audioActiveCallbacks),
		audioCallbackActivity.completed ? static_cast<double>(audioActiveBuffers) /
			static_cast<double>(audioCallbackActivity.completed) : 0.0,
		audioRms, static_cast<unsigned long long>(audioSilentPermille), audioPeak,
		static_cast<unsigned long long>(audioClippedSamples),
		static_cast<unsigned long long>(audioClippedPermille),
		static_cast<unsigned long long>(audioCallbackOverruns),
		static_cast<unsigned long long>(audioCallbackActivity.completed
			? audioCallbackTimeUs / audioCallbackActivity.completed : 0u),
		audioMaxCallbackUs, audioXrunCount, audioXrunDelta);
	gAndroidFrameSampleCount = 0u;
	gAndroidFrameSampleNext = 0u;
	gAndroidFramesInSummary = 0u;
	gAndroidNextSummary = now + std::chrono::seconds(3);
}

void presentGuestFrame(const uint8_t* pixels, uint32_t width, uint32_t height,
	uint32_t pitch, uint32_t bitsPerPixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask)
{
	if (!pixels || !width || !height || !redMask || !greenMask || !blueMask) return;
	const uint32_t sourceBytes = (bitsPerPixel + 7u) / 8u;
	if (sourceBytes < 2u || sourceBytes > 4u || pitch < width * sourceBytes) return;
	ANativeWindow* window = nullptr;
	{
		std::lock_guard<std::mutex> lock(gAndroidWindowMutex);
		window = gAndroidWindow;
		if (window) ANativeWindow_acquire(window);
	}
	if (!window) return;
	const int32_t requestedWidth = ANativeWindow_getWidth(window);
	const int32_t requestedHeight = ANativeWindow_getHeight(window);
	if (requestedWidth <= 0 || requestedHeight <= 0) {
		ANativeWindow_release(window);
		return;
	}

	bool setGeometry = false;
	{
		std::lock_guard<std::mutex> lock(gAndroidWindowMutex);
		if (gAndroidGeometryWindow != window || gAndroidGeometryWidth != static_cast<uint32_t>(requestedWidth) ||
			gAndroidGeometryHeight != static_cast<uint32_t>(requestedHeight)) {
			gAndroidGeometryWindow = window;
			gAndroidGeometryWidth = static_cast<uint32_t>(requestedWidth);
			gAndroidGeometryHeight = static_cast<uint32_t>(requestedHeight);
			setGeometry = true;
		}
	}
	if (setGeometry && ANativeWindow_setBuffersGeometry(window, requestedWidth,
		requestedHeight, WINDOW_FORMAT_RGBA_8888) != 0) {
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"ANativeWindow rejected %dx%d RGBA framebuffer geometry", requestedWidth, requestedHeight);
		gAndroidPresentErrors.fetch_add(1u, std::memory_order_relaxed);
		ANativeWindow_release(window);
		return;
	}

	using PresentClock = std::chrono::steady_clock;
	const auto frameStart = PresentClock::now();
	const auto lockStart = frameStart;
	ANativeWindow_Buffer buffer{};
	if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative", "ANativeWindow_lock failed for game frame");
		gAndroidPresentErrors.fetch_add(1u, std::memory_order_relaxed);
		ANativeWindow_release(window);
		return;
	}
	const auto conversionStart = PresentClock::now();
	if (buffer.format != WINDOW_FORMAT_RGBA_8888 || buffer.width <= 0 ||
		buffer.height <= 0 || buffer.stride < buffer.width) {
		ANativeWindow_unlockAndPost(window);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"unexpected Android framebuffer format/dimensions: format=%d size=%dx%d stride=%d",
			buffer.format, buffer.width, buffer.height, buffer.stride);
		gAndroidPresentErrors.fetch_add(1u, std::memory_order_relaxed);
		ANativeWindow_release(window);
		return;
	}
	const float scale = std::min(static_cast<float>(buffer.width) / static_cast<float>(width),
		static_cast<float>(buffer.height) / static_cast<float>(height));
	const uint32_t drawWidth = std::max(1u, std::min(static_cast<uint32_t>(buffer.width),
		static_cast<uint32_t>(static_cast<float>(width) * scale)));
	const uint32_t drawHeight = std::max(1u, std::min(static_cast<uint32_t>(buffer.height),
		static_cast<uint32_t>(static_cast<float>(height) * scale)));
	const uint32_t offsetX = (static_cast<uint32_t>(buffer.width) - drawWidth) / 2u;
	const uint32_t offsetY = (static_cast<uint32_t>(buffer.height) - drawHeight) / 2u;
	auto* destination = static_cast<uint8_t*>(buffer.bits);
	br::runtime::FramebufferConversionTimings conversionTimings{};
	if (!br::runtime::convertFramebufferToRgba8888(pixels, width, height, pitch,
		sourceBytes, redMask, greenMask, blueMask, destination,
		static_cast<uint32_t>(buffer.width), static_cast<uint32_t>(buffer.height),
		static_cast<uint32_t>(buffer.stride), offsetX, offsetY, drawWidth, drawHeight, &conversionTimings)) {
		ANativeWindow_unlockAndPost(window);
		ANativeWindow_release(window);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"invalid guest framebuffer conversion request (%ux%u, %u bpp)",
			width, height, bitsPerPixel);
		gAndroidPresentErrors.fetch_add(1u, std::memory_order_relaxed);
		return;
	}
	const auto postStart = PresentClock::now();
	const int32_t postResult = ANativeWindow_unlockAndPost(window);
	const auto postEnd = PresentClock::now();
	ANativeWindow_release(window);
	if (postResult != 0) {
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"ANativeWindow_unlockAndPost failed: %d", postResult);
		gAndroidPresentErrors.fetch_add(1u, std::memory_order_relaxed);
		return;
	}
	const uint64_t count = ++gAndroidPresentedFrames;
	++gAndroidFramesInSummary;
	const auto toMicros = [](PresentClock::duration elapsed) {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
	};
	const uint64_t cpuNow = threadCpuNowNs();
	AndroidFrameSample sample{};
	if (gAndroidLastFrameStart.time_since_epoch().count())
		sample.intervalUs = toMicros(frameStart - gAndroidLastFrameStart);
	if (cpuNow && gAndroidLastThreadCpuNs && cpuNow >= gAndroidLastThreadCpuNs)
		sample.guestCpuUs = (cpuNow - gAndroidLastThreadCpuNs) / 1000u;
	gAndroidLastThreadCpuNs = cpuNow;
	gAndroidLastFrameStart = frameStart;
	sample.lockUs = toMicros(conversionStart - lockStart);
	sample.convertUs = toMicros(postStart - conversionStart);
	sample.postUs = toMicros(postEnd - postStart);
	sample.convertSetupUs = conversionTimings.prepareNanoseconds / 1000u;
	sample.scaleMapUs = conversionTimings.scaleMapNanoseconds / 1000u;
	sample.convertPixelsUs = conversionTimings.pixelLoopNanoseconds / 1000u;
	sample.converterTempAllocBytes = conversionTimings.temporaryAllocationBytes;
	const auto renderStats = br::runtime::consumeWin32FrameRenderStats();
	sample.d3dDraws = renderStats.d3dDrawCalls;
	sample.d3dRasterNs = renderStats.d3dRasterNanoseconds;
	sample.ddrawFlips = renderStats.directDrawFlips;
	sample.gpuDraws = renderStats.gpuDrawCalls;
	sample.softwareFallbackDraws = renderStats.softwareFallbackDrawCalls;
	sample.gpuTriangles = renderStats.gpuTriangles;
	sample.softwareFallbackTriangles = renderStats.softwareFallbackTriangles;
	sample.gpuBackendBatches = renderStats.gpuBackendBatches;
	sample.d3dSetupNs = renderStats.d3dSetupNanoseconds;
	sample.d3dGeometryNs = renderStats.d3dGeometryNanoseconds;
	sample.d3dGpuSubmitNs = renderStats.d3dGpuSubmitNanoseconds;
	sample.d3dFallbackNs = renderStats.d3dFallbackNanoseconds;
	sample.d3dSyncReadbackNs = renderStats.d3dSyncReadbackNanoseconds;
	sample.gpuBackendSubmitNs = renderStats.gpuBackendSubmitNanoseconds;
	gAndroidFrameSamples[gAndroidFrameSampleNext] = sample;
	gAndroidFrameSampleNext = (gAndroidFrameSampleNext + 1u) % gAndroidFrameSamples.size();
	gAndroidFrameSampleCount = std::min(gAndroidFrameSampleCount + 1u, gAndroidFrameSamples.size());
	const auto now = PresentClock::now();
	if (!gAndroidFirstFrameLogged) {
		gAndroidFirstFrameLogged = true;
		const auto elapsed = gAndroidGuestStart.time_since_epoch().count()
			? std::chrono::duration_cast<std::chrono::milliseconds>(now - gAndroidGuestStart).count() : -1;
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative", "FIRST_FRAME_PRESENTED",
			"frame=%llu guest_dimensions=%ux%u surface=%dx%d elapsed_since_guest_start_ms=%lld",
			static_cast<unsigned long long>(count), width, height, buffer.width, buffer.height,
			static_cast<long long>(elapsed));
	}
	if (!gAndroidForeground.load(std::memory_order_acquire)) return;
	if (gAndroidMetricsReset.exchange(false, std::memory_order_acq_rel)) {
		gAndroidLastFrameStart = {};
		gAndroidLastThreadCpuNs = 0u;
		gAndroidNextSummary = {};
		gAndroidFrameSampleCount = gAndroidFrameSampleNext = 0u;
		gAndroidFramesInSummary = 0u;
	}
	const uint32_t contentHash = sampleFramebufferHash(pixels, width, height, pitch, bitsPerPixel);
	if (!gAndroidContentChangeLogged) {
		if (gAndroidInitialContentHash && contentHash != gAndroidInitialContentHash) {
			gAndroidContentChangeLogged = true;
			br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative", "FRAMEBUFFER_CONTENT_CHANGED",
				"frame=%llu sample_hash=%08x", static_cast<unsigned long long>(count), contentHash);
		} else if (!gAndroidInitialContentHash) gAndroidInitialContentHash = contentHash;
	}
	if (gAndroidNextSummary.time_since_epoch().count() == 0) gAndroidNextSummary = now + std::chrono::seconds(3);
	if (now >= gAndroidNextSummary) emitPerformanceSummary(now);
}

aaudio_data_callback_result_t androidAudioDataCallback(AAudioStream* stream,
	void*, void* audioData, int32_t frames)
{
	gAudioCallbackMonitor.enter();
	gAudioLastCallbackSteadyNs.store(steadyNowNs(), std::memory_order_relaxed);
	gAudioCallbackEverEntered.store(true, std::memory_order_relaxed);
	if (frames <= 0) {
		gAudioCallbackMonitor.complete();
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
	}
	const auto callbackStart = std::chrono::steady_clock::now();
	br::runtime::Win32AudioMixStats mixStats{};
	br::runtime::renderWin32DirectSoundPcm(static_cast<int16_t*>(audioData),
		static_cast<uint32_t>(frames), static_cast<uint32_t>(AAudioStream_getSampleRate(stream)),
		static_cast<uint16_t>(AAudioStream_getChannelCount(stream)), &mixStats);
	const uint32_t callbackUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - callbackStart).count());
	gAudioFrameCount.fetch_add(static_cast<uint32_t>(frames), std::memory_order_relaxed);
	gAudioSampleCount.fetch_add(static_cast<uint64_t>(frames) * AAudioStream_getChannelCount(stream), std::memory_order_relaxed);
	gAudioActiveBuffers.fetch_add(mixStats.activeBuffers, std::memory_order_relaxed);
	if (mixStats.activeBuffers) gAudioActiveCallbackCount.fetch_add(1u, std::memory_order_relaxed);
	gAudioClippedSamples.fetch_add(mixStats.clippedSamples, std::memory_order_relaxed);
	gAudioSilentSamples.fetch_add(mixStats.silentSamples, std::memory_order_relaxed);
	gAudioSumSquaredSamples.fetch_add(mixStats.sumSquaredSamples, std::memory_order_relaxed);
	gAudioCallbackTimeUs.fetch_add(callbackUs, std::memory_order_relaxed);
	if (static_cast<uint64_t>(callbackUs) * AAudioStream_getSampleRate(stream) >
		static_cast<uint64_t>(frames) * 1000000u)
		gAudioCallbackOverruns.fetch_add(1u, std::memory_order_relaxed);
	if ((gAudioCallbackSerial.fetch_add(1u, std::memory_order_relaxed) & 127u) == 127u) {
		const int32_t xrunCount = AAudioStream_getXRunCount(stream);
		gAudioXrunQueries.fetch_add(1u, std::memory_order_relaxed);
		if (xrunCount >= 0) gAudioXrunCount.store(xrunCount, std::memory_order_relaxed);
		else gAudioXrunQueryFailures.fetch_add(1u, std::memory_order_relaxed);
	}
	uint32_t observedPeak = gAudioPeakAbsoluteSample.load(std::memory_order_relaxed);
	while (observedPeak < mixStats.peakAbsoluteSample &&
		!gAudioPeakAbsoluteSample.compare_exchange_weak(observedPeak, mixStats.peakAbsoluteSample,
			std::memory_order_relaxed)) {}
	uint32_t observedMaxUs = gAudioMaxCallbackTimeUs.load(std::memory_order_relaxed);
	while (observedMaxUs < callbackUs &&
		!gAudioMaxCallbackTimeUs.compare_exchange_weak(observedMaxUs, callbackUs,
			std::memory_order_relaxed)) {}
	gAudioCallbackMonitor.complete();
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void androidAudioErrorCallback(AAudioStream*, void*, aaudio_result_t error)
{
	gAndroidAudioErrorPending.store(error, std::memory_order_release);
}

void closeAndroidAudio();

bool startAndroidAudio()
{
	if (gAndroidAudioStream) {
		const aaudio_stream_state_t state = AAudioStream_getState(gAndroidAudioStream);
		gAndroidAudioObservedState.store(static_cast<int32_t>(state), std::memory_order_relaxed);
		if (state == AAUDIO_STREAM_STATE_STARTED) return true;
		if (state == AAUDIO_STREAM_STATE_PAUSED) {
			const aaudio_result_t restarted = AAudioStream_requestStart(gAndroidAudioStream);
			if (restarted != AAUDIO_OK) br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_WARN,
				"BattleRealmsNative", "AUDIO_STREAM_RESUME_FAILED", "%s (%d)",
				AAudio_convertResultToText(restarted), restarted);
			else {
				gAndroidAudioStartedSteadyNs.store(steadyNowNs(), std::memory_order_relaxed);
				gAudioLastCallbackSteadyNs.store(0u, std::memory_order_relaxed);
				gAudioCallbackEverEntered.store(false, std::memory_order_relaxed);
			}
			return restarted == AAUDIO_OK;
		}
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_WARN, "BattleRealmsNative",
			"AUDIO_STREAM_REOPEN_REQUIRED", "state=%d; recreating output stream", static_cast<int>(state));
		closeAndroidAudio();
	}
	AAudioStreamBuilder* builder = nullptr;
	aaudio_result_t result = AAudio_createStreamBuilder(&builder);
	if (result != AAUDIO_OK || !builder) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"AUDIO_STREAM_OPEN_FAILED", "AAudio_createStreamBuilder: %s (%d)",
			AAudio_convertResultToText(result), result);
		return false;
	}
	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(builder, 2);
	AAudioStreamBuilder_setSampleRate(builder, 48000);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
	AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SONIFICATION);
	AAudioStreamBuilder_setDataCallback(builder, androidAudioDataCallback, nullptr);
	AAudioStreamBuilder_setErrorCallback(builder, androidAudioErrorCallback, nullptr);
	result = AAudioStreamBuilder_openStream(builder, &gAndroidAudioStream);
	AAudioStreamBuilder_delete(builder);
	if (result != AAUDIO_OK || !gAndroidAudioStream) {
		gAndroidAudioStream = nullptr;
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"AUDIO_STREAM_OPEN_FAILED", "AAudioStreamBuilder_openStream: %s (%d)",
			AAudio_convertResultToText(result), result);
		return false;
	}
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"AUDIO_STREAM_READY", "format=PCM_I16 rate=%d channels=%d burst=%d",
		AAudioStream_getSampleRate(gAndroidAudioStream), AAudioStream_getChannelCount(gAndroidAudioStream),
		AAudioStream_getFramesPerBurst(gAndroidAudioStream));
	result = AAudioStream_requestStart(gAndroidAudioStream);
	if (result != AAUDIO_OK) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"AUDIO_STREAM_START_FAILED", "AAudioStream_requestStart: %s (%d)",
			AAudio_convertResultToText(result), result);
		AAudioStream_close(gAndroidAudioStream);
		gAndroidAudioStream = nullptr;
		return false;
	}
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"AUDIO_STREAM_STARTED", "DirectSound PCM mixer connected to Android output");
	gAndroidAudioObservedState.store(static_cast<int32_t>(AAUDIO_STREAM_STATE_STARTED), std::memory_order_relaxed);
	gAndroidAudioStartedSteadyNs.store(steadyNowNs(), std::memory_order_relaxed);
	gAudioLastCallbackSteadyNs.store(0u, std::memory_order_relaxed);
	gAudioCallbackEverEntered.store(false, std::memory_order_relaxed);
	return true;
}

void closeAndroidAudio()
{
	if (!gAndroidAudioStream) return;
	const aaudio_stream_state_t state = AAudioStream_getState(gAndroidAudioStream);
	if (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_PAUSED) {
		const aaudio_result_t stopped = AAudioStream_requestStop(gAndroidAudioStream);
		if (stopped != AAUDIO_OK) br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_WARN,
			"BattleRealmsNative", "AUDIO_STREAM_STOP_FAILED", "%s (%d)",
			AAudio_convertResultToText(stopped), stopped);
	}
	const aaudio_result_t result = AAudioStream_close(gAndroidAudioStream);
	gAndroidAudioStream = nullptr;
	gAndroidAudioObservedState.store(-1, std::memory_order_relaxed);
	br::runtime::androidDiagnosticLogEvent(result == AAUDIO_OK ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
		"BattleRealmsNative", "AUDIO_STREAM_CLOSED", "result=%s (%d)",
		AAudio_convertResultToText(result), result);
}

void recoverAndroidAudio(const char* reason, aaudio_result_t error = AAUDIO_OK)
{
	const auto now = std::chrono::steady_clock::now();
	if (gAndroidAudioLastRecovery.time_since_epoch().count() != 0 &&
		now - gAndroidAudioLastRecovery < std::chrono::seconds(2)) return;
	gAndroidAudioLastRecovery = now;
	const uint32_t recovery = gAndroidAudioRecoveryCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
	br::runtime::androidDiagnosticLogEvent(error == AAUDIO_OK ? ANDROID_LOG_WARN : ANDROID_LOG_ERROR,
		"BattleRealmsNative", "AUDIO_RECOVERY_BEGIN",
		"reason=%s error=%s (%d) state=%d recovery=%u",
		reason, AAudio_convertResultToText(error), error,
		gAndroidAudioObservedState.load(std::memory_order_relaxed), recovery);
	closeAndroidAudio();
	if (!gAndroidForeground.load(std::memory_order_acquire)) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
			"AUDIO_RECOVERY_DEFERRED", "app is backgrounded; stream will reopen on foreground");
		return;
	}
	const bool reopened = startAndroidAudio();
	br::runtime::androidDiagnosticLogEvent(reopened ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
		"BattleRealmsNative", reopened ? "AUDIO_RECOVERY_SUCCEEDED" : "AUDIO_RECOVERY_FAILED",
		"reason=%s recovery=%u", reason, recovery);
}

void serviceAndroidAudio()
{
	const aaudio_result_t callbackError = gAndroidAudioErrorPending.exchange(AAUDIO_OK, std::memory_order_acq_rel);
	if (callbackError != AAUDIO_OK) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"AUDIO_STREAM_ERROR", "AAudio reported %s (%d)", AAudio_convertResultToText(callbackError), callbackError);
		recoverAndroidAudio("aaudio_error_callback", callbackError);
		return;
	}
	if (!gAndroidAudioStream) return;
	const aaudio_stream_state_t state = AAudioStream_getState(gAndroidAudioStream);
	gAndroidAudioObservedState.store(static_cast<int32_t>(state), std::memory_order_relaxed);
	if (state == AAUDIO_STREAM_STATE_DISCONNECTED) {
		recoverAndroidAudio("stream_disconnected");
		return;
	}
	if (!gAndroidForeground.load(std::memory_order_acquire)) return;
	if (state == AAUDIO_STREAM_STATE_STOPPED || state == AAUDIO_STREAM_STATE_CLOSED ||
		state == AAUDIO_STREAM_STATE_CLOSING) {
		recoverAndroidAudio("unexpected_terminal_stream_state");
		return;
	}
	if (state == AAUDIO_STREAM_STATE_PAUSED) {
		startAndroidAudio();
		return;
	}
	if (state != AAUDIO_STREAM_STATE_STARTED) return;
	const uint64_t nowNs = steadyNowNs();
	const uint64_t lastCallbackNs = gAudioLastCallbackSteadyNs.load(std::memory_order_relaxed);
	const uint64_t streamStartedNs = gAndroidAudioStartedSteadyNs.load(std::memory_order_relaxed);
	const uint64_t referenceNs = lastCallbackNs ? lastCallbackNs : streamStartedNs;
	const uint64_t limitNs = lastCallbackNs || gAudioCallbackEverEntered.load(std::memory_order_relaxed)
		? 2000000000ull : 4000000000ull;
	if (referenceNs && nowNs > referenceNs && nowNs - referenceNs > limitNs)
		recoverAndroidAudio(lastCallbackNs ? "audio_callback_stalled" : "audio_callback_never_started");
}

void onAndroidAppCommand(android_app* app, int32_t command)
{
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"NATIVE_APP_COMMAND", "command=%d window=%s", command, app->window ? "present" : "none");
	if (command == APP_CMD_RESUME || command == APP_CMD_GAINED_FOCUS) {
		gAndroidForeground.store(true, std::memory_order_release);
		gAndroidMetricsReset.store(true, std::memory_order_release);
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative", "FOREGROUND_METRICS_STARTED", "command=%d", command);
		if (gAndroidAudioStream) startAndroidAudio();
	} else if (command == APP_CMD_PAUSE || command == APP_CMD_LOST_FOCUS || command == APP_CMD_TERM_WINDOW) {
		gAndroidForeground.store(false, std::memory_order_release);
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative", "FOREGROUND_METRICS_PAUSED", "command=%d", command);
		if (gAndroidAudioStream && AAudioStream_getState(gAndroidAudioStream) == AAUDIO_STREAM_STATE_STARTED) {
			const aaudio_result_t paused = AAudioStream_requestPause(gAndroidAudioStream);
			if (paused == AAUDIO_OK) gAndroidAudioObservedState.store(
				static_cast<int32_t>(AAUDIO_STREAM_STATE_PAUSED), std::memory_order_relaxed);
			if (paused != AAUDIO_OK) br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_WARN,
				"BattleRealmsNative", "AUDIO_STREAM_PAUSE_FAILED", "%s (%d)",
				AAudio_convertResultToText(paused), paused);
		}
	}
	if (command == APP_CMD_INIT_WINDOW && app->window) {
		ANativeWindow_acquire(app->window);
		ANativeWindow* previous = nullptr;
		{
			std::lock_guard<std::mutex> lock(gAndroidWindowMutex);
			previous = gAndroidWindow;
			gAndroidWindow = app->window;
			gAndroidGeometryWindow = nullptr;
			gAndroidGeometryWidth = gAndroidGeometryHeight = 0u;
		}
		if (previous) ANativeWindow_release(previous);
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
			"NATIVE_SURFACE_READY", "width=%d height=%d", ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
	} else if (command == APP_CMD_TERM_WINDOW) {
		ANativeWindow* previous = nullptr;
		{
			std::lock_guard<std::mutex> lock(gAndroidWindowMutex);
			previous = gAndroidWindow;
			gAndroidWindow = nullptr;
			gAndroidGeometryWindow = nullptr;
			gAndroidGeometryWidth = gAndroidGeometryHeight = 0u;
		}
		if (previous) ANativeWindow_release(previous);
	}
}

br::runtime::Win32FileOpenResult openSafFile(
	const char* relativePath, uint32_t access, uint32_t creation)
{
	JavaVM* vm = gJavaVm;
	if (!vm || !relativePath) return {-1, 2u};
	JNIEnv* env = nullptr;
	bool detach = false;
	const jint state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (state == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {-1, 8u};
		detach = true;
	} else if (state != JNI_OK) {
		return {-1, 8u};
	}

	jclass providerClass = nullptr;
	jmethodID openMethod = nullptr;
	{
		std::lock_guard<std::mutex> lock(gSafProviderMutex);
		providerClass = static_cast<jclass>(env->NewLocalRef(gSafProviderClass));
		openMethod = gOpenReadOnlyFd;
	}
	br::runtime::Win32FileOpenResult opened{-1, 2u};
	if (providerClass && openMethod) {
		jstring path = env->NewStringUTF(relativePath);
		if (path) {
			const jlong encoded = env->CallStaticLongMethod(
				providerClass, openMethod, path, static_cast<jint>(access), static_cast<jint>(creation));
			const uint64_t bits = static_cast<uint64_t>(encoded);
			opened.fd = static_cast<int32_t>(bits & 0xffffffffu);
			opened.lastError = static_cast<uint32_t>(bits >> 32);
			env->DeleteLocalRef(path);
		}
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
			opened = {-1, 8u};
		}
		if (providerClass) env->DeleteLocalRef(providerClass);
	}
	if (detach) vm->DetachCurrentThread();
	return opened;
}

uint32_t querySafFileAttributes(const char* relativePath)
{
	JavaVM* vm = gJavaVm;
	if (!vm || !relativePath) return 0xffffffffu;
	JNIEnv* env = nullptr;
	bool detach = false;
	const jint state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (state == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 0xffffffffu;
		detach = true;
	} else if (state != JNI_OK) {
		return 0xffffffffu;
	}
	jclass providerClass = nullptr;
	jmethodID method = nullptr;
	{
		std::lock_guard<std::mutex> lock(gSafProviderMutex);
		providerClass = static_cast<jclass>(env->NewLocalRef(gSafProviderClass));
		method = gGetFileAttributes;
	}
	uint32_t attributes = 0xffffffffu;
	if (providerClass && method) {
		jstring path = env->NewStringUTF(relativePath);
		if (path) {
			attributes = static_cast<uint32_t>(env->CallStaticIntMethod(providerClass, method, path));
			env->DeleteLocalRef(path);
		}
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
			attributes = 0xffffffffu;
		}
		env->DeleteLocalRef(providerClass);
	}
	if (detach) vm->DetachCurrentThread();
	return attributes;
}

uint32_t deleteSafFile(const char* relativePath)
{
	JavaVM* vm = gJavaVm;
	if (!vm || !relativePath) return 2u;
	JNIEnv* env = nullptr; bool detach = false;
	if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 8u; detach = true;
	}
	jclass cls; jmethodID method;
	{ std::lock_guard<std::mutex> lock(gSafProviderMutex); cls = static_cast<jclass>(env->NewLocalRef(gSafProviderClass)); method = gDeleteFile; }
	uint32_t error = 2u;
	if (cls && method) { jstring path = env->NewStringUTF(relativePath); if (path) { error = static_cast<uint32_t>(env->CallStaticIntMethod(cls, method, path)); env->DeleteLocalRef(path); } env->DeleteLocalRef(cls); }
	if (env->ExceptionCheck()) { env->ExceptionClear(); error = 5u; }
	if (detach) vm->DetachCurrentThread();
	return error;
}

uint32_t createSafDirectory(const char* templatePath, const char* newPath)
{
	JavaVM* vm = gJavaVm;
	if (!vm || !templatePath || !newPath) return 87u;
	JNIEnv* env = nullptr;
	bool detach = false;
	const jint state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (state == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 8u;
		detach = true;
	} else if (state != JNI_OK) return 8u;
	jclass cls = nullptr;
	jmethodID method = nullptr;
	{
		std::lock_guard<std::mutex> lock(gSafProviderMutex);
		cls = static_cast<jclass>(env->NewLocalRef(gSafProviderClass));
		method = gCreateDirectory;
	}
	uint32_t error = 5u;
	if (cls && method) {
		jstring templateString = env->NewStringUTF(templatePath);
		jstring targetString = env->NewStringUTF(newPath);
		if (templateString && targetString)
			error = static_cast<uint32_t>(env->CallStaticIntMethod(cls, method, templateString, targetString));
		if (templateString) env->DeleteLocalRef(templateString);
		if (targetString) env->DeleteLocalRef(targetString);
		if (env->ExceptionCheck()) { env->ExceptionClear(); error = 5u; }
		env->DeleteLocalRef(cls);
	}
	if (detach) vm->DetachCurrentThread();
	return error;
}

bool querySafFiles(const char* relativeDirectory, const char* wildcard,
	std::vector<br::runtime::Win32FileFindEntry>& entries)
{
	JavaVM* vm = gJavaVm;
	if (!vm || !relativeDirectory || !wildcard) return false;
	JNIEnv* env = nullptr;
	bool detach = false;
	const jint state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (state == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
		detach = true;
	} else if (state != JNI_OK) return false;
	jclass providerClass = nullptr;
	jmethodID method = nullptr;
	{
		std::lock_guard<std::mutex> lock(gSafProviderMutex);
		providerClass = static_cast<jclass>(env->NewLocalRef(gSafProviderClass));
		method = gFindFiles;
	}
	bool ok = false;
	if (providerClass && method) {
		jstring directory = env->NewStringUTF(relativeDirectory);
		jstring pattern = env->NewStringUTF(wildcard);
		if (directory && pattern) {
			jobjectArray records = static_cast<jobjectArray>(env->CallStaticObjectMethod(
				providerClass, method, directory, pattern));
			if (records && !env->ExceptionCheck()) {
				const jsize count = env->GetArrayLength(records);
				for (jsize i = 0; i < count; ++i) {
					auto record = static_cast<jstring>(env->GetObjectArrayElement(records, i));
					if (!record) continue;
					const char* chars = env->GetStringUTFChars(record, nullptr);
					if (chars) {
						std::string value(chars);
						env->ReleaseStringUTFChars(record, chars);
						const size_t first = value.find('\t');
						const size_t second = first == std::string::npos ? first : value.find('\t', first + 1u);
						const size_t third = second == std::string::npos ? second : value.find('\t', second + 1u);
						if (first != std::string::npos && second != std::string::npos && third != std::string::npos) {
							try {
								br::runtime::Win32FileFindEntry entry;
								entry.name = value.substr(0, first);
								entry.attributes = static_cast<uint32_t>(std::stoul(value.substr(first + 1u, second - first - 1u)));
								entry.size = std::stoull(value.substr(second + 1u, third - second - 1u));
								entry.modifiedUnixMillis = std::stoull(value.substr(third + 1u));
								entries.push_back(std::move(entry));
							} catch (...) {}
						}
					}
					env->DeleteLocalRef(record);
				}
				ok = true;
				env->DeleteLocalRef(records);
			}
			if (env->ExceptionCheck()) env->ExceptionClear();
		}
		if (directory) env->DeleteLocalRef(directory);
		if (pattern) env->DeleteLocalRef(pattern);
		env->DeleteLocalRef(providerClass);
	}
	if (detach) vm->DetachCurrentThread();
	return ok;
}

void initializeRuntimeHandlers()
{
	gRuntimeHandlersReady = br::runtime::registerWin32BaseHandlers(gRuntimeImportHandlers) &&
		br::runtime::registerWin32StartupHandlers(gRuntimeImportHandlers) &&
		br::runtime::registerWin32SyncHandlers(gRuntimeImportHandlers) &&
		br::runtime::registerWin32MemoryHandlers(gRuntimeImportHandlers) &&
		br::runtime::registerWin32FileHandlers(gRuntimeImportHandlers);
}

[[noreturn]] void logNativeTerminate()
{
	try {
		if (const auto failure = std::current_exception()) std::rethrow_exception(failure);
	} catch (const std::exception& error) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_FATAL, "BattleRealmsNative",
			"NATIVE_TERMINATE", "uncaught native exception: %s", error.what());
	} catch (...) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_FATAL, "BattleRealmsNative",
			"NATIVE_TERMINATE", "uncaught non-standard native exception");
	}
	std::abort();
}

}

static void dispatchTouchEvents(const std::vector<br::input::MouseEvent>& events);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
	gJavaVm = vm;
	std::set_terminate(logNativeTerminate);
	__android_log_print(ANDROID_LOG_INFO, "BattleRealmsNative", "event=NATIVE_LIBRARY_LOADED pid=%d", getpid());
	return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeRuntimeAvailable(JNIEnv*, jclass)
{
    std::call_once(gRuntimeInit, initializeRuntimeHandlers);
	std::lock_guard<std::mutex> lock(gMainExecutableMutex);
	// This gate means the original PE has been mapped and its guest CPU context
	// can be launched with the linked lifted table. It is not a claim that every
	// game subsystem or campaign has passed runtime testing.
	return gRuntimeHandlersReady && lifted_count != 0u &&
		gMainExecutableRuntime.initialized() && gMainCpuReady &&
		!gGuestEntrypointRunning.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeRuntimeStatus(JNIEnv* env, jclass)
{
	std::call_once(gRuntimeInit, initializeRuntimeHandlers);
	std::lock_guard<std::mutex> lock(gMainExecutableMutex);
	const char* reason = !gRuntimeHandlersReady ? "Win32 import handler initialization failed" :
		(lifted_count == 0u ? "No lifted ARM64 function entries are linked" :
		(!gMainExecutableRuntime.initialized() ? "Original PE is not mapped; choose a valid game folder" :
		(!gMainCpuReady ? "Guest x86 CPU startup context is not prepared" :
		(gGuestEntrypointRunning.load(std::memory_order_acquire) ? "Guest game is already running" : "ready"))));
	return env->NewStringUTF(reason);
}

extern "C" JNIEXPORT void JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeSetDiagnosticsAttemptId(
	JNIEnv* env, jclass, jstring attemptId)
{
	const char* chars = attemptId ? env->GetStringUTFChars(attemptId, nullptr) : nullptr;
	br::runtime::setAndroidDiagnosticsAttemptId(chars);
	if (chars) env->ReleaseStringUTFChars(attemptId, chars);
}

extern "C" JNIEXPORT void JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeConfigureDiagnostics(
	JNIEnv* env, jclass, jstring path)
{
	const char* chars = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
	br::runtime::configureAndroidDiagnostics(chars);
	if (chars) env->ReleaseStringUTFChars(path, chars);
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"Runtime diagnostics log configured");
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
		"NATIVE_LIBRARY_READY", "lifted_entries=%u", lifted_count);
}

extern "C" JNIEXPORT void JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeSetDisplayMetrics(
	JNIEnv*, jclass, jint width, jint height)
{
	br::runtime::configureWin32StartupDisplayMetrics(
		width > 0 ? static_cast<uint32_t>(width) : 0u,
		height > 0 ? static_cast<uint32_t>(height) : 0u);
}

extern "C" JNIEXPORT void JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeSetUserLocale(
	JNIEnv* env, jclass, jstring languageTag)
{
	std::string tag;
	if (languageTag) {
		const char* chars = env->GetStringUTFChars(languageTag, nullptr);
		if (chars) {
			tag = chars;
			env->ReleaseStringUTFChars(languageTag, chars);
		}
	}
	std::transform(tag.begin(), tag.end(), tag.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	uint32_t lcid = 0x0409u, codePage = 1252u;
	if (tag == "en-gb") lcid = 0x0809u;
	else if (tag == "en-ca") lcid = 0x1009u;
	else if (tag == "en-au") lcid = 0x0c09u;
	else if (tag == "fr-fr") lcid = 0x040cu;
	else if (tag == "de-de") lcid = 0x0407u;
	else if (tag == "es-es") lcid = 0x0c0au;
	else if (tag == "it-it") lcid = 0x0410u;
	else if (tag == "pt-br") lcid = 0x0416u;
	else if (tag == "pt-pt") lcid = 0x0816u;
	else if (tag == "ja-jp") { lcid = 0x0411u; codePage = 932u; }
	else if (tag == "zh-cn" || tag == "zh-sg") { lcid = 0x0804u; codePage = 936u; }
	else if (tag == "zh-tw" || tag == "zh-hk") { lcid = 0x0404u; codePage = 950u; }
	else if (tag == "ko-kr") { lcid = 0x0412u; codePage = 949u; }
	else if (tag == "ru-ru") { lcid = 0x0419u; codePage = 1251u; }
	br::runtime::configureWin32StartupLocale(lcid, codePage);
}

extern "C" JNIEXPORT void JNICALL
Java_org_battlerealms_nativeport_GameActivity_nativeGameTouchAction(JNIEnv*, jclass, jint action)
{
	const int32_t x = std::max(0, gNativeLastGuestX.load(std::memory_order_relaxed));
	const int32_t y = std::max(0, gNativeLastGuestY.load(std::memory_order_relaxed));
	switch (action) {
	case 1: // Context/cancel (RMB).
		dispatchTouchEvents({
			{br::input::MouseAction::Move, x, y, 0},
			{br::input::MouseAction::RightDown, x, y, 0},
			{br::input::MouseAction::RightUp, x, y, 0}});
		break;
	case 2: // Escape.
		br::runtime::postGuestKeyMessage(0x1bu, true);
		br::runtime::postGuestKeyMessage(0x1bu, false);
		break;
	case 3: case 4: // Wheel zoom, if the game exposes it at the current cursor position.
		dispatchTouchEvents({{br::input::MouseAction::Wheel, x, y, action == 3 ? 120 : -120}});
		break;
	case 5: {
		const bool enabled = !gNativeRightClickMode.load(std::memory_order_relaxed);
		gNativeRightClickMode.store(enabled, std::memory_order_relaxed);
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsInput",
			"TOUCH_MOUSE_MODE_CHANGED", "tap_right=%d", enabled ? 1 : 0);
		break;
	}
	default:
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_WARN, "BattleRealmsInput",
			"UNKNOWN_TOUCH_HUD_ACTION", "action=%d", action);
		break;
	}
}

extern "C" JNIEXPORT jint JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeLiftedFunctionCount(JNIEnv*, jclass)
{
	return static_cast<jint>(lifted_count);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativePrepareMainExecutable(JNIEnv* env, jclass)
{
	std::call_once(gRuntimeInit, initializeRuntimeHandlers);
	if (!gRuntimeHandlersReady) return env->NewStringUTF("Win32 handler registry initialization failed.");
	std::lock_guard<std::mutex> lock(gMainExecutableMutex);
	if (gMainExecutableRuntime.initialized()) {
		if (!gMainCpuReady) {
			std::string contextError;
			gMainCpuReady = gMainExecutableRuntime.prepareInitialCpu(gMainCpu,
				br::runtime::sharedGuestAddressSpace(), contextError);
			if (!gMainCpuReady) {
				const std::string message = "x86 startup context initialization failed: " + contextError;
				return env->NewStringUTF(message.c_str());
			}
		}
		return env->NewStringUTF("Main PE mapped; imports bound; minimal x86 stack/TEB prepared; entrypoint not executed.");
	}
	const auto opened = openSafFile("Battle_Realms_F.exe", 0x80000000u, 3u);
	if (opened.fd < 0) {
		const std::string message = "Cannot read Battle_Realms_F.exe through the selected folder (Win32 error " +
			std::to_string(opened.lastError) + ").";
		return env->NewStringUTF(message.c_str());
	}
	struct stat metadata{};
	if (fstat(opened.fd, &metadata) != 0 || metadata.st_size <= 0 || metadata.st_size > 64 * 1024 * 1024) {
		close(opened.fd);
		return env->NewStringUTF("The selected executable has an invalid size or could not be inspected.");
	}
	std::vector<uint8_t> file(static_cast<size_t>(metadata.st_size));
	size_t offset = 0;
	while (offset < file.size()) {
		const ssize_t amount = pread(opened.fd, file.data() + offset, file.size() - offset,
			static_cast<off_t>(offset));
		if (amount <= 0) {
			close(opened.fd);
			return env->NewStringUTF("Reading Battle_Realms_F.exe through SAF failed.");
		}
		offset += static_cast<size_t>(amount);
	}
	close(opened.fd);
	std::string error;
	if (!gMainExecutableRuntime.initialize(file, br::runtime::sharedGuestAddressSpace(),
		gRuntimeImportHandlers, error)) {
		const std::string message = "PE initialization failed: " + error;
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative", "%s", message.c_str());
		return env->NewStringUTF(message.c_str());
	}
	if (!gMainExecutableRuntime.prepareInitialCpu(gMainCpu,
		br::runtime::sharedGuestAddressSpace(), error)) {
		const std::string message = "x86 startup context initialization failed: " + error;
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative", "%s", message.c_str());
		return env->NewStringUTF(message.c_str());
	}
	gMainCpuReady = true;
	unsigned handledApis = 0;
	for (const auto& stub : gMainExecutableRuntime.imports().apiStubs())
		if (gRuntimeImportHandlers.find(stub)) ++handledApis;
	const auto& stats = gMainExecutableRuntime.stats();
	char message[256]{};
	snprintf(message, sizeof(message),
		"Main PE mapped; entry=0x%08x; import refs=%u; API stubs=%u; handlers=%u; minimal x86 stack/TEB prepared; entrypoint not executed.",
		gMainExecutableRuntime.image().entryAddress(), stats.apiImportReferences,
		stats.uniqueApiStubs, handledApis);
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative", "%s", message);
	return env->NewStringUTF(message);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeExecuteMainEntrypoint(JNIEnv* env, jclass)
{
	uint32_t entry = 0u;
	{
		std::lock_guard<std::mutex> lock(gMainExecutableMutex);
		if (!gMainExecutableRuntime.initialized() || !gMainCpuReady)
			return env->NewStringUTF("Cannot run the entrypoint before the original executable is mapped.");
		bool expected = false;
		if (!gGuestEntrypointRunning.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
			return env->NewStringUTF("A guest entrypoint is already running; restart the launcher for a clean run.");
		entry = gMainExecutableRuntime.image().entryAddress();
	}
	GuestEntrypointRunReset resetRun;
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"Attempting original lifted entrypoint 0x%08x", entry);
	try {
		call_target(&gMainCpu, entry);
	} catch (const br::runtime::GuestProcessExit& exit) {
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
			"Guest called ExitProcess(%u); lifted entrypoint ended normally", exit.code);
		char message[128]{};
		snprintf(message, sizeof(message), "Guest exited normally with code %u. Check device logcat for its startup trace.", exit.code);
		return env->NewStringUTF(message);
	} catch (const std::exception& failure) {
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"Guest entrypoint stopped: %s", failure.what());
		return env->NewStringUTF("Guest entrypoint stopped; see device logcat for the first runtime failure.");
	}
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"Original lifted entrypoint returned at ESP=0x%08x", gMainCpu.r[ESP]);
	char message[128]{};
	snprintf(message, sizeof(message), "Lifted entrypoint returned (ESP=0x%08x). Check device logcat for its startup trace.",
		gMainCpu.r[ESP]);
	return env->NewStringUTF(message);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_battlerealms_nativeport_LauncherActivity_nativeEnableFileBridge(
	JNIEnv* env, jclass, jclass safProviderClass)
{
	if (!safProviderClass) return JNI_FALSE;
	jclass globalClass = static_cast<jclass>(env->NewGlobalRef(safProviderClass));
	if (!globalClass) return JNI_FALSE;
    jmethodID method = env->GetStaticMethodID(globalClass, "openFile", "(Ljava/lang/String;II)J");
    jmethodID attributesMethod = env->GetStaticMethodID(
		globalClass, "getFileAttributes", "(Ljava/lang/String;)I");
    jmethodID deleteMethod = env->GetStaticMethodID(globalClass, "deleteFile", "(Ljava/lang/String;)I");
    jmethodID createDirectoryMethod = env->GetStaticMethodID(globalClass, "createDirectory",
		"(Ljava/lang/String;Ljava/lang/String;)I");
    jmethodID findFilesMethod = env->GetStaticMethodID(globalClass, "findFiles",
		"(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
	if (!method || !attributesMethod || !deleteMethod || !createDirectoryMethod || !findFilesMethod) {
		env->DeleteGlobalRef(globalClass);
		if (env->ExceptionCheck()) env->ExceptionClear();
		return JNI_FALSE;
	}
	{
		std::lock_guard<std::mutex> lock(gSafProviderMutex);
		if (gSafProviderClass) env->DeleteGlobalRef(gSafProviderClass);
		gSafProviderClass = globalClass;
		gOpenReadOnlyFd = method;
        gGetFileAttributes = attributesMethod;
		gDeleteFile = deleteMethod;
		gCreateDirectory = createDirectoryMethod;
		gFindFiles = findFilesMethod;
	}
	br::runtime::configureWin32FileOpener(openSafFile);
	br::runtime::configureWin32FileAttributesQuery(querySafFileAttributes);
	br::runtime::configureWin32FileDelete(deleteSafFile);
	br::runtime::configureWin32FileDirectoryCreator(createSafDirectory);
	br::runtime::configureWin32FileFindQuery(querySafFiles);
	return JNI_TRUE;
}

static uint32_t mapAndroidKey(int32_t keyCode)
{
    if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z)
        return static_cast<uint32_t>('A' + keyCode - AKEYCODE_A);
    if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9)
        return static_cast<uint32_t>('0' + keyCode - AKEYCODE_0);
    switch (keyCode) {
    case AKEYCODE_DPAD_UP: return 0x26u;
    case AKEYCODE_DPAD_DOWN: return 0x28u;
    case AKEYCODE_DPAD_LEFT: return 0x25u;
    case AKEYCODE_DPAD_RIGHT: return 0x27u;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return 0x0du;
    case AKEYCODE_ESCAPE: case AKEYCODE_BACK: return 0x1bu;
    case AKEYCODE_DEL: return 0x08u;
    case AKEYCODE_TAB: return 0x09u;
    case AKEYCODE_SPACE: return 0x20u;
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return 0x10u;
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return 0x11u;
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return 0x12u;
    case AKEYCODE_MENU: return 0x5du;
    default: return 0u;
    }
}

static uint64_t touchClockMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void dispatchTouchEvents(const std::vector<br::input::MouseEvent>& events)
{
	std::lock_guard<std::mutex> lock(gAndroidTouchDispatchMutex);
	for (const auto& event : events) {
		const int32_t x = std::max(0, event.x), y = std::max(0, event.y);
		gNativeLastGuestX.store(x, std::memory_order_relaxed);
		gNativeLastGuestY.store(y, std::memory_order_relaxed);
		uint32_t message = 0u, wParam = gNativeTouchButtons;
		switch (event.action) {
		case br::input::MouseAction::Move: message = 0x0200u; break;
		case br::input::MouseAction::LeftDown: gNativeTouchButtons |= 1u; wParam = gNativeTouchButtons; message = 0x0201u; break;
		case br::input::MouseAction::LeftUp: gNativeTouchButtons &= ~1u; wParam = gNativeTouchButtons; message = 0x0202u; break;
		case br::input::MouseAction::RightDown: gNativeTouchButtons |= 2u; wParam = gNativeTouchButtons; message = 0x0204u; break;
		case br::input::MouseAction::RightUp: gNativeTouchButtons &= ~2u; wParam = gNativeTouchButtons; message = 0x0205u; break;
		case br::input::MouseAction::MiddleDown: gNativeTouchButtons |= 0x10u; wParam = gNativeTouchButtons; message = 0x0207u; break;
		case br::input::MouseAction::MiddleUp: gNativeTouchButtons &= ~0x10u; wParam = gNativeTouchButtons; message = 0x0208u; break;
		case br::input::MouseAction::Wheel:
			message = 0x020au;
			wParam = (gNativeTouchButtons & 0xffffu) |
				(static_cast<uint32_t>(static_cast<uint16_t>(event.wheel)) << 16);
			break;
		}
		if (message) {
			// Keep a compact action trace for Android gameplay verification. Log
			// button edges and wheel input, but omit high-rate pointer motion.
			const char* action = nullptr;
			switch (message) {
			case 0x0201u: action = "left_down"; break;
			case 0x0202u: action = "left_up"; break;
			case 0x0204u: action = "right_down"; break;
			case 0x0205u: action = "right_up"; break;
			case 0x0207u: action = "middle_down"; break;
			case 0x0208u: action = "middle_up"; break;
			case 0x020au: action = "wheel"; break;
			default: break;
			}
			const uint32_t guestWindow = br::runtime::activeGuestWindowHandle();
			br::runtime::postGuestMouseMessage(message, x, y, wParam);
			if (action) br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO,
				"BattleRealmsInput", "ANDROID_TOUCH_MOUSE_EVENT",
				"action=%s guest_x=%d guest_y=%d message=0x%04x buttons=0x%08x wheel=%d guest_hwnd=0x%08x guest_window_available=%d input_state_updated=1",
				action, x, y, message, wParam,
				message == 0x020au ? static_cast<int16_t>(wParam >> 16) : 0,
				guestWindow, guestWindow ? 1 : 0);
		}
	}
}

static void mapAndroidToGuest(android_app* app, float& x, float& y)
{
	uint32_t guestWidth = 0u, guestHeight = 0u;
	br::runtime::activeGuestWindowSize(guestWidth, guestHeight);
	if (app && app->window && guestWidth && guestHeight) {
		const int32_t surfaceWidth = ANativeWindow_getWidth(app->window);
		const int32_t surfaceHeight = ANativeWindow_getHeight(app->window);
		if (surfaceWidth > 0 && surfaceHeight > 0) {
			const float scale = std::min(static_cast<float>(surfaceWidth) / guestWidth,
				static_cast<float>(surfaceHeight) / guestHeight);
			const float left = (surfaceWidth - guestWidth * scale) * 0.5f;
			const float top = (surfaceHeight - guestHeight * scale) * 0.5f;
			x = (x - left) / scale; y = (y - top) / scale;
		}
		x = std::clamp(x, 0.0f, static_cast<float>(guestWidth - 1u));
		y = std::clamp(y, 0.0f, static_cast<float>(guestHeight - 1u));
	}
}

static int onInput(android_app* app, AInputEvent* event)
{
    if (!event) return 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const uint32_t key = mapAndroidKey(AKeyEvent_getKeyCode(event));
        if (!key) return 0;
        const int32_t action = AKeyEvent_getAction(event);
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 0;
        br::runtime::postGuestKeyMessage(key, action == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const int32_t action = AMotionEvent_getAction(event);
    const int32_t actionKind = action & AMOTION_EVENT_ACTION_MASK;
    if (actionKind != AMOTION_EVENT_ACTION_DOWN && actionKind != AMOTION_EVENT_ACTION_POINTER_DOWN &&
        actionKind != AMOTION_EVENT_ACTION_MOVE && actionKind != AMOTION_EVENT_ACTION_UP &&
        actionKind != AMOTION_EVENT_ACTION_POINTER_UP && actionKind != AMOTION_EVENT_ACTION_CANCEL &&
        actionKind != AMOTION_EVENT_ACTION_SCROLL) return 0;

    const size_t pointerIndex = actionKind == AMOTION_EVENT_ACTION_POINTER_UP
        ? static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT) : 0u;
    const size_t pointerCount = AMotionEvent_getPointerCount(event);
    float x0 = AMotionEvent_getX(event, 0), y0 = AMotionEvent_getY(event, 0);
    float x1 = pointerCount > 1 ? AMotionEvent_getX(event, 1) : x0;
    float y1 = pointerCount > 1 ? AMotionEvent_getY(event, 1) : y0;
    mapAndroidToGuest(app, x0, y0);
    mapAndroidToGuest(app, x1, y1);
    if (actionKind == AMOTION_EVENT_ACTION_SCROLL) {
        const int32_t wheel = static_cast<int32_t>(AMotionEvent_getAxisValue(event,
            AMOTION_EVENT_AXIS_VSCROLL, pointerIndex) * 120.0f);
        dispatchTouchEvents({{br::input::MouseAction::Wheel,
            static_cast<int>(x0), static_cast<int>(y0), wheel}});
		return 1;
    }
	br::input::TouchAction gestureAction;
	switch (actionKind) {
	case AMOTION_EVENT_ACTION_DOWN: gestureAction = br::input::TouchAction::Down; break;
	case AMOTION_EVENT_ACTION_POINTER_DOWN: gestureAction = br::input::TouchAction::PointerDown; break;
	case AMOTION_EVENT_ACTION_MOVE: gestureAction = br::input::TouchAction::Move; break;
	case AMOTION_EVENT_ACTION_POINTER_UP: gestureAction = br::input::TouchAction::PointerUp; break;
	case AMOTION_EVENT_ACTION_UP: gestureAction = br::input::TouchAction::Up; break;
	default: gestureAction = br::input::TouchAction::Cancel; break;
	}
	gTouchGestures.setRightClickMode(gNativeRightClickMode.load(std::memory_order_relaxed));
	dispatchTouchEvents(gTouchGestures.process({gestureAction, static_cast<int>(pointerCount), x0, y0,
		x1, y1, touchClockMs()}));
    return 1;
}

void android_main(android_app* app)
{
    app->onInputEvent = onInput;
	app->onAppCmd = onAndroidAppCommand;
	br::runtime::configureWin32FramePresenter(presentGuestFrame);
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative", "NATIVE_ACTIVITY_READY",
		"native activity ready; %u lifted functions linked; waiting for the game surface", lifted_count);
	startAndroidAudio();
    bool guestStarted = false;
    std::thread guestThread;
    while (app->destroyRequested == 0) {
        int events = 0;
        android_poll_source* source = nullptr;
        int ident = ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void**>(&source));
        if (ident >= 0 && source) source->process(app, source);
		serviceAndroidAudio();
		dispatchTouchEvents(gTouchGestures.tick(touchClockMs()));
        if (!guestStarted && app->window) {
            uint32_t entry = 0u;
            {
                std::lock_guard<std::mutex> lock(gMainExecutableMutex);
                if (gMainExecutableRuntime.initialized() && gMainCpuReady)
                    entry = gMainExecutableRuntime.image().entryAddress();
            }
            if (entry) {
                guestStarted = true;
				br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
					"GUEST_THREAD_START_REQUESTED", "entry=0x%08x", entry);
                bool expected = false;
                if (!gGuestEntrypointRunning.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
                        "refusing to start a second guest entrypoint on the shared x86 CPU context");
                    continue;
                }
                guestThread = std::thread([entry] {
                    GuestEntrypointRunReset resetRun;
					gAndroidGuestStart = std::chrono::steady_clock::now();
#if defined(BR_ANDROID_GUEST_PROFILE)
                    br::runtime::beginAndroidGuestProfileThread();
#endif
					br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
						"GUEST_ENTRYPOINT_ENTERED", "entry=0x%08x", entry);
                    try {
                        call_target(&gMainCpu, entry);
                        br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
                            "lifted entrypoint returned at ESP=0x%08x", gMainCpu.r[ESP]);
                    } catch (const br::runtime::GuestProcessExit& exit) {
                        br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
                            "guest called ExitProcess(%u)", exit.code);
                    } catch (const std::exception& failure) {
                        br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
                            "guest startup stopped: %s", failure.what());
                    }
#if defined(BR_ANDROID_GUEST_PROFILE)
                    br::runtime::endAndroidGuestProfileThread();
#endif
                });
            } else {
                br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_WARN, "BattleRealmsNative",
                    "game surface is ready but the launcher has not mapped the original executable");
                guestStarted = true;
            }
        }
    }
    br::runtime::postGuestQuitMessage(0u);
    if (guestThread.joinable()) guestThread.join();
	closeAndroidAudio();
}

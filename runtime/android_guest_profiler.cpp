#include "android_guest_profiler.h"

#if defined(__ANDROID__)

#include "android_diagnostics.h"

#include <android/log.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

// These names and layout match the RA3 lifted prelude. The rings are populated
// at each generated function entry when the lifted objects use RA3_TRACE.
thread_local uint32_t ra3_ring[256]{};
thread_local uint32_t ra3_ring_ra[256]{};
thread_local uint32_t ra3_ring_pos{};

namespace br::runtime {
thread_local uint32_t brActiveGuestFunction{};
thread_local uint32_t brActiveGuestCaller{};

namespace {

struct GuestThreadRing {
	uint32_t* activeFunction = nullptr;
	uint32_t* activeCaller = nullptr;
	long tid = 0;
	uint64_t previousCpuNs = 0;
};

struct SampleCount {
	uint64_t count = 0;
	uint32_t caller = 0;
};

std::mutex gProfileMutex;
std::vector<GuestThreadRing> gGuestThreadRings;
std::once_flag gProfilerStart;

uint64_t guestThreadCpuNs(long tid)
{
	char path[96]{};
	std::snprintf(path, sizeof(path), "/proc/self/task/%ld/schedstat", tid);
	FILE* file = std::fopen(path, "r");
	if (!file) return 0u;
	unsigned long long runtimeNs = 0u;
	const int read = std::fscanf(file, "%llu", &runtimeNs);
	std::fclose(file);
	return read == 1 ? static_cast<uint64_t>(runtimeNs) : 0u;
}

void profileLoop()
{
	using Clock = std::chrono::steady_clock;
	auto reportAt = Clock::now() + std::chrono::seconds(5);
	auto windowStartedAt = Clock::now();
	std::unordered_map<uint32_t, SampleCount> samples;
	uint64_t intervalSamples = 0u;
	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		{
			std::lock_guard<std::mutex> lock(gProfileMutex);
			for (GuestThreadRing& ring : gGuestThreadRings) {
				const uint32_t address = __atomic_load_n(ring.activeFunction, __ATOMIC_RELAXED);
				const uint32_t caller = __atomic_load_n(ring.activeCaller, __ATOMIC_RELAXED);
				if (address) {
					SampleCount& sample = samples[address];
					++sample.count;
					sample.caller = caller;
					++intervalSamples;
				}
			}
		}
		const auto now = Clock::now();
		if (now < reportAt) continue;

		struct RankedSample { uint64_t count; uint32_t address; uint32_t caller; };
		std::array<RankedSample, 20> top{};
		uint64_t guestCpuNs = 0u;
		size_t profiledThreads = 0u;
		{
			std::lock_guard<std::mutex> lock(gProfileMutex);
			profiledThreads = gGuestThreadRings.size();
			for (const auto& pair : samples) {
				const RankedSample value{pair.second.count, pair.first, pair.second.caller};
				auto at = top.begin();
				while (at != top.end() && at->count >= value.count) ++at;
				if (at != top.end()) {
					for (auto move = top.end() - 1; move != at; --move) *move = *(move - 1);
					*at = value;
				}
			}
			for (GuestThreadRing& ring : gGuestThreadRings) {
				const uint64_t cpuNs = guestThreadCpuNs(ring.tid);
				if (cpuNs && ring.previousCpuNs && cpuNs >= ring.previousCpuNs)
					guestCpuNs += cpuNs - ring.previousCpuNs;
				ring.previousCpuNs = cpuNs;
			}
		}

		char detail[3000]{};
		size_t used = 0u;
		for (const RankedSample& sample : top) {
			if (!sample.count) continue;
			const double percent = intervalSamples
				? 100.0 * static_cast<double>(sample.count) / static_cast<double>(intervalSamples)
				: 0.0;
			const int written = std::snprintf(detail + used, sizeof(detail) - used,
				" %08x=%llu(%.1f%%,caller=%08x)", sample.address,
				static_cast<unsigned long long>(sample.count), percent, sample.caller);
			if (written < 0 || static_cast<size_t>(written) >= sizeof(detail) - used) break;
			used += static_cast<size_t>(written);
		}
		const double windowMs = std::chrono::duration<double, std::milli>(now - windowStartedAt).count();
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"guest-profile mode=guest-instruction-residency host_api_time=excluded target_sample_period_ms=2 window_ms=%.0f guest_threads=%zu guest-thread-cpu=%llums samples=%llu top(address=count,share,caller):%s",
			windowMs,
			profiledThreads,
			static_cast<unsigned long long>(guestCpuNs / 1000000u),
			static_cast<unsigned long long>(intervalSamples), detail[0] ? detail : " <no lifted-function samples>");
		samples.clear();
		intervalSamples = 0u;
		reportAt = now + std::chrono::seconds(5);
		windowStartedAt = now;
	}
}

} // namespace

void beginAndroidGuestProfileThread()
{
	const long tid = static_cast<long>(::syscall(SYS_gettid));
	{
		std::lock_guard<std::mutex> lock(gProfileMutex);
		gGuestThreadRings.push_back({&brActiveGuestFunction, &brActiveGuestCaller, tid,
			guestThreadCpuNs(tid)});
	}
	std::call_once(gProfilerStart, [] {
		std::thread(profileLoop).detach();
	});
	androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"guest profiler attached to thread=%ld; sampling lifted function entries every 2ms", tid);
}

void endAndroidGuestProfileThread()
{
	const long tid = static_cast<long>(::syscall(SYS_gettid));
	std::lock_guard<std::mutex> lock(gProfileMutex);
	for (auto it = gGuestThreadRings.begin(); it != gGuestThreadRings.end(); ++it) {
		if (it->tid == tid) {
			gGuestThreadRings.erase(it);
			break;
		}
	}
}

} // namespace br::runtime

#endif // __ANDROID__

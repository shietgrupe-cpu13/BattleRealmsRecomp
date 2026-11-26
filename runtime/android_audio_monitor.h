#pragma once

#include <atomic>
#include <cstdint>

namespace br::runtime {

// Separates callback dispatch from callback completion. A callback that enters
// the mixer and never returns otherwise looks identical to a stream that stops
// invoking callbacks when only completed-call counts are sampled.
class AndroidAudioCallbackMonitor {
public:
	struct Window {
		uint64_t entered = 0;
		uint64_t completed = 0;
		uint32_t inFlight = 0;
	};

	void enter() noexcept
	{
		mEntered.fetch_add(1u, std::memory_order_relaxed);
		mInFlight.fetch_add(1u, std::memory_order_relaxed);
	}

	void complete() noexcept
	{
		mCompleted.fetch_add(1u, std::memory_order_relaxed);
		mInFlight.fetch_sub(1u, std::memory_order_relaxed);
	}

	Window consumeWindow() noexcept
	{
		return {mEntered.exchange(0u, std::memory_order_relaxed),
			mCompleted.exchange(0u, std::memory_order_relaxed),
			mInFlight.load(std::memory_order_relaxed)};
	}

private:
	std::atomic<uint64_t> mEntered{0u};
	std::atomic<uint64_t> mCompleted{0u};
	std::atomic<uint32_t> mInFlight{0u};
};

} // namespace br::runtime

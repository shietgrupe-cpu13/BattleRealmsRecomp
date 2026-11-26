#include "android_audio_monitor.h"

#include <cstdio>

namespace {
bool check(bool condition, const char* description)
{
	if (condition) return true;
	std::fprintf(stderr, "android_audio_monitor_smoke: failed: %s\n", description);
	return false;
}
}

int main()
{
	br::runtime::AndroidAudioCallbackMonitor monitor;
	monitor.enter();
	auto window = monitor.consumeWindow();
	if (!check(window.entered == 1u && window.completed == 0u && window.inFlight == 1u,
		"an entered but unfinished callback remains visible")) return 1;

	monitor.complete();
	window = monitor.consumeWindow();
	if (!check(window.entered == 0u && window.completed == 1u && window.inFlight == 0u,
		"a returned callback is counted and no longer in flight")) return 1;

	monitor.enter();
	monitor.enter();
	monitor.complete();
	monitor.complete();
	window = monitor.consumeWindow();
	if (!check(window.entered == 2u && window.completed == 2u && window.inFlight == 0u,
		"multiple callbacks aggregate within a sampling window")) return 1;

	window = monitor.consumeWindow();
	return check(window.entered == 0u && window.completed == 0u && window.inFlight == 0u,
		"window counters reset without changing the in-flight gauge") ? 0 : 1;
}

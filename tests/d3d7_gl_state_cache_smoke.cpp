#include "d3d7_gl_state_cache.h"

#include <array>
#include <cstdint>

int main()
{
	using br::runtime::d3d7::GlStateCache;
	GlStateCache cache;
	if (!cache.apply(cache.program, 7u) || cache.apply(cache.program, 7u)) return 1;
	if (cache.skippedCalls != 1u) return 2;
	cache.program.invalidate();
	if (!cache.apply(cache.program, 7u)) return 3;
	if (!cache.apply(cache.viewport, std::array<int32_t, 4>{0, 0, 1024, 768})) return 4;
	cache.invalidateBindings();
	if (!cache.apply(cache.framebuffer, 9u) || cache.apply(cache.program, 7u)) return 5;
	cache.invalidateDrawState();
	if (!cache.apply(cache.blend, true) || !cache.apply(cache.depthWrite, false)) return 6;
	return 0;
}

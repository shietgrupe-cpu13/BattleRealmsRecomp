#include "d3d7_texture_probe.h"

#include <cstdio>

int main()
{
	br::runtime::d3d7::TextureContentProbe probe{};
	probe.add(0x001fu, 2u, 0u, 0u, 255u, 255u);
	probe.add(0x0000u, 2u, 0u, 0u, 0u, 0u);
	probe.add(0xf800u, 2u, 255u, 0u, 0u, 255u);
	if (probe.pixels != 3u || probe.rgbBlackPixels != 1u ||
		probe.alphaZeroPixels != 1u || probe.opaqueBlackPixels != 0u ||
		probe.sourceFnv1a != 0x1a864c42u || probe.rgbaFnv1a != 0x4990f3c9u) {
		std::fprintf(stderr, "texture probe mismatch: pixels=%llu black=%llu alpha0=%llu opaqueBlack=%llu source=%08x rgba=%08x\n",
			static_cast<unsigned long long>(probe.pixels),
			static_cast<unsigned long long>(probe.rgbBlackPixels),
			static_cast<unsigned long long>(probe.alphaZeroPixels),
			static_cast<unsigned long long>(probe.opaqueBlackPixels),
			probe.sourceFnv1a, probe.rgbaFnv1a);
		return 1;
	}
	std::puts("PASS texture-source and converted-RGBA diagnostics");
	return 0;
}

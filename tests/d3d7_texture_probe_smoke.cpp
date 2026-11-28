#include "d3d7_texture_probe.h"

#include <cstdio>
#include <vector>

int main()
{
	using namespace br::runtime::d3d7;
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
	uint32_t pixels[16];
	for (uint32_t& pixel : pixels) pixel = 0xffffffffu;
	pixels[0] = 0x00000000u;
	pixels[1] = pixels[4] = 0xff000000u;
	GpuDrawState draw{};
	draw.activeTextureStages = 1u;
	draw.textures[0] = {1u, 4u, 4u, 16u, 32u, 0x00ff0000u, 0x0000ff00u,
		0x000000ffu, 0xff000000u, 1u, reinterpret_cast<uint8_t*>(pixels), sizeof(pixels)};
	GpuVertex vertices[3]{};
	vertices[0].texcoord[0] = {0.0f, 0.0f};
	vertices[1].texcoord[0] = {0.5f, 0.0f};
	vertices[2].texcoord[0] = {0.0f, 0.5f};
	const TextureUvProbe uv = probeTextureUv(draw, vertices, 3u);
	if (!uv.valid || !uv.withinUnitRange || uv.sampledPixels != 4u || uv.blackPixels != 3u || uv.alphaZeroPixels != 1u ||
		uv.minU != 0.0f || uv.minV != 0.0f || uv.maxU != 0.5f || uv.maxV != 0.5f) {
		std::fprintf(stderr, "UV/black-region probe mismatch: valid=%u inside=%u samples=%u black=%u range=%.2f,%.2f..%.2f,%.2f\n",
			uv.valid, uv.withinUnitRange, uv.sampledPixels, uv.blackPixels,
			uv.minU, uv.minV, uv.maxU, uv.maxV);
		return 2;
	}
	TextureUvProbe boundsOnly = probeTextureUv(draw, vertices, 3u, false);
	if (!boundsOnly.valid || boundsOnly.sampledPixels || boundsOnly.blackPixels) return 3;
	sampleTextureUvBlackPixels(draw, boundsOnly);
	if (boundsOnly.sampledPixels != 4u || boundsOnly.blackPixels != 3u || boundsOnly.alphaZeroPixels != 1u) return 4;
	std::vector<uint32_t> largePixels(256u * 256u, 0xff000000u);
	draw.textures[0].width = draw.textures[0].height = 256u;
	draw.textures[0].pitch = 256u * 4u;
	draw.textures[0].pixels = reinterpret_cast<uint8_t*>(largePixels.data());
	draw.textures[0].byteCount = static_cast<uint32_t>(largePixels.size() * sizeof(uint32_t));
	const TextureUvProbe bounded = probeTextureUv(draw, vertices, 3u);
	if (bounded.sampledPixels > 4096u || bounded.blackPixels != bounded.sampledPixels || !bounded.sampledPixels) return 5;
	std::puts("PASS texture-source and converted-RGBA diagnostics");
	return 0;
}

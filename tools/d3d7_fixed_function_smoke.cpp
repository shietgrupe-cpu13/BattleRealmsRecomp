#include "d3d7_fixed_function.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
	using namespace br::runtime::d3d7;
	TextureSamplerState sampler{};
	if (!decodeD3d7TextureSamplerState(2u, 1u, 2u, 3u, sampler) ||
		sampler.minFilter != TextureFilterMode::Linear || sampler.magFilter != TextureFilterMode::Point ||
		sampler.addressU != TextureAddressMode::Mirror || sampler.addressV != TextureAddressMode::Clamp ||
		decodeD3d7TextureSamplerState(3u, 1u, 1u, 1u, sampler) ||
		decodeD3d7TextureSamplerState(1u, 2u, 4u, 1u, sampler)) return 46;
	FvfLayout fvf144{}, fvf2c2{}, fvf1d2{}, fvfDims{};
	if (!decodeFvf(0x144u, fvf144) || fvf144.position != PositionKind::XYZRHW ||
		fvf144.positionBytes != 16u || fvf144.diffuseOffset != 16u ||
		fvf144.textureCount != 1u || fvf144.textureOffset[0] != 20u || fvf144.stride != 28u) return 25;
	if (!decodeFvf(0x2c2u, fvf2c2) || fvf2c2.position != PositionKind::XYZ ||
		fvf2c2.diffuseOffset != 12u || fvf2c2.specularOffset != 16u ||
		fvf2c2.textureCount != 2u || fvf2c2.textureOffset[0] != 20u ||
		fvf2c2.textureOffset[1] != 28u || fvf2c2.stride != 36u) return 26;
	if (!decodeFvf(0x1d2u, fvf1d2) || fvf1d2.position != PositionKind::XYZ ||
		fvf1d2.normalOffset != 12u || fvf1d2.diffuseOffset != 24u ||
		fvf1d2.specularOffset != 28u || fvf1d2.textureOffset[0] != 32u ||
		fvf1d2.stride != 40u) return 27;
	if (!decodeFvf(0x00070202u, fvfDims) || fvfDims.textureCount != 2u ||
		fvfDims.textureDimensions[0] != 1u || fvfDims.textureDimensions[1] != 3u ||
		fvfDims.textureOffset[0] != 12u || fvfDims.textureOffset[1] != 16u ||
		fvfDims.stride != 28u || decodeFvf(0x000001d3u, fvfDims)) return 28;
	const std::array<TextureFootprintVertex, 3> magnifiedFootprint{{
		{0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
		{64.0f, 0.0f, 1.0f, 0.25f, 0.0f},
		{0.0f, 64.0f, 1.0f, 0.0f, 0.25f}}};
	const std::array<TextureFootprintVertex, 3> minifiedFootprint{{
		{0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
		{64.0f, 0.0f, 1.0f, 4.0f, 0.0f},
		{0.0f, 64.0f, 1.0f, 0.0f, 4.0f}}};
	if (textureFootprintIsMinified(magnifiedFootprint, 64u, 64u) ||
		!textureFootprintIsMinified(minifiedFootprint, 64u, 64u) ||
		!textureFootprintIsMinified(magnifiedFootprint, 0u, 64u)) return 45;
	PixelFormatLayout rgb565{}, argb8888{}, invalidPixelFormat{};
	if (!makePixelFormatLayout(16u, 0xf800u, 0x07e0u, 0x001fu, 0u, rgb565) ||
		!makePixelFormatLayout(32u, 0x00ff0000u, 0x0000ff00u, 0x000000ffu,
			0xff000000u, argb8888) ||
		makePixelFormatLayout(16u, 0x5500u, 0x00e0u, 0x001fu, 0u,
			invalidPixelFormat)) return 37;
	uint8_t packedPixels[4]{};
	storePixel(packedPixels, rgb565, encodePixel({{1.0f, 0.5f, 0.25f, 1.0f}}, rgb565));
	if (loadPixel(packedPixels, rgb565) != 0xfc08u) return 38;
	const auto decoded565 = decodePixel(0xfc08u, rgb565);
	if (decoded565[0] != 1.0f || std::fabs(decoded565[1] - 32.0f / 63.0f) > 1.0e-6f ||
		std::fabs(decoded565[2] - 8.0f / 31.0f) > 1.0e-6f || decoded565[3] != 1.0f) return 39;
	for (uint32_t width = 1u; width <= 8u; ++width) {
		PixelFormatLayout layout{};
		const uint32_t maximum = (1u << width) - 1u;
		if (!makePixelFormatLayout(32u, maximum, 0x0000ff00u, 0x00ff0000u,
			0u, layout)) return 43;
		for (uint32_t value = 0u; value <= maximum; ++value) {
			const float reference = static_cast<float>(value) / static_cast<float>(maximum);
			if (decodePixelChannel(value, layout.red, 0.0f) != reference) return 44;
		}
	}
	const uint32_t encoded32 = encodePixel({{0.25f, 0.5f, 0.75f, 1.0f}}, argb8888);
	storePixel(packedPixels, argb8888, encoded32);
	if (encoded32 != 0xff4080bfu || loadPixel(packedPixels, argb8888) != encoded32) return 40;
	uint8_t vertexBytes[40]{};
	const float decodedPosition[3] = {1.25f, -2.0f, 3.5f};
	const float decodedNormal[3] = {0.0f, 0.0f, 1.0f};
	const float decodedUv[2] = {0.25f, 0.75f};
	std::memcpy(vertexBytes, decodedPosition, sizeof(decodedPosition));
	std::memcpy(vertexBytes + fvf1d2.normalOffset, decodedNormal, sizeof(decodedNormal));
	const uint32_t vertexDiffuse = 0xff80c040u, vertexSpecular = 0x406080a0u;
	std::memcpy(vertexBytes + fvf1d2.diffuseOffset, &vertexDiffuse, 4u);
	std::memcpy(vertexBytes + fvf1d2.specularOffset, &vertexSpecular, 4u);
	std::memcpy(vertexBytes + fvf1d2.textureOffset[0], decodedUv, sizeof(decodedUv));
	DecodedVertex decoded{};
	if (!decodeVertex(vertexBytes, fvf1d2, decoded) || decoded.position[0] != 1.25f ||
		decoded.position[1] != -2.0f || decoded.position[2] != 3.5f ||
		decoded.normal[2] != 1.0f || decoded.diffuse != vertexDiffuse ||
		decoded.specular != vertexSpecular || decoded.texture[0][0] != 0.25f ||
		decoded.texture[0][1] != 0.75f) return 29;
	if (!supportsBlendFactors(kBlendOne, kBlendOne) ||
		!supportsBlendFactors(kBlendSrcAlpha, kBlendOne) ||
		!supportsBlendFactors(kBlendSrcAlpha, kBlendInvSrcAlpha) ||
		!supportsBlendFactors(kBlendDestColor, kBlendInvDestAlpha) ||
		!supportsBlendFactors(kBlendBothSrcAlpha, kBlendZero) ||
		!supportsBlendFactors(kBlendBothInvSrcAlpha, kBlendZero)) return 1;
	if (supportsBlendFactors(0u, kBlendOne) || supportsBlendFactors(kBlendOne, 14u)) return 2;
	std::array<std::array<uint32_t, 32>, 8> stages{};
	initializeDefaultTextureStageStates(stages);
	if (stages[0][1] != kTextureOpModulate || stages[0][13] != 1u ||
		stages[0][14] != 1u || stages[1][1] != kTextureOpDisable ||
		stages[1][4] != kTextureOpDisable || stages[1][13] != 1u) return 17;
	std::array<uint32_t, 256> render{};
	render[7] = 1u; render[14] = 1u; render[15] = 0u; render[19] = 5u; render[20] = 6u;
	render[23] = 4u; render[27] = 0u; render[28] = 1u; render[29] = 1u;
	render[35] = 0u; render[137] = 0u; render[140] = 3u;
	stages[0][16] = 2u; stages[0][17] = 2u; stages[0][18] = 1u;
	stages[1][1] = kTextureOpModulate2x;
	stages[1][2] = kTextureArgTexture; stages[1][3] = kTextureArgCurrent;
	stages[1][4] = kTextureOpModulate;
	stages[1][5] = kTextureArgTexture; stages[1][6] = kTextureArgCurrent;
	stages[1][11] = 1u; stages[1][16] = 2u; stages[1][17] = 2u; stages[1][18] = 1u;
	if (!supportsObservedTwoTextureDraw(0x2c2u, render, stages)) return 18;
	if (supportsObservedTwoTextureDraw(0x142u, render, stages)) return 19;
	render[27] = 1u;
	if (supportsObservedTwoTextureDraw(0x2c2u, render, stages)) return 23;
	render[27] = 0u;
	render[35] = 1u;
	if (supportsObservedTwoTextureDraw(0x2c2u, render, stages)) return 20;
	render[35] = 0u; stages[1][11] = 0u;
	if (supportsObservedTwoTextureDraw(0x2c2u, render, stages)) return 21;
	std::array<std::array<uint32_t, 32>, 8> unlitStages{};
	initializeDefaultTextureStageStates(unlitStages);
	unlitStages[0][16] = unlitStages[0][17] = 2u;
	unlitStages[0][18] = 1u;
	std::array<uint32_t, 256> unlitRender{};
	unlitRender[7] = 1u; unlitRender[14] = 0u; unlitRender[15] = 1u;
	unlitRender[19] = 5u; unlitRender[20] = 6u; unlitRender[23] = 4u;
	unlitRender[24] = 2u; unlitRender[25] = 7u; unlitRender[27] = 1u;
	if (!supportsUnlitAlphaTextureDraw(0x152u, unlitRender, unlitStages) ||
		!supportsUnlitAlphaTextureDraw(0x142u, unlitRender, unlitStages) ||
		supportsUnlitAlphaTextureDraw(0x1d2u, unlitRender, unlitStages)) return 24;
	if (!supportsAlphaTest(0u, 0xffffffffu, 0u) || !supportsAlphaTest(1u, 0u, 7u) ||
		!supportsAlphaTest(1u, 255u, 1u) || supportsAlphaTest(2u, 0u, 7u) ||
		supportsAlphaTest(1u, 256u, 7u) || supportsAlphaTest(1u, 0u, 9u)) return 12;
	if (!supportsFogConfiguration(0u, 0u, 0u, false) ||
		!supportsFogConfiguration(1u, 0u, 0u, false) ||
		!supportsFogConfiguration(1u, 0u, 3u, true) ||
		supportsFogConfiguration(1u, 1u, 0u, true) ||
		supportsFogConfiguration(1u, 0u, 1u, false) ||
		supportsFogConfiguration(2u, 0u, 0u, true)) return 36;
	std::vector<uint16_t> indices;
	if (!expandTrianglePrimitive(kPrimitiveTriangleList, 6u, indices) ||
		indices != std::vector<uint16_t>{0u, 1u, 2u, 3u, 4u, 5u}) return 8;
	if (!expandTrianglePrimitive(kPrimitiveTriangleStrip, 4u, indices) ||
		indices != std::vector<uint16_t>{0u, 1u, 2u, 2u, 1u, 3u}) return 9;
	if (!expandTrianglePrimitive(kPrimitiveTriangleFan, 5u, indices) ||
		indices != std::vector<uint16_t>{0u, 1u, 2u, 0u, 2u, 3u, 0u, 3u, 4u}) return 10;
	if (expandTrianglePrimitive(kPrimitiveTriangleList, 4u, indices) ||
		expandTrianglePrimitive(kPrimitiveTriangleStrip, 2u, indices) ||
		expandTrianglePrimitive(3u, 4u, indices) ||
		expandTrianglePrimitive(kPrimitiveTriangleFan, 65536u, indices)) return 11;

	const BlendColor source{0.8f, 0.4f, 0.2f, 0.25f};
	const BlendColor destination{0.2f, 0.6f, 0.8f, 0.5f};
	const BlendColor additive = blend(kBlendOne, kBlendOne, source, destination);
	if (std::fabs(additive.r - 1.0f) > 1.0e-6f ||
		std::fabs(additive.g - 1.0f) > 1.0e-6f ||
		std::fabs(additive.b - 1.0f) > 1.0e-6f) return 3;
	const BlendColor alpha = blend(kBlendSrcAlpha, kBlendInvSrcAlpha, source, destination);
	if (std::fabs(alpha.r - 0.35f) > 1.0e-6f ||
		std::fabs(alpha.g - 0.55f) > 1.0e-6f ||
		std::fabs(alpha.b - 0.65f) > 1.0e-6f) return 4;
	const BlendColor both = blend(kBlendBothSrcAlpha, kBlendZero, source, destination);
	if (std::fabs(both.r - 0.35f) > 1.0e-6f ||
		std::fabs(both.g - 0.55f) > 1.0e-6f ||
		std::fabs(both.b - 0.65f) > 1.0e-6f) return 5;
	const BlendColor bothInverse = blend(kBlendBothInvSrcAlpha, kBlendZero, source, destination);
	if (std::fabs(bothInverse.r - 0.65f) > 1.0e-6f ||
		std::fabs(bothInverse.g - 0.45f) > 1.0e-6f ||
		std::fabs(bothInverse.b - 0.35f) > 1.0e-6f) return 6;
	const BlendColor saturated = blend(kBlendSrcAlphaSat, kBlendZero, source, destination);
	if (std::fabs(saturated.r - 0.2f) > 1.0e-6f ||
		std::fabs(saturated.g - 0.1f) > 1.0e-6f ||
		std::fabs(saturated.b - 0.05f) > 1.0e-6f ||
		std::fabs(saturated.a - source.a) > 1.0e-6f) return 7;
	const BlendColor diffuse{0.5f, 0.25f, 1.0f, 0.5f};
	const BlendColor tex0{0.5f, 0.5f, 0.5f, 0.5f};
	const BlendColor tex1{0.5f, 1.0f, 0.25f, 0.5f};
	BlendColor stage0{}, stage1Color{}, stage1Alpha{};
	if (!applyTextureOp(kTextureOpModulate, kTextureArgTexture, kTextureArgDiffuse,
		tex0, diffuse, diffuse, stage0) ||
		!applyTextureOp(kTextureOpModulate2x, kTextureArgTexture, kTextureArgCurrent,
			tex1, stage0, diffuse, stage1Color) ||
		!applyTextureOp(kTextureOpModulate, kTextureArgTexture, kTextureArgCurrent,
			tex1, stage0, diffuse, stage1Alpha)) return 13;
	if (std::fabs(stage0.r - 0.25f) > 1.0e-6f ||
		std::fabs(stage0.g - 0.125f) > 1.0e-6f ||
		std::fabs(stage1Color.r - 0.25f) > 1.0e-6f ||
		std::fabs(stage1Color.g - 0.25f) > 1.0e-6f ||
		std::fabs(stage1Color.b - 0.25f) > 1.0e-6f ||
		std::fabs(stage1Alpha.a - 0.125f) > 1.0e-6f) return 14;
	BlendColor stagesCombined{};
	if (!applyTwoTextureStages(stages, tex0, tex1, diffuse, stagesCombined) ||
		std::fabs(stagesCombined.r - 0.25f) > 1.0e-6f ||
		std::fabs(stagesCombined.g - 0.25f) > 1.0e-6f ||
		std::fabs(stagesCombined.b - 0.25f) > 1.0e-6f ||
		std::fabs(stagesCombined.a - 0.125f) > 1.0e-6f) return 22;
	BlendColor unsupported{};
	if (applyTextureOp(kTextureOpModulate2x, 0x102u, kTextureArgCurrent,
		tex1, stage0, diffuse, unsupported)) return 15;
	const BlendColor fogged = applyVertexFog(stage1Color,
		BlendColor{0.0f, 0.0f, 1.0f, 1.0f}, 0.25f);
	if (std::fabs(fogged.r - 0.0625f) > 1.0e-6f ||
		std::fabs(fogged.g - 0.0625f) > 1.0e-6f ||
		std::fabs(fogged.b - 0.8125f) > 1.0e-6f ||
		std::fabs(fogged.a - stage1Color.a) > 1.0e-6f) return 16;
	std::array<std::array<uint32_t, 32>, 8> threeStages{};
	initializeDefaultTextureStageStates(threeStages);
	threeStages[1][1] = kTextureOpSelectArg1;
	threeStages[1][2] = kTextureArgCurrent;
	threeStages[1][4] = kTextureOpSelectArg2;
	threeStages[1][5] = kTextureArgTexture;
	threeStages[1][6] = kTextureArgCurrent;
	threeStages[2][1] = kTextureOpModulate;
	threeStages[2][2] = kTextureArgCurrent;
	threeStages[2][3] = kTextureArgDiffuse;
	threeStages[2][4] = kTextureOpSelectArg1;
	threeStages[2][5] = kTextureArgCurrent;
	const std::array<BlendColor, 8> stageInputs{{tex0, tex1,
		BlendColor{0.5f, 0.5f, 0.5f, 0.5f}}};
	BlendColor threeStageResult{};
	if (activeTextureStageCount(threeStages) != 3u ||
		!applyTextureStages(threeStages, stageInputs, 3u, diffuse, threeStageResult) ||
		std::fabs(threeStageResult.r - 0.125f) > 1.0e-6f ||
		std::fabs(threeStageResult.g - 0.03125f) > 1.0e-6f ||
		std::fabs(threeStageResult.b - 0.5f) > 1.0e-6f ||
		std::fabs(threeStageResult.a - 0.25f) > 1.0e-6f) return 30;
	threeStages[1][4] = kTextureOpDisable;
	BlendColor alphaUnchanged{};
	if (!applyTextureStages(threeStages, stageInputs, 3u, diffuse, alphaUnchanged) ||
		std::fabs(alphaUnchanged.a - 0.25f) > 1.0e-6f) return 33;
	FixedFunctionMaterial lightMaterial{};
	lightMaterial.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
	lightMaterial.ambient = {0.0f, 0.0f, 0.0f, 1.0f};
	lightMaterial.specular = {1.0f, 1.0f, 1.0f, 1.0f};
	lightMaterial.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
	lightMaterial.power = 8.0f;
	FixedFunctionLight directional{};
	directional.type = 3u;
	directional.direction = {{0.0f, 0.0f, -1.0f}};
	directional.diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
	directional.specular = {1.0f, 1.0f, 1.0f, 1.0f};
	const bool lightEnabled = true;
	std::array<float, 3> transformedNormal{};
	if (!transformNormalInverseTranspose({{2.0f,0.0f,0.0f, 0.0f,1.0f,0.0f, 0.0f,0.0f,1.0f}},
		{{1.0f,1.0f,0.0f}}, transformedNormal) ||
		std::fabs(transformedNormal[0] - 0.4472136f) > 1.0e-5f ||
		std::fabs(transformedNormal[1] - 0.8944272f) > 1.0e-5f ||
		transformNormalInverseTranspose({{0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f}},
			{{0.0f,0.0f,1.0f}}, transformedNormal)) return 34;
	const LitVertexColor lit = illuminateVertex({{0.0f, 0.0f, 0.0f}},
		{{0.0f, 0.0f, 1.0f}}, lightMaterial, {0.0f, 0.0f, 0.0f, 1.0f},
		{0.5f, 0.25f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
		&directional, &lightEnabled, 1u, true);
	if (std::fabs(lit.diffuse.r - 0.5f) > 1.0e-6f ||
		std::fabs(lit.diffuse.g - 0.125f) > 1.0e-6f ||
		std::fabs(lit.diffuse.b - 0.25f) > 1.0e-6f ||
		std::fabs(lit.specular.r - 1.0f) > 1.0e-6f) return 31;
	const LitVertexColor backLit = illuminateVertex({{0.0f, 0.0f, 0.0f}},
		{{0.0f, 0.0f, -1.0f}}, lightMaterial, {0.0f, 0.0f, 0.0f, 1.0f},
		{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
		&directional, &lightEnabled, 1u, true);
	if (backLit.diffuse.r != 0.0f || backLit.diffuse.g != 0.0f || backLit.diffuse.b != 0.0f) return 32;
	lightMaterial.ambient = {0.5f, 0.5f, 0.5f, 1.0f};
	const LitVertexColor color2Ambient = illuminateVertex({{0.0f, 0.0f, 0.0f}},
		{{0.0f, 0.0f, 1.0f}}, lightMaterial, {0.2f, 0.2f, 0.2f, 1.0f},
		{1.0f, 1.0f, 1.0f, 1.0f}, {0.25f, 0.5f, 1.0f, 1.0f},
		nullptr, nullptr, 0u, true, 0u, 2u, 0u, 0u);
	if (std::fabs(color2Ambient.diffuse.r - 0.05f) > 1.0e-6f ||
		std::fabs(color2Ambient.diffuse.g - 0.1f) > 1.0e-6f ||
		std::fabs(color2Ambient.diffuse.b - 0.2f) > 1.0e-6f) return 35;
	FixedFunctionMaterial splashMaterial{};
	splashMaterial.diffuse = splashMaterial.ambient = {1.0f, 1.0f, 1.0f, 1.0f};
	std::array<uint32_t, 256> splashRenderStates{};
	splashRenderStates[kD3d7DiffuseMaterialSourceState] = 1u;  // COLOR1
	splashRenderStates[kD3d7SpecularMaterialSourceState] = 2u; // COLOR2
	splashRenderStates[kD3d7AmbientMaterialSourceState] = 1u;  // COLOR1
	splashRenderStates[kD3d7EmissiveMaterialSourceState] = 0u; // MATERIAL
	const LitVertexColor splashLit = illuminateVertexD3d7({{0.0f, 0.0f, 0.0f}},
		{{0.0f, -1.0f, 0.0f}}, splashMaterial, {1.0f, 1.0f, 1.0f, 1.0f},
		{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
		nullptr, nullptr, 0u, true, splashRenderStates);
	if (std::fabs(splashLit.diffuse.r - 1.0f) > 1.0e-6f ||
		std::fabs(splashLit.diffuse.g - 1.0f) > 1.0e-6f ||
		std::fabs(splashLit.diffuse.b - 1.0f) > 1.0e-6f) return 36;
	std::puts("PASS D3D7 blending, two-stage MODULATE/MODULATE2X, and vertex fog math");
	return 0;
}

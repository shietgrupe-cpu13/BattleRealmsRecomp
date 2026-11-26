#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

namespace br::runtime::d3d7 {

enum class PositionKind : uint8_t { Invalid, XYZ, XYZRHW, XYZW, XYZBlended };

struct FvfLayout {
	PositionKind position = PositionKind::Invalid;
	uint32_t positionOffset = 0u;
	uint32_t positionBytes = 0u;
	uint32_t normalOffset = UINT32_MAX;
	uint32_t pointSizeOffset = UINT32_MAX;
	uint32_t diffuseOffset = UINT32_MAX;
	uint32_t specularOffset = UINT32_MAX;
	uint32_t blendWeightOffset = UINT32_MAX;
	uint8_t blendWeightCount = 0u;
	bool lastBlendWeightIsPackedIndices = false;
	uint8_t textureCount = 0u;
	std::array<uint32_t, 8> textureOffset{};
	std::array<uint8_t, 8> textureDimensions{};
	uint32_t stride = 0u;
};

// Decode the Direct3D 7 Flexible Vertex Format bit layout once and use the
// resulting offsets throughout the transform/raster pipeline. Texture-size
// fields use D3DFVF_TEXTUREFORMAT{1,2,3,4}: 3->1 component, 0->2, 1->3,
// and 2->4 components.
constexpr bool decodeFvf(uint32_t fvf, FvfLayout& layout)
{
	constexpr uint32_t kPositionMask = 0x0000400eu;
	constexpr uint32_t kNormal = 0x00000010u;
	constexpr uint32_t kPointSize = 0x00000020u;
	constexpr uint32_t kDiffuse = 0x00000040u;
	constexpr uint32_t kSpecular = 0x00000080u;
	constexpr uint32_t kTextureCountMask = 0x00000f00u;
	constexpr uint32_t kTextureDimensionsMask = 0x00ff0000u;
	constexpr uint32_t kLastBetaUbyte4 = 0x00001000u;
	constexpr uint32_t kLastBetaD3dColor = 0x00008000u;
	constexpr uint32_t kKnownMask = kPositionMask | kNormal | kPointSize | kDiffuse |
		kSpecular | kTextureCountMask | kTextureDimensionsMask |
		kLastBetaUbyte4 | kLastBetaD3dColor;
	layout = {};
	if ((fvf & ~kKnownMask) || (fvf & kLastBetaUbyte4 && fvf & kLastBetaD3dColor)) return false;
	const uint32_t position = fvf & kPositionMask;
	uint32_t offset = 0u;
	switch (position) {
	case 0x00000002u:
		layout.position = PositionKind::XYZ; layout.positionBytes = 12u; break;
	case 0x00000004u:
		layout.position = PositionKind::XYZRHW; layout.positionBytes = 16u; break;
	case 0x00004002u:
		layout.position = PositionKind::XYZW; layout.positionBytes = 16u; break;
	case 0x00000006u: case 0x00000008u: case 0x0000000au:
	case 0x0000000cu: case 0x0000000eu:
		layout.position = PositionKind::XYZBlended;
		layout.blendWeightCount = static_cast<uint8_t>((position - 4u) / 2u);
		layout.positionBytes = 12u + uint32_t(layout.blendWeightCount) * 4u;
		layout.blendWeightOffset = 12u;
		layout.lastBlendWeightIsPackedIndices = (fvf & (kLastBetaUbyte4 | kLastBetaD3dColor)) != 0u;
		break;
	default: return false;
	}
	layout.positionOffset = offset;
	offset += layout.positionBytes;
	if (fvf & kNormal) {
		if (layout.position == PositionKind::XYZRHW || layout.position == PositionKind::XYZW) return false;
		layout.normalOffset = offset; offset += 12u;
	}
	if (fvf & kPointSize) { layout.pointSizeOffset = offset; offset += 4u; }
	if (fvf & kDiffuse) { layout.diffuseOffset = offset; offset += 4u; }
	if (fvf & kSpecular) { layout.specularOffset = offset; offset += 4u; }
	layout.textureCount = static_cast<uint8_t>((fvf & kTextureCountMask) >> 8u);
	if (layout.textureCount > 8u) return false;
	for (uint32_t i = 0; i < layout.textureCount; ++i) {
		const uint32_t format = (fvf >> (16u + i * 2u)) & 3u;
		const uint8_t dimensions = static_cast<uint8_t>(format == 3u ? 1u : format == 0u ? 2u : format == 1u ? 3u : 4u);
		layout.textureOffset[i] = offset;
		layout.textureDimensions[i] = dimensions;
		offset += uint32_t(dimensions) * 4u;
	}
	if (layout.textureCount < 8u) {
		const uint32_t unusedBits = 0x00ff0000u << (layout.textureCount * 2u);
		if (fvf & unusedBits) return false;
	}
	layout.stride = offset;
	return offset != 0u;
}

struct DecodedVertex {
	std::array<float, 4> position{{0.0f, 0.0f, 0.0f, 1.0f}};
	std::array<float, 3> normal{};
	float pointSize = 1.0f;
	uint32_t diffuse = 0xffffffffu;
	uint32_t specular = 0u;
	std::array<std::array<float, 4>, 8> texture{};
};

inline bool decodeVertex(const uint8_t* bytes, const FvfLayout& layout, DecodedVertex& vertex)
{
	if (!bytes || !layout.stride || layout.stride < layout.positionBytes) return false;
	auto readFloat = [bytes](uint32_t offset, float& value) {
		std::memcpy(&value, bytes + offset, sizeof(value));
		return true;
	};
	for (uint32_t i = 0; i < 3u; ++i) readFloat(layout.positionOffset + i * 4u, vertex.position[i]);
	if (layout.position == PositionKind::XYZRHW || layout.position == PositionKind::XYZW)
		readFloat(layout.positionOffset + 12u, vertex.position[3]);
	if (layout.normalOffset != UINT32_MAX)
		for (uint32_t i = 0; i < 3u; ++i) readFloat(layout.normalOffset + i * 4u, vertex.normal[i]);
	if (layout.pointSizeOffset != UINT32_MAX) readFloat(layout.pointSizeOffset, vertex.pointSize);
	if (layout.diffuseOffset != UINT32_MAX) std::memcpy(&vertex.diffuse, bytes + layout.diffuseOffset, 4u);
	if (layout.specularOffset != UINT32_MAX) std::memcpy(&vertex.specular, bytes + layout.specularOffset, 4u);
	for (uint32_t set = 0; set < layout.textureCount; ++set) {
		vertex.texture[set] = {{0.0f, 0.0f, 0.0f, 1.0f}};
		for (uint32_t component = 0; component < layout.textureDimensions[set]; ++component)
			readFloat(layout.textureOffset[set] + component * 4u, vertex.texture[set][component]);
	}
	return true;
}

// Direct3D 7 primitive type values from d3dtypes.h. DrawPrimitive supplies
// vertices in order, so normalize supported triangle topologies to the indexed
// triangle-list form consumed by the existing rasterizer.
constexpr uint32_t kPrimitiveTriangleList = 4u;
constexpr uint32_t kPrimitiveTriangleStrip = 5u;
constexpr uint32_t kPrimitiveTriangleFan = 6u;

constexpr bool supportsAlphaTest(uint32_t enabled, uint32_t reference,
	uint32_t comparison)
{
	if (enabled > 1u) return false;
	if (!enabled) return true;
	// D3DCMP_NEVER through D3DCMP_ALWAYS are values 1 through 8. AlphaRef is
	// an 8-bit threshold; the rasterizer implements all eight comparisons.
	return reference <= 255u && comparison >= 1u && comparison <= 8u;
}

constexpr bool supportsFogConfiguration(uint32_t enabled, uint32_t tableMode,
	uint32_t vertexMode, bool hasVertexFogFactor)
{
	if (enabled > 1u || tableMode > 3u || vertexMode > 3u) return false;
	if (!enabled || (tableMode == 0u && vertexMode == 0u)) return true;
	// The software path consumes interpolated vertex specular alpha as the
	// Direct3D vertex-fog factor. Table-based fog needs per-pixel depth lookup.
	return tableMode == 0u && vertexMode != 0u && hasVertexFogFactor;
}

inline bool expandTrianglePrimitive(uint32_t primitiveType, uint32_t vertexCount,
	std::vector<uint16_t>& indices)
{
	indices.clear();
	if (!vertexCount || vertexCount > 65535u) return false;
	size_t indexCount = 0;
	switch (primitiveType) {
	case kPrimitiveTriangleList:
		if (vertexCount % 3u) return false;
		indexCount = vertexCount;
		break;
	case kPrimitiveTriangleStrip:
	case kPrimitiveTriangleFan:
		if (vertexCount < 3u) return false;
		indexCount = size_t(vertexCount - 2u) * 3u;
		break;
	default:
		return false;
	}
	indices.reserve(indexCount);
	if (primitiveType == kPrimitiveTriangleList) {
		for (uint32_t i = 0; i < vertexCount; ++i)
			indices.push_back(static_cast<uint16_t>(i));
	} else if (primitiveType == kPrimitiveTriangleStrip) {
		for (uint32_t i = 0; i + 2u < vertexCount; ++i) {
			// D3D strips reverse the first two indices on odd triangles so the
			// winding remains consistent with a triangle list.
			if (i & 1u) {
				indices.push_back(static_cast<uint16_t>(i + 1u));
				indices.push_back(static_cast<uint16_t>(i));
			} else {
				indices.push_back(static_cast<uint16_t>(i));
				indices.push_back(static_cast<uint16_t>(i + 1u));
			}
		indices.push_back(static_cast<uint16_t>(i + 2u));
		}
	} else {
		for (uint32_t i = 1; i + 1u < vertexCount; ++i) {
			indices.push_back(0u);
			indices.push_back(static_cast<uint16_t>(i));
			indices.push_back(static_cast<uint16_t>(i + 1u));
		}
	}
	return indices.size() == indexCount;
}

// Direct3D 7 D3DBLEND values (d3dtypes.h). Keep interpretation generic so
// draw support depends on the actual factors, not a caller or captured pair.
constexpr uint32_t kBlendZero = 1u;
constexpr uint32_t kBlendOne = 2u;
constexpr uint32_t kBlendSrcColor = 3u;
constexpr uint32_t kBlendInvSrcColor = 4u;
constexpr uint32_t kBlendSrcAlpha = 5u;
constexpr uint32_t kBlendInvSrcAlpha = 6u;
constexpr uint32_t kBlendDestAlpha = 7u;
constexpr uint32_t kBlendInvDestAlpha = 8u;
constexpr uint32_t kBlendDestColor = 9u;
constexpr uint32_t kBlendInvDestColor = 10u;
constexpr uint32_t kBlendSrcAlphaSat = 11u;
constexpr uint32_t kBlendBothSrcAlpha = 12u;
constexpr uint32_t kBlendBothInvSrcAlpha = 13u;

struct BlendColor {
	float r;
	float g;
	float b;
	float a;
};

struct TextureFootprintVertex {
	float x;
	float y;
	float invW;
	float u;
	float v;
};

// Select the Direct3D minification filter when a screen pixel covers more
// than one texel. The estimate is evaluated at the triangle centroid so the
// decision is stable for a primitive and does not add work to every fragment.
inline bool textureFootprintIsMinified(
	const std::array<TextureFootprintVertex, 3>& vertices,
	uint32_t textureWidth, uint32_t textureHeight)
{
	if (!textureWidth || !textureHeight) return true;
	const auto& a = vertices[0];
	const auto& b = vertices[1];
	const auto& c = vertices[2];
	const float area = (b.x - a.x) * (c.y - a.y) -
		(b.y - a.y) * (c.x - a.x);
	if (!std::isfinite(area) || std::fabs(area) < 1.0e-8f) return true;
	const float inverseArea = 1.0f / area;
	const float dWdx[3] = {
		(b.y - c.y) * inverseArea,
		(c.y - a.y) * inverseArea,
		(a.y - b.y) * inverseArea};
	const float dWdy[3] = {
		(c.x - b.x) * inverseArea,
		(a.x - c.x) * inverseArea,
		(b.x - a.x) * inverseArea};
	const float qValues[3] = {a.invW, b.invW, c.invW};
	const float uValues[3] = {a.u * a.invW, b.u * b.invW, c.u * c.invW};
	const float vValues[3] = {a.v * a.invW, b.v * b.invW, c.v * c.invW};
	const float q = (qValues[0] + qValues[1] + qValues[2]) / 3.0f;
	if (!(q > 0.0f) || !std::isfinite(q)) return true;
	const float u = (uValues[0] + uValues[1] + uValues[2]) / 3.0f;
	const float v = (vValues[0] + vValues[1] + vValues[2]) / 3.0f;
	float dqdx = 0.0f, dqdy = 0.0f;
	float dudxNumerator = 0.0f, dudyNumerator = 0.0f;
	float dvdxNumerator = 0.0f, dvdyNumerator = 0.0f;
	for (uint32_t index = 0u; index < 3u; ++index) {
		dqdx += dWdx[index] * qValues[index];
		dqdy += dWdy[index] * qValues[index];
		dudxNumerator += dWdx[index] * uValues[index];
		dudyNumerator += dWdy[index] * uValues[index];
		dvdxNumerator += dWdx[index] * vValues[index];
		dvdyNumerator += dWdy[index] * vValues[index];
	}
	const float inverseQSquared = 1.0f / (q * q);
	const float dudx = (dudxNumerator * q - u * dqdx) * inverseQSquared;
	const float dudy = (dudyNumerator * q - u * dqdy) * inverseQSquared;
	const float dvdx = (dvdxNumerator * q - v * dqdx) * inverseQSquared;
	const float dvdy = (dvdyNumerator * q - v * dqdy) * inverseQSquared;
	if (!std::isfinite(dudx) || !std::isfinite(dudy) ||
		!std::isfinite(dvdx) || !std::isfinite(dvdy)) return true;
	const float texelWidth = static_cast<float>(textureWidth);
	const float texelHeight = static_cast<float>(textureHeight);
	const float footprintX = (dudx * texelWidth) * (dudx * texelWidth) +
		(dvdx * texelHeight) * (dvdx * texelHeight);
	const float footprintY = (dudy * texelWidth) * (dudy * texelWidth) +
		(dvdy * texelHeight) * (dvdy * texelHeight);
	return !std::isfinite(footprintX) || !std::isfinite(footprintY) ||
		std::max(footprintX, footprintY) > 1.0f;
}

struct PixelChannelLayout {
	uint32_t mask = 0u;
	uint32_t maximum = 0u;
	uint8_t shift = 0u;
};

struct PixelFormatLayout {
	uint8_t bytesPerPixel = 0u;
	PixelChannelLayout red{};
	PixelChannelLayout green{};
	PixelChannelLayout blue{};
	PixelChannelLayout alpha{};
};

inline bool makePixelFormatLayout(uint32_t bitsPerPixel, uint32_t redMask,
	uint32_t greenMask, uint32_t blueMask, uint32_t alphaMask,
	PixelFormatLayout& layout)
{
	if (bitsPerPixel != 16u && bitsPerPixel != 24u && bitsPerPixel != 32u) return false;
	layout = {};
	layout.bytesPerPixel = static_cast<uint8_t>((bitsPerPixel + 7u) / 8u);
	auto decodeMask = [](uint32_t mask, PixelChannelLayout& channel) {
		channel.mask = mask;
		if (!mask) return true;
		while (channel.shift < 32u && ((mask >> channel.shift) & 1u) == 0u) ++channel.shift;
		channel.maximum = mask >> channel.shift;
		return channel.maximum && (channel.maximum << channel.shift) == mask &&
			(channel.maximum & (channel.maximum + 1u)) == 0u;
	};
	return decodeMask(redMask, layout.red) && decodeMask(greenMask, layout.green) &&
		decodeMask(blueMask, layout.blue) && decodeMask(alphaMask, layout.alpha) &&
		redMask && greenMask && blueMask;
}

inline uint32_t loadPixel(const uint8_t* address, const PixelFormatLayout& format)
{
	uint32_t pixel = 0u;
	if (format.bytesPerPixel == 2u || format.bytesPerPixel == 4u) {
		std::memcpy(&pixel, address, format.bytesPerPixel);
	} else {
		for (uint32_t i = 0u; i < format.bytesPerPixel; ++i)
			pixel |= uint32_t(address[i]) << (i * 8u);
	}
	return pixel;
}

inline void storePixel(uint8_t* address, const PixelFormatLayout& format, uint32_t pixel)
{
	if (format.bytesPerPixel == 2u || format.bytesPerPixel == 4u) {
		std::memcpy(address, &pixel, format.bytesPerPixel);
	} else {
		for (uint32_t i = 0u; i < format.bytesPerPixel; ++i)
			address[i] = static_cast<uint8_t>(pixel >> (i * 8u));
	}
}

inline float decodePixelChannel(uint32_t pixel, const PixelChannelLayout& channel,
	float fallback)
{
	if (!channel.mask || !channel.maximum) return fallback;
	const uint32_t value = (pixel & channel.mask) >> channel.shift;
	if (channel.maximum <= 255u) {
		static const auto normalized = [] {
			std::array<std::array<float, 256>, 256> table{};
			for (uint32_t maximum = 1u; maximum <= 255u; ++maximum)
				for (uint32_t sample = 0u; sample <= maximum; ++sample)
					table[maximum][sample] = static_cast<float>(sample) /
						static_cast<float>(maximum);
			return table;
		}();
		return normalized[channel.maximum][value];
	}
	return static_cast<float>(value) / static_cast<float>(channel.maximum);
}

inline std::array<float, 4> decodePixel(uint32_t pixel, const PixelFormatLayout& format)
{
	return {{decodePixelChannel(pixel, format.red, 0.0f),
		decodePixelChannel(pixel, format.green, 0.0f),
		decodePixelChannel(pixel, format.blue, 0.0f),
		decodePixelChannel(pixel, format.alpha, 1.0f)}};
}

inline uint32_t encodePixelChannel(float value, const PixelChannelLayout& channel)
{
	if (!channel.mask || !channel.maximum) return 0u;
	const float clamped = std::isfinite(value) ? std::max(0.0f, std::min(1.0f, value)) : 0.0f;
	return (static_cast<uint32_t>(clamped * static_cast<float>(channel.maximum) + 0.5f)
		<< channel.shift) & channel.mask;
}

inline uint32_t encodePixel(const std::array<float, 4>& color,
	const PixelFormatLayout& format)
{
	return encodePixelChannel(color[0], format.red) |
		encodePixelChannel(color[1], format.green) |
		encodePixelChannel(color[2], format.blue) |
		encodePixelChannel(color[3], format.alpha);
}

struct FixedFunctionLight {
	uint32_t type = 0u; // D3DLIGHT_POINT=1, SPOT=2, DIRECTIONAL=3.
	BlendColor diffuse{};
	BlendColor specular{};
	BlendColor ambient{};
	std::array<float, 3> position{};
	std::array<float, 3> direction{};
	float range = 0.0f;
	float falloff = 1.0f;
	float attenuation0 = 1.0f;
	float attenuation1 = 0.0f;
	float attenuation2 = 0.0f;
	float theta = 0.0f;
	float phi = 0.0f;
};

struct FixedFunctionMaterial {
	BlendColor diffuse{1.0f, 1.0f, 1.0f, 1.0f};
	BlendColor ambient{1.0f, 1.0f, 1.0f, 1.0f};
	BlendColor specular{0.0f, 0.0f, 0.0f, 1.0f};
	BlendColor emissive{0.0f, 0.0f, 0.0f, 1.0f};
	float power = 0.0f;
};

struct LitVertexColor {
	BlendColor diffuse{};
	BlendColor specular{};
};

inline std::array<float, 3> normalizeVector(std::array<float, 3> v)
{
	const float lengthSquared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (!(lengthSquared > 1.0e-20f) || !std::isfinite(lengthSquared)) return {{0.0f, 0.0f, 0.0f}};
	const float inverseLength = 1.0f / std::sqrt(lengthSquared);
	return {{v[0] * inverseLength, v[1] * inverseLength, v[2] * inverseLength}};
}

inline bool transformNormalInverseTranspose(const std::array<float, 9>& matrix,
	const std::array<float, 3>& normal, std::array<float, 3>& transformed)
{
	const float a=matrix[0], b=matrix[1], c=matrix[2], d=matrix[3], e=matrix[4], f=matrix[5];
	const float g=matrix[6], h=matrix[7], i=matrix[8];
	const std::array<float, 9> cofactors{{e*i-f*h, f*g-d*i, d*h-e*g,
		c*h-b*i, a*i-c*g, b*g-a*h, b*f-c*e, c*d-a*f, a*e-b*d}};
	const float determinant = a*cofactors[0] + b*cofactors[1] + c*cofactors[2];
	if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12f) return false;
	const float inverseDeterminant = 1.0f / determinant;
	for (uint32_t column = 0u; column < 3u; ++column)
		transformed[column] = (normal[0]*cofactors[column] + normal[1]*cofactors[3u+column] +
			normal[2]*cofactors[6u+column]) * inverseDeterminant;
	transformed = normalizeVector(transformed);
	return true;
}

// D3D7 Gouraud fixed-function lighting for the common vertex-color pipeline.
// Inputs are in the same coordinate space; directional light direction points
// along the emitted ray, so the vector toward the light is its negation.
inline LitVertexColor illuminateVertex(const std::array<float, 3>& position,
	const std::array<float, 3>& normal, const FixedFunctionMaterial& material,
	const BlendColor& globalAmbient, const BlendColor& vertexDiffuse,
	const BlendColor& vertexSpecular, const FixedFunctionLight* lights,
	const bool* lightEnabled, size_t lightCount, bool colorVertex = true,
	uint32_t diffuseSource = 1u, uint32_t ambientSource = 0u,
	uint32_t specularSource = 0u, uint32_t emissiveSource = 0u)
{
	const auto saturate = [](float value) { return std::max(0.0f, std::min(1.0f, value)); };
	const std::array<float, 3> n = normalizeVector(normal);
	const std::array<float, 3> eye = normalizeVector({{-position[0], -position[1], -position[2]}});
	const auto selectSource = [&](uint32_t source, const BlendColor& materialColor) {
		if (!colorVertex || source == 0u) return materialColor;
		return source == 1u ? vertexDiffuse : source == 2u ? vertexSpecular : BlendColor{};
	};
	const BlendColor diffuseMaterial = selectSource(diffuseSource, material.diffuse);
	const BlendColor ambientMaterial = selectSource(ambientSource, material.ambient);
	const BlendColor specularMaterial = selectSource(specularSource, material.specular);
	const BlendColor emissiveMaterial = selectSource(emissiveSource, material.emissive);
	BlendColor base = emissiveMaterial;
	base.r += ambientMaterial.r * globalAmbient.r;
	base.g += ambientMaterial.g * globalAmbient.g;
	base.b += ambientMaterial.b * globalAmbient.b;
	BlendColor specular{0.0f, 0.0f, 0.0f, vertexSpecular.a};
	for (size_t i = 0; i < lightCount; ++i) {
		if (!lights || !lightEnabled || !lightEnabled[i]) continue;
		const FixedFunctionLight& light = lights[i];
		std::array<float, 3> toLight{};
		float attenuation = 1.0f;
		if (light.type == 3u) {
			toLight = normalizeVector({{-light.direction[0], -light.direction[1], -light.direction[2]}});
		} else if (light.type == 1u || light.type == 2u) {
			const std::array<float, 3> delta{{light.position[0] - position[0],
				light.position[1] - position[1], light.position[2] - position[2]}};
			const float distanceSquared = delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2];
			const float distance = std::sqrt(std::max(0.0f, distanceSquared));
			if (light.range > 0.0f && distance > light.range) continue;
			toLight = distance > 1.0e-10f
				? std::array<float, 3>{{delta[0]/distance, delta[1]/distance, delta[2]/distance}}
				: std::array<float, 3>{{0.0f, 0.0f, 1.0f}};
			const float denominator = light.attenuation0 + light.attenuation1 * distance +
				light.attenuation2 * distanceSquared;
			if (!(denominator > 1.0e-20f)) continue;
			attenuation = 1.0f / denominator;
			if (light.type == 2u) {
				const auto fromLight = std::array<float, 3>{{-toLight[0], -toLight[1], -toLight[2]}};
				const auto spotDirection = normalizeVector(light.direction);
				const float cosine = fromLight[0]*spotDirection[0] + fromLight[1]*spotDirection[1] + fromLight[2]*spotDirection[2];
				const float outer = std::cos(light.phi * 0.5f), inner = std::cos(light.theta * 0.5f);
				if (cosine < outer) continue;
				if (cosine < inner && inner > outer)
					attenuation *= std::pow((cosine - outer) / (inner - outer), std::max(0.0f, light.falloff));
			}
		} else continue;
		const float ndotl = std::max(0.0f, n[0]*toLight[0] + n[1]*toLight[1] + n[2]*toLight[2]);
		base.r += ambientMaterial.r * light.ambient.r + diffuseMaterial.r * light.diffuse.r * ndotl * attenuation;
		base.g += ambientMaterial.g * light.ambient.g + diffuseMaterial.g * light.diffuse.g * ndotl * attenuation;
		base.b += ambientMaterial.b * light.ambient.b + diffuseMaterial.b * light.diffuse.b * ndotl * attenuation;
		if (ndotl > 0.0f && material.power > 0.0f) {
			const auto halfVector = normalizeVector({{toLight[0]+eye[0], toLight[1]+eye[1], toLight[2]+eye[2]}});
			const float ndoth = std::pow(std::max(0.0f, n[0]*halfVector[0] + n[1]*halfVector[1] + n[2]*halfVector[2]), material.power) * attenuation;
			specular.r += specularMaterial.r * light.specular.r * ndoth;
			specular.g += specularMaterial.g * light.specular.g * ndoth;
			specular.b += specularMaterial.b * light.specular.b * ndoth;
		}
	}
	base.a = diffuseMaterial.a;
	specular.r = saturate(specular.r); specular.g = saturate(specular.g); specular.b = saturate(specular.b);
	base.r = saturate(base.r); base.g = saturate(base.g); base.b = saturate(base.b); base.a = saturate(base.a);
	return {base, specular};
}

// D3D7's material-source render-state IDs are not ordered the same way as the
// argument list above: SPECULARMATERIALSOURCE (146) precedes
// AMBIENTMATERIALSOURCE (147). Keep this mapping in one tested adapter so a
// caller cannot accidentally feed the specular source into ambient lighting.
constexpr size_t kD3d7DiffuseMaterialSourceState = 145u;
constexpr size_t kD3d7SpecularMaterialSourceState = 146u;
constexpr size_t kD3d7AmbientMaterialSourceState = 147u;
constexpr size_t kD3d7EmissiveMaterialSourceState = 148u;

template <size_t RenderStateCount>
inline LitVertexColor illuminateVertexD3d7(const std::array<float, 3>& position,
	const std::array<float, 3>& normal, const FixedFunctionMaterial& material,
	const BlendColor& globalAmbient, const BlendColor& vertexDiffuse,
	const BlendColor& vertexSpecular, const FixedFunctionLight* lights,
	const bool* lightEnabled, size_t lightCount, bool colorVertex,
	const std::array<uint32_t, RenderStateCount>& renderState)
{
	static_assert(RenderStateCount > kD3d7EmissiveMaterialSourceState,
		"D3D7 render-state storage must include material-source states");
	return illuminateVertex(position, normal, material, globalAmbient, vertexDiffuse,
		vertexSpecular, lights, lightEnabled, lightCount, colorVertex,
		renderState[kD3d7DiffuseMaterialSourceState],
		renderState[kD3d7AmbientMaterialSourceState],
		renderState[kD3d7SpecularMaterialSourceState],
		renderState[kD3d7EmissiveMaterialSourceState]);
}

constexpr uint32_t kTextureOpDisable = 1u;
constexpr uint32_t kTextureOpSelectArg1 = 2u;
constexpr uint32_t kTextureOpSelectArg2 = 3u;
constexpr uint32_t kTextureOpModulate = 4u;
constexpr uint32_t kTextureOpModulate2x = 5u;
constexpr uint32_t kTextureArgDiffuse = 0u;
constexpr uint32_t kTextureArgCurrent = 1u;
constexpr uint32_t kTextureArgTexture = 2u;

enum class TextureFilterMode : uint8_t { Point, Linear };
enum class TextureAddressMode : uint8_t { Wrap, Mirror, Clamp };

struct TextureSamplerState {
	TextureFilterMode minFilter = TextureFilterMode::Point;
	TextureFilterMode magFilter = TextureFilterMode::Point;
	TextureAddressMode addressU = TextureAddressMode::Wrap;
	TextureAddressMode addressV = TextureAddressMode::Wrap;
};

inline bool decodeD3d7TextureSamplerState(uint32_t minFilter, uint32_t magFilter,
	uint32_t addressU, uint32_t addressV, TextureSamplerState& output)
{
	const auto decodeFilter = [](uint32_t value, TextureFilterMode& decoded) {
		if (value == 1u) { decoded = TextureFilterMode::Point; return true; }
		if (value == 2u) { decoded = TextureFilterMode::Linear; return true; }
		return false;
	};
	const auto decodeAddress = [](uint32_t value, TextureAddressMode& decoded) {
		if (value == 1u) { decoded = TextureAddressMode::Wrap; return true; }
		if (value == 2u) { decoded = TextureAddressMode::Mirror; return true; }
		if (value == 3u) { decoded = TextureAddressMode::Clamp; return true; }
		return false;
	};
	TextureSamplerState decoded{};
	if (!decodeFilter(minFilter, decoded.minFilter) ||
		!decodeFilter(magFilter, decoded.magFilter) ||
		!decodeAddress(addressU, decoded.addressU) ||
		!decodeAddress(addressV, decoded.addressV)) return false;
	output = decoded;
	return true;
}

inline void initializeDefaultTextureStageStates(
	std::array<std::array<uint32_t, 32>, 8>& states)
{
	for (auto& stage : states) stage.fill(0u);
	states[0][1] = kTextureOpModulate;
	states[0][2] = kTextureArgTexture;
	states[0][3] = kTextureArgDiffuse;
	states[0][4] = kTextureOpModulate;
	states[0][5] = kTextureArgTexture;
	states[0][6] = kTextureArgDiffuse;
	states[0][11] = 0u; // TEXCOORDINDEX
	states[0][13] = states[0][14] = 1u; // WRAP
	states[0][16] = states[0][17] = 1u; // MAGFILTER/MINFILTER = POINT
	for (size_t index = 1u; index < states.size(); ++index) {
		states[index][1] = kTextureOpDisable;
		states[index][4] = kTextureOpDisable;
		states[index][11] = static_cast<uint32_t>(index); // TEXCOORDINDEX
		states[index][13] = states[index][14] = 1u;
		states[index][16] = states[index][17] = 1u;
	}
}

inline bool textureArgument(uint32_t selector, const BlendColor& texture,
	const BlendColor& current, const BlendColor& diffuse, BlendColor& output)
{
	switch (selector) {
	case kTextureArgDiffuse: output = diffuse; return true;
	case kTextureArgCurrent: output = current; return true;
	case kTextureArgTexture: output = texture; return true;
	default: return false;
	}
}

inline bool applyTextureOp(uint32_t operation, uint32_t arg1, uint32_t arg2,
	const BlendColor& texture, const BlendColor& current, const BlendColor& diffuse,
	BlendColor& output)
{
	if (operation < kTextureOpSelectArg1 || operation > kTextureOpModulate2x) return false;
	BlendColor a{}, b{};
	if (operation == kTextureOpSelectArg1)
		return textureArgument(arg1, texture, current, diffuse, output);
	if (operation == kTextureOpSelectArg2)
		return textureArgument(arg2, texture, current, diffuse, output);
	if (!textureArgument(arg1, texture, current, diffuse, a) ||
		!textureArgument(arg2, texture, current, diffuse, b)) return false;
	const float scale = operation == kTextureOpModulate2x ? 2.0f : 1.0f;
	const auto saturate = [](float value) { return std::max(0.0f, std::min(1.0f, value)); };
	output = {saturate(a.r * b.r * scale), saturate(a.g * b.g * scale),
		saturate(a.b * b.b * scale), saturate(a.a * b.a * scale)};
	return true;
}

inline bool textureOpUses(uint32_t operation, uint32_t arg1, uint32_t arg2, uint32_t selector)
{
	if (operation == kTextureOpSelectArg1) return arg1 == selector;
	if (operation == kTextureOpSelectArg2) return arg2 == selector;
	if (operation == kTextureOpModulate || operation == kTextureOpModulate2x)
		return arg1 == selector || arg2 == selector;
	return false;
}

inline uint32_t activeTextureStageCount(
	const std::array<std::array<uint32_t, 32>, 8>& states)
{
	uint32_t count = 0u;
	while (count < states.size() && states[count][1u] != kTextureOpDisable) ++count;
	return count;
}

inline bool applyTextureStages(
	const std::array<std::array<uint32_t, 32>, 8>& states,
	const std::array<BlendColor, 8>& textures, uint32_t stageCount,
	const BlendColor& diffuse, BlendColor& output)
{
	if (!stageCount || stageCount > states.size()) return false;
	BlendColor current = diffuse;
	for (uint32_t stage = 0; stage < stageCount; ++stage) {
		const auto& state = states[stage];
		BlendColor colorResult{};
		if (state[1u] == kTextureOpDisable) break;
		if (!applyTextureOp(state[1u], state[2u], state[3u], textures[stage], current, diffuse, colorResult))
			return false;
		float alpha = current.a;
		if (state[4u] != kTextureOpDisable) {
			BlendColor alphaResult{};
			if (!applyTextureOp(state[4u], state[5u], state[6u], textures[stage], current, diffuse, alphaResult))
				return false;
			alpha = alphaResult.a;
		}
		current = {colorResult.r, colorResult.g, colorResult.b, alpha};
	}
	output = current;
	return true;
}

inline bool applyTwoTextureStages(
	const std::array<std::array<uint32_t, 32>, 8>& states,
	const BlendColor& texture0, const BlendColor& texture1,
	const BlendColor& diffuse, BlendColor& output)
{
	BlendColor current = diffuse;
	const BlendColor textures[2] = {texture0, texture1};
	for (uint32_t stage = 0; stage < 2u; ++stage) {
		const auto& state = states[stage];
		BlendColor colorResult{}, alphaResult{};
		if (!applyTextureOp(state[1u], state[2u], state[3u], textures[stage],
			current, diffuse, colorResult) ||
			!applyTextureOp(state[4u], state[5u], state[6u], textures[stage],
			current, diffuse, alphaResult)) return false;
		current = {colorResult.r, colorResult.g, colorResult.b, alphaResult.a};
	}
	output = current;
	return true;
}

inline BlendColor applyVertexFog(const BlendColor& color, const BlendColor& fogColor,
	float fogFactor)
{
	const float f = std::max(0.0f, std::min(1.0f, fogFactor));
	return {color.r * f + fogColor.r * (1.0f - f),
		color.g * f + fogColor.g * (1.0f - f),
		color.b * f + fogColor.b * (1.0f - f), color.a};
}

constexpr bool isBlendFactor(uint32_t factor)
{
	return factor >= kBlendZero && factor <= kBlendBothInvSrcAlpha;
}

constexpr bool supportsBlendFactors(uint32_t sourceBlend, uint32_t destinationBlend)
{
	return isBlendFactor(sourceBlend) && isBlendFactor(destinationBlend);
}

inline bool supportsObservedTwoTextureDraw(uint32_t fvf,
	const std::array<uint32_t, 256>& renderState,
	const std::array<std::array<uint32_t, 32>, 8>& stage)
{
	return fvf == 0x000002c2u && renderState[7u] == 1u &&
		renderState[14u] == 1u && renderState[15u] == 0u &&
		renderState[27u] == 0u && renderState[28u] == 1u &&
		renderState[29u] == 1u && renderState[35u] == 0u &&
		renderState[137u] == 0u && renderState[140u] <= 3u &&
		renderState[23u] == 4u && supportsBlendFactors(renderState[19u], renderState[20u]) &&
		stage[0][1u] == kTextureOpModulate &&
		stage[0][2u] == kTextureArgTexture && stage[0][3u] == kTextureArgDiffuse &&
		stage[0][4u] == kTextureOpModulate &&
		stage[0][5u] == kTextureArgTexture && stage[0][6u] == kTextureArgDiffuse &&
		stage[0][11u] == 0u && stage[0][13u] == 1u && stage[0][14u] == 1u &&
		stage[0][16u] == 2u && stage[0][17u] == 2u && stage[0][18u] == 1u &&
		stage[1][1u] == kTextureOpModulate2x &&
		stage[1][2u] == kTextureArgTexture && stage[1][3u] == kTextureArgCurrent &&
		stage[1][4u] == kTextureOpModulate &&
		stage[1][5u] == kTextureArgTexture && stage[1][6u] == kTextureArgCurrent &&
		stage[1][11u] == 1u && stage[1][13u] == 1u && stage[1][14u] == 1u &&
		stage[1][16u] == 2u && stage[1][17u] == 2u && stage[1][18u] == 1u &&
		stage[2][1u] == kTextureOpDisable;
}

inline bool supportsUnlitAlphaTextureDraw(uint32_t fvf,
	const std::array<uint32_t, 256>& renderState,
	const std::array<std::array<uint32_t, 32>, 8>& stage)
{
	return (fvf == 0x00000142u || fvf == 0x00000152u) &&
		renderState[137u] == 0u && renderState[7u] == 1u &&
		renderState[14u] == 0u && renderState[15u] == 1u &&
		supportsAlphaTest(renderState[15u], renderState[24u], renderState[25u]) &&
		renderState[27u] == 1u &&
		supportsBlendFactors(renderState[19u], renderState[20u]) &&
		renderState[23u] == 4u &&
		stage[0][1u] == kTextureOpModulate && stage[0][2u] == kTextureArgTexture &&
		stage[0][3u] == kTextureArgDiffuse && stage[0][4u] == kTextureOpModulate &&
		stage[0][5u] == kTextureArgTexture && stage[0][6u] == kTextureArgDiffuse &&
		stage[0][11u] == 0u && stage[0][13u] == 1u && stage[0][14u] == 1u &&
		stage[0][16u] == 2u && stage[0][17u] == 2u && stage[0][18u] == 1u &&
		stage[1][1u] == kTextureOpDisable;
}

inline float channel(const BlendColor& color, uint32_t index)
{
	switch (index) {
	case 0u: return color.r;
	case 1u: return color.g;
	case 2u: return color.b;
	default: return color.a;
	}
}

inline float factor(uint32_t blend, uint32_t index,
	const BlendColor& source, const BlendColor& destination)
{
	const float src = channel(source, index);
	const float dst = channel(destination, index);
	switch (blend) {
	case kBlendZero: return 0.0f;
	case kBlendOne: return 1.0f;
	case kBlendSrcColor: return src;
	case kBlendInvSrcColor: return 1.0f - src;
	case kBlendSrcAlpha: return source.a;
	case kBlendInvSrcAlpha: return 1.0f - source.a;
	case kBlendDestAlpha: return destination.a;
	case kBlendInvDestAlpha: return 1.0f - destination.a;
	case kBlendDestColor: return dst;
	case kBlendInvDestColor: return 1.0f - dst;
	case kBlendSrcAlphaSat:
		return index == 3u ? 1.0f : std::min(source.a, 1.0f - destination.a);
	case kBlendBothSrcAlpha: return source.a;
	case kBlendBothInvSrcAlpha: return 1.0f - source.a;
	default: return 0.0f;
	}
}

inline BlendColor blend(uint32_t sourceBlend, uint32_t destinationBlend,
	const BlendColor& source, const BlendColor& destination)
{
	// These legacy source factors specify both halves of the blend equation.
	if (sourceBlend == kBlendBothSrcAlpha)
		destinationBlend = kBlendInvSrcAlpha;
	else if (sourceBlend == kBlendBothInvSrcAlpha)
		destinationBlend = kBlendSrcAlpha;
	const auto blendChannel = [&](uint32_t index) {
		return channel(source, index) * factor(sourceBlend, index, source, destination) +
			channel(destination, index) * factor(destinationBlend, index, source, destination);
	};
	return {blendChannel(0u), blendChannel(1u), blendChannel(2u), blendChannel(3u)};
}

} // namespace br::runtime::d3d7

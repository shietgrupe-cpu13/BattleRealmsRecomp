#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace br::runtime {

struct GuestFrameRect {
	uint32_t x = 0u;
	uint32_t y = 0u;
	uint32_t width = 0u;
	uint32_t height = 0u;
};

inline GuestFrameRect fitGuestFrame(uint32_t sourceWidth, uint32_t sourceHeight,
	uint32_t destinationWidth, uint32_t destinationHeight)
{
	if (!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight) return {};
	const uint64_t widthLimited = uint64_t(destinationWidth) * sourceHeight;
	const uint64_t heightLimited = uint64_t(destinationHeight) * sourceWidth;
	GuestFrameRect result;
	if (widthLimited <= heightLimited) {
		result.width = destinationWidth;
		result.height = std::max(1u, static_cast<uint32_t>(
			(uint64_t(destinationWidth) * sourceHeight) / sourceWidth));
	} else {
		result.height = destinationHeight;
		result.width = std::max(1u, static_cast<uint32_t>(
			(uint64_t(destinationHeight) * sourceWidth) / sourceHeight));
	}
	result.x = (destinationWidth - result.width) / 2u;
	result.y = (destinationHeight - result.height) / 2u;
	return result;
}

inline int32_t mapSurfaceCoordinateToGuest(float coordinate, uint32_t offset,
	uint32_t displayedExtent, uint32_t guestExtent)
{
	if (!guestExtent || !displayedExtent || !std::isfinite(coordinate)) return 0;
	const float guestCoordinate = (coordinate - static_cast<float>(offset)) *
		static_cast<float>(guestExtent) / static_cast<float>(displayedExtent);
	if (guestCoordinate <= 0.0f) return 0;
	if (guestCoordinate >= static_cast<float>(guestExtent - 1u))
		return static_cast<int32_t>(guestExtent - 1u);
	return static_cast<int32_t>(guestCoordinate);
}

inline void mapSurfacePointToGuest(float x, float y, uint32_t surfaceWidth, uint32_t surfaceHeight,
	uint32_t guestWidth, uint32_t guestHeight, int32_t& guestX, int32_t& guestY)
{
	const GuestFrameRect rect = fitGuestFrame(guestWidth, guestHeight, surfaceWidth, surfaceHeight);
	guestX = mapSurfaceCoordinateToGuest(x, rect.x, rect.width, guestWidth);
	guestY = mapSurfaceCoordinateToGuest(y, rect.y, rect.height, guestHeight);
}

inline uint8_t extractMaskedChannel(uint32_t packed, uint32_t mask)
{
	uint32_t shift = 0u;
	while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
	if (shift == 32u) return 0u;
	const uint32_t range = mask >> shift;
	if ((uint64_t(range) & (uint64_t(range) + 1u)) != 0u) return 0u;
	const uint32_t value = (packed & mask) >> shift;
	return static_cast<uint8_t>((uint64_t(value) * 255u + range / 2u) / range);
}

inline uint32_t readLittleEndianPixel(const uint8_t* bytes, uint32_t byteCount)
{
	if (!bytes || byteCount > 4u) return 0u;
	uint32_t value = 0u;
	for (uint32_t i = 0u; i < byteCount; ++i)
		value |= uint32_t(bytes[i]) << (i * 8u);
	return value;
}

} // namespace br::runtime

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace br::runtime {

struct FramebufferConversionTimings {
	uint64_t prepareNanoseconds = 0;
	uint64_t scaleMapNanoseconds = 0;
	uint64_t pixelLoopNanoseconds = 0;
	uint64_t temporaryAllocationBytes = 0;
	bool scaleMapRebuilt = false;
};

// CPU-side nearest-neighbour conversion used by the Android NativeWindow
// presenter.  Lookup tables and coordinate maps are built once per frame so
// the inner pixel loop does no divisions or per-channel mask scans.
inline bool convertFramebufferToRgba8888(const uint8_t* pixels,
	uint32_t width, uint32_t height, uint32_t pitch, uint32_t sourceBytes,
	uint32_t redMask, uint32_t greenMask, uint32_t blueMask,
	uint8_t* destination, uint32_t destinationWidth, uint32_t destinationHeight,
	uint32_t destinationStride, uint32_t offsetX, uint32_t offsetY,
	uint32_t drawWidth, uint32_t drawHeight, FramebufferConversionTimings* timings = nullptr)
{
	if (timings) *timings = {};
	if (!pixels || !destination || !width || !height || sourceBytes < 2u ||
		sourceBytes > 4u || pitch < width * sourceBytes || !redMask ||
		!greenMask || !blueMask || destinationStride < destinationWidth ||
		offsetX > destinationWidth || offsetY > destinationHeight ||
		drawWidth > destinationWidth - offsetX ||
		drawHeight > destinationHeight - offsetY || !drawWidth || !drawHeight)
		return false;

	const auto prepareStart = timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	struct Channel {
		uint32_t mask = 0u;
		uint32_t shift = 0u;
		uint32_t range = 0u;
		bool lookup = false;
		uint8_t values[256]{};
		uint8_t get(uint32_t packed) const
		{
			if (!mask) return 0u;
			const uint32_t value = (packed & mask) >> shift;
			if (lookup && value < 256u) return values[value];
			return static_cast<uint8_t>((uint64_t(value) * 255u + range / 2u) / range);
		}
	};
	auto makeChannel = [](uint32_t mask) {
		Channel channel{};
		channel.mask = mask;
		while (channel.shift < 32u && ((mask >> channel.shift) & 1u) == 0u)
			++channel.shift;
		if (channel.shift == 32u) {
			channel.mask = 0u;
			return channel;
		}
		channel.range = mask >> channel.shift;
		uint32_t bits = 0u;
		for (uint32_t value = channel.range; value; value >>= 1u) ++bits;
		const uint32_t contiguous = bits == 32u ? 0xffffffffu : ((1u << bits) - 1u);
		channel.lookup = channel.range == contiguous && bits <= 8u;
		if (channel.lookup) {
			for (uint32_t value = 0u; value <= channel.range; ++value)
				channel.values[value] = static_cast<uint8_t>(
					(uint64_t(value) * 255u + channel.range / 2u) / channel.range);
		}
		return channel;
	};
	const Channel red = makeChannel(redMask);
	const Channel green = makeChannel(greenMask);
	const Channel blue = makeChannel(blueMask);
	if (!red.range || !green.range || !blue.range) return false;

	// The game image overwrites every pixel in its scaled rectangle. Clear only
	// the letterbox bars so each posted frame does not write the entire target
	// once for black and then again for converted pixels.
	const size_t destinationRowBytes = size_t(destinationWidth) * 4u;
	for (uint32_t y = 0u; y < offsetY; ++y)
		std::memset(destination + size_t(y) * destinationStride * 4u, 0, destinationRowBytes);
	for (uint32_t y = offsetY + drawHeight; y < destinationHeight; ++y)
		std::memset(destination + size_t(y) * destinationStride * 4u, 0, destinationRowBytes);
	const size_t leftBarBytes = size_t(offsetX) * 4u;
	const size_t rightBarBytes = size_t(destinationWidth - offsetX - drawWidth) * 4u;
	if (leftBarBytes || rightBarBytes) {
		for (uint32_t y = 0u; y < drawHeight; ++y) {
			uint8_t* row = destination + size_t(y + offsetY) * destinationStride * 4u;
			if (leftBarBytes) std::memset(row, 0, leftBarBytes);
			if (rightBarBytes)
				std::memset(row + size_t(offsetX + drawWidth) * 4u, 0, rightBarBytes);
		}
	}
	if (timings) timings->prepareNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(std::chrono::steady_clock::now() - prepareStart).count());

	const auto scaleMapStart = timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	struct ScaleMapCache {
		uint32_t width = 0u, height = 0u, drawWidth = 0u, drawHeight = 0u;
		std::vector<uint32_t> sourceX, sourceY;
	};
	static thread_local ScaleMapCache scaleMap;
	if (scaleMap.width != width || scaleMap.height != height ||
		scaleMap.drawWidth != drawWidth || scaleMap.drawHeight != drawHeight) {
		if (timings) timings->scaleMapRebuilt = true;
		scaleMap.width = width;
		scaleMap.height = height;
		scaleMap.drawWidth = drawWidth;
		scaleMap.drawHeight = drawHeight;
		const size_t oldXCapacity = scaleMap.sourceX.capacity();
		const size_t oldYCapacity = scaleMap.sourceY.capacity();
		scaleMap.sourceX.resize(drawWidth);
		scaleMap.sourceY.resize(drawHeight);
		if (timings) timings->temporaryAllocationBytes =
			uint64_t(scaleMap.sourceX.capacity() - oldXCapacity +
				scaleMap.sourceY.capacity() - oldYCapacity) * sizeof(uint32_t);
		for (uint32_t x = 0u; x < drawWidth; ++x)
			scaleMap.sourceX[x] = static_cast<uint32_t>((uint64_t(x) * width) / drawWidth);
		for (uint32_t y = 0u; y < drawHeight; ++y)
			scaleMap.sourceY[y] = static_cast<uint32_t>((uint64_t(y) * height) / drawHeight);
	}
	if (timings) timings->scaleMapNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(std::chrono::steady_clock::now() - scaleMapStart).count());

	const auto pixelLoopStart = timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	for (uint32_t y = 0u; y < drawHeight; ++y) {
		const uint8_t* sourceRow = pixels + size_t(scaleMap.sourceY[y]) * pitch;
		uint8_t* destinationRow = destination + size_t(y + offsetY) * destinationStride * 4u + size_t(offsetX) * 4u;
		for (uint32_t x = 0u; x < drawWidth; ++x) {
			const uint8_t* sourcePixel = sourceRow + size_t(scaleMap.sourceX[x]) * sourceBytes;
			uint32_t packed = 0u;
			if (sourceBytes == 2u) {
				packed = uint32_t(sourcePixel[0]) | (uint32_t(sourcePixel[1]) << 8u);
			} else {
				for (uint32_t byte = 0u; byte < sourceBytes; ++byte)
					packed |= uint32_t(sourcePixel[byte]) << (byte * 8u);
			}
			uint8_t* out = destinationRow + size_t(x) * 4u;
			out[0] = red.get(packed);
			out[1] = green.get(packed);
			out[2] = blue.get(packed);
			out[3] = 255u;
		}
	}
	if (timings) timings->pixelLoopNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pixelLoopStart).count());
	return true;
}

} // namespace br::runtime

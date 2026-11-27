#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace br::runtime::audio {

inline int16_t decodeMsAdpcmNibble(uint8_t nibble, int32_t& delta,
	int16_t& sample1, int16_t& sample2,
	const std::pair<int16_t, int16_t>& coefficient)
{
	static constexpr int16_t adaptation[16] = {
		230, 230, 230, 230, 307, 409, 512, 614,
		768, 614, 512, 409, 307, 230, 230, 230
	};
	const int32_t signedNibble = (nibble & 8u) ? int32_t(nibble) - 16 : nibble;
	const int32_t prediction = int32_t(sample1) * coefficient.first +
		int32_t(sample2) * coefficient.second;
	// Microsoft ADPCM predicts with signed integer division by 256.
	const int32_t linear = prediction / 256;
	const int32_t decoded = std::clamp(linear + signedNibble * delta, -32768, 32767);
	const int64_t adaptedDelta = (int64_t(adaptation[nibble]) * delta) >> 8;
	// Keep the wide decoder state. Narrowing this to int16_t wraps on loud
	// transients and corrupts every following nibble in the ADPCM block.
	delta = static_cast<int32_t>(std::clamp<int64_t>(adaptedDelta, 16, INT32_MAX / 768));
	sample2 = sample1;
	sample1 = static_cast<int16_t>(decoded);
	return sample1;
}

} // namespace br::runtime::audio

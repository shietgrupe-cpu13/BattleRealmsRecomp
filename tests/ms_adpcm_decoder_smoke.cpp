#include "ms_adpcm.h"

#include <cassert>
#include <cstdint>

int main()
{
	// Reference arithmetic: 16,000 * 614 / 256 = 38,375. The next negative
	// nibble is then 500 - 8 * 38,375, clipped to signed PCM16 minimum.
	int32_t delta = 16'000;
	int16_t sample1 = 500;
	int16_t sample2 = -200;
	assert(br::runtime::audio::decodeMsAdpcmNibble(7, delta, sample1, sample2, {256, 0}) == 32767);
	assert(delta == 38'375);
	sample1 = 32'767;
	sample2 = 500;
	assert(br::runtime::audio::decodeMsAdpcmNibble(8, delta, sample1, sample2, {0, 256}) == -32768);

	// A large adapted delta must remain bounded and positive instead of
	// wrapping through a signed 16-bit temporary.
	delta = 1'000'000;
	sample1 = 0;
	sample2 = 0;
	(void)br::runtime::audio::decodeMsAdpcmNibble(7, delta, sample1, sample2, {0, 0});
	assert(delta == 2'398'437);
	return 0;
}

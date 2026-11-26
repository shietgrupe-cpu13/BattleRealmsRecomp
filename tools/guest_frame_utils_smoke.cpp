#include "guest_frame_utils.h"

#include <cstdint>
#include <cstdio>

int main()
{
	using br::runtime::fitGuestFrame;
	using br::runtime::extractMaskedChannel;
	using br::runtime::mapSurfacePointToGuest;
	using br::runtime::readLittleEndianPixel;

	const auto phoneFrame = fitGuestFrame(1024u, 768u, 2400u, 1080u);
	if (phoneFrame.x != 480u || phoneFrame.y != 0u ||
		phoneFrame.width != 1440u || phoneFrame.height != 1080u) {
		std::fprintf(stderr, "1024x768 aspect fit mismatch: %u,%u %ux%u\n",
			phoneFrame.x, phoneFrame.y, phoneFrame.width, phoneFrame.height);
		return 1;
	}
	const auto tabletFrame = fitGuestFrame(800u, 600u, 1280u, 720u);
	if (tabletFrame.x != 160u || tabletFrame.y != 0u ||
		tabletFrame.width != 960u || tabletFrame.height != 720u) {
		std::fprintf(stderr, "800x600 aspect fit mismatch: %u,%u %ux%u\n",
			tabletFrame.x, tabletFrame.y, tabletFrame.width, tabletFrame.height);
		return 2;
	}
	int32_t x = -1, y = -1;
	mapSurfacePointToGuest(1200.0f, 540.0f, 2400u, 1080u, 1024u, 768u, x, y);
	if (x != 512 || y != 384) {
		std::fprintf(stderr, "surface center did not map to guest center: %d,%d\n", x, y);
		return 3;
	}
	mapSurfacePointToGuest(100.0f, 540.0f, 2400u, 1080u, 1024u, 768u, x, y);
	if (x != 0 || y != 384) {
		std::fprintf(stderr, "letterbox touch did not clamp to guest edge: %d,%d\n", x, y);
		return 4;
	}
	mapSurfacePointToGuest(1919.0f, 1079.0f, 2400u, 1080u, 1024u, 768u, x, y);
	if (x != 1023 || y != 767) {
		std::fprintf(stderr, "bottom-right mapping mismatch: %d,%d\n", x, y);
		return 5;
	}
	const uint8_t rgb565Red[] = {0x00u, 0xf8u};
	const uint32_t red = readLittleEndianPixel(rgb565Red, 2u);
	if (extractMaskedChannel(red, 0xf800u) != 255u ||
		extractMaskedChannel(red, 0x07e0u) != 0u ||
		extractMaskedChannel(red, 0x001fu) != 0u) {
		std::fprintf(stderr, "RGB565 red conversion mismatch\n");
		return 6;
	}
	const uint8_t rgb565Green[] = {0xe0u, 0x07u};
	const uint32_t green = readLittleEndianPixel(rgb565Green, 2u);
	if (extractMaskedChannel(green, 0xf800u) != 0u ||
		extractMaskedChannel(green, 0x07e0u) != 255u ||
		extractMaskedChannel(green, 0x001fu) != 0u) {
		std::fprintf(stderr, "RGB565 green conversion mismatch\n");
		return 7;
	}
	const uint8_t rgb565Blue[] = {0x1fu, 0x00u};
	const uint32_t blue = readLittleEndianPixel(rgb565Blue, 2u);
	if (extractMaskedChannel(blue, 0xf800u) != 0u ||
		extractMaskedChannel(blue, 0x07e0u) != 0u ||
		extractMaskedChannel(blue, 0x001fu) != 255u) {
		std::fprintf(stderr, "RGB565 blue conversion mismatch\n");
		return 8;
	}
	std::puts("guest-frame regression passed: 4:3 aspect fit, touch mapping, RGB565 channel conversion");
	return 0;
}

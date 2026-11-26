#pragma once

#include <cstdint>
#include <vector>

namespace br::input {

enum class TouchAction { Down, PointerDown, Move, PointerUp, Up, Cancel };
enum class MouseAction { Move, LeftDown, LeftUp, RightDown, RightUp, MiddleDown, MiddleUp, Wheel };

struct TouchPacket {
	TouchAction action = TouchAction::Move;
	int pointerCount = 1;
	float x = 0.0f, y = 0.0f;
	float x2 = 0.0f, y2 = 0.0f;
	uint64_t timeMs = 0;
};

struct MouseEvent {
	MouseAction action = MouseAction::Move;
	int x = 0, y = 0;
	int wheel = 0;
};

// Converts touch gestures to ordinary Win32 mouse-button actions. Tap pairs are
// left as separate clicks so the original game's own double-click logic runs.
class TouchGestureRecognizer {
public:
	std::vector<MouseEvent> process(const TouchPacket& packet);
	std::vector<MouseEvent> tick(uint64_t nowMs);
	void setRightClickMode(bool enabled) { rightClickMode_ = enabled; }
	bool rightClickMode() const { return rightClickMode_; }
	void reset();

private:
	void release(std::vector<MouseEvent>& out, int x, int y);
	void click(std::vector<MouseEvent>& out, bool right, int x, int y);
	bool active_ = false, pending_ = false, consumed_ = false;
	bool moved_ = false, twoFinger_ = false, twoMoved_ = false;
	bool longHeld_ = false, pinching_ = false;
	int heldButton_ = 0;
	int startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;
	int centroidX_ = 0, centroidY_ = 0;
	float startDistance_ = 0.0f, lastDistance_ = 0.0f, wheelRemainder_ = 0.0f;
	uint64_t downAt_ = 0, twoAt_ = 0;
	bool rightClickMode_ = false;
};

} // namespace br::input

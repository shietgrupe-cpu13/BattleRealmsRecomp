#include "android_touch_gestures.h"

#include <algorithm>
#include <cmath>

namespace br::input {
namespace {
constexpr uint64_t kLongPressMs = 550;
constexpr uint64_t kTwoTapMs = 350;
constexpr float kMoveSlop = 28.0f;
constexpr float kPinchStep = 50.0f;
int distance(float x, float y) { return static_cast<int>(std::hypot(x, y)); }
}

void TouchGestureRecognizer::click(std::vector<MouseEvent>& out, bool right, int x, int y)
{
	out.push_back({MouseAction::Move, x, y, 0});
	out.push_back({right ? MouseAction::RightDown : MouseAction::LeftDown, x, y, 0});
	out.push_back({right ? MouseAction::RightUp : MouseAction::LeftUp, x, y, 0});
}

void TouchGestureRecognizer::release(std::vector<MouseEvent>& out, int x, int y)
{
	if (heldButton_ == 1) out.push_back({MouseAction::LeftUp, x, y, 0});
	if (heldButton_ == 2) out.push_back({MouseAction::RightUp, x, y, 0});
	if (heldButton_ == 3) out.push_back({MouseAction::MiddleUp, x, y, 0});
	heldButton_ = 0;
}

void TouchGestureRecognizer::reset()
{
	active_ = pending_ = consumed_ = moved_ = twoFinger_ = twoMoved_ = false;
	longHeld_ = pinching_ = false;
	heldButton_ = 0;
	wheelRemainder_ = 0.0f;
}

std::vector<MouseEvent> TouchGestureRecognizer::process(const TouchPacket& p)
{
	std::vector<MouseEvent> out;
	const int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
	lastX_ = x; lastY_ = y;
	switch (p.action) {
	case TouchAction::Down:
		if (active_) release(out, x, y);
		active_ = true; pending_ = true; consumed_ = moved_ = twoFinger_ = twoMoved_ = false;
		longHeld_ = pinching_ = false; heldButton_ = 0;
		startX_ = x; startY_ = y; downAt_ = p.timeMs;
		out.push_back({MouseAction::Move, x, y, 0});
		break;
	case TouchAction::PointerDown:
		if (active_ && p.pointerCount >= 2 && !moved_ && !longHeld_) {
			if (heldButton_) release(out, x, y);
			pending_ = false; twoFinger_ = true; twoMoved_ = false; consumed_ = false;
			twoAt_ = p.timeMs; centroidX_ = static_cast<int>((p.x + p.x2) * 0.5f);
			centroidY_ = static_cast<int>((p.y + p.y2) * 0.5f);
			startDistance_ = lastDistance_ = std::hypot(p.x2 - p.x, p.y2 - p.y);
			wheelRemainder_ = 0.0f; out.push_back({MouseAction::Move, centroidX_, centroidY_, 0});
		}
		break;
	case TouchAction::Move:
		if (!active_ || consumed_) break;
		if (p.pointerCount >= 2 && twoFinger_) {
			const float cx = (p.x + p.x2) * 0.5f, cy = (p.y + p.y2) * 0.5f;
			const float spacing = std::hypot(p.x2 - p.x, p.y2 - p.y);
			const float delta = spacing - lastDistance_; wheelRemainder_ += delta; lastDistance_ = spacing;
			if (std::abs(spacing - startDistance_) > kMoveSlop) { twoMoved_ = true; pinching_ = true; }
			while (wheelRemainder_ >= kPinchStep) { out.push_back({MouseAction::Wheel, static_cast<int>(cx), static_cast<int>(cy), 120}); wheelRemainder_ -= kPinchStep; }
			while (wheelRemainder_ <= -kPinchStep) { out.push_back({MouseAction::Wheel, static_cast<int>(cx), static_cast<int>(cy), -120}); wheelRemainder_ += kPinchStep; }
			if (!pinching_ && distance(cx - centroidX_, cy - centroidY_) > static_cast<int>(kMoveSlop * 2.0f)) {
				twoMoved_ = true; pinching_ = true; heldButton_ = 3;
				out.push_back({MouseAction::Move, centroidX_, centroidY_, 0});
				out.push_back({MouseAction::MiddleDown, centroidX_, centroidY_, 0});
			}
			if (heldButton_ == 3) out.push_back({MouseAction::Move, static_cast<int>(cx), static_cast<int>(cy), 0});
			break;
		}
		if (p.pointerCount != 1) break;
		if (pending_ && !moved_ && distance(x - startX_, y - startY_) > static_cast<int>(kMoveSlop)) {
			moved_ = true; pending_ = false;
			// A brief swipe pans with the right mouse button; a deliberate hold-drag
			// draws a selection box with the left button, like RA3-Native.
			heldButton_ = (p.timeMs - downAt_ >= 250) ? 1 : 2;
			out.push_back({heldButton_ == 1 ? MouseAction::LeftDown : MouseAction::RightDown, startX_, startY_, 0});
		}
		if (heldButton_) out.push_back({MouseAction::Move, x, y, 0});
		break;
	case TouchAction::PointerUp:
		if (active_ && twoFinger_ && p.pointerCount >= 2) {
			if (!twoMoved_ && p.timeMs - twoAt_ < kTwoTapMs)
				click(out, true, centroidX_, centroidY_); // two-finger tap = cancel/right-click
			release(out, x, y); consumed_ = true; pending_ = false; twoFinger_ = false;
		}
		break;
	case TouchAction::Up:
		if (!active_) break;
		if (!consumed_ && pending_ && !moved_) {
			const bool right = rightClickMode_ || (p.timeMs - downAt_ >= kLongPressMs);
			click(out, right, x, y);
		} else if (longHeld_) {
			release(out, x, y);
		} else if (heldButton_) {
			release(out, x, y);
		}
		active_ = pending_ = consumed_ = moved_ = twoFinger_ = false;
		longHeld_ = pinching_ = false;
		break;
	case TouchAction::Cancel:
		if (active_) release(out, x, y);
		active_ = pending_ = consumed_ = moved_ = twoFinger_ = false;
		longHeld_ = pinching_ = false;
		break;
	}
	return out;
}

std::vector<MouseEvent> TouchGestureRecognizer::tick(uint64_t nowMs)
{
	std::vector<MouseEvent> out;
	if (active_ && pending_ && !twoFinger_ && !moved_ && !longHeld_ && nowMs >= downAt_ + kLongPressMs) {
		pending_ = false; longHeld_ = true; heldButton_ = 2;
		out.push_back({MouseAction::Move, lastX_, lastY_, 0});
		out.push_back({MouseAction::RightDown, lastX_, lastY_, 0});
	}
	return out;
}

} // namespace br::input

#include "android_touch_gestures.h"

#include <cassert>
#include <vector>

using namespace br::input;

static TouchPacket event(TouchAction action, int count, float x, float y, uint64_t ms,
	float x2 = 0.0f, float y2 = 0.0f)
{
	return {action, count, x, y, x2, y2, ms};
}

int main()
{
	TouchGestureRecognizer input;
	auto out = input.process(event(TouchAction::Down, 1, 100, 120, 0));
	assert(out.size() == 1 && out[0].action == MouseAction::Move);
	out = input.process(event(TouchAction::Up, 1, 100, 120, 90));
	assert(out.size() == 3 && out[1].action == MouseAction::LeftDown && out[2].action == MouseAction::LeftUp);

	input.process(event(TouchAction::Down, 1, 100, 120, 1000));
	out = input.tick(1550);
	assert(out.size() == 2 && out[1].action == MouseAction::RightDown);
	out = input.process(event(TouchAction::Up, 1, 100, 120, 1650));
	assert(out.size() == 1 && out[0].action == MouseAction::RightUp);

	input.process(event(TouchAction::Down, 1, 100, 120, 2000));
	input.process(event(TouchAction::PointerDown, 2, 100, 120, 2050, 200, 120));
	out = input.process(event(TouchAction::PointerUp, 2, 100, 120, 2150, 200, 120));
	assert(out.size() == 3 && out[1].action == MouseAction::RightDown && out[2].action == MouseAction::RightUp);
	input.process(event(TouchAction::Up, 1, 100, 120, 2160));

	input.process(event(TouchAction::Down, 1, 100, 120, 3000));
	out = input.process(event(TouchAction::Move, 1, 160, 120, 3100));
	assert(out.size() == 2 && out[0].action == MouseAction::RightDown && out[1].action == MouseAction::Move);
	out = input.process(event(TouchAction::Up, 1, 160, 120, 3200));
	assert(out.size() == 1 && out[0].action == MouseAction::RightUp);

	input.process(event(TouchAction::Down, 1, 100, 120, 4000));
	out = input.process(event(TouchAction::Move, 1, 160, 120, 4400));
	assert(out.size() == 2 && out[0].action == MouseAction::LeftDown);
	out = input.process(event(TouchAction::Up, 1, 160, 120, 4500));
	assert(out.size() == 1 && out[0].action == MouseAction::LeftUp);

	input.process(event(TouchAction::Down, 1, 100, 120, 5000));
	input.process(event(TouchAction::PointerDown, 2, 100, 120, 5050, 200, 120));
	out = input.process(event(TouchAction::Move, 2, 70, 120, 5150, 230, 120));
	assert(!out.empty() && out[0].action == MouseAction::Wheel && out[0].wheel == 120);
	input.process(event(TouchAction::Cancel, 0, 80, 120, 5200));

	input.setRightClickMode(true);
	input.process(event(TouchAction::Down, 1, 100, 120, 6000));
	out = input.process(event(TouchAction::Up, 1, 100, 120, 6050));
	assert(out.size() == 3 && out[1].action == MouseAction::RightDown);
	return 0;
}

#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_GAMEPAD_EXTRAS_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_GAMEPAD_EXTRAS_H_

#include <cstdint>

struct SDL_Gamepad;

// DualSense touch pad and motion for gamepads without them (XInput and other generic pads).
//
// - Touch button (KYTY_PAD_TOUCH_BUTTON, default "back"): held, it is the touch pad pressed with a
//   finger on it; the left stick moves that finger (a swipe) and reads as centered to the game.
// - Tilt button (KYTY_PAD_TILT_BUTTON, default "guide"): a press turns tilt on, the next one off
//   (KYTY_PAD_TILT_MODE=hold: only while held; Windows opens Task View on a held Xbox button).
//   While on, the right stick tilts a virtual controller up to KYTY_PAD_TILT_DEGREES (default 40)
//   and reads as centered to the game; when off the controller levels out again. Acceleration,
//   angular velocity and orientation agree, whichever of them the game reads.
//   KYTY_PAD_TILT_INVERT=x|y|xy flips the stick directions.
//
// Button values are SDL gamepad button names (back, guide, start, leftstick, rightstick, misc1,
// paddle1, ...) or none. KYTY_PAD_EXTRAS=auto (default) applies to pads that lack a touch pad
// (touch button) or a gyroscope (tilt button), 1 to every pad, 0 to none.
namespace Libs::Graphics::GamepadExtras {

void OnAdded(SDL_Gamepad* pad, int id);
void OnRemoved(int id);
// true: the event is consumed and must not reach the game as it is.
[[nodiscard]] bool OnButton(int id, int sdl_button, bool down);
[[nodiscard]] bool OnAxis(int id, int sdl_axis, int value);
// Advances the tilt; returns the ms until the next call is due, -1 for none.
[[nodiscard]] int Poll(uint64_t now_ms);
void              ReleaseAll();

} // namespace Libs::Graphics::GamepadExtras

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_GAMEPAD_EXTRAS_H_ */

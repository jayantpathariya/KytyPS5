#include "graphics/presentation/window/gamepadExtras.h"

#include <SDL3/SDL.h>

#include "common/logging/log.h"
#include "libs/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::GamepadExtras {

namespace {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

constexpr uint64_t TILT_POLL_INTERVAL_MS = 8;
// Time constant of the virtual controller following the stick.
constexpr float TILT_SMOOTHING_S = 0.06f;
constexpr float TILT_DEADZONE    = 0.12f;
constexpr float PI               = 3.14159265358979f;

enum class Mode { Off, Auto, On };

struct Settings {
	Mode              mode   = Mode::Auto;
	SDL_GamepadButton touch  = SDL_GAMEPAD_BUTTON_BACK;
	SDL_GamepadButton tilt   = SDL_GAMEPAD_BUTTON_GUIDE;
	float             max_rad = 40.0f * PI / 180.0f;
	bool              invert_x = false;
	bool              invert_y = false;
	// A press switches the tilt on and off: Windows opens Task View while the Xbox button is held.
	bool tilt_toggle = true;
};

SDL_GamepadButton ButtonSetting(const char* name, SDL_GamepadButton fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return fallback;
	}
	if (std::strcmp(value, "none") == 0) {
		return SDL_GAMEPAD_BUTTON_INVALID;
	}
	const auto button = SDL_GetGamepadButtonFromString(value);
	if (button == SDL_GAMEPAD_BUTTON_INVALID) {
		LOGF("%s: unknown gamepad button '%s', using '%s'\n", name, value,
		     SDL_GetGamepadStringForButton(fallback));
		return fallback;
	}
	return button;
}

Settings ReadSettings() {
	Settings s;
	if (const char* mode = std::getenv("KYTY_PAD_EXTRAS"); mode != nullptr) {
		if (std::strcmp(mode, "0") == 0) {
			s.mode = Mode::Off;
		} else if (std::strcmp(mode, "1") == 0) {
			s.mode = Mode::On;
		}
	}
	s.touch = ButtonSetting("KYTY_PAD_TOUCH_BUTTON", s.touch);
	s.tilt  = ButtonSetting("KYTY_PAD_TILT_BUTTON", s.tilt);
	if (s.tilt != SDL_GAMEPAD_BUTTON_INVALID && s.tilt == s.touch) {
		LOGF("KYTY_PAD_TILT_BUTTON is the touch button: tilt disabled\n");
		s.tilt = SDL_GAMEPAD_BUTTON_INVALID;
	}
	if (const char* degrees = std::getenv("KYTY_PAD_TILT_DEGREES"); degrees != nullptr) {
		const float value = std::strtof(degrees, nullptr);
		if (std::isfinite(value)) {
			s.max_rad = std::clamp(value, 5.0f, 90.0f) * PI / 180.0f;
		}
	}
	if (const char* invert = std::getenv("KYTY_PAD_TILT_INVERT"); invert != nullptr) {
		s.invert_x = std::strchr(invert, 'x') != nullptr;
		s.invert_y = std::strchr(invert, 'y') != nullptr;
	}
	if (const char* mode = std::getenv("KYTY_PAD_TILT_MODE"); mode != nullptr) {
		s.tilt_toggle = std::strcmp(mode, "hold") != 0;
	}
	return s;
}

const Settings& GetSettings() {
	static const Settings settings = ReadSettings();
	return settings;
}

struct Pad {
	int          id    = 0;
	SDL_Gamepad* pad   = nullptr;
	bool         touch = false; // the touch button is emulated on this pad
	bool         tilt  = false; // the tilt button is emulated on this pad

	bool  touch_held = false;
	bool  tilt_held  = false;
	float touch_x    = 0.5f;
	float touch_y    = 0.5f;
	// Current virtual tilt (radians): pitch about +X (front edge up), roll about +Z (right side up).
	float    pitch   = 0.0f;
	float    roll    = 0.0f;
	bool     moving  = false; // the last motion sent had a non-zero angular velocity
	uint64_t last_ns = 0;
};

std::vector<Pad> g_pads;
uint64_t         g_next_poll_ms = 0;

Pad* Find(int id) {
	const auto it = std::find_if(g_pads.begin(), g_pads.end(), [id](const Pad& p) { return p.id == id; });
	return it != g_pads.end() ? &*it : nullptr;
}

float Stick(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
	return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, axis)) / 32767.0f, -1.0f, 1.0f);
}

void RestoreStick(const Pad& p, SDL_GamepadAxis x_axis, SDL_GamepadAxis y_axis, Controller::Axis x,
                  Controller::Axis y) {
	const auto value = [&](SDL_GamepadAxis axis) {
		return Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX,
		                                       SDL_GetGamepadAxis(p.pad, axis));
	};
	Controller::SetAxis(p.id, x, value(x_axis));
	Controller::SetAxis(p.id, y, value(y_axis));
}

void CenterStick(const Pad& p, Controller::Axis x, Controller::Axis y) {
	Controller::SetAxis(p.id, x, 128);
	Controller::SetAxis(p.id, y, 128);
}

void MoveFinger(Pad& p) {
	p.touch_x = 0.5f + 0.5f * Stick(p.pad, SDL_GAMEPAD_AXIS_LEFTX);
	p.touch_y = 0.5f + 0.5f * Stick(p.pad, SDL_GAMEPAD_AXIS_LEFTY);
	Controller::SetTouchPad(p.id, 0, true, p.touch_x, p.touch_y);
}

void SetTouch(Pad& p, bool down) {
	p.touch_held = down;
	if (down) {
		CenterStick(p, Controller::Axis::LeftX, Controller::Axis::LeftY);
		MoveFinger(p);
	} else {
		Controller::SetTouchPad(p.id, 0, false, p.touch_x, p.touch_y);
		RestoreStick(p, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, Controller::Axis::LeftX,
		             Controller::Axis::LeftY);
	}
	Controller::SetButton(p.id, Controller::PAD_BUTTON_TOUCH_PAD, down);
}

void SetTilt(Pad& p, bool down) {
	p.tilt_held = down;
	if (down) {
		CenterStick(p, Controller::Axis::RightX, Controller::Axis::RightY);
	} else {
		RestoreStick(p, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, Controller::Axis::RightX,
		             Controller::Axis::RightY);
	}
}

Quat Multiply(const Quat& a, const Quat& b) {
	return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
	const Vec3 t {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
	              2.0f * (q[0] * v[1] - q[1] * v[0])};
	return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]),
	        v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
	        v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

Quat Conjugate(const Quat& q) {
	return {-q[0], -q[1], -q[2], q[3]};
}

// Moves the virtual controller toward the stick and reports it; false once it rests level.
bool StepTilt(Pad& p, uint64_t now_ns) {
	const auto& s = GetSettings();

	float target_pitch = 0.0f;
	float target_roll  = 0.0f;
	if (p.tilt_held) {
		float      x         = Stick(p.pad, SDL_GAMEPAD_AXIS_RIGHTX);
		float      y         = Stick(p.pad, SDL_GAMEPAD_AXIS_RIGHTY);
		const auto magnitude = std::hypot(x, y);
		const auto scale     = magnitude > TILT_DEADZONE
		                           ? (std::min(magnitude, 1.0f) - TILT_DEADZONE) / (1.0f - TILT_DEADZONE) / magnitude
		                           : 0.0f;
		x *= scale * (s.invert_x ? -1.0f : 1.0f);
		y *= scale * (s.invert_y ? -1.0f : 1.0f);
		// Stick right: right side down. Stick forward (SDL y < 0): front edge down.
		target_roll  = -x * s.max_rad;
		target_pitch = y * s.max_rad;
	}

	const float dt =
	    p.last_ns != 0 ? std::clamp(static_cast<float>(now_ns - p.last_ns) * 1e-9f, 0.001f, 0.05f)
	                   : static_cast<float>(TILT_POLL_INTERVAL_MS) * 0.001f;
	const float alpha = 1.0f - std::exp(-dt / TILT_SMOOTHING_S);
	float       pitch = p.pitch + (target_pitch - p.pitch) * alpha;
	float       roll  = p.roll + (target_roll - p.roll) * alpha;
	const bool  rest  = !p.tilt_held && std::abs(pitch) < 1e-3f && std::abs(roll) < 1e-3f;
	if (rest) {
		pitch = 0.0f;
		roll  = 0.0f;
	}
	const float pitch_rate = (pitch - p.pitch) / dt;
	const float roll_rate  = (roll - p.roll) / dt;
	const bool  changed    = pitch != p.pitch || roll != p.roll;
	p.pitch                = pitch;
	p.roll                 = roll;
	p.last_ns              = rest ? 0 : now_ns;

	if (changed || p.moving) {
		const Quat pitch_q {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
		const Quat roll_q {0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)};
		const Quat orientation = Multiply(roll_q, pitch_q);
		// The accelerometer reads +1 G up at rest; in the tilted body frame:
		const Vec3 accel = Rotate(Conjugate(orientation), {0.0f, 1.0f, 0.0f});
		// Body-frame angular velocity of roll(t) * pitch(t).
		const Vec3 roll_in_body = Rotate(Conjugate(pitch_q), {0.0f, 0.0f, roll_rate});
		const Vec3 gyro {pitch_rate + roll_in_body[0], roll_in_body[1], roll_in_body[2]};
		Controller::SetVirtualMotion(p.id, accel.data(), gyro.data(), orientation.data());
		p.moving = changed;
	}
	return !rest || p.tilt_held;
}

} // namespace

void OnAdded(SDL_Gamepad* pad, int id) {
	const auto& s = GetSettings();
	if (s.mode == Mode::Off || pad == nullptr || Find(id) != nullptr) {
		return;
	}
	Pad p;
	p.id    = id;
	p.pad   = pad;
	p.touch = s.touch != SDL_GAMEPAD_BUTTON_INVALID &&
	          (s.mode == Mode::On || SDL_GetNumGamepadTouchpads(pad) == 0);
	p.tilt = s.tilt != SDL_GAMEPAD_BUTTON_INVALID &&
	         (s.mode == Mode::On || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO));
	if (!p.touch && !p.tilt) {
		return;
	}
	const char* name = SDL_GetGamepadName(pad);
	LOGF("Gamepad extras for %d (%s): touch pad on %s, tilt on %s + right stick\n", id,
	     name != nullptr ? name : "?", p.touch ? SDL_GetGamepadStringForButton(s.touch) : "off",
	     p.tilt ? SDL_GetGamepadStringForButton(s.tilt) : "off");
	for (const auto button: {s.touch, s.tilt}) {
		if (button != SDL_GAMEPAD_BUTTON_INVALID && !SDL_GamepadHasButton(pad, button)) {
			LOGF("\t this pad reports no '%s' button\n", SDL_GetGamepadStringForButton(button));
		}
	}
	g_pads.push_back(p);
}

void OnRemoved(int id) {
	std::erase_if(g_pads, [id](const Pad& p) { return p.id == id; });
}

bool OnButton(int id, int sdl_button, bool down) {
	auto* p = Find(id);
	if (p == nullptr) {
		return false;
	}
	const auto& s = GetSettings();
	if (p->touch && sdl_button == s.touch) {
		if (p->touch_held != down) {
			SetTouch(*p, down);
		}
		return true;
	}
	if (p->tilt && sdl_button == s.tilt) {
		const bool on = s.tilt_toggle ? (down ? !p->tilt_held : p->tilt_held) : down;
		if (p->tilt_held != on) {
			SetTilt(*p, on);
			g_next_poll_ms = 0;
			if (s.tilt_toggle) {
				LOGF("Gamepad extras: tilt %s\n", on ? "on" : "off");
			}
		}
		return true;
	}
	return false;
}

bool OnAxis(int id, int sdl_axis, [[maybe_unused]] int value) {
	auto* p = Find(id);
	if (p == nullptr) {
		return false;
	}
	if (p->touch_held && (sdl_axis == SDL_GAMEPAD_AXIS_LEFTX || sdl_axis == SDL_GAMEPAD_AXIS_LEFTY)) {
		MoveFinger(*p);
		return true;
	}
	// The tilt reads the stick in Poll.
	return p->tilt_held && (sdl_axis == SDL_GAMEPAD_AXIS_RIGHTX || sdl_axis == SDL_GAMEPAD_AXIS_RIGHTY);
}

int Poll(uint64_t now_ms) {
	const bool active = std::any_of(g_pads.begin(), g_pads.end(), [](const Pad& p) {
		return p.tilt && (p.tilt_held || p.last_ns != 0 || p.moving);
	});
	if (!active) {
		g_next_poll_ms = 0;
		return -1;
	}
	if (now_ms < g_next_poll_ms) {
		return static_cast<int>(g_next_poll_ms - now_ms);
	}
	g_next_poll_ms     = now_ms + TILT_POLL_INTERVAL_MS;
	const auto now_ns  = SDL_GetTicksNS();
	bool       pending = false;
	for (auto& p: g_pads) {
		if (p.tilt && (p.tilt_held || p.last_ns != 0 || p.moving)) {
			pending = StepTilt(p, now_ns) || p.moving || pending;
		}
	}
	return pending ? static_cast<int>(TILT_POLL_INTERVAL_MS) : -1;
}

void ReleaseAll() {
	for (auto& p: g_pads) {
		if (p.touch_held) {
			SetTouch(p, false);
		}
		if (p.tilt_held) {
			SetTilt(p, false);
		}
	}
}

} // namespace Libs::Graphics::GamepadExtras

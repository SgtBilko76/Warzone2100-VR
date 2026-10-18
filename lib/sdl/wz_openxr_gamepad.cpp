/*
	This file is part of Warzone 2100.
	Copyright (C) 2026  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#if defined(WZ_OPENXR_ENABLED) && defined(WZ_VULKAN_ENABLED)

#include "wz_openxr_gamepad.h"
#include "wz_openxr.h"
#include "sdl_gamepad.h"
#include "sdl_backend_private.h"
#include "lib/framework/input.h"
#include "lib/framework/frame.h"
#include "src/warzoneconfig.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>

/*
 * Touch controllers, laid out as in the 0 A.D. Quest port:
 *
 *   right controller laser -> cursor (on the panel or the table)
 *   right trigger          -> left click (select, drag a box, press buttons)
 *   right grip             -> right click (orders)
 *   left trigger           -> Shift (add to selection, queue orders)
 *   left grip              -> Ctrl (assign groups)
 *   left stick             -> pan the camera
 *   right stick            -> turn (left / right) and zoom (up / down) the camera, or scroll lists
 *   A                      -> Enter (confirm, chat)
 *   B, left menu button    -> Escape (back, game menu)
 *   X                      -> stop / hold position
 *   Y                      -> back (research tree, intelligence screen)
 *   both stick clicks held -> recenter the table and the panel
 *   short left stick click -> mixed reality on / off (the real room around the table)
 *
 * The sticks and buttons go through a virtual SDL gamepad, so the game's gamepad support handles them;
 * the cursor and the clicks come from the laser directly (gamepad pointer mode).
 */

namespace
{

SDL_JoystickID g_virtualId = 0;
SDL_Joystick* g_joystick = nullptr;

constexpr float kPressThreshold = 0.55f;   // triggers / grips: press above this...
constexpr float kReleaseThreshold = 0.35f; // ...release below this

struct PointerState
{
	bool leftMouse = false;
	bool rightMouse = false;
	bool shift = false;
	bool ctrl = false;
	bool positionValid = false;
	int x = 0, y = 0;
};
PointerState g_pointer;

Sint16 toAxis(float v)
{
	return static_cast<Sint16>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
}

void setButton(SDL_GamepadButton button, bool down)
{
	SDL_SetJoystickVirtualButton(g_joystick, static_cast<int>(button), down);
}

bool pressed(float value, bool wasDown)
{
	return value > (wasDown ? kReleaseThreshold : kPressThreshold);
}

void updateMouseButton(MOUSE_KEY_CODE button, bool down, bool& state)
{
	if (down == state)
	{
		return;
	}
	state = down;
	inputSetMouseButton(button, down, Vector2i(mouseX(), mouseY()));
	if (down)
	{
		wzxr::haptic(true, 0.3f, 0.02f);
	}
}

void updateKey(KEY_CODE key, bool down, bool& state)
{
	if (down != state)
	{
		state = down;
		inputSetKey(key, down);
	}
}

} // anonymous namespace

void wzOpenXRGamepadInit()
{
	if (!wzxr::isActive() || g_joystick)
	{
		return;
	}
	// VR is played with the controllers only: gamepad support cannot be switched off
	if (war_GetGamepadMode() == GamepadMode::Disabled)
	{
		war_SetGamepadMode(GamepadMode::Enabled);
	}
	wzGamepadInit();
	if (!SDL_WasInit(SDL_INIT_GAMEPAD))
	{
		debug(LOG_ERROR, "SDL gamepad subsystem unavailable - OpenXR controllers will not drive the game");
		return;
	}

	// Physical Touch controllers show up as Android gamepads but deliver no input to a focused XR app
	wzGamepadSetVirtualDevicesOnly(true);

	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name = "Meta Quest Touch Controllers";
	g_virtualId = SDL_AttachVirtualJoystick(&desc);
	if (g_virtualId == 0)
	{
		debug(LOG_ERROR, "SDL_AttachVirtualJoystick failed: %s", SDL_GetError());
		return;
	}
	g_joystick = SDL_OpenJoystick(g_virtualId);
	if (!g_joystick)
	{
		debug(LOG_ERROR, "SDL_OpenJoystick(virtual) failed: %s", SDL_GetError());
		SDL_DetachVirtualJoystick(g_virtualId);
		g_virtualId = 0;
		return;
	}
	debug(LOG_INFO, "OpenXR controllers attached as virtual gamepad (id %u), laser pointer drives the cursor", (unsigned)g_virtualId);
	wzGamepadSetExclusive(true);
	wzGamepadSetPointerMode(true);
}

void wzOpenXRGamepadUpdate()
{
	if (!g_joystick)
	{
		return;
	}

	wzxr::ControllerState s;
	wzxr::updateControllers(s); // all-released while unfocused, so nothing sticks

	// --- laser: the cursor follows it; it keeps its last position while pointing at nothing ---
	if (s.pointerValid && (!g_pointer.positionValid || s.pointerX != g_pointer.x || s.pointerY != g_pointer.y))
	{
		g_pointer.positionValid = true;
		g_pointer.x = s.pointerX;
		g_pointer.y = s.pointerY;
		inputSetMousePos(s.pointerX, s.pointerY);
	}

	// --- modifiers first, so clicks in the same update see them ---
	updateKey(KEY_LSHIFT, pressed(s.leftTrigger, g_pointer.shift), g_pointer.shift);
	updateKey(KEY_LCTRL, pressed(s.leftGrip, g_pointer.ctrl), g_pointer.ctrl);
	updateMouseButton(MOUSE_LMB, pressed(s.rightTrigger, g_pointer.leftMouse), g_pointer.leftMouse);
	updateMouseButton(MOUSE_RMB, pressed(s.rightGrip, g_pointer.rightMouse), g_pointer.rightMouse);

	// --- sticks: camera (see processGamepadCameraInput), or list scrolling over the interface ---
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFTX, toAxis(s.recentering ? 0.f : s.leftX));
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFTY, toAxis(s.recentering ? 0.f : s.leftY));
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHTX, toAxis(s.recentering ? 0.f : s.rightX));
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHTY, toAxis(s.recentering ? 0.f : s.rightY));
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
	SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);

	// --- buttons, through the game's gamepad handling ---
	setButton(SDL_GAMEPAD_BUTTON_SOUTH, false); // the gamepad's clicks - the laser clicks instead
	setButton(SDL_GAMEPAD_BUTTON_EAST, false);
	setButton(SDL_GAMEPAD_BUTTON_WEST, s.a);    // Enter
	setButton(SDL_GAMEPAD_BUTTON_START, s.b || s.menu); // Escape
	setButton(SDL_GAMEPAD_BUTTON_NORTH, s.x);   // stop / hold
	setButton(SDL_GAMEPAD_BUTTON_BACK, s.y);
	setButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
	setButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
	setButton(SDL_GAMEPAD_BUTTON_LEFT_STICK, false); // a short left stick click toggles mixed reality (presenter)
	setButton(SDL_GAMEPAD_BUTTON_RIGHT_STICK, s.rightStickClick && !s.recentering);
}

void wzOpenXRGamepadShutdown()
{
	if (g_joystick)
	{
		SDL_CloseJoystick(g_joystick);
		g_joystick = nullptr;
	}
	if (g_virtualId != 0)
	{
		SDL_DetachVirtualJoystick(g_virtualId);
		g_virtualId = 0;
	}
	if (g_pointer.leftMouse)
	{
		inputSetMouseButton(MOUSE_LMB, false, Vector2i(mouseX(), mouseY()));
	}
	if (g_pointer.rightMouse)
	{
		inputSetMouseButton(MOUSE_RMB, false, Vector2i(mouseX(), mouseY()));
	}
	if (g_pointer.shift)
	{
		inputSetKey(KEY_LSHIFT, false);
	}
	if (g_pointer.ctrl)
	{
		inputSetKey(KEY_LCTRL, false);
	}
	g_pointer = PointerState();
	wzGamepadSetPointerMode(false);
	wzGamepadSetExclusive(false);
	wzGamepadSetVirtualDevicesOnly(false);
}

#endif // defined(WZ_OPENXR_ENABLED) && defined(WZ_VULKAN_ENABLED)

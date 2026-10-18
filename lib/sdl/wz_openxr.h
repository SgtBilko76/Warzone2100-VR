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
/** @file wz_openxr.h
 * OpenXR presentation for standalone headsets (Meta Quest).
 *
 * Owns the XrInstance / XrSession and implements gfx_api::vk_external_presenter,
 * so the Vulkan backend renders into OpenXR swapchain images instead of a WSI swapchain.
 */

#pragma once

#if defined(WZ_OPENXR_ENABLED) && defined(WZ_VULKAN_ENABLED)

#include "lib/ivis_opengl/gfx_api_vk.h"

namespace wzxr
{
	/// Creates the OpenXR instance. Returns false if no OpenXR runtime is available.
	bool initialize();
	bool isActive();
	/// The Vulkan external presenter backed by the XR session (nullptr if not active).
	gfx_api::vk_external_presenter* presenter();
	/// Pumps the OpenXR event queue (session state changes). Call once per main loop iteration.
	void pollEvents();
	/// True when the runtime asked the app to exit (session exiting / instance loss).
	bool exitRequested();
	/// Size of the game's render target (the virtual screen), in pixels.
	void getTargetSize(int& w, int& h);

	/// Touch controller state, laid out like a standard gamepad (Y axes point down, as in SDL)
	struct ControllerState
	{
		float leftX = 0.f, leftY = 0.f;
		float rightX = 0.f, rightY = 0.f;
		float leftTrigger = 0.f, rightTrigger = 0.f;
		float leftGrip = 0.f, rightGrip = 0.f;
		bool a = false, b = false, x = false, y = false;
		bool menu = false;
		bool leftStickClick = false, rightStickClick = false;
		/// Both stick clicks are held to recenter - they mean nothing else meanwhile
		bool recentering = false;
		/// The right controller's laser points at the panel or the table: the point it hits, in logical screen coordinates
		bool pointerValid = false;
		int pointerX = 0, pointerY = 0;
	};
	/// Syncs the XR actions and updates the laser. Returns false while the session is not focused (no input).
	bool updateControllers(ControllerState& out);
	/// Buzzes a controller
	void haptic(bool rightHand, float amplitude, float seconds);
	// Mixed reality (passthrough) is on: the room shows around the table, so no sky
	bool mixedRealityActive();
	void shutdown();
}

#endif

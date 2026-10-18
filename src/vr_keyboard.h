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
/** @file vr_keyboard.h
 * On-screen keyboard for headsets (no physical keyboard, and native OpenXR apps get no system one).
 *
 * Shown next to whichever text field is being edited (any W_EDITBOX - it follows StartTextInput),
 * typed on with the controller cursor. Enabled in VR, or anywhere with WZ_VIRTUAL_KEYBOARD=1.
 */

#pragma once

bool vrKeyboardInit();
void vrKeyboardShutdown();

/// True while the keyboard is shown and the (logical screen) point lies on it
bool vrKeyboardContains(int x, int y);

/// While the point lies on the keyboard: the centre and radius of the key under it, to snap the cursor to
bool vrKeyboardMagnetTarget(int x, int y, int& outX, int& outY, int& outRadius);

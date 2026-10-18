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
/** @file vr_keyboard.cpp
 * On-screen keyboard for headsets, see vr_keyboard.h.
 *
 * Clicks on it are swallowed in the input layer (inputSetMouseButtonFilter), before the widgets see
 * them - a click anywhere outside an edit box ends its editing. The keys are fed into the text input
 * buffer like physical key presses. The widget itself only draws, and sits on an overlay screen so
 * the cursor counts as over the interface while pointing at it.
 */

#include "vr_keyboard.h"

#include "lib/framework/frame.h"
#include "lib/framework/input.h"
#include "lib/framework/wzapp.h"
#include "lib/framework/gamepad_input.h"
#include "lib/widget/widget.h"
#include "lib/widget/form.h"
#include "lib/ivis_opengl/pieblitfunc.h"
#include "lib/ivis_opengl/piepalette.h"
#include "lib/ivis_opengl/textdraw.h"
#include "lib/ivis_opengl/screen.h"
#include "vr_diorama.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

enum class KeyAction
{
	Text,     // types its label
	Editing,  // an editing key (backspace, return, arrows)
	Space,
	Shift,
	Hide,
	RowBreak, // layout only
};

struct VirtualKey
{
	std::string label;
	std::string shiftedLabel;
	KeyAction action;
	KEY_CODE code;  // for KeyAction::Editing
	float width;    // in key units
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // logical screen rect, from the last layout
	std::unique_ptr<WzText> text;
	std::unique_ptr<WzText> shiftedText;
};

struct KeyboardState
{
	bool shown = false;
	bool shift = false;
	void* field = nullptr;          // the text input owner the keyboard types into
	void* dismissedFor = nullptr;   // "Hide" pressed while this owner had the input
	WzTextInputRect fieldRect = {0, 0, 0, 0};
	int left = 0, top = 0, width = 0, height = 0;
	int unit = 0;
	int pressedKey = -1;
	std::vector<VirtualKey> keys;
};

KeyboardState state;
std::shared_ptr<W_SCREEN> keyboardOverlayScreen;
std::shared_ptr<WIDGET> keyboardWidget;

constexpr int KEY_GAP = 4;

void addKey(const char* label, const char* shiftedLabel, KeyAction action, KEY_CODE code, float width)
{
	VirtualKey key;
	key.label = label;
	key.shiftedLabel = shiftedLabel;
	key.action = action;
	key.code = code;
	key.width = width;
	state.keys.push_back(std::move(key));
}

void addRow(const char* lower, const char* upper)
{
	for (size_t i = 0; lower[i] && upper[i]; ++i)
	{
		const char l[2] = {lower[i], 0};
		const char u[2] = {upper[i], 0};
		addKey(l, u, KeyAction::Text, KEY_IGNORE, 1.0f);
	}
}

void rowBreak()
{
	addKey("", "", KeyAction::RowBreak, KEY_IGNORE, 0.f);
}

// Same layout as the 0 A.D. Quest port's keyboard
void buildLayout()
{
	if (!state.keys.empty())
	{
		return;
	}
	addRow("1234567890", "!\"#$%&/()=");
	addKey("Back", "Back", KeyAction::Editing, KEY_BACKSPACE, 1.6f);
	rowBreak();
	addRow("qwertyuiop", "QWERTYUIOP");
	addRow("-", "_");
	rowBreak();
	addRow("asdfghjkl", "ASDFGHJKL");
	addKey("Enter", "Enter", KeyAction::Editing, KEY_RETURN, 2.1f);
	rowBreak();
	addKey("Shift", "Shift", KeyAction::Shift, KEY_IGNORE, 1.6f);
	addRow("zxcvbnm,.", "ZXCVBNM;:");
	addRow("@", "+");
	rowBreak();
	addKey("Hide", "Hide", KeyAction::Hide, KEY_IGNORE, 1.6f);
	addKey("<", "<", KeyAction::Editing, KEY_LEFTARROW, 1.0f);
	addKey("Space", "Space", KeyAction::Space, KEY_IGNORE, 5.6f);
	addKey(">", ">", KeyAction::Editing, KEY_RIGHTARROW, 1.0f);
	addRow("'", "?");
}

bool keyboardWanted()
{
	static const bool forced = []() {
		const char* env = std::getenv("WZ_VIRTUAL_KEYBOARD");
		return env != nullptr && env[0] != '\0' && env[0] != '0';
	}();
	return forced || vrDioramaEnabled();
}

// Below the field if there is room, above it otherwise
void layout()
{
	buildLayout();
	const int sw = static_cast<int>(screenWidth);
	const int sh = static_cast<int>(screenHeight);
	state.unit = std::max(24, std::min(56, static_cast<int>(sw / 13.5f)));
	const int unit = state.unit;
	state.width = static_cast<int>(unit * 11.6f) + KEY_GAP;
	state.height = 5 * unit + 6 * KEY_GAP;

	const WzTextInputRect& f = state.fieldRect;
	state.left = std::clamp(f.x + (f.width - state.width) / 2, 4, std::max(4, sw - state.width - 4));
	state.top = f.y + f.height + 8;
	if (state.top + state.height > sh - 4)
	{
		state.top = f.y - state.height - 8;
		if (state.top < 4)
		{
			state.top = std::max(4, sh - state.height - 4); // no room on either side - bottom of the screen
		}
	}

	int x = state.left + KEY_GAP;
	int y = state.top + KEY_GAP;
	for (VirtualKey& key : state.keys)
	{
		if (key.action == KeyAction::RowBreak)
		{
			x = state.left + KEY_GAP;
			y += unit + KEY_GAP;
			continue;
		}
		const int w = static_cast<int>(unit * key.width);
		key.x0 = x;
		key.y0 = y;
		key.x1 = x + w - KEY_GAP;
		key.y1 = y + unit;
		x += w;
	}
}

void updateState()
{
	void* owner = nullptr;
	WzTextInputRect rect = {0, 0, 0, 0};
	if (!keyboardWanted() || !wzGetTextInputField(owner, rect))
	{
		owner = nullptr;
	}
	if (owner != state.dismissedFor)
	{
		state.dismissedFor = nullptr; // focus moved on - the next field shows the keyboard again
	}
	state.field = owner;
	state.fieldRect = rect;
	state.shown = owner != nullptr && owner != state.dismissedFor;
	if (!state.shown)
	{
		state.shift = false;
		state.pressedKey = -1;
	}
	else
	{
		layout();
	}

	if (keyboardWidget)
	{
		if (state.shown)
		{
			keyboardWidget->setGeometry(state.left, state.top, state.width, state.height);
		}
		else
		{
			keyboardWidget->setGeometry(0, 0, 0, 0);
		}
	}
}

bool insideArea(int x, int y)
{
	return state.shown && x >= state.left && x < state.left + state.width && y >= state.top && y < state.top + state.height;
}

int keyAt(int x, int y)
{
	if (!state.shown)
	{
		return -1;
	}
	for (size_t i = 0; i < state.keys.size(); ++i)
	{
		const VirtualKey& key = state.keys[i];
		if (key.action != KeyAction::RowBreak && x >= key.x0 && x < key.x1 && y >= key.y0 && y < key.y1)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

void typeText(const std::string& text)
{
	for (unsigned char c : text) // the layout is plain ASCII
	{
		inputAddVirtualText(c);
	}
}

void pressKey(int index)
{
	const VirtualKey& key = state.keys[index];
	switch (key.action)
	{
	case KeyAction::Text:
		typeText(state.shift ? key.shiftedLabel : key.label);
		state.shift = false; // one-shot, like a phone keyboard
		break;
	case KeyAction::Space:
		inputAddVirtualText(' ');
		break;
	case KeyAction::Editing:
		inputAddVirtualKey(key.code);
		break;
	case KeyAction::Shift:
		state.shift = !state.shift;
		break;
	case KeyAction::Hide:
		state.dismissedFor = state.field;
		break;
	case KeyAction::RowBreak:
		break;
	}
}

bool mouseButtonFilter(MOUSE_KEY_CODE button, bool pressed, Vector2i pos)
{
	updateState();
	if (!pressed)
	{
		state.pressedKey = -1;
		return false;
	}
	if (!state.shown)
	{
		// clicking the field again brings a hidden keyboard back
		const WzTextInputRect& f = state.fieldRect;
		if (state.dismissedFor && button == MOUSE_LMB && pos.x >= f.x && pos.x < f.x + f.width && pos.y >= f.y && pos.y < f.y + f.height)
		{
			state.dismissedFor = nullptr;
		}
		return false;
	}
	if (!insideArea(pos.x, pos.y))
	{
		return false; // outside: the click ends editing as usual
	}
	if (button == MOUSE_LMB)
	{
		state.pressedKey = keyAt(pos.x, pos.y);
		if (state.pressedKey >= 0)
		{
			pressKey(state.pressedKey);
			gamepadRumble(0.f, 0.3f, 20);
		}
	}
	return true; // any button on the keyboard is swallowed, also between keys
}

void drawKeyboard()
{
	updateState();
	if (!state.shown)
	{
		return;
	}

	pie_UniTransBoxFill(state.left, state.top, state.left + state.width, state.top + state.height, pal_RGBA(8, 10, 32, 235));

	const int hovered = keyAt(mouseX(), mouseY());
	const iV_fonts font = state.unit >= 40 ? font_medium_bold : font_regular_bold;
	for (size_t i = 0; i < state.keys.size(); ++i)
	{
		VirtualKey& key = state.keys[i];
		if (key.action == KeyAction::RowBreak)
		{
			continue;
		}
		PIELIGHT colour = pal_RGBA(38, 44, 92, 255);
		if (static_cast<int>(i) == hovered)
		{
			colour = pal_RGBA(70, 82, 160, 255);
		}
		if (static_cast<int>(i) == state.pressedKey || (key.action == KeyAction::Shift && state.shift))
		{
			colour = pal_RGBA(190, 125, 20, 255);
		}
		pie_UniTransBoxFill(key.x0, key.y0, key.x1, key.y1, colour);

		std::unique_ptr<WzText>& text = state.shift ? key.shiftedText : key.text;
		if (!text)
		{
			text = std::make_unique<WzText>();
		}
		text->setText(WzString::fromUtf8(state.shift ? key.shiftedLabel : key.label), font);
		const int tx = key.x0 + (key.x1 - key.x0 - text->width()) / 2;
		const int ty = key.y0 + (key.y1 - key.y0 - text->lineSize()) / 2 - text->aboveBase();
		text->render(tx, ty, WZCOL_TEXT_BRIGHT);
	}
}

class VirtualKeyboardWidget : public WIDGET
{
public:
	void display(int, int) override
	{
		drawKeyboard();
	}
};

} // namespace

bool vrKeyboardInit()
{
	keyboardOverlayScreen = W_SCREEN::make();
	// hiding the root form keeps it from taking mouse events, the keyboard widget itself still
	// counts as hovered (so the VR cursor stays on the HUD panel while pointing at it)
	keyboardOverlayScreen->psForm->hide();
	keyboardWidget = std::make_shared<VirtualKeyboardWidget>();
	keyboardOverlayScreen->psForm->attach(keyboardWidget);
	keyboardWidget->setGeometry(0, 0, 0, 0);
	// just below the gamepad cursor, above everything else
	widgRegisterOverlayScreen(keyboardOverlayScreen, std::numeric_limits<uint16_t>::max() - 1);
	inputSetMouseButtonFilter(mouseButtonFilter);
	return true;
}

void vrKeyboardShutdown()
{
	inputSetMouseButtonFilter(nullptr);
	if (keyboardOverlayScreen)
	{
		widgRemoveOverlayScreen(keyboardOverlayScreen);
		keyboardOverlayScreen = nullptr;
	}
	keyboardWidget = nullptr;
	state.keys.clear(); // releases the text textures while the renderer is still up
	state.shown = false;
}

bool vrKeyboardContains(int x, int y)
{
	return insideArea(x, y);
}

bool vrKeyboardMagnetTarget(int x, int y, int& outX, int& outY, int& outRadius)
{
	if (!insideArea(x, y))
	{
		return false;
	}
	// snap to the nearest key (also from the gaps between keys)
	int best = -1;
	int bestDist = std::numeric_limits<int>::max();
	for (size_t i = 0; i < state.keys.size(); ++i)
	{
		const VirtualKey& key = state.keys[i];
		if (key.action == KeyAction::RowBreak)
		{
			continue;
		}
		const int dx = std::max({key.x0 - x, 0, x - key.x1});
		const int dy = std::max({key.y0 - y, 0, y - key.y1});
		const int dist = dx * dx + dy * dy;
		if (dist < bestDist)
		{
			bestDist = dist;
			best = static_cast<int>(i);
		}
	}
	if (best < 0)
	{
		return false;
	}
	const VirtualKey& key = state.keys[best];
	outX = (key.x0 + key.x1) / 2;
	outY = (key.y0 + key.y1) / 2;
	outRadius = std::max(key.x1 - key.x0, key.y1 - key.y0) / 2 + KEY_GAP;
	return true;
}

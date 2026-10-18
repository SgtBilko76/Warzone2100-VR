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
/** @file vr_diorama.h
 * VR "diorama": the battlefield as a stereo 3D miniature on a virtual table.
 *
 * The game keeps rendering its regular frame from a top-down camera (used for culling,
 * picking and all 2D overlays). The ground rectangle that camera sees is mapped onto the
 * table; the 3D scene is rendered from each eye with that mapping, and the 2D overlay is
 * composited as a transparent layer lying on the table, so both line up.
 */

#pragma once

#include <glm/mat4x4.hpp>

/// True when a stereo presenter (VR headset) is available: the in-game view becomes the diorama
bool vrDioramaEnabled();

/// Upper limit for the camera distance, so the visible ground fits the renderable tile window
float vrDioramaMaxCameraDistance();

/// Turns the regular camera's projection into an orthographic top-down one (keeping the depth,
/// w = cameraDistance, that screen-space sizes are computed from): every overlay point then lies
/// vertically above / below the world point it belongs to, so it lines up with the stereo scene
void vrDioramaMakeOrthographic(glm::mat4& perspectiveMatrix, float cameraDistance);

/// Called by the 3D scene setup with this frame's (top-down) camera. groundHeight is the height
/// of the camera's orbit point, which becomes the table surface; cursorGroundHeight the height of
/// the terrain under the cursor, where the overlay is placed (so the cursor sits on the terrain).
void vrDioramaUpdateTable(const glm::mat4& perspectiveViewMatrix, float groundHeight, float cursorGroundHeight);

/// True while the scene is being recorded for one eye (effects without meaning on a table, like the skybox, are skipped)
bool vrDioramaRenderingEye();
// While an eye renders: the matrices for the sky around the player (rotation only, at infinity)
bool vrDioramaSkyMatrices(glm::mat4& perspective, glm::mat4& view);

/// The 2D phases of a stereo frame: the world overlay lies on the table, the interface on the HUD panel.
/// The cursor is drawn on the HUD panel only while it is over the interface, on the table otherwise.
bool vrDioramaDrawingWorldOverlay();
bool vrDioramaDrawingHud();

/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

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
/***************************************************************************/
/*
 * pieMode.h
 *
 * renderer control for pumpkin library functions.
 *
 */
/***************************************************************************/

#ifndef _pieMode_h
#define _pieMode_h

/***************************************************************************/

#include "lib/framework/frame.h"
#include "ivisdef.h"

/***************************************************************************/
/*
 *	Global Definitions
 */
/***************************************************************************/

extern iSurface rendSurface;

/***************************************************************************/
/*
 *	Global ProtoTypes
 */
/***************************************************************************/
bool pie_Initialise();
void pie_ShutDown();
void pie_ScreenFrameRenderBegin();
void pie_ScreenFrameRenderEnd();
bool pie_IsScreenFrameRendering();
void pie_UpdateSurfaceGeometry();
UDWORD pie_GetResScalingFactor();

namespace gfx_api
{

class CachedRenderGraph;
struct StereoEyeView;

} // namespace gfx_api

/// Stereo (VR) rendering of the in-game 3D scene, registered by the game. For in-game frames it
/// wants in stereo, the frame graph runs once per eye (3D scene passes only, into the eye targets),
/// once for the world overlay passes (into the regular target, composited on the table) and once
/// for the interface (into the HUD panel target).
struct StereoSceneRenderer
{
	virtual ~StereoSceneRenderer() = default;
	virtual bool wantsStereoFrame() = 0;
	/// Switches the scene matrices to one eye
	virtual void beginEye(size_t eye, const gfx_api::StereoEyeView& view) = 0;
	/// Restores the mono matrices (the overlay passes use them)
	virtual void endEyes() = 0;
	/// Brackets the 2D phases: the world overlay (on the table) and the HUD panel
	virtual void beginOverlay(bool hudPanel) = 0;
	virtual void endOverlay() = 0;
};
void pie_SetStereoSceneRenderer(StereoSceneRenderer* renderer);

gfx_api::CachedRenderGraph& pie_GetCachedFrameRenderGraph();

void pie_ResetInGame3DFrameContextForFrame();
bool pie_IsInGame3DFrameContextReady();

#endif

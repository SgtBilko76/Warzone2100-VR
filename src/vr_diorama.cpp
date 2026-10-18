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

#include "lib/framework/vector.h" // before any other glm include (swizzle configuration)
#include "lib/framework/frame.h"
#include "vr_diorama.h"

#include "lib/ivis_opengl/gfx_api.h"
#include "lib/ivis_opengl/piemode.h"
#include "display3d_render_graph.h"

#include <glm/gtc/matrix_transform.hpp>

namespace
{

// Table placement in the headset's tracking space (meters) - the presenter's anchor, i.e. the recentred
// head pose: in front of and below the head. (The HUD goes on the presenter's panel, above the table.)
const glm::vec3 kTableCenter(0.f, -0.5f, -0.75f);
constexpr float kTableWidth = 1.2f;
// Per-frame smoothing of the overlay's height (it follows the terrain under the cursor)
constexpr float kOverlayHeightSmoothing = 0.3f;
constexpr float kEyeNear = 0.05f;
constexpr float kEyeFar = 30.f;

// A camera further away than this shows more ground than the tile window (VISIBLE_XTILES) covers
constexpr float kMaxCameraDistance = 4000.f;

class DioramaRenderer final : public StereoSceneRenderer
{
public:
	void update(const glm::mat4& perspectiveViewMatrix, float groundHeight, float cursorGroundHeight);

	bool wantsStereoFrame() override;
	void beginEye(size_t eye, const gfx_api::StereoEyeView& view) override;
	void endEyes() override;
	void beginOverlay(bool hudPanel) override { m_overlay = hudPanel ? Overlay::Hud : Overlay::World; }
	void endOverlay() override { m_overlay = Overlay::None; }

	bool renderingEye() const { return m_renderingEye; }
	const glm::mat4& skyPerspective() const { return m_skyPerspective; }
	const glm::mat4& skyView() const { return m_skyView; }
	bool drawingWorldOverlay() const { return m_overlay == Overlay::World; }
	bool drawingHud() const { return m_overlay == Overlay::Hud; }

private:
	enum class Overlay { None, World, Hud };
	bool m_updated = false;
	bool m_renderingEye = false;
	Overlay m_overlay = Overlay::None;
	float m_overlayHeight = 0.f; // meters above the table surface (smoothed)

	// render space (world units) -> table space (meters, table center at the origin)
	glm::mat4 m_tableFromRender{1.f};
	float m_scale = 1.f; // meters per world unit
	float m_tableDepth = 0.f;

	// mono matrices, restored after the eyes
	glm::mat4 m_monoView{1.f};
	glm::mat4 m_monoPerspective{1.f};
	glm::mat4 m_monoPerspectiveView{1.f};
	glm::vec3 m_monoCameraPos{0.f};

	// the sky around the player: the eye's rotation only, so it lies at infinity
	glm::mat4 m_skyPerspective{1.f};
	glm::mat4 m_skyView{1.f};
};

DioramaRenderer g_renderer;
bool g_registered = false;

bool intersectGround(const glm::mat4& inverseViewProjection, float ndcX, float ndcY, float groundHeight, glm::vec3& out)
{
	glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, -1.f, 1.f);
	glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.f, 1.f);
	if (nearPoint.w == 0.f || farPoint.w == 0.f)
	{
		return false;
	}
	const glm::vec3 a = glm::vec3(nearPoint) / nearPoint.w;
	const glm::vec3 b = glm::vec3(farPoint) / farPoint.w;
	const float dy = b.y - a.y;
	if (std::abs(dy) < 1e-6f)
	{
		return false;
	}
	const float t = (groundHeight - a.y) / dy;
	out = a + (b - a) * t;
	return true;
}

void DioramaRenderer::update(const glm::mat4& perspectiveViewMatrix, float groundHeight, float cursorGroundHeight)
{
	m_updated = false;

	// The ground rectangle the (top-down) camera sees: this is what lies on the table
	const glm::mat4 inverseViewProjection = glm::inverse(perspectiveViewMatrix);
	glm::vec3 topLeft, topRight, bottomLeft, bottomRight;
	if (!intersectGround(inverseViewProjection, -1.f, 1.f, groundHeight, topLeft)
		|| !intersectGround(inverseViewProjection, 1.f, 1.f, groundHeight, topRight)
		|| !intersectGround(inverseViewProjection, -1.f, -1.f, groundHeight, bottomLeft)
		|| !intersectGround(inverseViewProjection, 1.f, -1.f, groundHeight, bottomRight))
	{
		return;
	}
	const glm::vec3 center = (topLeft + topRight + bottomLeft + bottomRight) * 0.25f;
	glm::vec3 right = ((topRight - topLeft) + (bottomRight - bottomLeft)) * 0.5f;
	glm::vec3 up = ((topLeft - bottomLeft) + (topRight - bottomRight)) * 0.5f;
	right.y = 0.f;
	up.y = 0.f;
	const float groundWidth = glm::length(right);
	const float groundDepth = glm::length(up);
	if (groundWidth < 1.f || groundDepth < 1.f)
	{
		return;
	}
	right /= groundWidth;
	up /= groundDepth;

	m_scale = kTableWidth / groundWidth;
	m_tableDepth = groundDepth * m_scale;

	// rows: table x = screen right, table y = height, table z = screen down (towards the viewer)
	glm::mat4 basis(1.f);
	basis[0][0] = right.x;  basis[1][0] = right.y;  basis[2][0] = right.z;
	basis[0][1] = 0.f;      basis[1][1] = 1.f;      basis[2][1] = 0.f;
	basis[0][2] = -up.x;    basis[1][2] = -up.y;    basis[2][2] = -up.z;
	m_tableFromRender = glm::scale(glm::mat4(1.f), glm::vec3(m_scale)) * basis * glm::translate(glm::mat4(1.f), -center);

	// The world overlay (the regular 2D frame) lies on the table, image top towards the far side, at
	// the height of the terrain under the cursor so the cursor sits on the terrain it points at
	const float targetHeight = (cursorGroundHeight - groundHeight) * m_scale;
	m_overlayHeight += (targetHeight - m_overlayHeight) * kOverlayHeightSmoothing;
	const glm::mat4 trackingFromTable = glm::translate(glm::mat4(1.f), kTableCenter);
	const glm::mat4 trackingFromQuad = trackingFromTable
		* glm::translate(glm::mat4(1.f), glm::vec3(0.f, m_overlayHeight, 0.f))
		* glm::rotate(glm::mat4(1.f), glm::radians(-90.f), glm::vec3(1.f, 0.f, 0.f));
	gfx_api::context::get().setStereoOverlayPlacement(trackingFromQuad, kTableWidth, m_tableDepth);

	m_updated = true;
}

bool DioramaRenderer::wantsStereoFrame()
{
	const bool wanted = m_updated;
	m_updated = false; // the camera must be updated again for the next stereo frame
	return wanted;
}

void DioramaRenderer::beginEye(size_t eye, const gfx_api::StereoEyeView& view)
{
	InGame3DFrameContext& fc = pie_GetInGame3DFrameContext();
	if (!m_renderingEye)
	{
		m_monoView = fc.viewMatrix;
		m_monoPerspective = fc.perspectiveMatrix;
		m_monoPerspectiveView = fc.perspectiveViewMatrix;
		m_monoCameraPos = fc.cameraPos;
		m_renderingEye = true;
	}

	// The game's matrix conventions: view space in world units with +Z forward, and the
	// projection is a GL frustum times scale(1, 1, -1) (see pie_PerspectiveGetConstrained)
	const glm::mat4 flipZ = glm::scale(glm::mat4(1.f), glm::vec3(1.f, 1.f, -1.f));
	const glm::mat4 trackingFromTable = glm::translate(glm::mat4(1.f), kTableCenter);
	const glm::mat4 eyeFromRender = view.viewFromTracking * trackingFromTable * m_tableFromRender; // meters
	const glm::mat4 viewMatrix = flipZ * glm::scale(glm::mat4(1.f), glm::vec3(1.f / m_scale)) * eyeFromRender;

	const float zNear = kEyeNear / m_scale;
	const float zFar = kEyeFar / m_scale;
	const glm::mat4 perspective = glm::frustum(view.tanLeft * zNear, view.tanRight * zNear, view.tanDown * zNear, view.tanUp * zNear, zNear, zFar) * flipZ;

	fc.viewMatrix = viewMatrix;
	fc.perspectiveMatrix = perspective;
	fc.perspectiveViewMatrix = perspective * viewMatrix;
	fc.cameraPos = glm::vec3(glm::inverse(viewMatrix) * glm::vec4(0.f, 0.f, 0.f, 1.f));

	// Sky: centred on the eye, turned with the table. A near plane of 1 world unit puts the sky's depth
	// (thousands of units away) behind everything on the table, so it only fills the background.
	const glm::mat3 eyeRotation = glm::mat3(eyeFromRender) / m_scale;
	m_skyView = flipZ * glm::mat4(eyeRotation);
	constexpr float kSkyNear = 1.f, kSkyFar = 200000.f;
	m_skyPerspective = glm::frustum(view.tanLeft * kSkyNear, view.tanRight * kSkyNear, view.tanDown * kSkyNear, view.tanUp * kSkyNear, kSkyNear, kSkyFar) * flipZ;
	(void)eye;
}

void DioramaRenderer::endEyes()
{
	if (!m_renderingEye)
	{
		return;
	}
	InGame3DFrameContext& fc = pie_GetInGame3DFrameContext();
	fc.viewMatrix = m_monoView;
	fc.perspectiveMatrix = m_monoPerspective;
	fc.perspectiveViewMatrix = m_monoPerspectiveView;
	fc.cameraPos = m_monoCameraPos;
	m_renderingEye = false;
}

} // anonymous namespace

bool vrDioramaEnabled()
{
	return gfx_api::context::isInitialized() && gfx_api::context::get().stereoRenderingAvailable();
}

float vrDioramaMaxCameraDistance()
{
	return kMaxCameraDistance;
}

void vrDioramaMakeOrthographic(glm::mat4& perspectiveMatrix, float cameraDistance)
{
	// Perspective: w = view depth (the matrix's w row is (0, 0, 1, 0), see pie_PerspectiveGetConstrained).
	// Replace it by the constant depth of the camera's orbit point, keeping the centre offset terms.
	for (int row = 0; row < 2; ++row)
	{
		perspectiveMatrix[3][row] += perspectiveMatrix[2][row] * cameraDistance;
		perspectiveMatrix[2][row] = 0.f;
	}
	perspectiveMatrix[0][3] = 0.f;
	perspectiveMatrix[1][3] = 0.f;
	perspectiveMatrix[2][3] = 0.f;
	perspectiveMatrix[3][3] = cameraDistance;
}

void vrDioramaUpdateTable(const glm::mat4& perspectiveViewMatrix, float groundHeight, float cursorGroundHeight)
{
	if (!g_registered)
	{
		pie_SetStereoSceneRenderer(&g_renderer);
		g_registered = true;
	}
	g_renderer.update(perspectiveViewMatrix, groundHeight, cursorGroundHeight);
}

bool vrDioramaRenderingEye()
{
	return g_renderer.renderingEye();
}

bool vrDioramaSkyMatrices(glm::mat4& perspective, glm::mat4& view)
{
	if (!g_renderer.renderingEye())
	{
		return false;
	}
	perspective = g_renderer.skyPerspective();
	view = g_renderer.skyView();
	return true;
}

bool vrDioramaDrawingWorldOverlay()
{
	return g_renderer.drawingWorldOverlay();
}

bool vrDioramaDrawingHud()
{
	return g_renderer.drawingHud();
}

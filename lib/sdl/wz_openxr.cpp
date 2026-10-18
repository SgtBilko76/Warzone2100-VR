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

#include "wz_openxr.h" // Must be prior to frame.h include
#include "lib/framework/frame.h"
#include "lib/framework/input.h"
#include "lib/framework/physfs_ext.h"
#include "lib/framework/wzapp.h"
#include "lib/ivis_opengl/screen.h"

#include <limits>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#if defined(__ANDROID__)
# include <jni.h>
# include <SDL3/SDL_system.h>
# define XR_USE_PLATFORM_ANDROID
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace
{

const char* xrResultStr(XrInstance instance, XrResult result)
{
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, buffer)))
	{
		snprintf(buffer, sizeof(buffer), "XrResult(%d)", static_cast<int>(result));
	}
	return buffer;
}

#define XR_CHECK_OR_RETURN(retval, call) \
	do { \
		XrResult _res = (call); \
		if (XR_FAILED(_res)) \
		{ \
			debug(LOG_ERROR, "%s failed: %s", #call, xrResultStr(g_instance, _res)); \
			return retval; \
		} \
	} while (0)

XrInstance g_instance = XR_NULL_HANDLE;

// The virtual screen the game renders to
constexpr int32_t kScreenWidth = 1920;
constexpr int32_t kScreenHeight = 1080;

// Layout of the virtual room (as in the 0 A.D. Quest port), in meters relative to the anchor - the
// recentred head pose, yaw only (OpenXR: x right, y up, -z forward). The game's "tracking space"
// (table placement, eye views) is this anchor space.
// The panel shows the menus, and in game the HUD (transparent where the battlefield is)
constexpr float kPanelUp = 0.08f;
constexpr float kPanelForward = 1.6f;
constexpr float kPanelWidth = 1.6f;
constexpr float kPanelHeight = kPanelWidth * static_cast<float>(kScreenHeight) / static_cast<float>(kScreenWidth);

// The laser: from the right controller's aim pose
constexpr float kLaserWidth = 0.003f;
constexpr float kLaserMissLength = 2.f;
constexpr double kRecenterHoldSeconds = 1.0;

struct Ray
{
	glm::vec3 origin;
	glm::vec3 direction;
};

// Intersects a ray with a quad layer (in its local XY plane, centred, image top at +Y). Returns the distance and
// the hit's image coordinates (0..1, top left origin).
bool intersectQuad(const Ray& ray, const glm::mat4& spaceFromQuad, float width, float height, float& outDistance, float& outU, float& outV)
{
	const glm::mat4 quadFromSpace = glm::inverse(spaceFromQuad);
	const glm::vec3 o = glm::vec3(quadFromSpace * glm::vec4(ray.origin, 1.f));
	const glm::vec3 d = glm::vec3(quadFromSpace * glm::vec4(ray.direction, 0.f));
	if (std::abs(d.z) < 1e-5f)
	{
		return false;
	}
	const float t = -o.z / d.z;
	if (t <= 0.f)
	{
		return false;
	}
	const glm::vec3 p = o + d * t;
	const float u = p.x / width + 0.5f;
	const float v = 0.5f - p.y / height;
	if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
	{
		return false;
	}
	outDistance = t;
	outU = u;
	outV = v;
	return true;
}

glm::mat4 poseToMatrix(const XrPosef& pose)
{
	const glm::quat orientation(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
	return glm::translate(glm::mat4(1.f), glm::vec3(pose.position.x, pose.position.y, pose.position.z)) * glm::mat4_cast(orientation);
}

class OpenXRPresenter final : public gfx_api::vk_external_presenter
{
public:
	bool initInstance();

	// vk_external_presenter
	bool createInstance(PFN_vkGetInstanceProcAddr gpa, const VkInstanceCreateInfo& createInfo, VkInstance& outInstance) override;
	bool getPhysicalDevice(VkInstance instance, VkPhysicalDevice& outPhysicalDevice) override;
	bool createDevice(PFN_vkGetInstanceProcAddr gpa, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& createInfo, VkDevice& outDevice) override;
	bool deviceReady(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex) override;
	bool createTargetImages(const std::vector<VkFormat>& preferredFormats, VkFormat& outFormat, VkExtent2D& outExtent, std::vector<VkImage>& outImages) override;
	void destroyTargetImages() override;
	bool acquireTargetImage(uint32_t& outIndex) override;
	void presentTargetImage() override;
	void shutdown() override;

	bool supportsStereo() const override { return m_eyeSwapchains[0] != XR_NULL_HANDLE && m_eyeSwapchains[1] != XR_NULL_HANDLE && m_hudSwapchain != XR_NULL_HANDLE; }
	bool getRecommendedEyeExtent(VkExtent2D& outExtent) override;
	bool getEyeTarget(size_t eye, VkImage& outImage, VkExtent2D& outExtent, gfx_api::StereoEyeView& outView) override;
	void setStereoFramePresented(bool presented) override { m_stereoPresented = presented; }
	void setStereoOverlayPlacement(const glm::mat4& trackingFromQuad, float widthMeters, float heightMeters) override;
	bool getHudTarget(VkImage& outImage, VkExtent2D& outExtent) override;
	void setStereoHudPlacement(const glm::mat4& trackingFromQuad, float widthMeters, float heightMeters) override;

	void pollEvents();
	bool exitRequested() const { return m_exitRequested; }
	bool updateControllers(wzxr::ControllerState& out);
	void haptic(bool rightHand, float amplitude, float seconds);
	bool mixedRealityActive() const { return m_mixedReality && m_passthroughLayer != XR_NULL_HANDLE; }
	void destroyInstance();

private:
	bool hasExtension(const char* name) const;
	bool createActions();
	XrAction createAction(XrActionType type, const char* name, const char* localizedName);
	void handleSessionStateChange(XrSessionState newState);
	void endFrameWithoutLayers();
	bool createColorSwapchain(uint32_t width, uint32_t height, VkFormat imageFormat, bool mutableSrgb, XrSwapchain& outSwapchain, std::vector<XrSwapchainImageVulkan2KHR>& outImages);
	void releaseEyeImages();

	// The anchor: recentred head pose (yaw only), the game's tracking space
	glm::mat4 localFromAnchor() const;
	bool locateInAnchor(XrSpace space, glm::mat4& outAnchorFromSpace) const;
	void recenter();
	glm::mat4 panelFromAnchorSpace() const; // anchor space <- panel quad
	void updateLaser(wzxr::ControllerState& out);
	bool fillLaserImage();
	bool buildLaserLayer(XrCompositionLayerQuad& outLayer);
	void startPassthrough();
	void stopPassthrough();
	void updateMixedRealityToggle(bool leftStickClick, bool rightStickClick);
	void saveMixedReality() const;

private:
	std::vector<XrExtensionProperties> m_availableExtensions;
	XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_localSpace = XR_NULL_HANDLE;
	XrSpace m_viewSpace = XR_NULL_HANDLE; // head-locked layers (the in-game HUD)
	XrSwapchain m_screenSwapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkan2KHR> m_screenImages;
	VkFormat m_viewFormat = VK_FORMAT_UNDEFINED;

	// Controller input
	XrActionSet m_actionSet = XR_NULL_HANDLE;
	XrAction m_leftStick = XR_NULL_HANDLE, m_rightStick = XR_NULL_HANDLE;
	XrAction m_leftTrigger = XR_NULL_HANDLE, m_rightTrigger = XR_NULL_HANDLE;
	XrAction m_leftGrip = XR_NULL_HANDLE, m_rightGrip = XR_NULL_HANDLE;
	XrAction m_buttonA = XR_NULL_HANDLE, m_buttonB = XR_NULL_HANDLE, m_buttonX = XR_NULL_HANDLE, m_buttonY = XR_NULL_HANDLE;
	XrAction m_menu = XR_NULL_HANDLE;
	XrAction m_leftStickClick = XR_NULL_HANDLE, m_rightStickClick = XR_NULL_HANDLE;
	XrAction m_rightAim = XR_NULL_HANDLE;
	XrSpace m_rightAimSpace = XR_NULL_HANDLE;
	XrAction m_leftHaptic = XR_NULL_HANDLE, m_rightHaptic = XR_NULL_HANDLE;
	bool m_isPico = false;

	// Mixed reality (XR_FB_passthrough): the real room around the panel and the table
	XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
	XrPassthroughLayerFB m_passthroughLayer = XR_NULL_HANDLE;
	bool m_mixedReality = false;
	bool m_prevLeftStickClick = false;
	bool m_leftClickAlone = false;
	double m_leftClickStart = 0.0;

	// Anchor (see localFromAnchor)
	bool m_anchorSet = false;
	float m_anchorYaw = 0.f;
	glm::vec3 m_anchorPosition{0.f};
	double m_recenterStart = -1.0; // both stick clicks held since (seconds), -1: not held

	// Laser, in anchor space (from the last controller update)
	bool m_laserValid = false;   // the aim pose is tracked
	glm::vec3 m_laserStart{0.f};
	glm::vec3 m_laserEnd{0.f};
	glm::vec3 m_headPosition{0.f, 0.f, 0.f};
	bool m_lastFrameStereo = false; // the table was shown last frame (in game)
	glm::mat4 m_overlayFromAnchor{1.f}; // anchor space <- table overlay quad

	// The laser beam: a thin quad layer showing a static solid-colour image
	XrSwapchain m_laserSwapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkan2KHR> m_laserImages;
	bool m_laserImageFilled = false;
	bool m_laserImageFailed = false;
	VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
	PFN_vkGetInstanceProcAddr m_vkGetInstanceProcAddr = nullptr;
	VkInstance m_vkInstance = VK_NULL_HANDLE;
	VkDevice m_vkDevice = VK_NULL_HANDLE;
	uint32_t m_vkQueueFamily = 0, m_vkQueueIndex = 0;

	XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
	bool m_sessionRunning = false;
	bool m_exitRequested = false;

	// Stereo: one swapchain per eye for the in-game scene
	XrViewConfigurationView m_viewConfig[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	XrSwapchain m_eyeSwapchains[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	std::vector<XrSwapchainImageVulkan2KHR> m_eyeImages[2];
	bool m_eyeAcquired[2] = { false, false };
	uint32_t m_eyeImageIndex[2] = { 0, 0 };
	XrView m_views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	bool m_viewsValid = false;
	bool m_stereoPresented = false;
	uint32_t m_eyeWidth = 0, m_eyeHeight = 0;
	XrExtent2Df m_overlaySize = { 1.f, 1.f };
	// Stereo: the in-game HUD on its own upright panel
	XrSwapchain m_hudSwapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkan2KHR> m_hudImages;
	bool m_hudAcquired = false;
	uint32_t m_hudImageIndex = 0;

	// Frame state
	bool m_frameBegun = false;
	bool m_imageAcquired = false;
	XrTime m_predictedDisplayTime = 0;

#if defined(__ANDROID__)
	jobject m_activity = nullptr;
#endif
};

OpenXRPresenter* g_presenter = nullptr;

bool OpenXRPresenter::hasExtension(const char* name) const
{
	return std::any_of(m_availableExtensions.begin(), m_availableExtensions.end(), [name](const XrExtensionProperties& p) {
		return strcmp(p.extensionName, name) == 0;
	});
}

bool OpenXRPresenter::initInstance()
{
#if defined(__ANDROID__)
	JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
	jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
	if (!env || !activity)
	{
		debug(LOG_ERROR, "OpenXR: no Android JNI environment / activity");
		return false;
	}
	JavaVM* vm = nullptr;
	env->GetJavaVM(&vm);
	m_activity = env->NewGlobalRef(activity);
	env->DeleteLocalRef(activity);

	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR))) || !xrInitializeLoaderKHR)
	{
		debug(LOG_ERROR, "OpenXR: xrInitializeLoaderKHR not found");
		return false;
	}
	XrLoaderInitInfoAndroidKHR loaderInitInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInitInfo.applicationVM = vm;
	loaderInitInfo.applicationContext = m_activity;
	XR_CHECK_OR_RETURN(false, xrInitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfo)));
#endif

	uint32_t extCount = 0;
	XR_CHECK_OR_RETURN(false, xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
	m_availableExtensions.assign(extCount, XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES });
	XR_CHECK_OR_RETURN(false, xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, m_availableExtensions.data()));

	if (!hasExtension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
	{
		debug(LOG_ERROR, "OpenXR: runtime lacks %s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		return false;
	}

	std::vector<const char*> extensions = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
#if defined(__ANDROID__)
	extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
	for (const char* optional : { XR_FB_PASSTHROUGH_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_FB_COLOR_SPACE_EXTENSION_NAME, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME })
	{
		if (hasExtension(optional))
		{
			extensions.push_back(optional);
		}
	}

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
#if defined(__ANDROID__)
	XrInstanceCreateInfoAndroidKHR androidCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidCreateInfo.applicationVM = vm;
	androidCreateInfo.applicationActivity = m_activity;
	createInfo.next = &androidCreateInfo;
#endif
	strncpy(createInfo.applicationInfo.applicationName, "Warzone 2100", XR_MAX_APPLICATION_NAME_SIZE - 1);
	createInfo.applicationInfo.applicationVersion = 1;
	strncpy(createInfo.applicationInfo.engineName, "Warzone 2100", XR_MAX_ENGINE_NAME_SIZE - 1);
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.enabledExtensionNames = extensions.data();
	XR_CHECK_OR_RETURN(false, xrCreateInstance(&createInfo, &g_instance));

	XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props)))
	{
		debug(LOG_INFO, "OpenXR runtime: %s %u.%u.%u", props.runtimeName,
			  XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
	}

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK_OR_RETURN(false, xrGetSystem(g_instance, &systemInfo, &m_systemId));

	// Pico headsets (weaker GPU than the Quest 3) get a smaller eye image, as in the 0 A.D. port
	XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
	XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	std::string names;
	if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, m_systemId, &systemProperties)))
	{
		names += systemProperties.systemName;
	}
	if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &instanceProperties)))
	{
		names += " ";
		names += instanceProperties.runtimeName;
	}
	std::transform(names.begin(), names.end(), names.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	m_isPico = names.find("pico") != std::string::npos || names.find("bytedance") != std::string::npos;
	debug(LOG_INFO, "OpenXR system: %s%s", names.c_str(), m_isPico ? " (Pico)" : "");
	return true;
}

bool OpenXRPresenter::createInstance(PFN_vkGetInstanceProcAddr gpa, const VkInstanceCreateInfo& vkCreateInfo, VkInstance& outInstance)
{
	PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetRequirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR pfnCreateVulkanInstance = nullptr;
	XR_CHECK_OR_RETURN(false, xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetRequirements)));
	XR_CHECK_OR_RETURN(false, xrGetInstanceProcAddr(g_instance, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVulkanInstance)));

	// Must be called before creating the Vulkan instance
	XrGraphicsRequirementsVulkan2KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
	XR_CHECK_OR_RETURN(false, pfnGetRequirements(g_instance, m_systemId, &requirements));
	const uint32_t requestedApi = vkCreateInfo.pApplicationInfo ? vkCreateInfo.pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
	debug(LOG_3D, "OpenXR Vulkan requirements: min %u.%u, max %u.%u, requested %u.%u",
		  XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
		  XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported),
		  VK_VERSION_MAJOR(requestedApi), VK_VERSION_MINOR(requestedApi));

	XrVulkanInstanceCreateInfoKHR createInfo = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
	createInfo.systemId = m_systemId;
	createInfo.pfnGetInstanceProcAddr = gpa;
	createInfo.vulkanCreateInfo = &vkCreateInfo;
	createInfo.vulkanAllocator = nullptr;
	VkResult vkResult = VK_SUCCESS;
	XR_CHECK_OR_RETURN(false, pfnCreateVulkanInstance(g_instance, &createInfo, &outInstance, &vkResult));
	if (vkResult != VK_SUCCESS)
	{
		debug(LOG_ERROR, "xrCreateVulkanInstanceKHR: vkCreateInstance failed: %d", static_cast<int>(vkResult));
		return false;
	}
	m_vkGetInstanceProcAddr = gpa; // for the laser image fill
	return true;
}

bool OpenXRPresenter::getPhysicalDevice(VkInstance instance, VkPhysicalDevice& outPhysicalDevice)
{
	PFN_xrGetVulkanGraphicsDevice2KHR pfnGetDevice = nullptr;
	XR_CHECK_OR_RETURN(false, xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDevice)));
	XrVulkanGraphicsDeviceGetInfoKHR getInfo = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
	getInfo.systemId = m_systemId;
	getInfo.vulkanInstance = instance;
	XR_CHECK_OR_RETURN(false, pfnGetDevice(g_instance, &getInfo, &outPhysicalDevice));
	return true;
}

bool OpenXRPresenter::createDevice(PFN_vkGetInstanceProcAddr gpa, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& vkCreateInfo, VkDevice& outDevice)
{
	PFN_xrCreateVulkanDeviceKHR pfnCreateDevice = nullptr;
	XR_CHECK_OR_RETURN(false, xrGetInstanceProcAddr(g_instance, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateDevice)));
	XrVulkanDeviceCreateInfoKHR createInfo = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
	createInfo.systemId = m_systemId;
	createInfo.pfnGetInstanceProcAddr = gpa;
	createInfo.vulkanPhysicalDevice = physicalDevice;
	createInfo.vulkanCreateInfo = &vkCreateInfo;
	VkResult vkResult = VK_SUCCESS;
	XR_CHECK_OR_RETURN(false, pfnCreateDevice(g_instance, &createInfo, &outDevice, &vkResult));
	if (vkResult != VK_SUCCESS)
	{
		debug(LOG_ERROR, "xrCreateVulkanDeviceKHR: vkCreateDevice failed: %d", static_cast<int>(vkResult));
		return false;
	}
	return true;
}

bool OpenXRPresenter::deviceReady(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex)
{
	XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
	binding.instance = instance;
	binding.physicalDevice = physicalDevice;
	binding.device = device;
	binding.queueFamilyIndex = queueFamilyIndex;
	binding.queueIndex = queueIndex;
	m_vkInstance = instance;
	m_vkDevice = device;
	m_vkQueueFamily = queueFamilyIndex;
	m_vkQueueIndex = queueIndex;

	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = m_systemId;
	XR_CHECK_OR_RETURN(false, xrCreateSession(g_instance, &sessionInfo, &m_session));

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	XR_CHECK_OR_RETURN(false, xrCreateReferenceSpace(m_session, &spaceInfo, &m_localSpace));
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK_OR_RETURN(false, xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace));

	// Prefer the highest refresh rate the headset offers
	if (hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumRates = nullptr;
		PFN_xrRequestDisplayRefreshRateFB pfnRequestRate = nullptr;
		xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnEnumRates));
		xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnRequestRate));
		uint32_t count = 0;
		if (pfnEnumRates && pfnRequestRate && XR_SUCCEEDED(pfnEnumRates(m_session, 0, &count, nullptr)) && count > 0)
		{
			std::vector<float> rates(count);
			pfnEnumRates(m_session, count, &count, rates.data());
			// 72 Hz keeps GPU headroom for the full 3D scene; raise once performance allows
			const float wanted = 72.f;
			if (std::find(rates.begin(), rates.end(), wanted) != rates.end())
			{
				pfnRequestRate(m_session, wanted);
			}
		}
	}

	uint32_t viewCount = 0;
	if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, m_viewConfig)) && viewCount == 2)
	{
		// As in the 0 A.D. Quest port: a fixed 1840 px per eye (the recommended size varies between
		// launches), with the recommended aspect - or the Quest 3 panel's (2064x2208) when the runtime
		// reports an implausible one
		const float kEyeWidth = m_isPico ? 1600.f : 1840.f;
		const float recommendedWidth = static_cast<float>(m_viewConfig[0].recommendedImageRectWidth);
		const float recommendedHeight = static_cast<float>(m_viewConfig[0].recommendedImageRectHeight);
		const float width = std::min(kEyeWidth, static_cast<float>(m_viewConfig[0].maxImageRectWidth));
		float aspect = recommendedHeight / std::max(1.f, recommendedWidth);
		if (aspect < 0.9f || aspect > 1.2f)
		{
			aspect = 2208.f / 2064.f;
		}
		m_eyeWidth = static_cast<uint32_t>(width) & ~7u;
		m_eyeHeight = std::min(static_cast<uint32_t>(width * aspect), m_viewConfig[0].maxImageRectHeight) & ~7u;
		debug(LOG_INFO, "OpenXR eye resolution: %ux%u (recommended %ux%u, max %ux%u)", m_eyeWidth, m_eyeHeight,
			  m_viewConfig[0].recommendedImageRectWidth, m_viewConfig[0].recommendedImageRectHeight,
			  m_viewConfig[0].maxImageRectWidth, m_viewConfig[0].maxImageRectHeight);
	}
	else
	{
		m_eyeWidth = m_eyeHeight = 0;
		debug(LOG_ERROR, "OpenXR: no stereo view configuration - stereo rendering unavailable");
	}

	if (!createActions())
	{
		debug(LOG_ERROR, "OpenXR: controller actions unavailable - no controller input");
	}
	startPassthrough();

	debug(LOG_INFO, "OpenXR session created");
	return true;
}

XrAction OpenXRPresenter::createAction(XrActionType type, const char* name, const char* localizedName)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	XrAction action = XR_NULL_HANDLE;
	XR_CHECK_OR_RETURN(XR_NULL_HANDLE, xrCreateAction(m_actionSet, &info, &action));
	return action;
}

bool OpenXRPresenter::createActions()
{
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(setInfo.actionSetName, "gamepad", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(setInfo.localizedActionSetName, "Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	XR_CHECK_OR_RETURN(false, xrCreateActionSet(g_instance, &setInfo, &m_actionSet));

	m_leftStick = createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Left stick");
	m_rightStick = createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Right stick");
	m_leftTrigger = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Left trigger");
	m_rightTrigger = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "Right trigger");
	m_leftGrip = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Left grip");
	m_rightGrip = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "Right grip");
	m_buttonA = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A");
	m_buttonB = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B");
	m_buttonX = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X");
	m_buttonY = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y");
	m_menu = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");
	m_leftStickClick = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Left stick click");
	m_rightStickClick = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click", "Right stick click");
	m_rightAim = createAction(XR_ACTION_TYPE_POSE_INPUT, "right_aim", "Laser pointer");
	m_leftHaptic = createAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "left_haptic", "Left vibration");
	m_rightHaptic = createAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "right_haptic", "Right vibration");

	struct Binding { XrAction action; const char* path; };
	const Binding bindings[] = {
		{ m_rightAim, "/user/hand/right/input/aim/pose" },
		{ m_leftHaptic, "/user/hand/left/output/haptic" },
		{ m_rightHaptic, "/user/hand/right/output/haptic" },
		{ m_leftStick, "/user/hand/left/input/thumbstick" },
		{ m_rightStick, "/user/hand/right/input/thumbstick" },
		{ m_leftTrigger, "/user/hand/left/input/trigger/value" },
		{ m_rightTrigger, "/user/hand/right/input/trigger/value" },
		{ m_leftGrip, "/user/hand/left/input/squeeze/value" },
		{ m_rightGrip, "/user/hand/right/input/squeeze/value" },
		{ m_buttonA, "/user/hand/right/input/a/click" },
		{ m_buttonB, "/user/hand/right/input/b/click" },
		{ m_buttonX, "/user/hand/left/input/x/click" },
		{ m_buttonY, "/user/hand/left/input/y/click" },
		{ m_menu, "/user/hand/left/input/menu/click" },
		{ m_leftStickClick, "/user/hand/left/input/thumbstick/click" },
		{ m_rightStickClick, "/user/hand/right/input/thumbstick/click" },
	};
	std::vector<XrActionSuggestedBinding> suggested;
	for (const auto& b : bindings)
	{
		ASSERT_OR_RETURN(false, b.action != XR_NULL_HANDLE, "Failed to create action for %s", b.path);
		XrPath path = XR_NULL_PATH;
		XR_CHECK_OR_RETURN(false, xrStringToPath(g_instance, b.path, &path));
		suggested.push_back({ b.action, path });
	}

	// Meta Touch and Pico controllers have the same buttons and paths
	std::vector<const char*> profiles = { "/interaction_profiles/oculus/touch_controller" };
	if (hasExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME))
	{
		profiles.push_back("/interaction_profiles/bytedance/pico4_controller");
		profiles.push_back("/interaction_profiles/bytedance/pico4s_controller");
		profiles.push_back("/interaction_profiles/bytedance/pico_neo3_controller");
	}
	bool anyProfile = false;
	for (const char* profileName : profiles)
	{
		XrPath profile = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(g_instance, profileName, &profile)))
		{
			continue;
		}
		XrInteractionProfileSuggestedBinding suggestedBindings = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggestedBindings.interactionProfile = profile;
		suggestedBindings.suggestedBindings = suggested.data();
		suggestedBindings.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
		const XrResult res = xrSuggestInteractionProfileBindings(g_instance, &suggestedBindings);
		if (XR_SUCCEEDED(res))
		{
			anyProfile = true;
		}
		else
		{
			debug(LOG_INFO, "OpenXR: no bindings for %s: %s", profileName, xrResultStr(g_instance, res));
		}
	}
	ASSERT_OR_RETURN(false, anyProfile, "OpenXR: no controller profile accepted");

	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &m_actionSet;
	XR_CHECK_OR_RETURN(false, xrAttachSessionActionSets(m_session, &attachInfo));

	XrActionSpaceCreateInfo aimSpaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	aimSpaceInfo.action = m_rightAim;
	aimSpaceInfo.poseInActionSpace.orientation.w = 1.f;
	XR_CHECK_OR_RETURN(false, xrCreateActionSpace(m_session, &aimSpaceInfo, &m_rightAimSpace));
	return true;
}

bool OpenXRPresenter::updateControllers(wzxr::ControllerState& out)
{
	out = wzxr::ControllerState();
	if (m_actionSet == XR_NULL_HANDLE || m_sessionState != XR_SESSION_STATE_FOCUSED)
	{
		return false;
	}

	XrActiveActionSet activeSet = { m_actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	if (XR_FAILED(xrSyncActions(m_session, &syncInfo)))
	{
		return false;
	}

	auto getVec2 = [this](XrAction action, float& x, float& y) {
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
		if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &info, &state)) && state.isActive)
		{
			x = state.currentState.x;
			y = -state.currentState.y; // OpenXR: up is positive; gamepads: down is positive
		}
	};
	auto getFloat = [this](XrAction action) {
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
		return (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &info, &state)) && state.isActive) ? state.currentState : 0.f;
	};
	auto getBool = [this](XrAction action) {
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &state)) && state.isActive && state.currentState;
	};

	getVec2(m_leftStick, out.leftX, out.leftY);
	getVec2(m_rightStick, out.rightX, out.rightY);
	out.leftTrigger = getFloat(m_leftTrigger);
	out.rightTrigger = getFloat(m_rightTrigger);
	out.leftGrip = getFloat(m_leftGrip);
	out.rightGrip = getFloat(m_rightGrip);
	out.a = getBool(m_buttonA);
	out.b = getBool(m_buttonB);
	out.x = getBool(m_buttonX);
	out.y = getBool(m_buttonY);
	out.menu = getBool(m_menu);
	out.leftStickClick = getBool(m_leftStickClick);
	out.rightStickClick = getBool(m_rightStickClick);

	// The head, for the anchor and the laser beam's facing
	glm::mat4 anchorFromHead(1.f);
	if (locateInAnchor(m_viewSpace, anchorFromHead))
	{
		if (!m_anchorSet)
		{
			recenter(); // first tracked pose: place the room around the player
			locateInAnchor(m_viewSpace, anchorFromHead);
		}
		m_headPosition = glm::vec3(anchorFromHead[3]);
	}

	// Recenter: hold both stick clicks for a second (as in 0 A.D.)
	if (out.leftStickClick && out.rightStickClick)
	{
		const double now = static_cast<double>(wzGetTicks()) / 1000.0;
		if (m_recenterStart < 0.0)
		{
			m_recenterStart = now;
		}
		else if (now - m_recenterStart > kRecenterHoldSeconds)
		{
			recenter();
			haptic(false, 0.5f, 0.08f);
			haptic(true, 0.5f, 0.08f);
			m_recenterStart = std::numeric_limits<double>::max();
		}
		out.recentering = true;
	}
	else
	{
		m_recenterStart = -1.0;
	}

	updateMixedRealityToggle(out.leftStickClick, out.rightStickClick);
	updateLaser(out);
	return true;
}

static const char kMixedRealityFile[] = "vr_mixed_reality.txt";

void OpenXRPresenter::startPassthrough()
{
	if (PHYSFS_exists(kMixedRealityFile))
	{
		if (PHYSFS_File* f = PHYSFS_openRead(kMixedRealityFile))
		{
			char c = '0';
			WZ_PHYSFS_readBytes(f, &c, 1);
			PHYSFS_close(f);
			m_mixedReality = (c == '1');
		}
	}
	if (!hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME))
	{
		debug(LOG_INFO, "OpenXR: no passthrough on this headset - mixed reality unavailable");
		return;
	}
	PFN_xrCreatePassthroughFB pfnCreatePassthrough = nullptr;
	PFN_xrCreatePassthroughLayerFB pfnCreateLayer = nullptr;
	xrGetInstanceProcAddr(g_instance, "xrCreatePassthroughFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreatePassthrough));
	xrGetInstanceProcAddr(g_instance, "xrCreatePassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateLayer));
	if (!pfnCreatePassthrough || !pfnCreateLayer)
	{
		return;
	}
	XrPassthroughCreateInfoFB info = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
	info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	XrResult res = pfnCreatePassthrough(m_session, &info, &m_passthrough);
	if (XR_FAILED(res))
	{
		debug(LOG_ERROR, "xrCreatePassthroughFB failed: %s", xrResultStr(g_instance, res));
		m_passthrough = XR_NULL_HANDLE;
		return;
	}
	XrPassthroughLayerCreateInfoFB layerInfo = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
	layerInfo.passthrough = m_passthrough;
	layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	res = pfnCreateLayer(m_session, &layerInfo, &m_passthroughLayer);
	if (XR_FAILED(res))
	{
		debug(LOG_ERROR, "xrCreatePassthroughLayerFB failed: %s", xrResultStr(g_instance, res));
		m_passthroughLayer = XR_NULL_HANDLE;
		return;
	}
	debug(LOG_INFO, "OpenXR passthrough ready, mixed reality %s (short left stick click toggles)", m_mixedReality ? "on" : "off");
}

void OpenXRPresenter::stopPassthrough()
{
	PFN_xrDestroyPassthroughLayerFB pfnDestroyLayer = nullptr;
	PFN_xrDestroyPassthroughFB pfnDestroyPassthrough = nullptr;
	xrGetInstanceProcAddr(g_instance, "xrDestroyPassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyLayer));
	xrGetInstanceProcAddr(g_instance, "xrDestroyPassthroughFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyPassthrough));
	if (m_passthroughLayer != XR_NULL_HANDLE && pfnDestroyLayer)
	{
		pfnDestroyLayer(m_passthroughLayer);
	}
	m_passthroughLayer = XR_NULL_HANDLE;
	if (m_passthrough != XR_NULL_HANDLE && pfnDestroyPassthrough)
	{
		pfnDestroyPassthrough(m_passthrough);
	}
	m_passthrough = XR_NULL_HANDLE;
}

void OpenXRPresenter::saveMixedReality() const
{
	if (PHYSFS_File* f = PHYSFS_openWrite(kMixedRealityFile))
	{
		const char c = m_mixedReality ? '1' : '0';
		WZ_PHYSFS_writeBytes(f, &c, 1);
		PHYSFS_close(f);
	}
}

// As in the 0 A.D. Quest port: a short click of the left stick on its own toggles mixed reality
void OpenXRPresenter::updateMixedRealityToggle(bool leftStickClick, bool rightStickClick)
{
	const double now = static_cast<double>(wzGetTicks()) / 1000.0;
	if (leftStickClick && !m_prevLeftStickClick)
	{
		m_leftClickAlone = !rightStickClick;
		m_leftClickStart = now;
	}
	if (rightStickClick)
	{
		m_leftClickAlone = false; // recentering
	}
	if (!leftStickClick && m_prevLeftStickClick && m_leftClickAlone && now - m_leftClickStart < 0.6)
	{
		if (m_passthroughLayer != XR_NULL_HANDLE)
		{
			m_mixedReality = !m_mixedReality;
			saveMixedReality();
			haptic(false, 0.5f, 0.05f);
			debug(LOG_INFO, "Mixed reality %s", m_mixedReality ? "on" : "off");
		}
	}
	m_prevLeftStickClick = leftStickClick;
}

glm::mat4 OpenXRPresenter::localFromAnchor() const
{
	return glm::translate(glm::mat4(1.f), m_anchorPosition) * glm::rotate(glm::mat4(1.f), m_anchorYaw, glm::vec3(0.f, 1.f, 0.f));
}

bool OpenXRPresenter::locateInAnchor(XrSpace space, glm::mat4& outAnchorFromSpace) const
{
	if (space == XR_NULL_HANDLE || m_localSpace == XR_NULL_HANDLE || m_predictedDisplayTime == 0)
	{
		return false;
	}
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(space, m_localSpace, m_predictedDisplayTime, &location))
		|| !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		|| !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		return false;
	}
	outAnchorFromSpace = glm::inverse(localFromAnchor()) * poseToMatrix(location.pose);
	return true;
}

void OpenXRPresenter::recenter()
{
	// Use the head pose: keep only its yaw so the table stays level
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if (m_viewSpace == XR_NULL_HANDLE || m_predictedDisplayTime == 0
		|| XR_FAILED(xrLocateSpace(m_viewSpace, m_localSpace, m_predictedDisplayTime, &location))
		|| !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		|| !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		return;
	}
	const glm::mat4 localFromHead = poseToMatrix(location.pose);
	const glm::vec3 forward = glm::vec3(localFromHead * glm::vec4(0.f, 0.f, -1.f, 0.f));
	m_anchorYaw = std::atan2(-forward.x, -forward.z);
	m_anchorPosition = glm::vec3(localFromHead[3]);
	m_anchorSet = true;
	debug(LOG_INFO, "OpenXR: recentred (yaw %.1f, position %.2f %.2f %.2f)", glm::degrees(m_anchorYaw), m_anchorPosition.x, m_anchorPosition.y, m_anchorPosition.z);
}

glm::mat4 OpenXRPresenter::panelFromAnchorSpace() const
{
	return glm::translate(glm::mat4(1.f), glm::vec3(0.f, kPanelUp, -kPanelForward));
}

void OpenXRPresenter::updateLaser(wzxr::ControllerState& out)
{
	glm::mat4 anchorFromAim(1.f);
	m_laserValid = locateInAnchor(m_rightAimSpace, anchorFromAim);
	if (!m_laserValid)
	{
		return;
	}
	Ray ray;
	ray.direction = glm::normalize(glm::vec3(anchorFromAim * glm::vec4(0.f, 0.f, -1.f, 0.f)));
	ray.origin = glm::vec3(anchorFromAim[3]);
	m_laserStart = ray.origin + ray.direction * 0.02f;
	m_laserEnd = ray.origin + ray.direction * kLaserMissLength;

	// Panel (menus, or the HUD in game) and table (in game): the nearest hit wins - but on the HUD
	// only where there is interface to point at, the battlefield shows through the rest
	float panelDistance = 0.f, panelU = 0.f, panelV = 0.f;
	const bool panelHit = intersectQuad(ray, panelFromAnchorSpace(), kPanelWidth, kPanelHeight, panelDistance, panelU, panelV);
	float tableDistance = 0.f, tableU = 0.f, tableV = 0.f;
	const bool tableHit = m_lastFrameStereo && intersectQuad(ray, m_overlayFromAnchor, m_overlaySize.width, m_overlaySize.height, tableDistance, tableU, tableV);

	auto toLogical = [](float u, float v, int& x, int& y) {
		x = std::clamp(static_cast<int>(u * static_cast<float>(screenWidth)), 0, std::max(0, static_cast<int>(screenWidth) - 1));
		y = std::clamp(static_cast<int>(v * static_cast<float>(screenHeight)), 0, std::max(0, static_cast<int>(screenHeight) - 1));
	};
	int panelX = 0, panelY = 0;
	toLogical(panelU, panelV, panelX, panelY);
	const bool usePanel = panelHit && (!m_lastFrameStereo || inputPointerTargetAt(panelX, panelY)) && (!tableHit || panelDistance <= tableDistance);
	if (usePanel)
	{
		out.pointerValid = true;
		out.pointerX = panelX;
		out.pointerY = panelY;
		m_laserEnd = ray.origin + ray.direction * panelDistance;
	}
	else if (tableHit)
	{
		out.pointerValid = true;
		toLogical(tableU, tableV, out.pointerX, out.pointerY);
		m_laserEnd = ray.origin + ray.direction * tableDistance;
	}
}

void OpenXRPresenter::haptic(bool rightHand, float amplitude, float seconds)
{
	const XrAction action = rightHand ? m_rightHaptic : m_leftHaptic;
	if (action == XR_NULL_HANDLE || m_sessionState != XR_SESSION_STATE_FOCUSED)
	{
		return;
	}
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = amplitude;
	vibration.duration = static_cast<XrDuration>(seconds * 1e9);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = action;
	xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXRPresenter::createColorSwapchain(uint32_t width, uint32_t height, VkFormat imageFormat, bool mutableSrgb, XrSwapchain& outSwapchain, std::vector<XrSwapchainImageVulkan2KHR>& outImages)
{
	XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	if (mutableSrgb)
	{
		createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	}
	createInfo.format = imageFormat;
	createInfo.sampleCount = 1;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	XR_CHECK_OR_RETURN(false, xrCreateSwapchain(m_session, &createInfo, &outSwapchain));

	uint32_t imageCount = 0;
	XR_CHECK_OR_RETURN(false, xrEnumerateSwapchainImages(outSwapchain, 0, &imageCount, nullptr));
	outImages.assign(imageCount, XrSwapchainImageVulkan2KHR{ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
	XR_CHECK_OR_RETURN(false, xrEnumerateSwapchainImages(outSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(outImages.data())));
	return true;
}

bool OpenXRPresenter::getRecommendedEyeExtent(VkExtent2D& outExtent)
{
	if (!supportsStereo())
	{
		return false;
	}
	outExtent = { m_eyeWidth, m_eyeHeight };
	return true;
}

bool OpenXRPresenter::getEyeTarget(size_t eye, VkImage& outImage, VkExtent2D& outExtent, gfx_api::StereoEyeView& outView)
{
	if (eye > 1 || !supportsStereo() || !m_frameBegun || !m_viewsValid)
	{
		return false;
	}
	if (!m_eyeAcquired[eye])
	{
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XR_CHECK_OR_RETURN(false, xrAcquireSwapchainImage(m_eyeSwapchains[eye], &acquireInfo, &m_eyeImageIndex[eye]));
		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		XR_CHECK_OR_RETURN(false, xrWaitSwapchainImage(m_eyeSwapchains[eye], &waitInfo));
		m_eyeAcquired[eye] = true;
	}
	outImage = m_eyeImages[eye][m_eyeImageIndex[eye]].image;
	outExtent = { m_eyeWidth, m_eyeHeight };

	const XrPosef& pose = m_views[eye].pose;
	// the game's tracking space is the anchor space
	outView.viewFromTracking = glm::inverse(poseToMatrix(pose)) * localFromAnchor();
	outView.tanLeft = std::tan(m_views[eye].fov.angleLeft);
	outView.tanRight = std::tan(m_views[eye].fov.angleRight);
	outView.tanUp = std::tan(m_views[eye].fov.angleUp);
	outView.tanDown = std::tan(m_views[eye].fov.angleDown);
	outView.width = outExtent.width;
	outView.height = outExtent.height;
	return true;
}

static XrPosef toXrPose(const glm::mat4& trackingFromQuad)
{
	const glm::quat q = glm::quat_cast(glm::mat3(trackingFromQuad));
	XrPosef pose;
	pose.orientation = { q.x, q.y, q.z, q.w };
	pose.position = { trackingFromQuad[3].x, trackingFromQuad[3].y, trackingFromQuad[3].z };
	return pose;
}

void OpenXRPresenter::setStereoOverlayPlacement(const glm::mat4& trackingFromQuad, float widthMeters, float heightMeters)
{
	m_overlayFromAnchor = trackingFromQuad;
	m_overlaySize = { widthMeters, heightMeters };
}

bool OpenXRPresenter::getHudTarget(VkImage& outImage, VkExtent2D& outExtent)
{
	if (!supportsStereo() || !m_frameBegun)
	{
		return false;
	}
	if (!m_hudAcquired)
	{
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XR_CHECK_OR_RETURN(false, xrAcquireSwapchainImage(m_hudSwapchain, &acquireInfo, &m_hudImageIndex));
		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		XR_CHECK_OR_RETURN(false, xrWaitSwapchainImage(m_hudSwapchain, &waitInfo));
		m_hudAcquired = true;
	}
	outImage = m_hudImages[m_hudImageIndex].image;
	outExtent = { static_cast<uint32_t>(kScreenWidth), static_cast<uint32_t>(kScreenHeight) };
	return true;
}

void OpenXRPresenter::setStereoHudPlacement(const glm::mat4&, float, float)
{
	// the HUD goes on the anchored panel, where the menus are (see presentTargetImage)
}

void OpenXRPresenter::releaseEyeImages()
{
	if (m_hudAcquired)
	{
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_hudSwapchain, &releaseInfo);
		m_hudAcquired = false;
	}
	for (int eye = 0; eye < 2; ++eye)
	{
		if (m_eyeAcquired[eye])
		{
			XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(m_eyeSwapchains[eye], &releaseInfo);
			m_eyeAcquired[eye] = false;
		}
	}
}

bool OpenXRPresenter::createTargetImages(const std::vector<VkFormat>& preferredFormats, VkFormat& outFormat, VkExtent2D& outExtent, std::vector<VkImage>& outImages)
{
	ASSERT_OR_RETURN(false, m_session != XR_NULL_HANDLE, "No XR session");

	if (m_screenSwapchain == XR_NULL_HANDLE)
	{
		uint32_t formatCount = 0;
		XR_CHECK_OR_RETURN(false, xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr));
		std::vector<int64_t> formats(formatCount);
		XR_CHECK_OR_RETURN(false, xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()));

		// The game writes gamma-encoded values: back the target with an sRGB image and render
		// through a UNORM view, so the compositor linearizes it correctly.
		auto srgbOf = [](VkFormat f) {
			switch (f)
			{
				case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
				case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
				default: return VK_FORMAT_UNDEFINED;
			}
		};
		VkFormat imageFormat = VK_FORMAT_UNDEFINED;
		bool mutableSrgb = false;
		for (VkFormat preferred : preferredFormats)
		{
			const VkFormat srgb = srgbOf(preferred);
			if (srgb != VK_FORMAT_UNDEFINED && std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) != formats.end())
			{
				imageFormat = srgb;
				m_viewFormat = preferred;
				mutableSrgb = true;
				break;
			}
			if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(preferred)) != formats.end())
			{
				imageFormat = preferred;
				m_viewFormat = preferred;
				break;
			}
		}
		ASSERT_OR_RETURN(false, imageFormat != VK_FORMAT_UNDEFINED, "OpenXR runtime offers none of the preferred swapchain formats");

		if (!createColorSwapchain(kScreenWidth, kScreenHeight, imageFormat, mutableSrgb, m_screenSwapchain, m_screenImages))
		{
			return false;
		}
		const uint32_t imageCount = static_cast<uint32_t>(m_screenImages.size());
		if (m_eyeWidth > 0 && m_eyeHeight > 0)
		{
			for (int eye = 0; eye < 2; ++eye)
			{
				if (!createColorSwapchain(m_eyeWidth, m_eyeHeight,
										  imageFormat, mutableSrgb, m_eyeSwapchains[eye], m_eyeImages[eye]))
				{
					debug(LOG_ERROR, "OpenXR: failed to create eye swapchain %d - stereo rendering unavailable", eye);
				}
			}
			if (!createColorSwapchain(kScreenWidth, kScreenHeight, imageFormat, mutableSrgb, m_hudSwapchain, m_hudImages))
			{
				debug(LOG_ERROR, "OpenXR: failed to create the HUD swapchain - stereo rendering unavailable");
			}
		}
		m_swapchainFormat = imageFormat;

		// The laser beam's image: tiny and static, filled with its colour once (fillLaserImage)
		XrSwapchainCreateInfo laserInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		laserInfo.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
		laserInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		laserInfo.format = imageFormat;
		laserInfo.sampleCount = 1;
		laserInfo.width = 8;
		laserInfo.height = 8;
		laserInfo.faceCount = 1;
		laserInfo.arraySize = 1;
		laserInfo.mipCount = 1;
		uint32_t laserImageCount = 0;
		if (XR_SUCCEEDED(xrCreateSwapchain(m_session, &laserInfo, &m_laserSwapchain))
			&& XR_SUCCEEDED(xrEnumerateSwapchainImages(m_laserSwapchain, 0, &laserImageCount, nullptr)) && laserImageCount > 0)
		{
			m_laserImages.assign(laserImageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
			xrEnumerateSwapchainImages(m_laserSwapchain, laserImageCount, &laserImageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(m_laserImages.data()));
		}
		else
		{
			debug(LOG_ERROR, "OpenXR: failed to create the laser swapchain - no laser beam");
			m_laserImageFailed = true;
		}
		debug(LOG_INFO, "OpenXR screen swapchain: %u images, %dx%d, format %d (view %d), stereo: %s", imageCount, kScreenWidth, kScreenHeight, (int)imageFormat, (int)m_viewFormat, supportsStereo() ? "yes" : "no");
	}

	outFormat = m_viewFormat;
	outExtent = { static_cast<uint32_t>(kScreenWidth), static_cast<uint32_t>(kScreenHeight) };
	outImages.clear();
	for (const auto& img : m_screenImages)
	{
		outImages.push_back(img.image);
	}
	return true;
}

void OpenXRPresenter::destroyTargetImages()
{
	if (m_imageAcquired)
	{
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_screenSwapchain, &releaseInfo);
		m_imageAcquired = false;
	}
	if (m_frameBegun)
	{
		endFrameWithoutLayers();
	}
	releaseEyeImages();
	if (m_screenSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(m_screenSwapchain);
		m_screenSwapchain = XR_NULL_HANDLE;
	}
	m_screenImages.clear();
	for (int eye = 0; eye < 2; ++eye)
	{
		if (m_eyeSwapchains[eye] != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(m_eyeSwapchains[eye]);
			m_eyeSwapchains[eye] = XR_NULL_HANDLE;
		}
		m_eyeImages[eye].clear();
	}
	if (m_hudSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(m_hudSwapchain);
		m_hudSwapchain = XR_NULL_HANDLE;
	}
	m_hudImages.clear();
	if (m_laserSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(m_laserSwapchain);
		m_laserSwapchain = XR_NULL_HANDLE;
	}
	m_laserImages.clear();
	m_laserImageFilled = false;
}

void OpenXRPresenter::endFrameWithoutLayers()
{
	releaseEyeImages();
	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = m_predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = 0;
	xrEndFrame(m_session, &endInfo);
	m_frameBegun = false;
}

bool OpenXRPresenter::fillLaserImage()
{
	if (m_laserImageFilled)
	{
		return true;
	}
	if (m_laserImageFailed || m_laserSwapchain == XR_NULL_HANDLE || m_laserImages.empty() || !m_vkGetInstanceProcAddr || m_vkDevice == VK_NULL_HANDLE)
	{
		return false;
	}
	m_laserImageFailed = true; // until it worked - don't retry every frame

	auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(m_vkGetInstanceProcAddr(m_vkInstance, "vkGetDeviceProcAddr"));
	if (!getDeviceProcAddr)
	{
		return false;
	}
#define WZ_XR_LOAD_VK(name) auto p_##name = reinterpret_cast<PFN_##name>(getDeviceProcAddr(m_vkDevice, #name)); if (!p_##name) { debug(LOG_ERROR, "OpenXR laser: %s unavailable", #name); return false; }
	WZ_XR_LOAD_VK(vkGetDeviceQueue)
	WZ_XR_LOAD_VK(vkCreateCommandPool)
	WZ_XR_LOAD_VK(vkDestroyCommandPool)
	WZ_XR_LOAD_VK(vkAllocateCommandBuffers)
	WZ_XR_LOAD_VK(vkBeginCommandBuffer)
	WZ_XR_LOAD_VK(vkEndCommandBuffer)
	WZ_XR_LOAD_VK(vkCmdPipelineBarrier)
	WZ_XR_LOAD_VK(vkCmdClearColorImage)
	WZ_XR_LOAD_VK(vkCreateFence)
	WZ_XR_LOAD_VK(vkDestroyFence)
	WZ_XR_LOAD_VK(vkWaitForFences)
	WZ_XR_LOAD_VK(vkQueueSubmit)
#undef WZ_XR_LOAD_VK

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XR_CHECK_OR_RETURN(false, xrAcquireSwapchainImage(m_laserSwapchain, &acquireInfo, &imageIndex));
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(m_laserSwapchain, &waitInfo)))
	{
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_laserSwapchain, &releaseInfo);
		return false;
	}
	const VkImage image = m_laserImages[imageIndex].image;

	VkQueue queue = VK_NULL_HANDLE;
	p_vkGetDeviceQueue(m_vkDevice, m_vkQueueFamily, m_vkQueueIndex, &queue);
	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = m_vkQueueFamily;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool ok = queue != VK_NULL_HANDLE && p_vkCreateCommandPool(m_vkDevice, &poolInfo, nullptr, &pool) == VK_SUCCESS;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (ok)
	{
		VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandPool = pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		ok = p_vkAllocateCommandBuffers(m_vkDevice, &allocInfo, &cmd) == VK_SUCCESS;
	}
	if (ok)
	{
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		p_vkBeginCommandBuffer(cmd, &beginInfo);

		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier toTransfer = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toTransfer.srcAccessMask = 0;
		toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.image = image;
		toTransfer.subresourceRange = range;
		p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

		VkClearColorValue colour = {};
		colour.float32[0] = 1.f; // red, as in 0 A.D. (linear values, the image is sRGB)
		colour.float32[1] = 0.012f;
		colour.float32[2] = 0.006f;
		colour.float32[3] = 1.f;
		p_vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);

		// the runtime expects colour swapchain images back in the colour attachment layout
		VkImageMemoryBarrier toAttachment = toTransfer;
		toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAttachment);
		ok = p_vkEndCommandBuffer(cmd) == VK_SUCCESS;
	}
	if (ok)
	{
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		ok = p_vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &fence) == VK_SUCCESS;
	}
	if (ok)
	{
		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		ok = p_vkQueueSubmit(queue, 1, &submitInfo, fence) == VK_SUCCESS
			&& p_vkWaitForFences(m_vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
	}
	if (fence != VK_NULL_HANDLE)
	{
		p_vkDestroyFence(m_vkDevice, fence, nullptr);
	}
	if (pool != VK_NULL_HANDLE)
	{
		p_vkDestroyCommandPool(m_vkDevice, pool, nullptr); // frees the command buffer
	}
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_laserSwapchain, &releaseInfo);
	if (!ok)
	{
		debug(LOG_ERROR, "OpenXR: filling the laser image failed - no laser beam");
		return false;
	}
	m_laserImageFilled = true;
	m_laserImageFailed = false;
	return true;
}

bool OpenXRPresenter::buildLaserLayer(XrCompositionLayerQuad& outLayer)
{
	if (!m_laserValid || !fillLaserImage())
	{
		return false;
	}
	const glm::vec3 axis = m_laserEnd - m_laserStart;
	const float length = glm::length(axis);
	if (length < 0.01f)
	{
		return false;
	}
	// a thin strip along the ray, turned to face the head
	const glm::vec3 y = axis / length;
	const glm::vec3 middle = (m_laserStart + m_laserEnd) * 0.5f;
	const glm::vec3 toHead = m_headPosition - middle;
	glm::vec3 z = toHead - y * glm::dot(toHead, y);
	if (glm::length(z) < 1e-4f)
	{
		z = std::abs(y.y) < 0.9f ? glm::cross(y, glm::vec3(0.f, 1.f, 0.f)) : glm::cross(y, glm::vec3(1.f, 0.f, 0.f));
	}
	z = glm::normalize(z);
	const glm::vec3 x = glm::normalize(glm::cross(y, z));
	z = glm::cross(x, y);
	glm::mat4 anchorFromLaser(1.f);
	anchorFromLaser[0] = glm::vec4(x, 0.f);
	anchorFromLaser[1] = glm::vec4(y, 0.f);
	anchorFromLaser[2] = glm::vec4(z, 0.f);
	anchorFromLaser[3] = glm::vec4(middle, 1.f);

	outLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	outLayer.layerFlags = 0;
	outLayer.space = m_localSpace;
	outLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	outLayer.subImage.swapchain = m_laserSwapchain;
	outLayer.subImage.imageRect.offset = { 0, 0 };
	outLayer.subImage.imageRect.extent = { 8, 8 };
	outLayer.subImage.imageArrayIndex = 0;
	outLayer.pose = toXrPose(localFromAnchor() * anchorFromLaser);
	outLayer.size = { kLaserWidth, length };
	return true;
}

bool OpenXRPresenter::acquireTargetImage(uint32_t& outIndex)
{
	pollEvents();
	if (!m_sessionRunning || m_screenSwapchain == XR_NULL_HANDLE)
	{
		return false;
	}

	if (!m_frameBegun)
	{
		XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frameState = { XR_TYPE_FRAME_STATE };
		XR_CHECK_OR_RETURN(false, xrWaitFrame(m_session, &waitInfo, &frameState));
		XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
		XR_CHECK_OR_RETURN(false, xrBeginFrame(m_session, &beginInfo));
		m_frameBegun = true;
		m_predictedDisplayTime = frameState.predictedDisplayTime;

		if (!frameState.shouldRender)
		{
			endFrameWithoutLayers();
			return false;
		}

		m_viewsValid = false;
		if (supportsStereo())
		{
			XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
			locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			locateInfo.displayTime = m_predictedDisplayTime;
			locateInfo.space = m_localSpace;
			XrViewState viewState = { XR_TYPE_VIEW_STATE };
			uint32_t viewCount = 0;
			m_views[0] = { XR_TYPE_VIEW };
			m_views[1] = { XR_TYPE_VIEW };
			if (XR_SUCCEEDED(xrLocateViews(m_session, &locateInfo, &viewState, 2, &viewCount, m_views)) && viewCount == 2
				&& (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
			{
				m_viewsValid = true;
			}
		}
	}

	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XR_CHECK_OR_RETURN(false, xrAcquireSwapchainImage(m_screenSwapchain, &acquireInfo, &outIndex));
	XrSwapchainImageWaitInfo imageWaitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	imageWaitInfo.timeout = XR_INFINITE_DURATION;
	XR_CHECK_OR_RETURN(false, xrWaitSwapchainImage(m_screenSwapchain, &imageWaitInfo));
	m_imageAcquired = true;
	return true;
}

void OpenXRPresenter::presentTargetImage()
{
	if (!m_imageAcquired)
	{
		if (m_frameBegun)
		{
			endFrameWithoutLayers();
		}
		return;
	}

	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_screenSwapchain, &releaseInfo);
	m_imageAcquired = false;

	const bool stereo = m_stereoPresented && m_eyeAcquired[0] && m_eyeAcquired[1];
	const bool hud = stereo && m_hudAcquired;
	const bool mixedReality = m_mixedReality && m_passthroughLayer != XR_NULL_HANDLE;

	XrCompositionLayerProjectionView projViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (stereo)
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			// the pose / fov the eye was rendered with
			projViews[eye].pose = m_views[eye].pose;
			projViews[eye].fov = m_views[eye].fov;
			projViews[eye].subImage.swapchain = m_eyeSwapchains[eye];
			projViews[eye].subImage.imageRect.offset = { 0, 0 };
			projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(m_eyeWidth), static_cast<int32_t>(m_eyeHeight) };
			projViews[eye].subImage.imageArrayIndex = 0;
		}
		projection.space = m_localSpace;
		projection.viewCount = 2;
		projection.views = projViews;
		if (mixedReality)
		{
			// the empty background is keyed transparent (see wzMixedRealityKey in the blit shaders)
			projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		}
	}
	releaseEyeImages();

	m_lastFrameStereo = stereo;
	const glm::mat4 anchor = localFromAnchor();
	const XrPosef panelPose = toXrPose(anchor * panelFromAnchorSpace());

	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	quad.space = m_localSpace;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage.swapchain = m_screenSwapchain;
	quad.subImage.imageRect.offset = { 0, 0 };
	quad.subImage.imageRect.extent = { kScreenWidth, kScreenHeight };
	quad.subImage.imageArrayIndex = 0;
	if (stereo)
	{
		// the 2D world overlay (cursor, unit markers) lies on the table, over the 3D scene; cleared transparent, premultiplied
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.pose = toXrPose(anchor * m_overlayFromAnchor);
		quad.size = m_overlaySize;
	}
	else
	{
		// menus: the opaque panel
		quad.layerFlags = 0;
		quad.pose = panelPose;
		quad.size = { kPanelWidth, kPanelHeight };
	}

	// in game the HUD floats on the same panel, over the battlefield
	XrCompositionLayerQuad hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	if (hud)
	{
		hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		hudQuad.space = m_localSpace;
		hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		hudQuad.subImage.swapchain = m_hudSwapchain;
		hudQuad.subImage.imageRect.offset = { 0, 0 };
		hudQuad.subImage.imageRect.extent = { kScreenWidth, kScreenHeight };
		hudQuad.subImage.imageArrayIndex = 0;
		hudQuad.pose = panelPose;
		hudQuad.size = { kPanelWidth, kPanelHeight };
	}

	XrCompositionLayerQuad laserQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const bool laser = buildLaserLayer(laserQuad);

	XrCompositionLayerPassthroughFB passthroughLayer = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
	const XrCompositionLayerBaseHeader* layers[5];
	uint32_t layerCount = 0;
	if (mixedReality)
	{
		// the real room, behind everything
		passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		passthroughLayer.space = XR_NULL_HANDLE;
		passthroughLayer.layerHandle = m_passthroughLayer;
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughLayer);
	}
	if (stereo)
	{
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
	}
	layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
	if (hud)
	{
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
	}
	if (laser)
	{
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&laserQuad);
	}
	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = m_predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	XrResult res = xrEndFrame(m_session, &endInfo);
	if (XR_FAILED(res))
	{
		debug(LOG_ERROR, "xrEndFrame failed: %s", xrResultStr(g_instance, res));
	}
	m_frameBegun = false;
}

void OpenXRPresenter::handleSessionStateChange(XrSessionState newState)
{
	debug(LOG_INFO, "OpenXR session state: %d -> %d", (int)m_sessionState, (int)newState);
	m_sessionState = newState;
	switch (newState)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
			beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			XrResult res = xrBeginSession(m_session, &beginInfo);
			if (XR_FAILED(res))
			{
				debug(LOG_ERROR, "xrBeginSession failed: %s", xrResultStr(g_instance, res));
				break;
			}
			m_sessionRunning = true;
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			if (m_imageAcquired)
			{
				XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_screenSwapchain, &releaseInfo);
				m_imageAcquired = false;
			}
			if (m_frameBegun)
			{
				endFrameWithoutLayers();
			}
			xrEndSession(m_session);
			m_sessionRunning = false;
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			m_sessionRunning = false;
			m_exitRequested = true;
			break;
		default:
			break;
	}
}

void OpenXRPresenter::pollEvents()
{
	if (g_instance == XR_NULL_HANDLE)
	{
		return;
	}
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(g_instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const auto& stateEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
				if (stateEvent.session == m_session)
				{
					handleSessionStateChange(stateEvent.state);
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				m_exitRequested = true;
				break;
			default:
				break;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

void OpenXRPresenter::shutdown()
{
	destroyTargetImages();
	stopPassthrough();
	if (m_rightAimSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_rightAimSpace);
		m_rightAimSpace = XR_NULL_HANDLE;
	}
	if (m_actionSet != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(m_actionSet); // also destroys its actions
		m_actionSet = XR_NULL_HANDLE;
	}
	if (m_viewSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_viewSpace);
		m_viewSpace = XR_NULL_HANDLE;
	}
	if (m_localSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_localSpace);
		m_localSpace = XR_NULL_HANDLE;
	}
	if (m_session != XR_NULL_HANDLE)
	{
		if (m_sessionRunning)
		{
			xrEndSession(m_session);
			m_sessionRunning = false;
		}
		xrDestroySession(m_session);
		m_session = XR_NULL_HANDLE;
	}
}

void OpenXRPresenter::destroyInstance()
{
	shutdown();
	if (g_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(g_instance);
		g_instance = XR_NULL_HANDLE;
	}
#if defined(__ANDROID__)
	if (m_activity)
	{
		JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
		if (env)
		{
			env->DeleteGlobalRef(m_activity);
		}
		m_activity = nullptr;
	}
#endif
}

} // anonymous namespace

namespace wzxr
{

bool initialize()
{
	if (g_presenter)
	{
		return true;
	}
	g_presenter = new OpenXRPresenter();
	if (!g_presenter->initInstance())
	{
		debug(LOG_ERROR, "OpenXR initialization failed");
		g_presenter->destroyInstance();
		delete g_presenter;
		g_presenter = nullptr;
		return false;
	}
	return true;
}

bool isActive()
{
	return g_presenter != nullptr;
}

gfx_api::vk_external_presenter* presenter()
{
	return g_presenter;
}

void pollEvents()
{
	if (g_presenter)
	{
		g_presenter->pollEvents();
	}
}

bool exitRequested()
{
	return g_presenter && g_presenter->exitRequested();
}

bool updateControllers(ControllerState& out)
{
	if (!g_presenter)
	{
		out = ControllerState();
		return false;
	}
	return g_presenter->updateControllers(out);
}

void haptic(bool rightHand, float amplitude, float seconds)
{
	if (g_presenter)
	{
		g_presenter->haptic(rightHand, amplitude, seconds);
	}
}

bool mixedRealityActive()
{
	return g_presenter && g_presenter->mixedRealityActive();
}

void getTargetSize(int& w, int& h)
{
	w = kScreenWidth;
	h = kScreenHeight;
}

void shutdown()
{
	if (g_presenter)
	{
		g_presenter->destroyInstance();
		delete g_presenter;
		g_presenter = nullptr;
	}
}

} // namespace wzxr

#endif // defined(WZ_OPENXR_ENABLED) && defined(WZ_VULKAN_ENABLED)

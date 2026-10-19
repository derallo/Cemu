#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VrRuntime.h"

#include "AndroidInputHelpers.h"
#include "input/emulated/EmulatedController.h"
#include "input/emulated/VPADController.h"

#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_EXTENSION_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "VrRuntimeAndroid.h"

#define LOG_TAG "CemuVR"

namespace
{
	// Logcat buffers roll over, and VR sessions are often tested hours after
	// they start. Mirror every message into the app's external files dir.
	std::string g_logFilePath;
	FILE* g_logFile = nullptr;
	std::mutex g_logMutex;

	void LogVr(int priority, const char* format, ...)
	{
		char buffer[2048];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		__android_log_print(priority, LOG_TAG, "%s", buffer);

		std::lock_guard<std::mutex> lock(g_logMutex);
		if (!g_logFilePath.empty())
		{
			if (!g_logFile)
				g_logFile = fopen(g_logFilePath.c_str(), "a");
			if (g_logFile)
			{
				fprintf(g_logFile, "%s\n", buffer);
				fflush(g_logFile);
			}
		}
	}
}

#define LOGI(...) LogVr(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGE(...) LogVr(ANDROID_LOG_ERROR, __VA_ARGS__)

// OpenXR virtual screen backend.
//
// The emulator keeps rendering into its own ring of VkImages (SwapchainInfoVk
// "external images" mode). Every frame is blitted into an OpenXR swapchain and
// submitted as a quad layer. Touch controllers are mapped onto a Wii U GamePad.

namespace
{
	struct VrState
	{
		bool initialized = false;
		std::atomic_bool sessionCreated{false};
		std::atomic_bool sessionRunning{false};
		std::atomic_bool exitRequested{false};
		std::atomic_bool inputThreadRun{false};

		JavaVM* vm = nullptr;
		jobject context = nullptr;

		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId systemId = XR_NULL_SYSTEM_ID;
		XrSession session = XR_NULL_HANDLE;
		XrSpace localSpace = XR_NULL_HANDLE;

		std::vector<std::string> instanceExtStorage;
		std::vector<const char*> instanceExts;
		std::vector<std::string> deviceExtStorage;
		std::vector<const char*> deviceExts;

		PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanGraphicsRequirements = nullptr;
		PFN_xrGetVulkanInstanceExtensionsKHR pfnGetVulkanInstanceExtensions = nullptr;
		PFN_xrGetVulkanGraphicsDeviceKHR pfnGetVulkanGraphicsDevice = nullptr;
		PFN_xrGetVulkanDeviceExtensionsKHR pfnGetVulkanDeviceExtensions = nullptr;

		VkInstance vkInstance = VK_NULL_HANDLE;
		VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice vkDevice = VK_NULL_HANDLE;
		uint32_t queueFamilyIndex = 0;
		VkQueue queue = VK_NULL_HANDLE;

		VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
		VkExtent2D extent{1280, 720};
		std::vector<VkImage> colorImages;
		std::vector<VkDeviceMemory> colorMemories;

		XrSwapchain swapchain = XR_NULL_HANDLE;
		std::vector<VkImage> swapchainImages;
		uint32_t swapchainWidth = 1280;
		uint32_t swapchainHeight = 720;

		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkFence frameFence = VK_NULL_HANDLE;

		XrActionSet actionSet = XR_NULL_HANDLE;
		XrAction triggerLeft = XR_NULL_HANDLE;
		XrAction triggerRight = XR_NULL_HANDLE;
		XrAction squeezeLeft = XR_NULL_HANDLE;
		XrAction squeezeRight = XR_NULL_HANDLE;
		XrAction thumbstickLeft = XR_NULL_HANDLE;
		XrAction thumbstickRight = XR_NULL_HANDLE;
		XrAction buttonA = XR_NULL_HANDLE;
		XrAction buttonB = XR_NULL_HANDLE;
		XrAction buttonX = XR_NULL_HANDLE;
		XrAction buttonY = XR_NULL_HANDLE;
		XrAction menuButton = XR_NULL_HANDLE;
		XrAction stickClickLeft = XR_NULL_HANDLE;
		XrAction stickClickRight = XR_NULL_HANDLE;

		bool controllerInitialized = false;
		bool exitChordLatched = false;
		bool userExitRequested = false;
		std::atomic_int lastSyncResult{0};
	};

	VrState g;

	const char* SessionStateName(XrSessionState state)
	{
		switch (state)
		{
		case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
		case XR_SESSION_STATE_IDLE: return "IDLE";
		case XR_SESSION_STATE_READY: return "READY";
		case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
		case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
		case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
		case XR_SESSION_STATE_STOPPING: return "STOPPING";
		case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
		case XR_SESSION_STATE_EXITING: return "EXITING";
		default: return "?";
		}
	}

	bool XrOk(XrResult result, const char* what)
	{
		if (XR_FAILED(result))
		{
			LOGE("%s failed: %d", what, result);
			return false;
		}
		return true;
	}

	std::vector<std::string> SplitExtensionList(const std::string& list)
	{
		std::vector<std::string> out;
		size_t start = 0;
		while (start < list.size())
		{
			while (start < list.size() && list[start] == ' ')
				start++;
			size_t end = list.find(' ', start);
			if (end == std::string::npos)
				end = list.size();
			if (end > start)
				out.emplace_back(list.substr(start, end - start));
			start = end + 1;
		}
		return out;
	}

	template<typename T>
	T GetXrFunction(const char* name)
	{
		T fn = nullptr;
		XrResult result = xrGetInstanceProcAddr(g.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn));
		if (XR_FAILED(result) || !fn)
		{
			LOGE("missing OpenXR entry point %s (%d)", name, result);
			return nullptr;
		}
		return fn;
	}

	bool InitializeLoader()
	{
		PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
		XrResult result = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
		if (XR_FAILED(result) || !initializeLoader)
		{
			LOGE("xrInitializeLoaderKHR not available: %d", result);
			return false;
		}

		XrLoaderInitInfoAndroidKHR initInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		initInfo.applicationVM = g.vm;
		initInfo.applicationContext = g.context;
		return XrOk(initializeLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&initInfo)), "xrInitializeLoaderKHR");
	}

	bool CreateAction(XrActionType type, const char* name, const char* localizedName, XrAction& out)
	{
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		return XrOk(xrCreateAction(g.actionSet, &info, &out), name);
	}

	bool CreateActions()
	{
		XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		if (!XrOk(xrCreateActionSet(g.instance, &setInfo, &g.actionSet), "xrCreateActionSet"))
			return false;

		if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_left", "Left trigger", g.triggerLeft)) return false;
		if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_right", "Right trigger", g.triggerRight)) return false;
		if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_left", "Left squeeze", g.squeezeLeft)) return false;
		if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_right", "Right squeeze", g.squeezeRight)) return false;
		if (!CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_left", "Left thumbstick", g.thumbstickLeft)) return false;
		if (!CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_right", "Right thumbstick", g.thumbstickRight)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", g.buttonA)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", g.buttonB)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", g.buttonX)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", g.buttonY)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", g.menuButton)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_left", "Left stick click", g.stickClickLeft)) return false;
		if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_right", "Right stick click", g.stickClickRight)) return false;

		XrPath touchProfile = XR_NULL_PATH;
		XrPath leftHand = XR_NULL_PATH, rightHand = XR_NULL_PATH;
		xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller", &touchProfile);
		xrStringToPath(g.instance, "/user/hand/left", &leftHand);
		xrStringToPath(g.instance, "/user/hand/right", &rightHand);

		struct Binding { XrAction action; const char* path; };
		std::vector<Binding> table = {
			{g.triggerLeft, "/user/hand/left/input/trigger/value"},
			{g.squeezeLeft, "/user/hand/left/input/squeeze/value"},
			{g.thumbstickLeft, "/user/hand/left/input/thumbstick"},
			{g.stickClickLeft, "/user/hand/left/input/thumbstick/click"},
			{g.menuButton, "/user/hand/left/input/menu/click"},
			{g.triggerRight, "/user/hand/right/input/trigger/value"},
			{g.squeezeRight, "/user/hand/right/input/squeeze/value"},
			{g.thumbstickRight, "/user/hand/right/input/thumbstick"},
			{g.stickClickRight, "/user/hand/right/input/thumbstick/click"},
			{g.buttonA, "/user/hand/right/input/a/click"},
			{g.buttonB, "/user/hand/right/input/b/click"},
			{g.buttonX, "/user/hand/left/input/x/click"},
			{g.buttonY, "/user/hand/left/input/y/click"},
		};

		std::vector<XrActionSuggestedBinding> bindings;
		std::vector<const char*> bindingPaths;
		for (auto& entry : table)
		{
			XrPath path = XR_NULL_PATH;
			XrResult pathResult = xrStringToPath(g.instance, entry.path, &path);
			if (XR_FAILED(pathResult) || path == XR_NULL_PATH)
				LOGE("bad binding path %s (%d)", entry.path, pathResult);
			bindings.push_back({entry.action, path});
			bindingPaths.push_back(entry.path);
		}

		XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggested.interactionProfile = touchProfile;
		suggested.countSuggestedBindings = (uint32_t)bindings.size();
		suggested.suggestedBindings = bindings.data();

		(void)leftHand;
		(void)rightHand;
		return XrOk(xrSuggestInteractionProfileBindings(g.instance, &suggested), "xrSuggestInteractionProfileBindings");
	}

	bool CreateSession()
	{
		XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
		binding.instance = g.vkInstance;
		binding.physicalDevice = g.vkPhysicalDevice;
		binding.device = g.vkDevice;
		binding.queueFamilyIndex = g.queueFamilyIndex;
		binding.queueIndex = 0;

		XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
		info.next = &binding;
		info.systemId = g.systemId;
		if (!XrOk(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession"))
			return false;

		XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
		if (!XrOk(xrCreateReferenceSpace(g.session, &spaceInfo, &g.localSpace), "xrCreateReferenceSpace"))
			return false;

		XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attachInfo.countActionSets = 1;
		attachInfo.actionSets = &g.actionSet;
		if (!XrOk(xrAttachSessionActionSets(g.session, &attachInfo), "xrAttachSessionActionSets"))
			return false;

		g.sessionCreated = true;
		LOGI("OpenXR session created");
		return true;
	}

	bool CreateSwapchain()
	{
		uint32_t formatCount = 0;
		if (!XrOk(xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats(count)"))
			return false;
		std::vector<int64_t> formats(formatCount);
		if (!XrOk(xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats"))
			return false;

		int64_t chosen = formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0];
		for (int64_t preferred : { (int64_t)VK_FORMAT_R8G8B8A8_UNORM, (int64_t)VK_FORMAT_B8G8R8A8_UNORM, (int64_t)VK_FORMAT_R8G8B8A8_SRGB })
		{
			if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
			{
				chosen = preferred;
				break;
			}
		}
		g.colorFormat = (VkFormat)chosen;
		LOGI("swapchain format: %d (of %u)", (int)chosen, formatCount);

		XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.format = chosen;
		info.sampleCount = 1;
		info.width = g.extent.width;
		info.height = g.extent.height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!XrOk(xrCreateSwapchain(g.session, &info, &g.swapchain), "xrCreateSwapchain"))
			return false;

		uint32_t imageCount = 0;
		if (!XrOk(xrEnumerateSwapchainImages(g.swapchain, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages(count)"))
			return false;
		std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		if (!XrOk(xrEnumerateSwapchainImages(g.swapchain, imageCount, &imageCount,
			reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages"))
			return false;
		g.swapchainImages.clear();
		for (auto& image : images)
			g.swapchainImages.push_back(image.image);
		g.swapchainWidth = g.extent.width;
		g.swapchainHeight = g.extent.height;
		LOGI("swapchain: %ux%u, %u images", g.swapchainWidth, g.swapchainHeight, imageCount);
		return true;
	}

	bool CreateColorImages()
	{
		VkPhysicalDeviceMemoryProperties memoryProperties;
		vkGetPhysicalDeviceMemoryProperties(g.vkPhysicalDevice, &memoryProperties);

		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = g.colorFormat;
		info.extent = {g.extent.width, g.extent.height, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		for (int i = 0; i < 2; i++)
		{
			VkImage image = VK_NULL_HANDLE;
			if (vkCreateImage(g.vkDevice, &info, nullptr, &image) != VK_SUCCESS)
			{
				LOGE("vkCreateImage failed");
				return false;
			}
			VkMemoryRequirements requirements;
			vkGetImageMemoryRequirements(g.vkDevice, image, &requirements);

			uint32_t memoryType = UINT32_MAX;
			for (uint32_t t = 0; t < memoryProperties.memoryTypeCount; t++)
			{
				if ((requirements.memoryTypeBits & (1u << t)) &&
					(memoryProperties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				{
					memoryType = t;
					break;
				}
			}
			if (memoryType == UINT32_MAX)
			{
				LOGE("no device-local memory type for VR image");
				return false;
			}

			VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocateInfo.allocationSize = requirements.size;
			allocateInfo.memoryTypeIndex = memoryType;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			if (vkAllocateMemory(g.vkDevice, &allocateInfo, nullptr, &memory) != VK_SUCCESS)
			{
				LOGE("vkAllocateMemory failed");
				return false;
			}
			vkBindImageMemory(g.vkDevice, image, memory, 0);
			g.colorImages.push_back(image);
			g.colorMemories.push_back(memory);
		}
		return true;
	}

	bool CreateFrameResources()
	{
		VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = g.queueFamilyIndex;
		if (vkCreateCommandPool(g.vkDevice, &poolInfo, nullptr, &g.commandPool) != VK_SUCCESS)
		{
			LOGE("vkCreateCommandPool failed");
			return false;
		}

		VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocateInfo.commandPool = g.commandPool;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(g.vkDevice, &allocateInfo, &g.commandBuffer) != VK_SUCCESS)
		{
			LOGE("vkAllocateCommandBuffers failed");
			return false;
		}

		VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(g.vkDevice, &fenceInfo, nullptr, &g.frameFence) != VK_SUCCESS)
		{
			LOGE("vkCreateFence failed");
			return false;
		}
		return true;
	}

	void PumpEvents()
	{
		for (;;)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			XrResult result = xrPollEvent(g.instance, &event);
			if (result == XR_EVENT_UNAVAILABLE)
				break;
			if (XR_FAILED(result))
			{
				LOGE("xrPollEvent failed: %d", result);
				break;
			}
			if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				continue;

			auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
			LOGI("session state -> %s", SessionStateName(changed->state));
			switch (changed->state)
			{
			case XR_SESSION_STATE_READY:
				if (!g.sessionRunning)
				{
					XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (XR_SUCCEEDED(xrBeginSession(g.session, &beginInfo)))
					{
						g.sessionRunning = true;
						LOGI("session begun");
					}
				}
				break;
			case XR_SESSION_STATE_STOPPING:
				if (g.sessionRunning)
				{
					xrEndSession(g.session);
					g.sessionRunning = false;
				}
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				g.sessionRunning = false;
				g.exitRequested = true;
				g.inputThreadRun = false;
				break;
			default:
				break;
			}
		}
	}

	bool GetBool(XrAction action)
	{
		XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = action;
		if (XR_FAILED(xrGetActionStateBoolean(g.session, &info, &state)))
			return false;
		return state.isActive && state.currentState;
	}

	float GetFloat(XrAction action)
	{
		XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = action;
		if (XR_FAILED(xrGetActionStateFloat(g.session, &info, &state)))
			return 0.0f;
		return state.isActive ? state.currentState : 0.0f;
	}

	XrVector2f GetVector2(XrAction action)
	{
		XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = action;
		if (XR_FAILED(xrGetActionStateVector2f(g.session, &info, &state)))
			return {0.0f, 0.0f};
		return state.isActive ? state.currentState : XrVector2f{0.0f, 0.0f};
	}
}

namespace VrRuntime
{
	bool IsActive()
	{
		return g.initialized;
	}

	bool IsSessionReady()
	{
		return g.sessionCreated && !g.colorImages.empty();
	}

	void AppendInstanceExtensions(std::vector<const char*>& extensions)
	{
		if (!g.initialized)
			return;
		for (auto& ext : g.instanceExts)
		{
			if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
				extensions.push_back(ext);
		}
	}

	void AppendDeviceExtensions(std::vector<const char*>& extensions)
	{
		if (!g.initialized)
			return;
		for (auto& ext : g.deviceExts)
		{
			if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
				extensions.push_back(ext);
		}
	}

	void OnVulkanReady(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	                   uint32_t queueFamilyIndex, VkQueue queue)
	{
		if (!g.initialized || g.sessionCreated)
			return;

		// The runtime requires this query before xrCreateSession and the returned
		// physical device must be the one used for the session binding.
		VkPhysicalDevice runtimePhysicalDevice = VK_NULL_HANDLE;
		if (!XrOk(g.pfnGetVulkanGraphicsDevice(g.instance, g.systemId, instance, &runtimePhysicalDevice),
			"xrGetVulkanGraphicsDeviceKHR"))
			return;
		if (runtimePhysicalDevice != physicalDevice)
		{
			LOGE("OpenXR physical device %p differs from Cemu's %p", (void*)runtimePhysicalDevice, (void*)physicalDevice);
			return;
		}
		LOGI("OpenXR physical device matches Cemu's (%p)", (void*)physicalDevice);

		g.vkInstance = instance;
		g.vkPhysicalDevice = physicalDevice;
		g.vkDevice = device;
		g.queueFamilyIndex = queueFamilyIndex;
		g.queue = queue;

		if (!CreateSession())
			return;
		if (!CreateSwapchain())
			return;
		if (!CreateColorImages())
			return;
		if (!CreateFrameResources())
			return;

		// Poll OpenXR input independently of rendering: a stalled emulator must
		// not freeze controller state (and input keeps working while frames hitch).
		g.inputThreadRun = false; // input is polled inside the frame loop

		LOGI("VR runtime ready (%ux%u)", g.extent.width, g.extent.height);
	}

	VkFormat GetColorFormat()
	{
		return g.colorFormat;
	}

	VkExtent2D GetColorExtent()
	{
		return g.extent;
	}

	const std::vector<VkImage>& GetColorImages()
	{
		return g.colorImages;
	}

	void UpdateInput()
	{
		if (!g.sessionRunning || g.actionSet == XR_NULL_HANDLE)
			return;

		XrActiveActionSet activeSet{g.actionSet, XR_NULL_PATH};
		XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
		syncInfo.countActiveActionSets = 1;
		syncInfo.activeActionSets = &activeSet;
		XrResult syncResult = xrSyncActions(g.session, &syncInfo);
		if (XR_FAILED(syncResult))
		{
			static int syncFailures = 0;
			if (syncFailures++ < 5)
				LOGE("xrSyncActions failed: %d", syncResult);
			return;
		}
		g.lastSyncResult = (int)syncResult;

		static bool loggedProfile = false;
		if (!loggedProfile)
		{
			loggedProfile = true;
			for (const char* hand : {"/user/hand/left", "/user/hand/right"})
			{
				XrPath handPath = XR_NULL_PATH;
				xrStringToPath(g.instance, hand, &handPath);
				XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
				XrResult r = xrGetCurrentInteractionProfile(g.session, handPath, &state);
				char pathBuffer[256] = "<none>";
				uint32_t length = 0;
				if (XR_SUCCEEDED(r) && state.interactionProfile != XR_NULL_PATH)
					xrPathToString(g.instance, state.interactionProfile, sizeof(pathBuffer), &length, pathBuffer);
				LOGI("interaction profile %s -> %s (result %d)", hand, pathBuffer, r);
			}
		}

		auto& controller = EmulatedControllerManager::GetController(0);
		if (!g.controllerInitialized)
		{
			controller.SetType(EmulatedController::Type::VPAD);
			g.controllerInitialized = true;
		}

		using ButtonId = VPADController::ButtonId;
		controller.SetButtonValue(ButtonId::kButtonId_A, GetBool(g.buttonA));
		controller.SetButtonValue(ButtonId::kButtonId_B, GetBool(g.buttonB));
		controller.SetButtonValue(ButtonId::kButtonId_X, GetBool(g.buttonX));
		controller.SetButtonValue(ButtonId::kButtonId_Y, GetBool(g.buttonY));
		controller.SetButtonValue(ButtonId::kButtonId_ZL, GetFloat(g.triggerLeft) > 0.5f);
		controller.SetButtonValue(ButtonId::kButtonId_ZR, GetFloat(g.triggerRight) > 0.5f);
		controller.SetButtonValue(ButtonId::kButtonId_L, GetFloat(g.squeezeLeft) > 0.5f);
		controller.SetButtonValue(ButtonId::kButtonId_R, GetFloat(g.squeezeRight) > 0.5f);
		controller.SetButtonValue(ButtonId::kButtonId_Plus, GetBool(g.menuButton));
		controller.SetButtonValue(ButtonId::kButtonId_StickL, GetBool(g.stickClickLeft));
		controller.SetButtonValue(ButtonId::kButtonId_StickR, GetBool(g.stickClickRight));

		static int inputLogCount = 0;
		static auto lastInputLog = std::chrono::steady_clock::now();
		const auto inputNow = std::chrono::steady_clock::now();
		if (inputLogCount < 240 && inputNow - lastInputLog > std::chrono::seconds(5))
		{
			lastInputLog = inputNow;
			inputLogCount++;
			const XrVector2f ls = GetVector2(g.thumbstickLeft), rs = GetVector2(g.thumbstickRight);
			char leftProfile[256] = "<none>", rightProfile[256] = "<none>";
			uint32_t length = 0;
			for (int hand = 0; hand < 2; hand++)
			{
				XrPath handPath = XR_NULL_PATH;
				xrStringToPath(g.instance, hand == 0 ? "/user/hand/left" : "/user/hand/right", &handPath);
				XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
				if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g.session, handPath, &profile)) &&
					profile.interactionProfile != XR_NULL_PATH)
					xrPathToString(g.instance, profile.interactionProfile, sizeof(leftProfile), &length,
						hand == 0 ? leftProfile : rightProfile);
			}
			LOGI("input state (sync=%d): A=%d B=%d X=%d Y=%d menu=%d TL=%.2f TR=%.2f SL=%.2f SR=%.2f L=(%.2f,%.2f) R=(%.2f,%.2f) profiles=%s|%s",
				g.lastSyncResult.load(), GetBool(g.buttonA), GetBool(g.buttonB), GetBool(g.buttonX), GetBool(g.buttonY),
				GetBool(g.menuButton), GetFloat(g.triggerLeft), GetFloat(g.triggerRight),
				GetFloat(g.squeezeLeft), GetFloat(g.squeezeRight), ls.x, ls.y, rs.x, rs.y,
				leftProfile, rightProfile);
		}

		// Edge log: one line per button press regardless of the 5 s sampling rate.
		{
			static bool prevA = false, prevB = false, prevX = false, prevY = false, prevMenu = false;
			static bool prevTL = false, prevTR = false, prevSL = false, prevSR = false;
			const bool curA = GetBool(g.buttonA), curB = GetBool(g.buttonB);
			const bool curX = GetBool(g.buttonX), curY = GetBool(g.buttonY);
			const bool curMenu = GetBool(g.menuButton);
			const bool curTL = GetFloat(g.triggerLeft) > 0.5f, curTR = GetFloat(g.triggerRight) > 0.5f;
			const bool curSL = GetFloat(g.squeezeLeft) > 0.5f, curSR = GetFloat(g.squeezeRight) > 0.5f;
			auto edge = [](bool now, bool& prev, const char* name) {
				if (now && !prev)
					LOGI("input event: %s", name);
				prev = now;
			};
			edge(curA, prevA, "A"); edge(curB, prevB, "B"); edge(curX, prevX, "X"); edge(curY, prevY, "Y");
			edge(curMenu, prevMenu, "menu"); edge(curTL, prevTL, "trigger L"); edge(curTR, prevTR, "trigger R");
			edge(curSL, prevSL, "squeeze L"); edge(curSR, prevSR, "squeeze R");
		}

		// Exit gesture: press both thumbsticks.
		const bool stickL = GetBool(g.stickClickLeft);
		const bool stickR = GetBool(g.stickClickRight);
		if (stickL && stickR && !g.exitChordLatched)
		{
			g.userExitRequested = true;
			g.exitChordLatched = true;
		}
		else if (!stickL && !stickR)
		{
			g.exitChordLatched = false;
		}

		// One-time evidence that OpenXR actions reach the emulator.
		static bool inputSeen = false;
		if (!inputSeen)
		{
			const bool a = GetBool(g.buttonA), b = GetBool(g.buttonB), x = GetBool(g.buttonX), y = GetBool(g.buttonY);
			const bool menu = GetBool(g.menuButton);
			const XrVector2f ls = GetVector2(g.thumbstickLeft), rs = GetVector2(g.thumbstickRight);
			if (a || b || x || y || menu || stickL || stickR ||
				std::abs(ls.x) > 0.2f || std::abs(ls.y) > 0.2f || std::abs(rs.x) > 0.2f || std::abs(rs.y) > 0.2f)
			{
				LOGI("Touch input received: A=%d B=%d X=%d Y=%d menu=%d L=(%.2f,%.2f) R=(%.2f,%.2f)",
					a, b, x, y, menu, ls.x, ls.y, rs.x, rs.y);
				inputSeen = true;
			}
		}

		auto applyStick = [&controller](XrVector2f stick, ButtonId left, ButtonId right, ButtonId up, ButtonId down)
		{
			controller.SetAxisValue(left, std::max(0.0f, -stick.x));
			controller.SetAxisValue(right, std::max(0.0f, stick.x));
			controller.SetAxisValue(up, std::max(0.0f, stick.y));
			controller.SetAxisValue(down, std::max(0.0f, -stick.y));
		};
		applyStick(GetVector2(g.thumbstickLeft), ButtonId::kButtonId_StickL_Left, ButtonId::kButtonId_StickL_Right,
		           ButtonId::kButtonId_StickL_Up, ButtonId::kButtonId_StickL_Down);
		applyStick(GetVector2(g.thumbstickRight), ButtonId::kButtonId_StickR_Left, ButtonId::kButtonId_StickR_Right,
		           ButtonId::kButtonId_StickR_Up, ButtonId::kButtonId_StickR_Down);
	}

	bool ConsumeExitRequest()
	{
		const bool requested = g.userExitRequested;
		g.userExitRequested = false;
		return requested;
	}

	bool PresentFrame(VkImage sourceImage, VkSemaphore waitSemaphore)
	{
		if (!g.sessionCreated || g.exitRequested)
			return false;

		PumpEvents();
		if (!g.sessionRunning)
		{
			static bool logged = false;
			if (!logged) { LOGI("PresentFrame called before session running; dropping frames"); logged = true; }
			return false;
		}

		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		if (XR_FAILED(xrWaitFrame(g.session, &waitInfo, &frameState)))
			return false;

		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		if (XR_FAILED(xrBeginFrame(g.session, &beginInfo)))
			return false;

		UpdateInput();

		auto endEmptyFrame = [&frameState]()
		{
			XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
			endInfo.displayTime = frameState.predictedDisplayTime;
			endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			endInfo.layerCount = 0;
			xrEndFrame(g.session, &endInfo);
		};

		XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		uint32_t imageIndex = 0;
		if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain, &acquireInfo, &imageIndex)))
		{
			endEmptyFrame();
			return false;
		}

		XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		imageWait.timeout = XR_INFINITE_DURATION;
		if (XR_FAILED(xrWaitSwapchainImage(g.swapchain, &imageWait)))
		{
			endEmptyFrame();
			return false;
		}

		// Blit the emulator frame into the OpenXR image.
		vkWaitForFences(g.vkDevice, 1, &g.frameFence, VK_TRUE, UINT64_MAX);
		vkResetFences(g.vkDevice, 1, &g.frameFence);
		vkResetCommandBuffer(g.commandBuffer, 0);

		VkCommandBufferBeginInfo bufferBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bufferBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(g.commandBuffer, &bufferBegin);

		VkImageMemoryBarrier barriers[2]{};
		barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].image = sourceImage;
		barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].image = g.swapchainImages[imageIndex];
		barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barriers[1].srcAccessMask = 0;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		vkCmdPipelineBarrier(g.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);

		VkImageBlit region{};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.srcOffsets[1] = {(int32_t)g.extent.width, (int32_t)g.extent.height, 1};
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstOffsets[1] = {(int32_t)g.swapchainWidth, (int32_t)g.swapchainHeight, 1};
		vkCmdBlitImage(g.commandBuffer,
			sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			g.swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region, VK_FILTER_LINEAR);

		VkImageMemoryBarrier toSampled = barriers[1];
		toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toSampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(g.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &toSampled);

		vkEndCommandBuffer(g.commandBuffer);

		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &waitSemaphore;
		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &g.commandBuffer;
		if (vkQueueSubmit(g.queue, 1, &submit, g.frameFence) != VK_SUCCESS)
		{
			endEmptyFrame();
			return false;
		}
		vkWaitForFences(g.vkDevice, 1, &g.frameFence, VK_TRUE, UINT64_MAX);

		XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		if (XR_FAILED(xrReleaseSwapchainImage(g.swapchain, &releaseInfo)))
		{
			endEmptyFrame();
			return false;
		}

		XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad.space = g.localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = g.swapchain;
		quad.subImage.imageRect = {{0, 0}, {(int32_t)g.swapchainWidth, (int32_t)g.swapchainHeight}};
		quad.subImage.imageArrayIndex = 0;
		quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
		quad.pose.position = {0.0f, 0.0f, -2.0f};
		quad.size = {2.4f, 1.35f};

		const XrCompositionLayerBaseHeader* layers[1] = {
			reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)
		};
		XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = 1;
		endInfo.layers = layers;
		if (XR_FAILED(xrEndFrame(g.session, &endInfo)))
			return false;

		static uint64_t frameCounter = 0;
		static auto lastReport = std::chrono::steady_clock::now();
		static uint64_t framesSinceReport = 0;
		frameCounter++;
		framesSinceReport++;
		if (frameCounter == 1)
			LOGI("first VR frame submitted");
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
		if (elapsed >= 5000)
		{
			LOGI("VR fps: %.1f (%llu frames in %lld ms)", framesSinceReport * 1000.0 / (double)elapsed,
				(unsigned long long)framesSinceReport, (long long)elapsed);
			lastReport = now;
			framesSinceReport = 0;
		}

		return true;
	}
}

bool VrInitializeAndroid(JNIEnv* env, jobject context)
{
	if (g.initialized)
		return true;

	env->GetJavaVM(&g.vm);
	g.context = context ? env->NewGlobalRef(context) : nullptr;

	// <external files dir>/vr.log
	if (context)
	{
		jclass contextClass = env->GetObjectClass(context);
		jmethodID getFilesDir = env->GetMethodID(contextClass, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
		jobject filesDir = getFilesDir ? env->CallObjectMethod(context, getFilesDir, nullptr) : nullptr;
		if (filesDir)
		{
			jclass fileClass = env->GetObjectClass(filesDir);
			jmethodID getPath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
			jstring path = getPath ? (jstring)env->CallObjectMethod(filesDir, getPath) : nullptr;
			if (path)
			{
				const char* chars = env->GetStringUTFChars(path, nullptr);
				g_logFilePath = std::string(chars) + "/vr.log";
				env->ReleaseStringUTFChars(path, chars);
				env->DeleteLocalRef(path);
			}
			env->DeleteLocalRef(fileClass);
			env->DeleteLocalRef(filesDir);
		}
		env->DeleteLocalRef(contextClass);
	}

	if (!InitializeLoader())
		return false;

	uint32_t extensionCount = 0;
	if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "xrEnumerateInstanceExtensionProperties(count)"))
		return false;
	std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()), "xrEnumerateInstanceExtensionProperties"))
		return false;

	bool hasVulkanEnable = false;
	for (auto& extension : extensions)
	{
		if (strcmp(extension.extensionName, "XR_KHR_vulkan_enable") == 0)
			hasVulkanEnable = true;
	}
	if (!hasVulkanEnable)
	{
		LOGE("XR_KHR_vulkan_enable is not available");
		return false;
	}

	const char* enabledExtensions[] = {"XR_KHR_vulkan_enable"};
	XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(createInfo.applicationInfo.applicationName, "Cemu", XR_MAX_APPLICATION_NAME_SIZE - 1);
	createInfo.applicationInfo.applicationVersion = 1;
	strncpy(createInfo.applicationInfo.engineName, "Cemu", XR_MAX_ENGINE_NAME_SIZE - 1);
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	createInfo.enabledExtensionCount = 1;
	createInfo.enabledExtensionNames = enabledExtensions;
	if (!XrOk(xrCreateInstance(&createInfo, &g.instance), "xrCreateInstance"))
		return false;

	g.pfnGetVulkanGraphicsRequirements = GetXrFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>("xrGetVulkanGraphicsRequirementsKHR");
	g.pfnGetVulkanInstanceExtensions = GetXrFunction<PFN_xrGetVulkanInstanceExtensionsKHR>("xrGetVulkanInstanceExtensionsKHR");
	g.pfnGetVulkanGraphicsDevice = GetXrFunction<PFN_xrGetVulkanGraphicsDeviceKHR>("xrGetVulkanGraphicsDeviceKHR");
	g.pfnGetVulkanDeviceExtensions = GetXrFunction<PFN_xrGetVulkanDeviceExtensionsKHR>("xrGetVulkanDeviceExtensionsKHR");
	if (!g.pfnGetVulkanGraphicsRequirements || !g.pfnGetVulkanInstanceExtensions ||
		!g.pfnGetVulkanGraphicsDevice || !g.pfnGetVulkanDeviceExtensions)
		return false;

	XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XrOk(xrGetSystem(g.instance, &systemInfo, &g.systemId), "xrGetSystem"))
		return false;

	// Required by the runtime before xrCreateSession can use the Vulkan binding.
	XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	if (!XrOk(g.pfnGetVulkanGraphicsRequirements(g.instance, g.systemId, &requirements), "xrGetVulkanGraphicsRequirementsKHR"))
		return false;
	LOGI("Vulkan requirement: %llu..%llu", (unsigned long long)requirements.minApiVersionSupported,
		(unsigned long long)requirements.maxApiVersionSupported);


	{
		uint32_t size = 0;
		if (!XrOk(g.pfnGetVulkanInstanceExtensions(g.instance, g.systemId, 0, &size, nullptr), "xrGetVulkanInstanceExtensionsKHR(count)"))
			return false;
		std::string buffer(size, '\0');
		if (!XrOk(g.pfnGetVulkanInstanceExtensions(g.instance, g.systemId, size, &size, buffer.data()), "xrGetVulkanInstanceExtensionsKHR"))
			return false;
		buffer.resize(strlen(buffer.c_str()));
		g.instanceExtStorage = SplitExtensionList(buffer);
		for (auto& entry : g.instanceExtStorage)
			g.instanceExts.push_back(entry.c_str());
	}
	{
		uint32_t size = 0;
		if (!XrOk(g.pfnGetVulkanDeviceExtensions(g.instance, g.systemId, 0, &size, nullptr), "xrGetVulkanDeviceExtensionsKHR(count)"))
			return false;
		std::string buffer(size, '\0');
		if (!XrOk(g.pfnGetVulkanDeviceExtensions(g.instance, g.systemId, size, &size, buffer.data()), "xrGetVulkanDeviceExtensionsKHR"))
			return false;
		buffer.resize(strlen(buffer.c_str()));
		g.deviceExtStorage = SplitExtensionList(buffer);
		for (auto& entry : g.deviceExtStorage)
			g.deviceExts.push_back(entry.c_str());
	}

	if (!CreateActions())
		return false;

	g.initialized = true;
	LOGI("VR runtime initialized (system %llu, %zu/%zu extensions)", (unsigned long long)g.systemId,
		g.instanceExts.size(), g.deviceExts.size());
	return true;
}

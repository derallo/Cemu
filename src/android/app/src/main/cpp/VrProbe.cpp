// Temporary diagnostic: can a 2D panel app on Horizon OS create an OpenXR session?
// Logs each step to logcat (tag: CemuVRProbe). Remove once the VR architecture is settled.

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

// Cemu compiles with VK_NO_PROTOTYPES (it loads Vulkan dynamically), but this
// probe links against the system loader directly.
#ifdef VK_NO_PROTOTYPES
#undef VK_NO_PROTOTYPES
#endif

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_EXTENSION_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "CemuVRProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace
{
	std::atomic_bool g_probeRunning{false};

	// Cemu itself exports global function pointers named vkCreateInstance etc.,
	// so we cannot link against libvulkan directly. Resolve through dlopen instead.
	struct VulkanDispatch
	{
		PFN_vkCreateInstance CreateInstance = nullptr;
		PFN_vkDestroyInstance DestroyInstance = nullptr;
		PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
		PFN_vkCreateDevice CreateDevice = nullptr;
		PFN_vkDestroyDevice DestroyDevice = nullptr;
		PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;

		bool Load()
		{
			void* handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
			if (!handle)
			{
				LOGE("dlopen(libvulkan.so) failed: %s", dlerror());
				return false;
			}
			#define LOAD_VK(name, member) member = reinterpret_cast<PFN_##name>(dlsym(handle, #name)); \
				if (!member) { LOGE("dlsym(%s) failed", #name); return false; }
			LOAD_VK(vkCreateInstance, CreateInstance)
			LOAD_VK(vkDestroyInstance, DestroyInstance)
			LOAD_VK(vkGetPhysicalDeviceQueueFamilyProperties, GetPhysicalDeviceQueueFamilyProperties)
			LOAD_VK(vkCreateDevice, CreateDevice)
			LOAD_VK(vkDestroyDevice, DestroyDevice)
			LOAD_VK(vkGetDeviceQueue, GetDeviceQueue)
			#undef LOAD_VK
			return true;
		}
	};

	VulkanDispatch g_vk;

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

	std::vector<std::string> SplitExtensionList(const std::string& s)
	{
		std::vector<std::string> out;
		size_t start = 0;
		while (start < s.size())
		{
			while (start < s.size() && s[start] == ' ')
				start++;
			size_t end = s.find(' ', start);
			if (end == std::string::npos)
				end = s.size();
			if (end > start)
				out.emplace_back(s.substr(start, end - start));
			start = end + 1;
		}
		return out;
	}

	class Probe
	{
	public:
		void Run()
		{
			if (!CreateInstance())
				return;
			if (!GetSystem())
				return;
			if (!CreateVulkan())
				return;
			if (!CreateSession())
				return;
			RunSessionLoop();
			Cleanup();
		}

	private:
		bool CreateInstance()
		{
			uint32_t extCount = 0;
			if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr)))
			{
				LOGE("xrEnumerateInstanceExtensionProperties(count) failed");
				return false;
			}
			std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
			if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data())))
			{
				LOGE("xrEnumerateInstanceExtensionProperties(list) failed");
				return false;
			}

			bool hasEnable = false, hasEnable2 = false;
			for (auto& e : exts)
			{
				if (strcmp(e.extensionName, "XR_KHR_vulkan_enable") == 0)
					hasEnable = true;
				if (strcmp(e.extensionName, "XR_KHR_vulkan_enable2") == 0)
					hasEnable2 = true;
			}
			LOGI("extensions=%u vulkan_enable=%d vulkan_enable2=%d", extCount, hasEnable, hasEnable2);
			if (!hasEnable)
			{
				LOGE("XR_KHR_vulkan_enable not available");
				return false;
			}

			const char* enabled[] = {"XR_KHR_vulkan_enable"};
			XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
			strncpy(ci.applicationInfo.applicationName, "CemuVRProbe", XR_MAX_APPLICATION_NAME_SIZE - 1);
			ci.applicationInfo.applicationVersion = 1;
			strncpy(ci.applicationInfo.engineName, "Cemu", XR_MAX_ENGINE_NAME_SIZE - 1);
			ci.applicationInfo.engineVersion = 1;
			ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
			ci.enabledExtensionCount = 1;
			ci.enabledExtensionNames = enabled;

			XrResult result = xrCreateInstance(&ci, &m_instance);
			if (XR_FAILED(result))
			{
				LOGE("xrCreateInstance failed: %d", result);
				return false;
			}
			LOGI("xrCreateInstance ok");

			#define RESOLVE_XR(fn) \
				xrGetInstanceProcAddr(m_instance, #fn, reinterpret_cast<PFN_xrVoidFunction*>(&m_##fn)); \
				if (!m_##fn) { LOGE("missing entry point %s", #fn); return false; }
			RESOLVE_XR(xrGetVulkanGraphicsRequirementsKHR)
			RESOLVE_XR(xrGetVulkanInstanceExtensionsKHR)
			RESOLVE_XR(xrGetVulkanGraphicsDeviceKHR)
			RESOLVE_XR(xrGetVulkanDeviceExtensionsKHR)
			#undef RESOLVE_XR

			XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
			if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
				LOGI("runtime=%s %u.%u.%u", props.runtimeName,
					XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
			return true;
		}

		bool GetSystem()
		{
			XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
			info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			XrResult result = xrGetSystem(m_instance, &info, &m_systemId);
			if (XR_FAILED(result))
			{
				LOGE("xrGetSystem failed: %d", result);
				return false;
			}
			XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
			if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &sysProps)))
				LOGI("system=%s vendorId=%u", sysProps.systemName, sysProps.vendorId);
			return true;
		}

		bool CreateVulkan()
		{
			if (!g_vk.Load())
				return false;

			XrGraphicsRequirementsVulkanKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
			XrResult result = m_xrGetVulkanGraphicsRequirementsKHR(m_instance, m_systemId, &reqs);
			if (XR_FAILED(result))
			{
				LOGE("xrGetVulkanGraphicsRequirementsKHR failed: %d", result);
				return false;
			}

			uint32_t len = 0;
			if (XR_FAILED(m_xrGetVulkanInstanceExtensionsKHR(m_instance, m_systemId, 0, &len, nullptr)))
			{
				LOGE("xrGetVulkanInstanceExtensionsKHR(size) failed");
				return false;
			}
			std::string instanceExtBuf(len, '\0');
			if (XR_FAILED(m_xrGetVulkanInstanceExtensionsKHR(m_instance, m_systemId, len, &len, instanceExtBuf.data())))
			{
				LOGE("xrGetVulkanInstanceExtensionsKHR(data) failed");
				return false;
			}
			instanceExtBuf.resize(strlen(instanceExtBuf.c_str()));
			auto instanceExts = SplitExtensionList(instanceExtBuf);
			LOGI("required VkInstance extensions (%zu): %s", instanceExts.size(), instanceExtBuf.c_str());

			VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
			appInfo.pApplicationName = "CemuVRProbe";
			appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
			appInfo.pEngineName = "Cemu";
			appInfo.apiVersion = VK_API_VERSION_1_1;

			std::vector<const char*> instanceExtPtrs;
			for (auto& e : instanceExts)
				instanceExtPtrs.push_back(e.c_str());

			VkInstanceCreateInfo vkci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
			vkci.pApplicationInfo = &appInfo;
			vkci.enabledExtensionCount = (uint32_t)instanceExtPtrs.size();
			vkci.ppEnabledExtensionNames = instanceExtPtrs.data();

			VkResult vkResult = g_vk.CreateInstance(&vkci, nullptr, &m_vkInstance);
			if (vkResult != VK_SUCCESS)
			{
				LOGE("vkCreateInstance failed: %d", vkResult);
				return false;
			}
			LOGI("vkCreateInstance ok");

			result = m_xrGetVulkanGraphicsDeviceKHR(m_instance, m_systemId, m_vkInstance, &m_vkPhysicalDevice);
			if (XR_FAILED(result))
			{
				LOGE("xrGetVulkanGraphicsDeviceKHR failed: %d", result);
				return false;
			}

			len = 0;
			if (XR_FAILED(m_xrGetVulkanDeviceExtensionsKHR(m_instance, m_systemId, 0, &len, nullptr)))
			{
				LOGE("xrGetVulkanDeviceExtensionsKHR(size) failed");
				return false;
			}
			std::string deviceExtBuf(len, '\0');
			if (XR_FAILED(m_xrGetVulkanDeviceExtensionsKHR(m_instance, m_systemId, len, &len, deviceExtBuf.data())))
			{
				LOGE("xrGetVulkanDeviceExtensionsKHR(data) failed");
				return false;
			}
			deviceExtBuf.resize(strlen(deviceExtBuf.c_str()));
			auto deviceExts = SplitExtensionList(deviceExtBuf);
			LOGI("required VkDevice extensions (%zu): %s", deviceExts.size(), deviceExtBuf.c_str());

			uint32_t queueFamilyCount = 0;
			g_vk.GetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, nullptr);
			std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
			g_vk.GetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, queueFamilies.data());

			m_queueFamilyIndex = UINT32_MAX;
			for (uint32_t i = 0; i < queueFamilyCount; i++)
			{
				if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				{
					m_queueFamilyIndex = i;
					break;
				}
			}
			if (m_queueFamilyIndex == UINT32_MAX)
			{
				LOGE("no graphics queue family");
				return false;
			}

			float priority = 1.0f;
			VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			queueInfo.queueFamilyIndex = m_queueFamilyIndex;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &priority;

			std::vector<const char*> deviceExtPtrs;
			for (auto& e : deviceExts)
				deviceExtPtrs.push_back(e.c_str());

			VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			deviceInfo.queueCreateInfoCount = 1;
			deviceInfo.pQueueCreateInfos = &queueInfo;
			deviceInfo.enabledExtensionCount = (uint32_t)deviceExtPtrs.size();
			deviceInfo.ppEnabledExtensionNames = deviceExtPtrs.data();

			vkResult = g_vk.CreateDevice(m_vkPhysicalDevice, &deviceInfo, nullptr, &m_vkDevice);
			if (vkResult != VK_SUCCESS)
			{
				LOGE("vkCreateDevice failed: %d", vkResult);
				return false;
			}
			g_vk.GetDeviceQueue(m_vkDevice, m_queueFamilyIndex, 0, &m_vkQueue);
			LOGI("vkCreateDevice ok");
			return true;
		}

		bool CreateSession()
		{
			XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
			binding.instance = m_vkInstance;
			binding.physicalDevice = m_vkPhysicalDevice;
			binding.device = m_vkDevice;
			binding.queueFamilyIndex = m_queueFamilyIndex;
			binding.queueIndex = 0;

			XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
			info.next = &binding;
			info.systemId = m_systemId;

			XrResult result = xrCreateSession(m_instance, &info, &m_session);
			if (XR_FAILED(result))
			{
				LOGE("xrCreateSession failed: %d", result);
				return false;
			}
			LOGI("xrCreateSession ok");

			XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
			if (XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &m_localSpace)))
			{
				LOGE("xrCreateReferenceSpace failed");
				return false;
			}
			return true;
		}

		void RunSessionLoop()
		{
			bool begun = false;
			auto start = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8))
			{
				XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
				XrResult result = xrPollEvent(m_instance, &event);
				if (result == XR_SUCCESS)
				{
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					{
						auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
						LOGI("session state -> %s (%d)", SessionStateName(changed->state), changed->state);
						if (changed->state == XR_SESSION_STATE_READY && !begun)
						{
							XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
							beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
							XrResult beginResult = xrBeginSession(m_session, &beginInfo);
							LOGI("xrBeginSession -> %d", beginResult);
							begun = XR_SUCCEEDED(beginResult);
						}
						else if (changed->state == XR_SESSION_STATE_STOPPING && begun)
						{
							xrEndSession(m_session);
							begun = false;
						}
						else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING)
						{
							return;
						}
					}
					continue;
				}
				if (result == XR_EVENT_UNAVAILABLE)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				LOGE("xrPollEvent failed: %d", result);
				break;
			}

			if (begun)
			{
				LOGI("attempting frames");
				for (int i = 0; i < 30; i++)
				{
					XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
					XrFrameState frameState{XR_TYPE_FRAME_STATE};
					XrResult result = xrWaitFrame(m_session, &waitInfo, &frameState);
					if (XR_FAILED(result))
					{
						LOGE("xrWaitFrame failed: %d", result);
						break;
					}
					XrFrameBeginInfo beginFrameInfo{XR_TYPE_FRAME_BEGIN_INFO};
					result = xrBeginFrame(m_session, &beginFrameInfo);
					if (XR_FAILED(result))
					{
						LOGE("xrBeginFrame failed: %d", result);
						break;
					}
					XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
					endInfo.displayTime = frameState.predictedDisplayTime;
					endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
					endInfo.layerCount = 0;
					endInfo.layers = nullptr;
					result = xrEndFrame(m_session, &endInfo);
					if (XR_FAILED(result))
					{
						LOGE("xrEndFrame failed: %d", result);
						break;
					}
					if (i == 0)
						LOGI("first frame submitted");
				}
				LOGI("frame loop done");
			}
		}

		void Cleanup()
		{
			if (m_session != XR_NULL_HANDLE)
				xrDestroySession(m_session);
			if (m_vkDevice != VK_NULL_HANDLE)
				g_vk.DestroyDevice(m_vkDevice, nullptr);
			if (m_vkInstance != VK_NULL_HANDLE)
				g_vk.DestroyInstance(m_vkInstance, nullptr);
			if (m_instance != XR_NULL_HANDLE)
				xrDestroyInstance(m_instance);
			LOGI("cleanup done");
		}

		XrInstance m_instance = XR_NULL_HANDLE;
		XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
		XrSession m_session = XR_NULL_HANDLE;
		XrSpace m_localSpace = XR_NULL_HANDLE;
		VkInstance m_vkInstance = VK_NULL_HANDLE;
		VkPhysicalDevice m_vkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_vkDevice = VK_NULL_HANDLE;
		VkQueue m_vkQueue = VK_NULL_HANDLE;
		uint32_t m_queueFamilyIndex = UINT32_MAX;

		PFN_xrGetVulkanGraphicsRequirementsKHR m_xrGetVulkanGraphicsRequirementsKHR = nullptr;
		PFN_xrGetVulkanInstanceExtensionsKHR m_xrGetVulkanInstanceExtensionsKHR = nullptr;
		PFN_xrGetVulkanGraphicsDeviceKHR m_xrGetVulkanGraphicsDeviceKHR = nullptr;
		PFN_xrGetVulkanDeviceExtensionsKHR m_xrGetVulkanDeviceExtensionsKHR = nullptr;
	};
}

namespace
{
	JavaVM* g_javaVM = nullptr;
	jobject g_appContext = nullptr;

	// The Android OpenXR loader must be initialized with the VM and an app context
	// before any other call, otherwise even extension enumeration fails.
	bool InitializeLoader()
	{
		PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
		XrResult result = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
		if (XR_FAILED(result) || !initializeLoader)
		{
			LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed: %d", result);
			return false;
		}

		XrLoaderInitInfoAndroidKHR initInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		initInfo.applicationVM = g_javaVM;
		initInfo.applicationContext = g_appContext;
		result = initializeLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&initInfo));
		if (XR_FAILED(result))
		{
			LOGE("xrInitializeLoaderKHR failed: %d", result);
			return false;
		}
		LOGI("xrInitializeLoaderKHR ok");
		return true;
	}
}

extern "C" JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_VrProbe_startProbe(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject context)
{
	if (g_probeRunning.exchange(true))
		return;

	env->GetJavaVM(&g_javaVM);
	if (g_appContext)
		env->DeleteGlobalRef(g_appContext);
	g_appContext = context ? env->NewGlobalRef(context) : nullptr;

	std::thread([]() {
		LOGI("==== OpenXR probe start ====");
		if (InitializeLoader())
		{
			Probe probe;
			probe.Run();
		}
		LOGI("==== OpenXR probe end ====");
		g_probeRunning = false;
	}).detach();
}

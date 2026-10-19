#pragma once

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

// Bridge between Cemu's Vulkan renderer and the Android/OpenXR implementation
// (src/android/app/src/main/cpp/VrRuntime.cpp).
//
// On non-Android platforms every function here is a no-op. On Android the
// implementation is present in the same shared library as the renderer.
namespace VrRuntime
{
	// True when the app launched in VR mode and the OpenXR runtime is initialized.
	bool IsActive();

	// True when the session exists and external color images are ready.
	bool IsSessionReady();

	// Extensions that must be enabled on Cemu's VkInstance / VkDevice.
	void AppendInstanceExtensions(std::vector<const char*>& extensions);
	void AppendDeviceExtensions(std::vector<const char*>& extensions);

	// Called once Cemu's Vulkan device exists. Creates the XR session, swapchain
	// and the offscreen color images the renderer draws into.
	void OnVulkanReady(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	                   uint32_t queueFamilyIndex, VkQueue queue);

	VkFormat GetColorFormat();
	VkExtent2D GetColorExtent();
	const std::vector<VkImage>& GetColorImages();

	// Blit the finished Cemu image into the OpenXR swapchain and submit a quad
	// layer. Must be called from the render thread, after the render command
	// buffer signalled waitSemaphore.
	bool PresentFrame(VkImage sourceImage, VkSemaphore waitSemaphore);

	// Feed Touch controller state into Cemu's emulated controller.
	void UpdateInput();

	// True once per "exit VR" gesture (both thumbstick clicks) until consumed.
	bool ConsumeExitRequest();

	// Ends and destroys the OpenXR session. Must run before Cemu destroys the
	// Vulkan device the session is bound to (i.e. before stopping the title),
	// otherwise the next launch would create a second session on a dead device.
	void Shutdown();
}

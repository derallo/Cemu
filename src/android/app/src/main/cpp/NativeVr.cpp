#include "VrRuntimeAndroid.h"

#include "Cafe/HW/Latte/Renderer/Vulkan/VrRuntime.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeVr_initialize(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject context)
{
	return VrInitializeAndroid(env, context) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeVr_isActive([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return VrRuntime::IsActive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeVr_consumeExitRequest([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return VrRuntime::ConsumeExitRequest() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeVr_shutdown([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	VrRuntime::Shutdown();
}

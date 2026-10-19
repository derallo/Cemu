#pragma once

#include <jni.h>

// Initializes the OpenXR loader, instance and system. Must be called before
// Cemu creates its Vulkan instance (the runtime's required extensions are
// queried here).
bool VrInitializeAndroid(JNIEnv* env, jobject context);

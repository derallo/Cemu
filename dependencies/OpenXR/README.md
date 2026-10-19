# OpenXR SDK (vendored subset)

Files in this directory come from the Khronos OpenXR SDK, release **1.1.57**
(Apache License 2.0, see `LICENSE`).

| Path | Source |
| --- | --- |
| `include/openxr/*.h` | [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) `release-1.1.57`, `include/openxr/` |
| `lib/arm64-v8a/libopenxr_loader.so` | Maven Central `org.khronos.openxr:openxr_loader_for_android:1.1.57`, `jni/arm64-v8a/` |

Only `arm64-v8a` is vendored because Cemu's Android build currently targets only
that ABI (`abiFilters("arm64-v8a")`).

## Notes

- The loader is packaged through `jniLibs.srcDir("../../../../dependencies/OpenXR/lib")`
  in `src/android/app/build.gradle.kts`, and linked by path in
  `src/android/app/src/main/cpp/CMakeLists.txt`.
- On Android the loader must be initialized with `xrInitializeLoaderKHR` and an
  `XrLoaderInitInfoAndroidKHR` (JavaVM + app Context) before any other call,
  otherwise even `xrEnumerateInstanceExtensionProperties` fails.
- Vulkan entry points are resolved via `dlopen`/`dlsym` in this project because
  Cemu exports its own global function pointers named `vkCreateInstance` etc.

## Updating

```
curl -sL -o openxr_loader.aar \
  https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/<ver>/openxr_loader_for_android-<ver>.aar
unzip -j openxr_loader.aar jni/arm64-v8a/libopenxr_loader.so -d lib/arm64-v8a
curl -sL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-<ver>.tar.gz | \
  tar xz --strip-components=3 OpenXR-SDK-release-<ver>/include/openxr
```

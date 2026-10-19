add_library(CemuAndroid SHARED
        AndroidFilesystemCallbacks.cpp
        AndroidInputHelpers.cpp
        CompressTitleCallbacks.cpp
        GameTitleLoader.cpp
        Image.cpp
        JNIUtils.cpp
        NativeAccount.cpp
        NativeActiveSettings.cpp
        NativeEmulatedUSBDevices.cpp
        NativeEmulation.cpp
        NativeGameTitles.cpp
        NativeGraphicPacks.cpp
        NativeInput.cpp
        NativeLib.cpp
        NativeLocalization.cpp
        NativeLogging.cpp
        NativeSettings.cpp
        NativeSwkbd.cpp
        NativeVr.cpp
        VrProbe.cpp
        VrRuntime.cpp
        WuaConverter.cpp
)

cemu_use_precompiled_header(CemuAndroid)

target_include_directories(CemuAndroid PRIVATE
        ${CMAKE_SOURCE_DIR}/dependencies/OpenXR/include
)

target_link_libraries(CemuAndroid PRIVATE
        -landroid
        ${CMAKE_SOURCE_DIR}/dependencies/OpenXR/lib/${ANDROID_ABI}/libopenxr_loader.so
        CemuCommon
        CemuAudio
        CemuComponents
        CemuCafe
        CemuBin
        CemuGui
        ZArchive::zarchive
        stb
)

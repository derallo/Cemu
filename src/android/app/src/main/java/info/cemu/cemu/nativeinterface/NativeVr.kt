package info.cemu.cemu.nativeinterface

import android.content.Context

object NativeVr {
    init {
        System.loadLibrary("CemuAndroid")
    }

    external fun initialize(context: Context): Boolean
    external fun isActive(): Boolean
    external fun consumeExitRequest(): Boolean

    /** Ends and destroys the OpenXR session (call before stopping the title). */
    external fun shutdown()
}

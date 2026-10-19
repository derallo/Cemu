package info.cemu.cemu.nativeinterface

import android.content.Context

object NativeVr {
    init {
        System.loadLibrary("CemuAndroid")
    }

    external fun initialize(context: Context): Boolean
    external fun isActive(): Boolean
}

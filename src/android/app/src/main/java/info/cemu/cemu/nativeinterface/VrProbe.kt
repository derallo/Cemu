package info.cemu.cemu.nativeinterface

import android.content.Context

/**
 * Temporary diagnostic used to find out whether a 2D panel app on Horizon OS is
 * allowed to create an OpenXR session. Results are written to logcat under the
 * tag `CemuVRProbe`.
 */
object VrProbe {
    init {
        System.loadLibrary("CemuAndroid")
    }

    external fun startProbe(context: Context)
}

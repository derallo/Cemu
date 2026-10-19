package info.cemu.cemu.emulation

import android.os.Bundle
import androidx.lifecycle.lifecycleScope
import info.cemu.cemu.nativeinterface.NativeVr
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Immersive VR entry point. Declared with `com.oculus.intent.category.VR` in the
 * manifest so Horizon OS gives the OpenXR session VR focus instead of running
 * the emulator inside a 2D panel.
 */
class EmulationVrActivity : EmulationActivity() {
    override fun isVrMode(): Boolean = true

    override fun onCreate(savedInstanceState: Bundle?) {
        // Also reachable from the launcher tile: without a title there is nothing
        // to emulate, so return to the library instead of crashing.
        val path = intent?.getStringExtra(EXTRA_LAUNCH_PATH) ?: intent?.data?.toString()
        if (path == null) {
            finish()
            return
        }
        super.onCreate(savedInstanceState)

        // Both thumbsticks = leave VR and return to the library.
        lifecycleScope.launch {
            while (isActive) {
                delay(250)
                if (NativeVr.consumeExitRequest()) {
                    finish()
                    break
                }
            }
        }
    }
}

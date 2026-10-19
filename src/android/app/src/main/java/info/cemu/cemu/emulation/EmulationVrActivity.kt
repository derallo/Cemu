package info.cemu.cemu.emulation

/**
 * Immersive VR entry point. Declared with `com.oculus.intent.category.VR` in the
 * manifest so Horizon OS gives the OpenXR session VR focus instead of running
 * the emulator inside a 2D panel.
 */
class EmulationVrActivity : EmulationActivity() {
    override fun isVrMode(): Boolean = true

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        // Also reachable from the launcher tile: without a title there is nothing
        // to emulate, so return to the library instead of crashing.
        val path = intent?.getStringExtra(EXTRA_LAUNCH_PATH) ?: intent?.data?.toString()
        if (path == null) {
            finish()
            return
        }
        super.onCreate(savedInstanceState)
    }
}

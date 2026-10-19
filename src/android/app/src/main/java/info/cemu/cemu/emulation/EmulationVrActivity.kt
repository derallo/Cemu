package info.cemu.cemu.emulation

import android.content.Intent
import android.os.Bundle
import android.util.Log
import androidx.lifecycle.lifecycleScope
import info.cemu.cemu.MainActivity
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeVr
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.system.exitProcess

/**
 * Immersive VR entry point. Declared with `com.oculus.intent.category.VR` in the
 * manifest so Horizon OS gives the OpenXR session VR focus instead of running
 * the emulator inside a 2D panel.
 */
class EmulationVrActivity : EmulationActivity() {
    /** True once this instance actually started a title (not the launcher tile). */
    private var runsEmulation = false

    private var teardownThread: Thread? = null

    override fun isVrMode(): Boolean = true

    override fun onCreate(savedInstanceState: Bundle?) {
        // EmulationActivity.onCreate() finishes the activity when the intent has
        // no title; super must run first (Android throws SuperNotCalledException
        // otherwise), so this happens after the fact.
        super.onCreate(savedInstanceState)
        if (isFinishing) {
            // Started from the "Cemu VR" launcher tile: show the library instead.
            startActivity(Intent(this, MainActivity::class.java))
            return
        }
        runsEmulation = true

        // Both thumbsticks = leave VR and return to the library.
        lifecycleScope.launch {
            while (isActive) {
                delay(250)
                if (NativeVr.consumeExitRequest()) {
                    stopEmulationAndFinish()
                    break
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing && runsEmulation) {
            stopEmulation()
        }
    }

    /**
     * Cemu's Android core has no background teardown of its own: if the activity
     * merely finishes, the emulation threads keep running at ~200% CPU and keep
     * /vol/content mounted, so the next launch dies with
     * "fsc_mount failed (virtual path: /vol/content/)". Stop the title before
     * leaving the activity.
     */
    private fun stopEmulation() {
        if (!teardownStarted.compareAndSet(false, true)) {
            return
        }
        val thread = Thread({
            try {
                // Order matters: the session is bound to the VkDevice that
                // shutdownTitle() destroys.
                NativeVr.shutdown()
                NativeEmulation.shutdownTitle()
            } catch (t: Throwable) {
                Log.e("CemuVr", "failed to stop emulation", t)
            }
        }, "cemu-vr-teardown")
        thread.isDaemon = true
        teardownThread = thread
        thread.start()
    }

    private fun stopEmulationAndFinish() {
        stopEmulation()
        lifecycleScope.launch {
            // Never keep the player stuck if the emulator refuses to shut down.
            val finished = withContext(Dispatchers.IO) {
                val thread = teardownThread
                thread?.join(3000)
                thread?.isAlive != true
            }
            if (finished) {
                finish()
            } else {
                // A half-stopped emulator would break the next launch; restart
                // the process instead.
                exitProcess(0)
            }
        }
    }

    companion object {
        /**
         * Process-wide: two activity instances can tear down at once (e.g. the
         * task is cleared while a second instance starts), and a double
         * ShutdownTitle() aborts the process.
         */
        private val teardownStarted = AtomicBoolean(false)
    }
}

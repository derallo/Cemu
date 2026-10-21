package info.cemu.cemu.emulation

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Bundle
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import info.cemu.cemu.BuildConfig
import info.cemu.cemu.MainActivity
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeVr
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
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

    /** Guards against a second teardown from onDestroy after the exit gesture. */
    private val teardownStarted = AtomicBoolean(false)

    /**
     * Deterministic exit for automation and for a paused session: BACK does
     * not reach this activity in VR and the both-sticks gesture needs a
     * running session, but a broadcast arrives in both states.
     * `adb shell am broadcast -a <applicationId>.EXIT_VR`
     */
    private val exitReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            Log.i(TAG, "exit broadcast received")
            stopEmulationAndFinish()
        }
    }
    private var exitReceiverRegistered = false

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

        // Exported so `adb shell am broadcast` can reach it; receiving it can
        // only stop the game, never start anything.
        ContextCompat.registerReceiver(
            this,
            exitReceiver,
            IntentFilter(ACTION_EXIT_VR),
            ContextCompat.RECEIVER_EXPORTED,
        )
        exitReceiverRegistered = true

        // A BACK that does reach the activity exits VR deterministically too.
        onBackPressedDispatcher.addCallback(
            this,
            object : androidx.activity.OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    stopEmulationAndFinish()
                }
            },
        )

        // Both thumbsticks = leave VR and return to the library. The same loop
        // pauses emulation whenever Horizon OS pauses the OpenXR session (for
        // example when the headset is taken off): otherwise the game keeps
        // rendering at ~90% GPU and heats the headset while nobody is looking
        // at it. Resuming the session resumes the title.
        lifecycleScope.launch {
            // Seed from the live state: starting at "false" made the first poll
            // fire a pointless resumeTitle() into a session that was already
            // running.
            var wasRunning = NativeVr.isSessionRunning()
            while (isActive) {
                delay(250)
                if (NativeVr.consumeExitRequest()) {
                    stopEmulationAndFinish()
                    break
                }
                val running = NativeVr.isSessionRunning()
                if (running == wasRunning) {
                    continue
                }
                wasRunning = running
                // Run the transition off the main thread, but do not let it
                // wedge this loop: if one call ever blocks, the next
                // pause/resume must still be observed.
                lifecycleScope.launch(Dispatchers.IO) {
                    try {
                        if (running) {
                            Log.i(TAG, "session resumed; resuming emulation")
                            NativeEmulation.resumeTitle()
                        } else {
                            Log.i(TAG, "session paused; pausing emulation")
                            NativeEmulation.pauseTitle()
                        }
                    } catch (t: Throwable) {
                        Log.e(TAG, "pause/resume transition failed", t)
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        if (exitReceiverRegistered) {
            unregisterReceiver(exitReceiver)
            exitReceiverRegistered = false
        }
        super.onDestroy()
        if (isFinishing && runsEmulation) {
            stopEmulation()
        }
    }

    /**
     * Cemu's Android core has no background teardown of its own: if the activity
     * merely finishes, the emulation threads keep running at ~200% CPU and keep
     * /vol/content mounted, so the next launch dies with
     * "fsc_mount failed (virtual path: /vol/content/)". Stop the title, then end
     * the process.
     *
     * Ending the process is deliberate, not a workaround for missing cleanup:
     * Horizon OS does not grant XR_SESSION_STATE_READY to a second
     * xrCreateSession in the same process, so an in-process relaunch creates a
     * session that stays IDLE forever (audio without video). A fresh process per
     * VR session is the only reliable lifecycle.
     */
    private fun stopEmulation() {
        if (!teardownStarted.compareAndSet(false, true)) {
            return
        }

        // Never leave a wedged emulator holding the CPU: if the graceful
        // teardown does not finish, force the process down anyway.
        Thread({
            Thread.sleep(TEARDOWN_TIMEOUT_MS)
            Log.w(TAG, "teardown timed out; forcing process exit")
            Runtime.getRuntime().halt(0)
        }, "cemu-vr-teardown-watchdog").also {
            it.isDaemon = true
            it.start()
        }

        Thread({
            try {
                // Order matters: the session is bound to the VkDevice that
                // shutdownTitle() destroys.
                NativeVr.shutdown()
                NativeEmulation.shutdownTitle()
            } catch (t: Throwable) {
                Log.e(TAG, "failed to stop emulation", t)
            } finally {
                Log.i(TAG, "teardown complete; exiting process")
                exitProcess(0)
            }
        }, "cemu-vr-teardown").also {
            it.isDaemon = false
            it.start()
        }
    }

    private fun stopEmulationAndFinish() {
        // Detach the task first so the player lands back in the library instead
        // of on a black activity while the teardown thread runs.
        finishAndRemoveTask()
        stopEmulation()
    }

    companion object {
        private const val TAG = "CemuVr"
        private const val TEARDOWN_TIMEOUT_MS = 6_000L

        /** Broadcast action that exits VR; also used by scripts/quest-eval.sh. */
        val ACTION_EXIT_VR: String = BuildConfig.APPLICATION_ID + ".EXIT_VR"
    }
}

package com.wither.nsmbw;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Process;
import android.system.Os;
import android.util.Log;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity running libmain.so (the translated game, its runtime, SDL and Dawn,
 * linked into one library), with the on-screen remote and the phone's motion on top.
 */
public final class GameActivity extends SDLActivity
        implements TouchControlsView.Listener, MotionInput.Listener {
    private static final String TAG = "NSMBW";

    /** Player 1's on-screen remote and motion (runtime/src/android/android_bridge.cpp). */
    static native void nativeSetTouchState(int buttons, boolean shake, float tilt);

    private AppSettings settings;
    private TouchControlsView touch;
    private MotionInput motion;
    private boolean nativeReady;
    private int touchButtons;
    private boolean touchShake, motionShake;
    private float motionTilt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        settings = new AppSettings(this);
        try {
            AppFiles.prepareRuntime(this);
            AppFiles.writeConfig(this, settings);
            // Read by runtime/src/platform/host_platform.cpp before anything else starts.
            Os.setenv("WIICOMPILED_RUNTIME_DIR", AppFiles.runtimeDir(this).getAbsolutePath(), true);
            Os.setenv("WIICOMPILED_DATA_DIR", AppFiles.dataDir(this).getAbsolutePath(), true);
        } catch (Exception e) {
            Log.e(TAG, "could not prepare the game's files", e);
            Toast.makeText(this, "Could not prepare the game's files: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        super.onCreate(savedInstanceState);
        nativeReady = !SDLActivity.mBrokenLibraries;

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        touch = new TouchControlsView(this);
        touch.setListener(this);
        applyControlSettings();
        addContentView(touch, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        motion = new MotionInput(this, this);
    }

    /** SDL is linked into libmain.so, so that one library is all there is to load. */
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (motion != null) {
            motion.start(settings.motionTilt(), settings.motionShake());
        }
    }

    @Override
    protected void onPause() {
        if (motion != null) motion.stop();
        if (touch != null) touch.release();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        } else if (touch != null) {
            touch.release();  // a dialog or the shade took the screen: let go of every button
        }
    }

    @Override
    public void onBackPressed() {
        onMenu();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            // The runtime starts once per process; the next start must get a fresh one.
            Process.killProcess(Process.myPid());
        }
    }

    // --- input ---------------------------------------------------------------------------------

    @Override
    public void onTouchInput(int buttons, boolean shake) {
        touchButtons = buttons;
        touchShake = shake;
        pushInput();
    }

    @Override
    public void onMotion(float tilt, boolean shake) {
        motionTilt = tilt;
        motionShake = shake;
        pushInput();
    }

    private void pushInput() {
        if (!nativeReady) return;
        try {
            nativeSetTouchState(touchButtons, touchShake || motionShake, motionTilt);
        } catch (UnsatisfiedLinkError e) {
            nativeReady = false;
            Log.e(TAG, "touch input bridge missing from libmain.so", e);
        }
    }

    private void applyControlSettings() {
        touch.setControlsVisible(settings.touchControls());
        touch.setOpacity(settings.touchOpacity());
        touch.setHaptics(settings.haptics());
    }

    private void hideSystemBars() {
        getWindow().setDecorFitsSystemWindows(false);
        final WindowInsetsController controller = getWindow().getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // --- menu ----------------------------------------------------------------------------------

    @Override
    public void onMenu() {
        touch.release();
        final String[] items = {"Display", "Controls", "Quit Game"};
        new AlertDialog.Builder(this)
                .setTitle("Menu")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showDisplayMenu();
                    else if (which == 1) showControlsMenu();
                    else finish();
                })
                .setNegativeButton("Back", null)
                .show();
    }

    private void showDisplayMenu() {
        final String[] items = {
            "Aspect Ratio: " + aspectLabel(settings.aspectMode()),
            "Render Resolution: " + AppSettings.resolutionLabel(settings.resolutionScale()),
            "FPS Counter: " + (settings.showFps() ? "On" : "Off"),
        };
        new AlertDialog.Builder(this)
                .setTitle("Display")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showAspectChoice();
                    else if (which == 1) showResolutionChoice();
                    else { settings.setShowFps(!settings.showFps()); displayChanged(); showDisplayMenu(); }
                })
                .setNegativeButton("Back", (dialog, which) -> onMenu())
                .show();
    }

    private static String aspectLabel(int mode) {
        switch (mode) {
            case AppSettings.ASPECT_4_3: return "Original 4:3";
            case AppSettings.ASPECT_16_9: return "16:9";
            default: return "Fill Screen";
        }
    }

    private void showAspectChoice() {
        final String[] labels = {"Original 4:3", "16:9", "Fill Screen"};
        new AlertDialog.Builder(this)
                .setTitle("Aspect Ratio")
                .setSingleChoiceItems(labels, settings.aspectMode(), (dialog, which) -> {
                    settings.setAspectMode(which);
                    displayChanged();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    private void showResolutionChoice() {
        final float[] scales = AppSettings.RESOLUTION_SCALES;
        final String[] labels = new String[scales.length];
        int selected = 2;
        for (int i = 0; i < scales.length; ++i) {
            labels[i] = AppSettings.resolutionLabel(scales[i]);
            if (Math.abs(scales[i] - settings.resolutionScale()) < 0.01f) selected = i;
        }
        new AlertDialog.Builder(this)
                .setTitle("Render Resolution")
                .setSingleChoiceItems(labels, selected, (dialog, which) -> {
                    settings.setResolutionScale(scales[which]);
                    displayChanged();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    /** Display settings are written to Config.toml, which the runtime reads when the game starts. */
    private void displayChanged() {
        try {
            AppFiles.writeConfig(this, settings);
        } catch (Exception e) {
            Log.e(TAG, "could not save the display settings", e);
        }
        Toast.makeText(this, "Applies the next time the game starts", Toast.LENGTH_SHORT).show();
    }

    private void showControlsMenu() {
        final String[] items = {
            "Touch Controls: " + (settings.touchControls() ? "On" : "Off"),
            "Touch Control Opacity: " + Math.round(settings.touchOpacity() * 100) + "%",
            "Tilt (turn the phone): " + (settings.motionTilt() ? "On" : "Off"),
            "Shake (jolt the phone): " + (settings.motionShake() ? "On" : "Off"),
            "Vibration: " + (settings.haptics() ? "On" : "Off"),
        };
        new AlertDialog.Builder(this)
                .setTitle("Controls")
                .setItems(items, (dialog, which) -> {
                    switch (which) {
                        case 0: settings.setTouchControls(!settings.touchControls()); break;
                        case 1: {
                            final float next = settings.touchOpacity() >= 0.95f ? 0.2f : settings.touchOpacity() + 0.2f;
                            settings.setTouchOpacity(Math.min(1f, next));
                            break;
                        }
                        case 2: settings.setMotionTilt(!settings.motionTilt()); break;
                        case 3: settings.setMotionShake(!settings.motionShake()); break;
                        default: settings.setHaptics(!settings.haptics()); break;
                    }
                    applyControlSettings();
                    motion.stop();
                    motion.start(settings.motionTilt(), settings.motionShake());
                    showControlsMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> onMenu())
                .show();
    }
}

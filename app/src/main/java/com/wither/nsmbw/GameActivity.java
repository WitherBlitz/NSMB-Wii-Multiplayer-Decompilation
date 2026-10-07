package com.wither.nsmbw;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
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

    /** The Display menu: aspect, render scale (480 lines per 1x), FPS counter (DisplaySettings). */
    static native void nativeApplyDisplaySettings(int aspect, float renderScale, boolean showFps);

    /** "GPU name\n1|0": the GPU the game draws with and whether its compatibility fixes are on. */
    static native String nativeGraphicsInfo();

    /** What this phone is called in LAN room lists (netplay). */
    static native void nativeSetDeviceName(String name);

    private WifiManager.MulticastLock multicastLock;

    private AppSettings settings;
    private boolean bootWidescreen;  // the game reads 4:3 versus widescreen once, when it starts
    private TouchControlsView touch;
    private MotionInput motion;
    private boolean nativeReady;
    private int touchButtons;
    private boolean touchShake, motionShake;
    private float motionTilt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        settings = new AppSettings(this);
        bootWidescreen = settings.aspectMode() != AppSettings.ASPECT_4_3;
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
        if (nativeReady) {
            nativeSetDeviceName(deviceName());
        }
        // LAN rooms are found with broadcasts, which many Wi-Fi drivers drop unless an app holds this.
        final WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("nsmbw-lan");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }

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

    /** The name the user gave the phone, else its model. */
    private String deviceName() {
        String name = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        if (name == null || name.trim().isEmpty()) {
            name = Build.MODEL;
        }
        return name == null ? "Phone" : name.trim();
    }

    /**
     * Network play: the runtime has written the session (netplay_start.cpp); start the game again in a
     * fresh process. The launcher waits for this one to end, then opens the game, which finds the
     * session and boots into it. Called from the game thread.
     */
    void restartForSession() {
        runOnUiThread(() -> {
            final Intent intent = new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_RESTART_GAME, true)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            startActivity(intent);
            finish();
        });
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
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
            "Graphics Compatibility: " + gpuCompatLabel(),
        };
        new AlertDialog.Builder(this)
                .setTitle("Display")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showAspectChoice();
                    else if (which == 1) showResolutionChoice();
                    else if (which == 2) { settings.setShowFps(!settings.showFps()); displayChanged(); showDisplayMenu(); }
                    else showGpuCompatChoice();
                })
                .setNegativeButton("Back", (dialog, which) -> onMenu())
                .show();
    }

    /** [GPU name, "1" or "0"], or null before the renderer has started. */
    private String[] graphicsInfo() {
        if (!nativeReady) return null;
        try {
            final String info = nativeGraphicsInfo();
            final String[] parts = info == null ? new String[0] : info.split("\n", 2);
            return parts.length == 2 ? parts : null;
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** "Auto (on for Adreno (TM) 840)": the setting, and for Auto what the game chose for this GPU. */
    private String gpuCompatLabel() {
        final int mode = settings.gpuCompat();
        if (mode == AppSettings.GPU_COMPAT_ON) return "On";
        if (mode == AppSettings.GPU_COMPAT_OFF) return "Off";
        final String[] info = graphicsInfo();
        if (info == null) return "Auto";
        return "Auto (" + ("1".equals(info[1]) ? "on" : "off") + " for " + info[0] + ")";
    }

    /**
     * Fixes for phone GPUs whose drivers break the game's 3D models (scrambled textures, exploding
     * characters). Auto turns them on for the GPU the game detects; On and Off force them.
     */
    private void showGpuCompatChoice() {
        final String[] info = graphicsInfo();
        final String[] labels = {
            "Auto (recommended)" + (info != null ? ": " + ("1".equals(info[1]) ? "on" : "off") + " for " + info[0] : ""),
            "On: always use the GPU fixes",
            "Off: never use them",
        };
        new AlertDialog.Builder(this)
                .setTitle("Graphics Compatibility")
                .setSingleChoiceItems(labels, settings.gpuCompat(), (dialog, which) -> {
                    final boolean changed = which != settings.gpuCompat();
                    settings.setGpuCompat(which);
                    try {
                        AppFiles.writeConfig(this, settings);
                    } catch (Exception e) {
                        Log.e(TAG, "could not save the graphics compatibility setting", e);
                    }
                    if (changed) {
                        Toast.makeText(this, "Graphics Compatibility applies the next time the game starts",
                                Toast.LENGTH_LONG).show();
                    }
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
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

    /** The aspect the game actually renders: Fill follows the screen, never narrower than 16:9. */
    private float renderAspect() {
        switch (settings.aspectMode()) {
            case AppSettings.ASPECT_4_3: return 4f / 3f;
            case AppSettings.ASPECT_16_9: return 16f / 9f;
            default: {
                final android.view.View decor = getWindow().getDecorView();
                final int w = Math.max(decor.getWidth(), decor.getHeight());
                final int h = Math.min(decor.getWidth(), decor.getHeight());
                return h > 0 ? Math.max((float) w / h, 16f / 9f) : 16f / 9f;
            }
        }
    }

    private void showResolutionChoice() {
        final float[] scales = AppSettings.RESOLUTION_SCALES;
        final String[] labels = new String[scales.length];
        final float aspect = renderAspect();
        int selected = 2;
        for (int i = 0; i < scales.length; ++i) {
            final int lines = Math.round(480 * scales[i]);
            labels[i] = AppSettings.resolutionLabel(scales[i]) + "  ·  " + Math.round(lines * aspect) + "×" + lines;
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

    /**
     * Display changes apply at once (the runtime picks them up at the next frame) and are saved to
     * Config.toml for the next start. Only 4:3 versus widescreen waits for a restart, because the
     * game asks for its aspect once, while it boots.
     */
    private void displayChanged() {
        try {
            AppFiles.writeConfig(this, settings);
        } catch (Exception e) {
            Log.e(TAG, "could not save the display settings", e);
        }
        if (nativeReady) {
            try {
                nativeApplyDisplaySettings(settings.aspectMode(), settings.resolutionScale(), settings.showFps());
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "display bridge missing from libmain.so", e);
            }
        }
        if ((settings.aspectMode() != AppSettings.ASPECT_4_3) != bootWidescreen) {
            Toast.makeText(this, "Switching between 4:3 and widescreen applies the next time the game starts",
                    Toast.LENGTH_LONG).show();
        }
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

// android_bridge.cpp - JNI entry points the Android app (com.wither.nsmbw) calls into libmain.so.
#if defined(__ANDROID__)

#include "display_settings.h"
#include "runtime_config.h"
#include "settings_overlay.h"
#include "virtual_remote.h"

#include <jni.h>

#include <cstdint>

extern "C" {

// GameActivity.nativeSetTouchState: the on-screen remote (VirtualRemote::TouchButton bits) and the
// phone's motion (shake, tilt -1..1) for player 1. Called from the UI thread on every change.
JNIEXPORT void JNICALL Java_com_wither_nsmbw_GameActivity_nativeSetTouchState(
    JNIEnv*, jclass, jint buttons, jboolean shake, jfloat tilt)
{
    VirtualRemote::SetTouchState(static_cast<uint32_t>(buttons), shake == JNI_TRUE, tilt);
}

// GameActivity.nativeApplyDisplaySettings: the Display menu. The aspect (0 original 4:3, 1 16:9,
// 2 fill the screen) and the render scale (480 lines per 1x) apply at the next frame; 4:3 versus a
// widescreen mode only at the next start, since the game reads it at boot.
JNIEXPORT void JNICALL Java_com_wither_nsmbw_GameActivity_nativeApplyDisplaySettings(
    JNIEnv*, jclass, jint aspect, jfloat renderScale, jboolean showFps)
{
    DisplaySettings::Settings settings;
    settings.aspect = aspect < 0 || aspect > 2 ? DisplaySettings::kAspectFill : static_cast<int32_t>(aspect);
    settings.renderScale = renderScale > 0.0f ? renderScale : 1.0f;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetAspectMode(settings.aspect);
    RuntimeConfigFile::SetResolutionMultiplier(settings.renderScale);
    settings_overlay::SetShowFps(showFps == JNI_TRUE);
}

} // extern "C"

#endif // __ANDROID__

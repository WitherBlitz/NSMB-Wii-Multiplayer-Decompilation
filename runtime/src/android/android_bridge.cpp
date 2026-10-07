// android_bridge.cpp - JNI entry points the Android app (com.wither.nsmbw) calls into libmain.so.
#if defined(__ANDROID__)

#include "display_settings.h"
#include "graphics_info.h"
#include "runtime_config.h"
#include "settings_overlay.h"
#include "virtual_remote.h"

#include "netplay/net_socket.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace AndroidBridge {

// Network play: ask the activity to start the game again in a fresh process (GameActivity
// .restartForSession), then park this thread until Android ends the process.
bool RestartGame() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return false;
    }
    jclass type = env->GetObjectClass(activity);
    jmethodID restart = type != nullptr ? env->GetMethodID(type, "restartForSession", "()V") : nullptr;
    if (restart == nullptr) {
        env->ExceptionClear();
        return false;
    }
    env->CallVoidMethod(activity, restart);
    env->DeleteLocalRef(activity);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace AndroidBridge

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

// GameActivity.nativeGraphicsInfo: the GPU the game is drawing with and whether the GPU
// compatibility fixes are on, as "<name>\n<1|0>"; empty until the renderer has started.
JNIEXPORT jstring JNICALL Java_com_wither_nsmbw_GameActivity_nativeGraphicsInfo(JNIEnv* env, jclass)
{
    const std::string name = GraphicsInfo::Name();
    if (name.empty()) {
        return env->NewStringUTF("");
    }
    const std::string info = name + "\n" + (GraphicsInfo::CompatActive() ? "1" : "0");
    return env->NewStringUTF(info.c_str());
}

// GameActivity.nativeSetDeviceName: what this phone is called in room lists.
JNIEXPORT void JNICALL Java_com_wither_nsmbw_GameActivity_nativeSetDeviceName(JNIEnv* env, jclass, jstring name)
{
    if (name == nullptr) {
        return;
    }
    const char* utf8 = env->GetStringUTFChars(name, nullptr);
    if (utf8 != nullptr) {
        Netplay::SetDeviceName(utf8);
        env->ReleaseStringUTFChars(name, utf8);
    }
}

} // extern "C"

#endif // __ANDROID__

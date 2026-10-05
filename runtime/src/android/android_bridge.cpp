// android_bridge.cpp - JNI entry points the Android app (com.wither.nsmbw) calls into libmain.so.
#if defined(__ANDROID__)

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

} // extern "C"

#endif // __ANDROID__

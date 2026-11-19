// SPDX-License-Identifier: GPL-3.0-or-later
#include <jni.h>
#include "duet_session_lock.h"

extern "C" JNIEXPORT jint JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeSessionLock_openLease(JNIEnv* env, jclass,
                                                               jstring path) {
    const char* name = env->GetStringUTFChars(path, nullptr);
    if (!name) return -ENOMEM;
    const int fd = DuetOpenSessionLease(name);
    env->ReleaseStringUTFChars(path, name);
    return fd;
}

// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay;

import androidx.annotation.Keep;

/** Deliberately independent of the emulator core; also loaded by native child processes. */
@Keep
final class NativeSessionLock {
    static { System.loadLibrary("duet_session"); }
    private NativeSessionLock() {}
    static native int openLease(String path);
}

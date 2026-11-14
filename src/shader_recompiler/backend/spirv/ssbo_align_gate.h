// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

#include "common/settings.h"

namespace Shader::Backend::SPIRV {

/// A/B gate for splitting misaligned 64/128-bit storage accesses. On by default, set
/// EGLANTINE_SSBO_ALIGN_FIX=0 to get the old vector views. Clear the shader cache when switching.
/// Switch 2 titles only.
inline bool SsboAlignFixEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EGLANTINE_SSBO_ALIGN_FIX");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled && Settings::CurrentProgramIsSwitch2();
}

} // namespace Shader::Backend::SPIRV

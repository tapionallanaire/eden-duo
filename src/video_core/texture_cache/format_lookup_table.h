// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

VideoCore::Surface::PixelFormat PixelFormatFromTextureInfo(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

/// The pixel format, or nothing when the combination is not a known texture format (a garbage
/// descriptor); PixelFormatFromTextureInfo logs that case and falls back to A8B8G8R8_UNORM.
std::optional<VideoCore::Surface::PixelFormat> TryPixelFormatFromTextureInfo(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

} // namespace VideoCommon

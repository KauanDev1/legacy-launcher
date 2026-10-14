// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>

#include "imgui.h"

// Visual theme of the GUI launcher: palette, fonts and ImGui style.
namespace lr::gui::theme {

constexpr ImU32 rgb(uint32_t hex, uint8_t alpha = 255) {
  return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha);
}

// Palette (dark, neutral with a grass-green accent).
inline constexpr uint32_t kBackground = 0x0F1115;
inline constexpr uint32_t kSurface = 0x171A21;
inline constexpr uint32_t kRaised = 0x1F232C;
inline constexpr uint32_t kBorder = 0x2A2F3A;
inline constexpr uint32_t kText = 0xE6E8EE;
inline constexpr uint32_t kMuted = 0x8B93A7;
inline constexpr uint32_t kAccent = 0x4ADE80;
inline constexpr uint32_t kAccentHover = 0x22C55E;
inline constexpr uint32_t kAccentPressed = 0x16A34A;
inline constexpr uint32_t kOnAccent = 0x0B1F12;
inline constexpr uint32_t kInfo = 0x60A5FA;
inline constexpr uint32_t kWarning = 0xFBBF24;
inline constexpr uint32_t kError = 0xF87171;
// Derived: hover/pressed shades of the raised surface for secondary controls.
inline constexpr uint32_t kRaisedHover = 0x272C37;
inline constexpr uint32_t kRaisedPressed = 0x2E3441;

inline ImVec4 vec(uint32_t hex, float alpha = 1.0f) {
  return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

struct Fonts {
  ImFont* body = nullptr;      // Inter Regular 16
  ImFont* small = nullptr;     // Inter Regular 13.5 (metadata, descriptions)
  ImFont* label = nullptr;     // Inter SemiBold 13.5 (pill, stage state, section labels)
  ImFont* strong = nullptr;    // Inter SemiBold 16 (labels, buttons)
  ImFont* heading = nullptr;   // Inter SemiBold 18 (card titles, file name, CTA)
  ImFont* title = nullptr;     // Inter Bold 28 (app name)
  ImFont* mono = nullptr;      // JetBrains Mono 13.5 (log)
};

// The loaded fonts (all point to ImGui's default font if the TTFs were missing).
const Fonts& fonts();
// UI scale chosen at startup (display content scale), used to scale hand-drawn geometry.
float scale();
// Shorthand: `px * scale()`.
inline float px(float v) { return v * scale(); }

// `ui_scale` multiplies every size (HiDPI), `framebuffer_scale` (drawable / window size) rasterises the fonts at
// the physical resolution. Loads the fonts from `fonts_dir` (falls back to ImGui's default font with a warning on
// stderr if a file is missing) and applies the style. Call once, after ImGui::CreateContext.
void setup(const std::filesystem::path& fonts_dir, float ui_scale, float framebuffer_scale);

}  // namespace lr::gui::theme

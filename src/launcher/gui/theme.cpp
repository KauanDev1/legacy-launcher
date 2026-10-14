// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/gui/theme.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/log.h"

namespace lr::gui::theme {
namespace {

Fonts g_fonts;
float g_scale = 1.0f;

// Latin-1 + Latin Extended-A (Portuguese accents), general punctuation (… · — “ ”), arrows and ▶.
constexpr ImWchar kGlyphRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027,
                                    0x2190, 0x2193, 0x25B6, 0x25B6, 0};

// Loads a TTF through memory so a missing/unreadable file is a warning, not an ImGui assertion.
ImFont* load(const std::filesystem::path& file, float size_px, float fb_scale) {
  std::ifstream in(file, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) bytes.clear();
  if (bytes.empty()) {
    log::warn("font not found or unreadable: " + file.string() + " (using ImGui's default font)");
    return nullptr;
  }
  void* data = IM_ALLOC(bytes.size());
  std::copy(bytes.begin(), bytes.end(), static_cast<char*>(data));
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes.size()), size_px * fb_scale, &cfg,
                                                    kGlyphRanges);
}

void apply_style(float s) {
  ImGuiStyle& st = ImGui::GetStyle();
  st = ImGuiStyle();
  st.WindowPadding = ImVec2(24, 20);
  st.FramePadding = ImVec2(12, 8);
  st.ItemSpacing = ImVec2(10, 10);
  st.ItemInnerSpacing = ImVec2(8, 6);
  st.CellPadding = ImVec2(8, 6);
  st.ScrollbarSize = 10;
  st.GrabMinSize = 10;
  st.WindowRounding = 0;
  st.ChildRounding = 12;
  st.FrameRounding = 8;
  st.PopupRounding = 8;
  st.ScrollbarRounding = 8;
  st.GrabRounding = 8;
  st.TabRounding = 8;
  st.WindowBorderSize = 0;
  st.ChildBorderSize = 1;
  st.PopupBorderSize = 1;
  st.FrameBorderSize = 1;
  st.DisabledAlpha = 0.45f;
  st.ScaleAllSizes(s);

  ImVec4* c = st.Colors;
  for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = vec(kText);  // nothing keeps a default (blue) colour
  c[ImGuiCol_Text] = vec(kText);
  c[ImGuiCol_TextDisabled] = vec(kMuted);
  c[ImGuiCol_WindowBg] = vec(kBackground);
  c[ImGuiCol_ChildBg] = vec(kSurface);
  c[ImGuiCol_PopupBg] = vec(kRaised);
  c[ImGuiCol_Border] = vec(kBorder);
  c[ImGuiCol_BorderShadow] = vec(0, 0.0f);
  c[ImGuiCol_FrameBg] = vec(kRaised);
  c[ImGuiCol_FrameBgHovered] = vec(kRaisedHover);
  c[ImGuiCol_FrameBgActive] = vec(kRaisedPressed);
  c[ImGuiCol_TitleBg] = vec(kSurface);
  c[ImGuiCol_TitleBgActive] = vec(kSurface);
  c[ImGuiCol_TitleBgCollapsed] = vec(kSurface);
  c[ImGuiCol_MenuBarBg] = vec(kSurface);
  c[ImGuiCol_ScrollbarBg] = vec(0, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = vec(kBorder);
  c[ImGuiCol_ScrollbarGrabHovered] = vec(0x3A404D);
  c[ImGuiCol_ScrollbarGrabActive] = vec(0x4A5161);
  c[ImGuiCol_CheckMark] = vec(kAccent);
  c[ImGuiCol_SliderGrab] = vec(kAccent);
  c[ImGuiCol_SliderGrabActive] = vec(kAccentPressed);
  c[ImGuiCol_Button] = vec(kRaised);
  c[ImGuiCol_ButtonHovered] = vec(kRaisedHover);
  c[ImGuiCol_ButtonActive] = vec(kRaisedPressed);
  c[ImGuiCol_Header] = vec(kRaised);
  c[ImGuiCol_HeaderHovered] = vec(kRaisedHover);
  c[ImGuiCol_HeaderActive] = vec(kRaisedPressed);
  c[ImGuiCol_Separator] = vec(kBorder);
  c[ImGuiCol_SeparatorHovered] = vec(kAccent, 0.6f);
  c[ImGuiCol_SeparatorActive] = vec(kAccent);
  c[ImGuiCol_ResizeGrip] = vec(0, 0.0f);
  c[ImGuiCol_ResizeGripHovered] = vec(kAccent, 0.4f);
  c[ImGuiCol_ResizeGripActive] = vec(kAccent, 0.8f);
  c[ImGuiCol_Tab] = vec(kRaised);
  c[ImGuiCol_TabHovered] = vec(kRaisedHover);
  c[ImGuiCol_TabSelected] = vec(kRaisedPressed);
  c[ImGuiCol_TabSelectedOverline] = vec(kAccent);
  c[ImGuiCol_TabDimmed] = vec(kSurface);
  c[ImGuiCol_TabDimmedSelected] = vec(kRaised);
  c[ImGuiCol_TabDimmedSelectedOverline] = vec(kBorder);
  c[ImGuiCol_PlotLines] = vec(kAccent);
  c[ImGuiCol_PlotLinesHovered] = vec(kAccentHover);
  c[ImGuiCol_PlotHistogram] = vec(kAccent);
  c[ImGuiCol_PlotHistogramHovered] = vec(kAccentHover);
  c[ImGuiCol_TableHeaderBg] = vec(kRaised);
  c[ImGuiCol_TableBorderStrong] = vec(kBorder);
  c[ImGuiCol_TableBorderLight] = vec(kBorder);
  c[ImGuiCol_TableRowBg] = vec(0, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = vec(0xFFFFFF, 0.02f);
  c[ImGuiCol_TextLink] = vec(kAccent);
  c[ImGuiCol_TextSelectedBg] = vec(kAccent, 0.25f);
  c[ImGuiCol_DragDropTarget] = vec(kAccent);
  c[ImGuiCol_NavCursor] = vec(kAccent);
  c[ImGuiCol_NavWindowingHighlight] = vec(kAccent, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = vec(0, 0.5f);
  c[ImGuiCol_ModalWindowDimBg] = vec(0, 0.6f);
}

}  // namespace

const Fonts& fonts() { return g_fonts; }
float scale() { return g_scale; }

void setup(const std::filesystem::path& fonts_dir, float ui_scale, float framebuffer_scale) {
  g_scale = ui_scale;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  const float s = ui_scale;
  const float fb = framebuffer_scale > 0.0f ? framebuffer_scale : 1.0f;
  std::error_code ec;
  if (!std::filesystem::is_directory(fonts_dir, ec)) {
    log::warn("font directory not found: " + fonts_dir.string() + " (using ImGui's default font)");
  } else {
    g_fonts.body = load(fonts_dir / "Inter-Regular.ttf", 16.0f * s, fb);
    g_fonts.small = load(fonts_dir / "Inter-Regular.ttf", 13.5f * s, fb);
    g_fonts.strong = load(fonts_dir / "Inter-SemiBold.ttf", 16.0f * s, fb);
    g_fonts.heading = load(fonts_dir / "Inter-SemiBold.ttf", 18.0f * s, fb);
    g_fonts.title = load(fonts_dir / "Inter-Bold.ttf", 28.0f * s, fb);
    g_fonts.mono = load(fonts_dir / "JetBrainsMono-Regular.ttf", 13.5f * s, fb);
  }
  ImFont* fallback = g_fonts.body;
  if (!fallback) {
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * s * fb;
    fallback = io.Fonts->AddFontDefault(&cfg);
  }
  for (ImFont** f : {&g_fonts.body, &g_fonts.small, &g_fonts.strong, &g_fonts.heading, &g_fonts.title, &g_fonts.mono})
    if (!*f) *f = fallback;
  io.FontDefault = g_fonts.body;
  io.FontGlobalScale = 1.0f / fb;
  apply_style(s);
}

}  // namespace lr::gui::theme

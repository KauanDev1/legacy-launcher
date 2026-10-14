// SPDX-License-Identifier: MIT
#include "launcher/gui/widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "imgui_internal.h"
#include "launcher/gui/theme.h"

namespace lr::gui::widgets {
namespace {

using theme::px;
using theme::rgb;

constexpr float kPi = 3.14159265358979f;

// Rendered size of `font` (fonts are rasterised at framebuffer resolution and scaled back by FontGlobalScale).
float font_size(ImFont* font) { return font->FontSize * ImGui::GetIO().FontGlobalScale; }

ImVec2 measure(ImFont* font, std::string_view s) {
  return font->CalcTextSizeA(font_size(font), FLT_MAX, 0.0f, s.data(), s.data() + s.size());
}

void draw_text(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 col, std::string_view s) {
  dl->AddText(font, font_size(font), ImVec2(std::round(pos.x), std::round(pos.y)), col, s.data(), s.data() + s.size());
}

std::string_view visible_label(const char* label) {
  const char* end = std::strstr(label, "##");
  return end ? std::string_view(label, static_cast<size_t>(end - label)) : std::string_view(label);
}

ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

// Opaque mix of two palette colours (t = 0 -> a, 1 -> b).
ImU32 mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int shift) {
    const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
    return static_cast<uint32_t>(std::lround(x + (y - x) * t)) << shift;
  };
  return rgb(ch(16) | ch(8) | ch(0));
}

float play_icon_width(ImFont* font) { return font_size(font) * 0.62f; }

void draw_play_icon(ImDrawList* dl, ImVec2 left_center, float h, ImU32 col) {
  const float w = h * 0.86f;
  const ImVec2 a(left_center.x, left_center.y - h * 0.5f), b(left_center.x + w, left_center.y),
      c(left_center.x, left_center.y + h * 0.5f);
  dl->AddTriangleFilled(a, b, c, col);
}

void draw_check(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float thickness) {
  const ImVec2 pts[3] = {ImVec2(c.x - r * 0.42f, c.y + r * 0.02f), ImVec2(c.x - r * 0.12f, c.y + r * 0.32f),
                         ImVec2(c.x + r * 0.44f, c.y - r * 0.30f)};
  dl->AddPolyline(pts, 3, col, ImDrawFlags_None, thickness);
}

void draw_cross(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float thickness) {
  const float d = r * 0.36f;
  dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), col, thickness);
  dl->AddLine(ImVec2(c.x + d, c.y - d), ImVec2(c.x - d, c.y + d), col, thickness);
}

void stage_glyph(ImDrawList* dl, ImVec2 c, float r, StageState state) {
  const float t = static_cast<float>(ImGui::GetTime());
  switch (state) {
    case StageState::Pending:
      dl->AddCircle(c, r - px(0.75f), rgb(0x3A404D), 32, px(1.5f));
      break;
    case StageState::Running: {
      dl->AddCircle(c, r - px(1.0f), rgb(theme::kAccent, 50), 32, px(2.0f));
      const float a0 = t * 2.0f * kPi * 0.9f;
      dl->PathArcTo(c, r - px(1.0f), a0, a0 + kPi * 0.6f, 16);
      dl->PathStroke(rgb(theme::kAccent), ImDrawFlags_None, px(2.5f));
      dl->AddCircleFilled(c, px(3.0f), rgb(theme::kAccent));
      break;
    }
    case StageState::Done:
      dl->AddCircleFilled(c, r, rgb(theme::kAccent), 32);
      draw_check(dl, c, r, rgb(theme::kOnAccent), px(2.0f));
      break;
    case StageState::Cached:
      dl->AddCircleFilled(c, r, rgb(theme::kAccent, 46), 32);
      draw_check(dl, c, r, rgb(theme::kAccent, 210), px(2.0f));
      break;
    case StageState::Error:
      dl->AddCircleFilled(c, r, rgb(theme::kError), 32);
      draw_cross(dl, c, r, rgb(0x2A0E0E), px(2.2f));
      break;
  }
}

// Thin rounded progress bar (track + accent fill).
void draw_progress_bar(ImDrawList* dl, ImVec2 pos, ImVec2 size, float fraction) {
  const float r = size.y * 0.5f;
  dl->AddRectFilled(pos, pos + size, rgb(theme::kRaised), r);
  const float f = std::clamp(fraction, 0.0f, 1.0f);
  if (f > 0.0f)
    dl->AddRectFilled(pos, ImVec2(pos.x + std::max(size.y, size.x * f), pos.y + size.y), rgb(theme::kAccent), r);
}

// Erases the last UTF-8 code point of `s` (before position `end`), returning the new end.
size_t prev_char(std::string_view s, size_t end) {
  if (end == 0) return 0;
  --end;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return end;
}
size_t next_char(std::string_view s, size_t pos) {
  if (pos >= s.size()) return s.size();
  ++pos;
  while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) ++pos;
  return pos;
}

}  // namespace

std::string fit_text(ImFont* font, std::string_view s, float max_width, bool middle) {
  if (measure(font, s).x <= max_width) return std::string(s);
  static constexpr std::string_view kEllipsis = "…";
  if (!middle) {
    size_t end = s.size();
    while (end > 0) {
      end = prev_char(s, end);
      std::string out = std::string(s.substr(0, end)) + std::string(kEllipsis);
      if (measure(font, out).x <= max_width) return out;
    }
    return std::string(kEllipsis);
  }
  size_t head = s.size() / 2, tail = s.size() / 2;
  while (head > 0 && (static_cast<unsigned char>(s[head]) & 0xC0) == 0x80) --head;
  tail = head;
  bool cut_head = false;
  while (head > 0 || tail < s.size()) {
    if ((cut_head && head > 0) || tail >= s.size()) head = prev_char(s, head);
    else tail = next_char(s, tail);
    cut_head = !cut_head;
    std::string out = std::string(s.substr(0, head)) + std::string(kEllipsis) + std::string(s.substr(tail));
    if (measure(font, out).x <= max_width) return out;
  }
  return std::string(kEllipsis);
}

void text(ImFont* font, ImU32 color, std::string_view s) {
  ImGui::PushFont(font);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(s.data(), s.data() + s.size());
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void text_wrapped(ImFont* font, ImU32 color, std::string_view s) {
  ImGui::PushTextWrapPos(0.0f);
  text(font, color, s);
  ImGui::PopTextWrapPos();
}

ImVec2 button_size(const char* label, ImFont* font, ButtonIcon icon) {
  font = font ? font : theme::fonts().strong;
  float w = measure(font, visible_label(label)).x + px(18) * 2.0f;
  if (icon == ButtonIcon::Play) w += play_icon_width(font) + px(10);
  return ImVec2(std::round(w), std::round(font_size(font) + px(10) * 2.0f));
}

bool button(const char* label, ImVec2 size, ButtonKind kind, bool enabled, ButtonIcon icon, ImFont* font) {
  font = font ? font : theme::fonts().strong;
  const ImVec2 natural = button_size(label, font, icon);
  ImVec2 sz = size;
  if (sz.x == -1.0f) sz.x = ImGui::GetContentRegionAvail().x;
  else if (sz.x <= 0.0f) sz.x = natural.x;
  if (sz.y <= 0.0f) sz.y = natural.y;

  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::PushID(label);
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::InvisibleButton("##button", sz);
  ImGui::EndDisabled();
  ImGui::PopID();
  const bool hovered = enabled && ImGui::IsItemHovered();
  const bool held = enabled && ImGui::IsItemActive();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  ImU32 bg = 0, border = 0, fg = rgb(theme::kText);
  if (!enabled) {
    bg = kind == ButtonKind::Ghost ? 0 : rgb(theme::kRaised, 140);
    border = kind == ButtonKind::Ghost ? 0 : rgb(theme::kBorder);
    fg = rgb(theme::kMuted, kind == ButtonKind::Ghost ? 110 : 170);
  } else if (kind == ButtonKind::Primary) {
    bg = rgb(held ? theme::kAccentPressed : hovered ? theme::kAccentHover : theme::kAccent);
    fg = rgb(theme::kOnAccent);
  } else if (kind == ButtonKind::Secondary) {
    bg = rgb(held ? theme::kRaisedPressed : hovered ? theme::kRaisedHover : theme::kRaised);
    border = rgb(hovered ? 0x3A404D : theme::kBorder);
  } else {
    bg = held ? rgb(theme::kRaisedPressed) : hovered ? rgb(theme::kRaised) : 0;
    fg = rgb(hovered ? theme::kText : theme::kMuted);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 end = pos + sz;
  const float rounding = px(8);
  if (bg) dl->AddRectFilled(pos, end, bg, rounding);
  if (border) dl->AddRect(pos, end, border, rounding, ImDrawFlags_None, px(1));

  const std::string_view text_label = visible_label(label);
  const ImVec2 ts = measure(font, text_label);
  float content_w = ts.x;
  if (icon == ButtonIcon::Play) content_w += play_icon_width(font) + px(10);
  float x = pos.x + (sz.x - content_w) * 0.5f;
  const float cy = pos.y + sz.y * 0.5f;
  if (icon == ButtonIcon::Play) {
    draw_play_icon(dl, ImVec2(x, cy), font_size(font) * 0.66f, fg);
    x += play_icon_width(font) + px(10);
  }
  draw_text(dl, font, ImVec2(x, cy - ts.y * 0.5f), fg, text_label);
  return clicked && enabled;
}

void tooltip_for_last_item(const char* text_) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) return;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(8)));
  ImGui::PushStyleColor(ImGuiCol_Border, rgb(theme::kBorder));
  if (ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(px(320));
    text(theme::fonts().small, rgb(theme::kText), text_);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void begin_card(const char* id, ImVec2 size, bool auto_height, ImVec2 padding) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(padding.x), px(padding.y)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(12));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(theme::kSurface));
  ImGui::PushStyleColor(ImGuiCol_Border, rgb(theme::kBorder));
  ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
  if (auto_height) flags |= ImGuiChildFlags_AutoResizeY;
  ImGui::BeginChild(id, size, flags);
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

void end_card() { ImGui::EndChild(); }

void card_title(const char* title, const char* right_note) {
  const auto& f = theme::fonts();
  const float y = ImGui::GetCursorPosY();
  text(f.heading, rgb(theme::kText), title);
  if (!right_note) return;
  const ImVec2 ns = measure(f.small, right_note);
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ns.x);
  ImGui::SetCursorPosY(y + (font_size(f.heading) - font_size(f.small)) * 0.7f);
  text(f.small, rgb(theme::kMuted), right_note);
}

ImVec2 status_pill_size(StatusPill pill) {
  const ImVec2 ts = measure(theme::fonts().label, status_label(pill));
  return ImVec2(std::round(px(12) + px(8) + px(8) + ts.x + px(14)), std::round(px(30)));
}

void status_pill(StatusPill pill, ImVec2 top_left) {
  uint32_t col = theme::kMuted;
  switch (pill) {
    case StatusPill::Ready: col = theme::kAccent; break;
    case StatusPill::Installing: col = theme::kInfo; break;
    case StatusPill::Error: col = theme::kError; break;
    case StatusPill::NotInstalled: col = theme::kMuted; break;
  }
  const ImVec2 sz = status_pill_size(pill);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 end = top_left + sz;
  dl->AddRectFilled(top_left, end, rgb(col, 26), sz.y * 0.5f);
  dl->AddRect(top_left, end, rgb(col, 80), sz.y * 0.5f, ImDrawFlags_None, px(1));
  const ImVec2 dot(top_left.x + px(12) + px(4), top_left.y + sz.y * 0.5f);
  float dot_alpha = 1.0f;
  if (pill == StatusPill::Installing)
    dot_alpha = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
  dl->AddCircleFilled(dot, px(4), rgb(col, static_cast<uint8_t>(255.0f * dot_alpha)), 16);
  const std::string_view label = status_label(pill);
  const ImVec2 ts = measure(theme::fonts().label, label);
  draw_text(dl, theme::fonts().label, ImVec2(top_left.x + px(12) + px(8) + px(8), dot.y - ts.y * 0.5f), rgb(col),
            label);
}

void logo_mark(ImVec2 p, float size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 end(p.x + size, p.y + size);
  const int vtx0 = dl->VtxBuffer.Size;
  dl->AddRectFilled(p, end, rgb(theme::kAccent), size * 0.26f);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, dl->VtxBuffer.Size, p, end, rgb(theme::kAccent),
                                                rgb(theme::kAccentPressed));
  iso_block(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.53f), size, rgb(0xFFFFFF, 235), rgb(theme::kOnAccent, 215),
            rgb(theme::kOnAccent, 150));
}

void iso_block(ImDrawList* dl, ImVec2 c, float size, ImU32 top_col, ImU32 left_col, ImU32 right_col) {
  const float w = size * 0.27f, h = size * 0.155f, d = size * 0.30f;
  const ImVec2 top(c.x, c.y - h * 2.0f), l(c.x - w, c.y - h), r(c.x + w, c.y - h), mid(c.x, c.y);
  const ImVec2 lb(l.x, l.y + d), rb(r.x, r.y + d), mb(mid.x, mid.y + d);
  const ImVec2 top_face[4] = {top, r, mid, l}, left_face[4] = {l, mid, mb, lb}, right_face[4] = {mid, r, rb, mb};
  dl->AddConvexPolyFilled(top_face, 4, top_col);
  dl->AddConvexPolyFilled(left_face, 4, left_col);
  dl->AddConvexPolyFilled(right_face, 4, right_col);
}

bool drop_zone(const char* id, ImVec2 size, std::string_view primary, std::string_view secondary, bool has_file,
               bool clickable, ImU32 secondary_color) {
  const auto& f = theme::fonts();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!clickable);
  const bool clicked = ImGui::InvisibleButton(id, size);
  ImGui::EndDisabled();
  const bool hovered = clickable && ImGui::IsItemHovered();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 end = pos + size;
  const float rounding = px(10);
  dl->AddRectFilled(pos, end, hovered ? rgb(theme::kAccent, 12) : rgb(theme::kBackground, 110), rounding);
  dashed_rect(dl, ImVec2(pos.x + 0.5f, pos.y + 0.5f), ImVec2(end.x - 0.5f, end.y - 0.5f),
              hovered ? rgb(theme::kAccent, 170) : has_file ? rgb(theme::kAccent, 90) : rgb(0x3A404D), rounding,
              px(1.25f), px(6), px(4));

  ImFont* pf = has_file ? f.heading : f.strong;
  const float inner_w = size.x - px(24) * 2.0f;
  const std::string p1 = fit_text(pf, primary, inner_w, !has_file ? false : true);
  const std::string p2 = fit_text(f.small, secondary, inner_w, true);
  const ImVec2 s1 = measure(pf, p1), s2 = measure(f.small, p2);
  const float gap = p2.empty() ? 0.0f : px(6);
  const float block_h = s1.y + (p2.empty() ? 0.0f : gap + s2.y);
  const float y = pos.y + (size.y - block_h) * 0.5f;
  draw_text(dl, pf, ImVec2(pos.x + (size.x - s1.x) * 0.5f, y), rgb(has_file ? theme::kText : 0xC4C9D4), p1);
  if (!p2.empty()) draw_text(dl, f.small, ImVec2(pos.x + (size.x - s2.x) * 0.5f, y + s1.y + gap), secondary_color, p2);
  return clicked && clickable;
}

void gradient_rect(ImVec2 a, ImVec2 b, float rounding, uint32_t tint, float strength) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int vtx0 = dl->VtxBuffer.Size;
  dl->AddRectFilled(a, b, rgb(theme::kSurface), rounding);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, dl->VtxBuffer.Size, a,
                                                ImVec2(a.x + (b.x - a.x) * 0.8f, a.y + (b.y - a.y) * 0.8f),
                                                mix(theme::kSurface, tint, strength), rgb(theme::kSurface));
}

void chevron(ImDrawList* dl, ImVec2 c, float size, bool open, ImU32 col) {
  const float h = size * 0.5f;
  if (open) {
    const ImVec2 pts[3] = {ImVec2(c.x - h, c.y - h * 0.5f), ImVec2(c.x, c.y + h * 0.5f),
                           ImVec2(c.x + h, c.y - h * 0.5f)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, px(1.75f));
  } else {
    const ImVec2 pts[3] = {ImVec2(c.x - h * 0.5f, c.y - h), ImVec2(c.x + h * 0.5f, c.y),
                           ImVec2(c.x - h * 0.5f, c.y + h)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, px(1.75f));
  }
}

void dashed_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float rounding, float thickness, float dash,
                 float gap) {
  dl->PathRect(a, b, rounding);
  std::vector<ImVec2> pts(dl->_Path.begin(), dl->_Path.end());
  dl->PathClear();
  if (pts.size() < 2) return;
  pts.push_back(pts.front());
  bool drawing = true;
  float left = dash;  // length left in the current dash or gap
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    ImVec2 p = pts[i];
    const ImVec2 q = pts[i + 1];
    float seg = std::hypot(q.x - p.x, q.y - p.y);
    if (seg <= 0.0f) continue;
    const ImVec2 dir((q.x - p.x) / seg, (q.y - p.y) / seg);
    while (seg > 0.0f) {
      const float step = std::min(left, seg);
      const ImVec2 n(p.x + dir.x * step, p.y + dir.y * step);
      if (drawing) dl->AddLine(p, n, col, thickness);
      p = n;
      seg -= step;
      left -= step;
      if (left <= 0.0f) {
        drawing = !drawing;
        left = drawing ? dash : gap;
      }
    }
  }
}

void Stepper::row(const StepView& s) {
  const auto& f = theme::fonts();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const float r = px(11);
  const float text_x = start.x + r * 2.0f + px(14);
  const float text_w = start.x + width - text_x;
  const ImVec2 center(start.x + r, start.y + r);

  if (has_prev_) {
    const bool done = prev_state_ == StageState::Done || prev_state_ == StageState::Cached;
    dl->AddLine(ImVec2(prev_center_.x, prev_center_.y + r + px(5)), ImVec2(center.x, center.y - r - px(5)),
                done ? rgb(theme::kAccent, 110) : rgb(theme::kBorder), px(2));
  }
  stage_glyph(dl, center, r, s.state);

  // Title line, vertically centred on the glyph, with the state label right-aligned.
  const bool pending = s.state == StageState::Pending;
  const float title_h = font_size(f.strong);
  const float title_y = center.y - title_h * 0.5f - px(1);
  std::string_view state_label;
  ImU32 state_col = rgb(theme::kMuted);
  std::string pct;
  switch (s.state) {
    case StageState::Pending: state_label = s.cancelled ? "Cancelada" : "Pendente"; break;
    case StageState::Running:
      pct = std::to_string(static_cast<int>(std::floor(s.fraction * 100.0f))) + "%";
      state_label = pct;
      state_col = rgb(theme::kAccent);
      break;
    case StageState::Done: state_label = "Concluída"; state_col = rgb(theme::kAccent); break;
    case StageState::Cached: state_label = "Em cache"; state_col = rgb(theme::kAccent, 170); break;
    case StageState::Error: state_label = "Falhou"; state_col = rgb(theme::kError); break;
  }
  const ImVec2 ls = measure(f.label, state_label);
  draw_text(dl, f.label, ImVec2(start.x + width - ls.x, title_y + (title_h - ls.y) * 0.5f + px(1)), state_col,
            state_label);
  const float title_room = text_w - ls.x - px(12);
  draw_text(dl, f.strong, ImVec2(text_x, title_y), rgb(pending ? 0xB4BAC8 : theme::kText),
            fit_text(f.strong, s.title, title_room, false));

  float y = title_y + title_h + px(3);
  draw_text(dl, f.small, ImVec2(text_x, y), rgb(theme::kMuted), fit_text(f.small, s.description, text_w, false));
  y += font_size(f.small);

  if (s.state == StageState::Running) {
    y += px(10);
    const float bar_h = px(6);
    draw_progress_bar(dl, ImVec2(text_x, y), ImVec2(text_w, bar_h), s.fraction);
    y += bar_h + px(8);
    if (!s.message.empty()) {
      draw_text(dl, f.small, ImVec2(text_x, y), rgb(0xB4BAC8), fit_text(f.small, s.message, text_w, false));
      y += font_size(f.small);
    }
  }

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, std::max(y, center.y + r) - start.y + px(12)));
  has_prev_ = true;
  prev_center_ = center;
  prev_state_ = s.state;
}

void error_alert(const char* id, const char* title, const std::string& message, const std::string& hint) {
  const auto& f = theme::fonts();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(12)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(10));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(6)));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, mix(theme::kSurface, theme::kError, 0.08f));
  ImGui::PushStyleColor(ImGuiCol_Border, rgb(theme::kError, 150));
  ImGui::BeginChild(id, ImVec2(-FLT_MIN, 0.0f),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleColor(2);
  text(f.strong, rgb(theme::kError), title);
  text_wrapped(f.small, rgb(theme::kText), message);
  if (!hint.empty()) text_wrapped(f.small, rgb(theme::kMuted), "Dica: " + hint);
  ImGui::EndChild();
  ImGui::PopStyleVar(4);
}

}  // namespace lr::gui::widgets

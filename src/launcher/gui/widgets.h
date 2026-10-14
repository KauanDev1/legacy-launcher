// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "imgui.h"
#include "launcher/gui/ui_model.h"

// Hand-drawn building blocks of the launcher (cards, buttons, stepper, pill). Sizes are in unscaled pixels and
// multiplied by theme::scale() internally unless noted.
namespace lr::gui::widgets {

enum class ButtonKind { Primary, Secondary, Ghost };
enum class ButtonIcon { None, Play };

// Natural size (scaled) of a button with `label` (text after "##" is the id only) in `font`.
ImVec2 button_size(const char* label, ImFont* font, ButtonIcon icon = ButtonIcon::None);
// A rounded button at the cursor; size components <= 0 take the natural size, -1 width = available width.
// A disabled button is drawn muted (not faded accent) and never reports a click. Returns true when clicked.
bool button(const char* label, ImVec2 size, ButtonKind kind, bool enabled = true, ButtonIcon icon = ButtonIcon::None,
            ImFont* font = nullptr);
// Tooltip on the last item, also when it is disabled.
void tooltip_for_last_item(const char* text);

// A rounded surface card (child window) of `size` (already scaled; 0 = fill, per ImGui::BeginChild) with inner
// `padding` (unscaled). `auto_height` fits the height to the contents. Always pair with end_card().
void begin_card(const char* id, ImVec2 size, bool auto_height = false, ImVec2 padding = ImVec2(20, 18));
void end_card();
// Card heading; `right_note` (muted, optional) is right-aligned on the same line.
void card_title(const char* title, const char* right_note = nullptr);

// The header status pill (dot + label) drawn at `top_left` (no ImGui item; the caller reserves the space).
ImVec2 status_pill_size(StatusPill pill);
void status_pill(StatusPill pill, ImVec2 top_left);

// The app mark: a rounded green tile with an isometric block, `size` (scaled px) square at `top_left`.
void logo_mark(ImVec2 top_left, float size);
// Isometric block (the mark's glyph) of nominal `size` centred near `c`, one colour per visible face.
void iso_block(ImDrawList* dl, ImVec2 c, float size, ImU32 top_col, ImU32 left_col, ImU32 right_col);

// Drop-zone field of `size` (scaled) at the cursor: dashed rounded outline, `primary` text (large) and `secondary`
// (muted, may be empty) centred. `secondary_color` colours the secondary line. Returns true when clicked
// (only when `clickable`).
bool drop_zone(const char* id, ImVec2 size, std::string_view primary, std::string_view secondary, bool has_file,
               bool clickable, ImU32 secondary_color);

// Rounded rect fading from `tint` (mixed into the surface colour by `strength`) at the top-left corner to the plain
// surface colour at the bottom-right.
void gradient_rect(ImVec2 a, ImVec2 b, float rounding, uint32_t tint, float strength);

// Disclosure chevron (right when closed, down when open) centred at `c`.
void chevron(ImDrawList* dl, ImVec2 c, float size, bool open, ImU32 col);

// Dashed rounded rectangle outline.
void dashed_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float rounding, float thickness, float dash, float gap);

// One stepper row (glyph, title, state label, description; progress bar + message when running).
struct StepView {
  std::string_view title;
  std::string_view description;
  StageState state = StageState::Pending;
  float fraction = 0.0f;  // eased displayed fraction
  std::string_view message;
  bool cancelled = false;
};
// Draws the rows of a vertical stepper (with connectors between glyphs) filling the available width.
class Stepper {
 public:
  void row(const StepView& step);

 private:
  bool has_prev_ = false;
  ImVec2 prev_center_{};
  StageState prev_state_ = StageState::Pending;
};

// Inline alert card: red border and tint, bold `title`, wrapped `message`, muted "Dica: " `hint` (optional).
void error_alert(const char* id, const char* title, const std::string& message, const std::string& hint);

// `text` shortened with "…" (in the middle when `middle`, else at the end) to fit `max_width` in `font`.
std::string fit_text(ImFont* font, std::string_view text, float max_width, bool middle);

// Text in a given font and colour.
void text(ImFont* font, ImU32 color, std::string_view s);
void text_wrapped(ImFont* font, ImU32 color, std::string_view s);

}  // namespace lr::gui::widgets

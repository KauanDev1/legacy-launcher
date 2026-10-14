// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// A rounded surface card (child window) of `size` (scaled px; 0 = fill / auto per ImGui::BeginChild).
// `auto_height` fits the height to the contents. Always pair with end_card().
void begin_card(const char* id, ImVec2 size, bool auto_height = false);
void end_card();
// Card heading; `right_note` (muted, optional) is right-aligned on the same line.
void card_title(const char* title, const char* right_note = nullptr);

// The header status pill (dot + label), right-aligned at `right_x` on the current line.
ImVec2 status_pill_size(StatusPill pill);
void status_pill(StatusPill pill, ImVec2 top_left);

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

// A thin rounded progress bar of `size` (scaled px) at the cursor.
void progress_bar(float fraction, ImVec2 size);

// Inline alert card: red border and tint, bold `title`, wrapped `message`, muted "Dica: " `hint` (optional).
void error_alert(const char* id, const char* title, const std::string& message, const std::string& hint);

// `text` shortened with "…" (in the middle when `middle`, else at the end) to fit `max_width` in `font`.
std::string fit_text(ImFont* font, std::string_view text, float max_width, bool middle);

// Text in a given font and colour.
void text(ImFont* font, ImU32 color, std::string_view s);
void text_wrapped(ImFont* font, ImU32 color, std::string_view s);

}  // namespace lr::gui::widgets

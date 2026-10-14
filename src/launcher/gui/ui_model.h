// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "pipeline/types.h"

// Pure, ImGui-free presentation helpers of the GUI launcher (unit-tested).
namespace lr::gui {

enum class StageState { Pending, Running, Done, Cached, Error };

// Portuguese display text of a pipeline stage: Title Case name and a one-line description.
struct StageText {
  std::string_view title;
  std::string_view description;
};
StageText stage_text(pipeline::StageId id);

// The header's status pill. Precedence: an install in progress, then an error, then a complete build.
enum class StatusPill { NotInstalled, Installing, Ready, Error };
StatusPill derive_status(bool installing, bool has_error, bool build_complete);
std::string_view status_label(StatusPill pill);

// `path` with a leading $HOME component replaced by "~" (only on a whole-component match). Pure: callers pass
// getenv("HOME"); null/empty/"/" leaves the path untouched.
std::string abbreviate_home(const std::filesystem::path& path, const char* home);

// Eases the displayed progress towards `target` (exponential approach with `rate` per second, snapping when
// within half a percent). Moving backwards (a new run) snaps immediately.
float approach_progress(float displayed, float target, float dt_seconds, float rate = 10.0f);

// UI scale on top of the window's logical coordinates. `env_override` (LEGACYRECOMP_UI_SCALE) wins when it parses
// as a number in [0.5, 4]. Otherwise the display scale (`dpi` / 96; 1 when dpi <= 0) divided by the framebuffer
// scale (drawable / window size: backends with logical coordinates already scale by it), rounded to quarter
// steps and clamped to [1, 3].
float ui_scale(float dpi, float framebuffer_scale, const char* env_override);

// Colour class of a log line: "[ERROR]"/"error:"/"FAILED:" lines are errors, "[WARN]"/"warning:" warnings.
enum class LogTone { Normal, Warning, Error };
LogTone classify_log_line(std::string_view line);

}  // namespace lr::gui

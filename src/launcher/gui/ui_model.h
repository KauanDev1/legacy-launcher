// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "pipeline/types.h"

// Pure, ImGui-free presentation helpers of the GUI launcher (unit-tested).
namespace lr::gui {

enum class StageState { Pending, Running, Done, Cached, Error };

// Portuguese display text of a pipeline stage: name (sentence case) and a one-line description.
struct StageText {
  std::string_view title;
  std::string_view description;
};
StageText stage_text(pipeline::StageId id);

// Portuguese version of a running stage's progress message (Progress::message, English, from the pipeline).
// Known messages are translated (file names and counters kept); anything else becomes "Em andamento…", so the
// stepper never shows raw English. The log keeps the raw lines.
std::string stage_message(pipeline::StageId id, std::string_view raw);

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

// Environment the UI scale may come from (getenv() results; null/empty = unset).
struct ScaleEnv {
  const char* override_scale = nullptr;   // LEGACYRECOMP_UI_SCALE, a float in [0.5, 4]
  const char* gdk_scale = nullptr;        // GDK_SCALE (desktop hint)
  const char* qt_scale_factor = nullptr;  // QT_SCALE_FACTOR (desktop hint)
};
// UI scale on top of the window's logical coordinates. A valid override wins. Otherwise a framebuffer scale
// > 1 (Wayland/HiDPI backends) already scales everything, so 1. Otherwise GDK_SCALE, then QT_SCALE_FACTOR,
// if in [1, 4] (rounded to quarter steps, clamped to [1, 3]); else 1. The display DPI is deliberately not
// used: X11 reports the monitor's physical DPI, not the desktop scale.
float ui_scale(float framebuffer_scale, const ScaleEnv& env);

// Colour class of a log line: "[ERROR]"/"error:"/"FAILED:" lines are errors, "[WARN]"/"warning:" warnings.
enum class LogTone { Normal, Warning, Error };
LogTone classify_log_line(std::string_view line);

}  // namespace lr::gui

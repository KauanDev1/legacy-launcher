// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>
#include <string>

#include "pipeline/types.h"

namespace lr::cli {

// Console progress. tty: one redrawn line `\r[stage] [####....] 45% message`. Non-tty: one line each time the
// stage changes or the percentage enters a new 10% step.
// Cancellation: a process-wide flag set by request_cancel() (async-signal-safe; see install_signal_handlers()).
class ProgressBar : public pipeline::ProgressSink {
 public:
  ProgressBar(std::ostream& out, bool is_tty);

  void on_progress(const pipeline::Progress& progress) override;
  void on_log(std::string_view line) override;
  bool cancelled() const override;
  // Ends a pending tty line with a newline.
  void finish();

  static void request_cancel();
  static void reset_cancel();
  // SIGINT, SIGTERM and SIGHUP. First signal: sets the cancel flag (tools run in their own process groups, so
  // only we see it) and the pipeline stops at the next check, killing the running tool. Second signal (any of
  // the three): SIGKILL to the running tool's process group (pipeline::current_tool_pgid()), then the default
  // action is restored and the signal re-raised, so the launcher dies immediately without orphaning the tool.
  static void install_signal_handlers();

 private:
  void draw_tty(const pipeline::Progress& p);

  std::ostream& out_;
  bool tty_;
  bool line_open_ = false;  // tty: a progress line is on screen without a trailing newline
  int last_stage_ = -1;
  int last_step_ = -1;
  std::string last_msg_;
  pipeline::Progress last_{pipeline::StageId::Doctor, 0.0f, ""};
  bool have_last_ = false;
};

}  // namespace lr::cli

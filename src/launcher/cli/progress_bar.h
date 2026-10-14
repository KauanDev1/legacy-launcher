// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>
#include <string>

#include "pipeline/types.h"

namespace lr::cli {

// Console progress. tty: one redrawn line `\r[stage] [####....] 45% message`. Non-tty: one line each time the
// stage changes or the percentage enters a new 10% step.
// Cancellation: a process-wide flag set by request_cancel() (async-signal-safe; see install_sigint_handler()).
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
  // First Ctrl+C: sets the cancel flag (tools run in their own process groups, so only we see it) and the
  // pipeline stops at the next check, killing the running tool. Second Ctrl+C: default action restored and the
  // signal re-raised, so the launcher dies immediately.
  static void install_sigint_handler();

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

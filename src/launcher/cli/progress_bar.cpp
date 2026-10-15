// SPDX-License-Identifier: MIT
#include "launcher/cli/progress_bar.h"

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <ostream>

#include "pipeline/process.h"

namespace lr::cli {
namespace {

std::atomic<bool> g_cancel{false};
static_assert(std::atomic<bool>::is_always_lock_free);

constexpr int kBarWidth = 40;

// Async-signal-safe: lock-free atomics, kill, signal and raise only.
extern "C" void on_cancel_signal(int sig) {
  if (!g_cancel.exchange(true, std::memory_order_relaxed)) return;  // first signal: clean cancel
  // Second signal: give up on a clean stop. The tool tree lives in its own process group and would outlive us.
  const pid_t pgid = pipeline::current_tool_pgid();
  if (pgid > 0) ::kill(-pgid, SIGKILL);  // never kill(0) / kill(-0): that is our own group
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

}  // namespace

ProgressBar::ProgressBar(std::ostream& out, bool is_tty) : out_(out), tty_(is_tty) {}

void ProgressBar::request_cancel() { g_cancel.store(true, std::memory_order_relaxed); }
void ProgressBar::reset_cancel() { g_cancel.store(false, std::memory_order_relaxed); }
bool ProgressBar::cancelled() const { return g_cancel.load(std::memory_order_relaxed); }

void ProgressBar::install_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = on_cancel_signal;
  sigemptyset(&sa.sa_mask);
  // Block the other two while one runs, so a SIGHUP racing a SIGTERM cannot interleave the two handlers.
  for (int s : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&sa.sa_mask, s);
  for (int s : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(s, &sa, nullptr);
}

void ProgressBar::draw_tty(const pipeline::Progress& p) {
  const float f = std::clamp(p.fraction, 0.0f, 1.0f);
  const int pct = static_cast<int>(f * 100.0f);
  const int filled = pct * kBarWidth / 100;
  out_ << "\r\x1b[K[" << pipeline::stage_name(p.stage) << "] [" << std::string(static_cast<size_t>(filled), '#')
       << std::string(static_cast<size_t>(kBarWidth - filled), '.') << "] " << pct << "% " << p.message;
  out_.flush();
  line_open_ = true;
}

void ProgressBar::on_progress(const pipeline::Progress& p) {
  last_ = p;
  have_last_ = true;
  const int stage = static_cast<int>(p.stage);
  if (tty_) {
    // A new stage keeps the previous stage's final line on screen.
    if (line_open_ && stage != last_stage_) {
      out_ << "\n";
      line_open_ = false;
    }
    last_stage_ = stage;
    draw_tty(p);
    return;
  }
  const int pct = static_cast<int>(std::clamp(p.fraction, 0.0f, 1.0f) * 100.0f);
  const int step = pct / 10;
  // The terminal "done"/"cached" line is always shown once, even when the 100% step was already printed.
  const bool final_msg = (p.message == "done" || p.message == "cached") && last_msg_ != p.message;
  if (stage == last_stage_ && step == last_step_ && !final_msg) return;
  last_stage_ = stage;
  last_step_ = step;
  last_msg_ = p.message;
  out_ << "[" << pipeline::stage_name(p.stage) << "] " << pct << "% " << p.message << "\n";
  out_.flush();
}

void ProgressBar::on_log(std::string_view line) {
  if (tty_ && line_open_) out_ << "\r\x1b[K";
  out_ << line << "\n";
  if (tty_ && have_last_ && line_open_) draw_tty(last_);
  out_.flush();
}

void ProgressBar::finish() {
  if (tty_ && line_open_) {
    out_ << "\n";
    line_open_ = false;
    out_.flush();
  }
}

}  // namespace lr::cli

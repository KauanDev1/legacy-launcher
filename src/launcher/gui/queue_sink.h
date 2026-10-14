// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.h"
#include "pipeline/types.h"

namespace lr::gui {

// Thread-safe bridge between the pipeline worker thread and the UI thread: the worker posts events, the UI drains
// them once per frame. Never calls back into anything (safe to use from a lr::log sink).
class QueueSink : public pipeline::ProgressSink {
 public:
  struct Event {
    enum Kind { Progress, Log, Finished } kind = Log;
    pipeline::Progress progress{pipeline::StageId::Doctor, 0.0f, ""};
    std::string text;
    std::optional<Error> error;  // Finished: set on failure (incl. Cancelled)
  };

  void on_progress(const pipeline::Progress& progress) override;
  void on_log(std::string_view line) override;
  bool cancelled() const override;

  void post_finished(std::optional<Error> error);
  // Returns and clears all pending events, in posting order.
  std::vector<Event> drain();
  void cancel();

 private:
  std::mutex mutex_;
  std::vector<Event> events_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace lr::gui

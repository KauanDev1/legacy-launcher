// SPDX-License-Identifier: MIT
#include "launcher/gui/queue_sink.h"

#include <utility>

namespace lr::gui {

void QueueSink::on_progress(const pipeline::Progress& progress) {
  Event e;
  e.kind = Event::Progress;
  e.progress = progress;
  std::lock_guard lock(mutex_);
  events_.push_back(std::move(e));
}

void QueueSink::on_log(std::string_view line) {
  Event e;
  e.kind = Event::Log;
  e.text = std::string(line);
  std::lock_guard lock(mutex_);
  events_.push_back(std::move(e));
}

bool QueueSink::cancelled() const { return cancelled_.load(std::memory_order_relaxed); }

void QueueSink::post_finished(std::optional<Error> error) {
  Event e;
  e.kind = Event::Finished;
  e.error = std::move(error);
  std::lock_guard lock(mutex_);
  events_.push_back(std::move(e));
}

std::vector<QueueSink::Event> QueueSink::drain() {
  std::vector<Event> out;
  std::lock_guard lock(mutex_);
  out.swap(events_);
  return out;
}

void QueueSink::cancel() { cancelled_.store(true, std::memory_order_relaxed); }

}  // namespace lr::gui

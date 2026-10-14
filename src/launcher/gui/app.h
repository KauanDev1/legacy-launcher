// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "launcher/gui/queue_sink.h"
#include "pipeline/types.h"

namespace lr::gui {

// The launcher window contents (Dear ImGui). Owns the worker threads: the destructor cancels a running install and
// joins every thread, so no thread outlives the App.
class App {
 public:
  struct Settings {
    std::filesystem::path root;
    std::filesystem::path iso;  // prefilled ISO field
  };

  explicit App(Settings settings);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Draws one frame (between ImGui::NewFrame and ImGui::Render) covering the whole viewport.
  void frame();
  void set_iso(const std::string& path);
  void start_install();
  bool installing() const { return worker_.joinable() && !install_done_; }
  // True while something on screen moves (install running, progress easing, text cursor): the main loop then
  // renders every vsync instead of sleeping until input.
  bool animating() const;

 private:
  enum class StageState { Pending, Running, Done, Cached, Error };
  struct StageRow {
    explicit StageRow(pipeline::StageId i) : id(i) {}
    pipeline::StageId id;
    StageState state = StageState::Pending;
    float fraction = 0.0f;
    std::string message;
  };

  void pump_events();
  void handle_progress(const pipeline::Progress& p);
  void handle_finished(const std::optional<Error>& error);
  void refresh_from_disk();
  void browse();
  void play();
  void join_finished_threads();
  void add_log(std::string line);
  void draw_stage_table();

  Settings settings_;
  std::array<char, 1024> iso_buf_{};
  std::vector<StageRow> stages_;
  std::deque<std::string> log_;
  bool log_scroll_ = false;
  bool build_ready_ = false;  // build stage complete and bin/legacyrecomp-game present
  std::optional<Error> last_error_;
  std::string status_;
  std::string browse_program_;  // zenity / kdialog, empty if neither is installed

  // Install worker. The Options live inside the thread's closure for the whole run (stages hold them by reference).
  std::unique_ptr<QueueSink> sink_;
  std::thread worker_;
  bool install_done_ = false;

  // Log lines from lr::log (any thread) are queued here; the sink callback never re-enters lr::log.
  QueueSink log_queue_;

  // Game launch thread.
  std::unique_ptr<QueueSink> game_sink_;
  std::thread game_thread_;
  bool game_running_ = false;

  // "Procurar..." dialog thread.
  std::thread browse_thread_;
  std::mutex browse_mutex_;
  std::optional<std::string> browse_result_;
  bool browse_running_ = false;
};

}  // namespace lr::gui

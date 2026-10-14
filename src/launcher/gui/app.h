// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "launcher/gui/queue_sink.h"
#include "launcher/gui/ui_model.h"
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
  struct StageRow {
    explicit StageRow(pipeline::StageId i) : id(i) {}
    pipeline::StageId id;
    StageState state = StageState::Pending;
    float fraction = 0.0f;
    float shown = 0.0f;  // eased fraction drawn on screen
    std::string message;
    bool cancelled = false;
  };
  struct LogLine {
    std::string text;
    LogTone tone = LogTone::Normal;
  };

  void pump_events();
  void handle_progress(const pipeline::Progress& p);
  void handle_finished(const std::optional<Error>& error);
  void refresh_from_disk();
  void browse();
  void play();
  void add_log(std::string line);
  bool has_iso() const { return iso_buf_[0] != 0; }

  // Layout (one call per region of the window).
  void draw_header();
  void draw_image_card(bool compact);
  void draw_install_card(float height);
  void draw_play_card(float height);
  void draw_log_panel(float height);

  Settings settings_;
  std::array<char, 1024> iso_buf_{};
  std::vector<StageRow> stages_;
  std::deque<LogLine> log_;
  bool log_scroll_ = false;
  bool log_open_ = false;      // the "Registro" panel; opens by itself when an install fails
  bool editing_path_ = false;  // the ISO path is being typed instead of browsed/dropped
  bool focus_path_ = false;
  int scroll_to_stage_ = -1;  // stepper row to bring into view (it just started or failed)
  bool build_ready_ = false;  // build stage complete and bin/legacyrecomp-game present
  std::optional<Error> install_error_;
  std::optional<Error> game_error_;
  std::string install_status_;
  std::string game_status_;
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
  std::atomic<bool> browse_running_{false};  // cleared by the dialog thread, read by the UI thread
};

}  // namespace lr::gui

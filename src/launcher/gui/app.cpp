// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/gui/app.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

#include "common/log.h"
#include "common/paths.h"
#include "imgui.h"
#include "pipeline/pipeline.h"
#include "pipeline/process.h"
#include "pipeline/state.h"

namespace lr::gui {
namespace {

constexpr size_t kMaxLogLines = 2000;
constexpr pipeline::StageId kAllStages[] = {
    pipeline::StageId::Doctor,  pipeline::StageId::Extract,   pipeline::StageId::Verify, pipeline::StageId::Patch,
    pipeline::StageId::Analyse, pipeline::StageId::Recompile, pipeline::StageId::Build};

const char* stage_label(pipeline::StageId id) {
  switch (id) {
    case pipeline::StageId::Doctor: return "doctor: verificar o sistema";
    case pipeline::StageId::Extract: return "extract: extrair a ISO";
    case pipeline::StageId::Verify: return "verify: conferir os arquivos";
    case pipeline::StageId::Patch: return "patch: aplicar a atualizacao";
    case pipeline::StageId::Analyse: return "analyse: analisar o executavel";
    case pipeline::StageId::Recompile: return "recompile: gerar o codigo C++";
    case pipeline::StageId::Build: return "build: compilar o jogo";
  }
  return "?";
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

App::App(Settings settings) : settings_(std::move(settings)) {
  for (auto id : kAllStages) stages_.emplace_back(id);
  std::snprintf(iso_buf_.data(), iso_buf_.size(), "%s", settings_.iso.c_str());
  if (pipeline::find_program("zenity")) browse_program_ = "zenity";
  else if (pipeline::find_program("kdialog")) browse_program_ = "kdialog";
  // Non-reentrant by contract: lr::log holds its mutex while calling the sink, so only push to the queue.
  log::set_sink([this](log::Level, std::string_view msg) { log_queue_.on_log(msg); });
  refresh_from_disk();
}

App::~App() {
  if (sink_) sink_->cancel();
  if (worker_.joinable()) worker_.join();
  if (game_thread_.joinable()) game_thread_.join();
  if (browse_thread_.joinable()) browse_thread_.join();
  log::set_sink({});
}

void App::refresh_from_disk() {
  const DataPaths p = DataPaths::from_root(settings_.root);
  auto store = pipeline::StateStore::load(p.state_file);
  build_ready_ = false;
  if (!store) {
    add_log("state.toml ilegivel: " + store.error().message);
    return;
  }
  for (auto& row : stages_) {
    if (row.state != StageState::Pending) continue;
    if (store.value().complete_fingerprint(row.id)) {
      row.state = StageState::Cached;
      row.fraction = 1.0f;
      row.message = "concluida";
    }
  }
  build_ready_ = store.value().complete_fingerprint(pipeline::StageId::Build).has_value() &&
                 ::access((p.bin / "legacyrecomp-game").c_str(), X_OK) == 0;
}

void App::set_iso(const std::string& path) { std::snprintf(iso_buf_.data(), iso_buf_.size(), "%s", path.c_str()); }

void App::add_log(std::string line) {
  log_.push_back(std::move(line));
  while (log_.size() > kMaxLogLines) log_.pop_front();
  log_scroll_ = true;
}

void App::start_install() {
  if (installing()) return;
  if (worker_.joinable()) worker_.join();
  last_error_.reset();
  status_.clear();
  install_done_ = false;
  for (auto& row : stages_) row = StageRow(row.id);
  sink_ = std::make_unique<QueueSink>();
  pipeline::Options opts;
  opts.iso = iso_buf_[0] ? std::filesystem::path(iso_buf_.data()) : std::filesystem::path();
  opts.root = settings_.root;
  QueueSink* sink = sink_.get();
  // `opts` is moved into the closure, which lives as long as the thread: run() keeps references to it.
  worker_ = std::thread([sink, opts = std::move(opts)]() {
    auto r = pipeline::run(opts, *sink);
    sink->post_finished(r ? std::nullopt : std::optional<Error>(r.error()));
  });
}

void App::handle_progress(const pipeline::Progress& p) {
  for (auto& row : stages_) {
    if (row.id != p.stage) continue;
    if (p.message == "cached") {
      row.state = StageState::Cached;
      row.fraction = 1.0f;
      row.message = "em cache";
    } else if (p.message == "done" && p.fraction >= 1.0f) {
      row.state = StageState::Done;
      row.fraction = 1.0f;
      row.message = "concluida";
    } else {
      row.state = StageState::Running;
      row.fraction = p.fraction;
      row.message = p.message == "start" ? "iniciando" : p.message;
    }
  }
}

void App::handle_finished(const std::optional<Error>& error) {
  install_done_ = true;
  if (error) {
    last_error_ = error;
    for (auto& row : stages_)
      if (row.state == StageState::Running) {
        row.state = error->code == ErrorCode::Cancelled ? StageState::Pending : StageState::Error;
        row.message = error->code == ErrorCode::Cancelled ? "cancelada" : "falhou";
      }
    status_ = error->code == ErrorCode::Cancelled ? "Instalacao cancelada; as etapas concluidas ficam em cache."
                                                  : "A instalacao falhou.";
  } else {
    status_ = "Instalacao concluida.";
  }
  refresh_from_disk();
}

void App::pump_events() {
  for (auto& e : log_queue_.drain()) add_log(e.text);
  if (sink_) {
    for (auto& e : sink_->drain()) {
      switch (e.kind) {
        case QueueSink::Event::Progress: handle_progress(e.progress); break;
        case QueueSink::Event::Log: add_log(std::move(e.text)); break;
        case QueueSink::Event::Finished: handle_finished(e.error); break;
      }
    }
  }
  if (install_done_ && worker_.joinable()) worker_.join();
  if (game_sink_) {
    for (auto& e : game_sink_->drain()) {
      if (e.kind == QueueSink::Event::Log) {
        status_ = e.text;
      } else if (e.kind == QueueSink::Event::Finished) {
        game_running_ = false;
        if (e.error) {
          last_error_ = e.error;
          status_ = "Nao foi possivel iniciar o jogo.";
        }
      }
    }
    if (!game_running_ && game_thread_.joinable()) game_thread_.join();
  }
  {
    std::lock_guard lock(browse_mutex_);
    if (browse_result_) {
      set_iso(*browse_result_);
      browse_result_.reset();
    }
    if (!browse_running_ && browse_thread_.joinable()) browse_thread_.join();
  }
}

void App::browse() {
  if (browse_running_ || browse_program_.empty()) return;
  if (browse_thread_.joinable()) browse_thread_.join();
  browse_running_ = true;
  std::string program = browse_program_;
  browse_thread_ = std::thread([this, program] {
    std::vector<std::string> args = program == "zenity"
                                        ? std::vector<std::string>{"--file-selection", "--file-filter=*.iso"}
                                        : std::vector<std::string>{"--getopenfilename", ".", "*.iso"};
    auto out = pipeline::capture_output(program, args);  // non-zero exit = dialog cancelled
    std::lock_guard lock(browse_mutex_);
    if (out && !trim(out.value()).empty()) browse_result_ = trim(out.value());
    browse_running_ = false;
  });
}

void App::play() {
  if (game_running_) return;
  if (game_thread_.joinable()) game_thread_.join();
  last_error_.reset();
  status_ = "Jogo em execucao...";
  game_running_ = true;
  game_sink_ = std::make_unique<QueueSink>();
  QueueSink* sink = game_sink_.get();
  pipeline::Options opts;
  opts.root = settings_.root;
  game_thread_ = std::thread([sink, opts = std::move(opts)]() {
    auto r = pipeline::launch(opts, std::span<const std::string>{});
    if (r && r.value() == 3)
      sink->on_log("M1: o jogo parou na primeira importacao de kernel nao implementada (esperado neste marco).");
    else if (r)
      sink->on_log("O jogo terminou com codigo " + std::to_string(r.value()) + ".");
    sink->post_finished(r ? std::nullopt : std::optional<Error>(r.error()));
  });
}

void App::draw_stage_table() {
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.55f, 0.85f, 1));
  struct PopColor { ~PopColor() { ImGui::PopStyleColor(); } } pop_color;
  if (!ImGui::BeginTable("stages", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) return;
  ImGui::TableSetupColumn("Etapa", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("Estado", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupColumn("Progresso", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableHeadersRow();
  for (const auto& row : stages_) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(stage_label(row.id));
    ImGui::TableNextColumn();
    switch (row.state) {
      case StageState::Pending: ImGui::TextDisabled("pendente"); break;
      case StageState::Running: ImGui::Text("em andamento %d%%", static_cast<int>(row.fraction * 100.0f)); break;
      case StageState::Done: ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "concluida"); break;
      case StageState::Cached: ImGui::TextColored(ImVec4(0.4f, 0.75f, 0.9f, 1), "%s", row.message.c_str()); break;
      case StageState::Error: ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "erro"); break;
    }
    ImGui::TableNextColumn();
    if (row.state == StageState::Running || row.state == StageState::Done || row.state == StageState::Cached) {
      ImGui::ProgressBar(row.fraction, ImVec2(-FLT_MIN, 0),
                         row.state == StageState::Running ? row.message.c_str() : "");
    }
  }
  ImGui::EndTable();
}

void App::frame() {
  pump_events();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("LegacyRecomp", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  ImGui::SeparatorText("LegacyRecomp - Minecraft: Xbox 360 Edition");
  ImGui::TextDisabled("Pasta de dados: %s", settings_.root.c_str());
  ImGui::Spacing();

  const bool busy = installing();
  ImGui::Text("Imagem ISO do jogo");
  const float browse_w = browse_program_.empty() ? 0.0f : 110.0f;
  ImGui::SetNextItemWidth(-(browse_w + (browse_w > 0 ? ImGui::GetStyle().ItemSpacing.x : 0)));
  ImGui::BeginDisabled(busy);
  ImGui::InputTextWithHint("##iso", "caminho do arquivo .iso (ou arraste-o para esta janela)", iso_buf_.data(),
                           iso_buf_.size());
  if (!browse_program_.empty()) {
    ImGui::SameLine();
    ImGui::BeginDisabled(browse_running_);
    if (ImGui::Button("Procurar...", ImVec2(browse_w, 0))) browse();
    ImGui::EndDisabled();
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  const bool has_progress = std::any_of(stages_.begin(), stages_.end(), [](const StageRow& r) {
    return r.state == StageState::Done || r.state == StageState::Cached;
  });
  ImGui::BeginDisabled(busy || (iso_buf_[0] == 0 && !has_progress));
  if (ImGui::Button("Instalar", ImVec2(120, 0))) start_install();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!busy);
  if (ImGui::Button("Cancelar", ImVec2(120, 0)) && sink_) sink_->cancel();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(busy || !build_ready_ || game_running_);
  if (ImGui::Button("Jogar", ImVec2(120, 0))) play();
  ImGui::EndDisabled();
  if (!build_ready_ && !busy) {
    ImGui::SameLine();
    ImGui::TextDisabled("(instale o jogo primeiro)");
  }

  ImGui::Spacing();
  draw_stage_table();

  if (!status_.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", status_.c_str());
  }
  if (last_error_ && last_error_->code != ErrorCode::Cancelled) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.45f, 1));
    ImGui::TextWrapped("Erro: %s", last_error_->message.c_str());
    ImGui::PopStyleColor();
    if (!last_error_->hint.empty()) ImGui::TextWrapped("Dica: %s", last_error_->hint.c_str());
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Registro");
  ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  for (const auto& line : log_) ImGui::TextUnformatted(line.c_str());
  if (log_scroll_) {
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
    log_scroll_ = false;
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace lr::gui

// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/gui/app.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

#include "common/log.h"
#include "common/paths.h"
#include "imgui.h"
#include "launcher/gui/theme.h"
#include "launcher/gui/widgets.h"
#include "pipeline/pipeline.h"
#include "pipeline/process.h"
#include "pipeline/state.h"

namespace lr::gui {
namespace {

using theme::px;
using theme::rgb;

constexpr size_t kMaxLogLines = 2000;
constexpr pipeline::StageId kAllStages[] = {
    pipeline::StageId::Doctor,  pipeline::StageId::Extract,   pipeline::StageId::Verify, pipeline::StageId::Patch,
    pipeline::StageId::Analyse, pipeline::StageId::Recompile, pipeline::StageId::Build};

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

std::string home_relative(const std::filesystem::path& p) { return abbreviate_home(p, std::getenv("HOME")); }

// Vertical gap between the window's regions (bigger than ItemSpacing, which applies inside cards).
float region_gap() { return px(16); }

// Moves the cursor so the next item starts `gap` below the previous one (instead of ItemSpacing.y).
void vertical_gap(float gap) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + gap - ImGui::GetStyle().ItemSpacing.y); }

}  // namespace

App::App(Settings settings) : settings_(std::move(settings)) {
  for (auto id : kAllStages) stages_.emplace_back(id);
  std::snprintf(iso_buf_.data(), iso_buf_.size(), "%s", settings_.iso.c_str());
  if (pipeline::find_program("zenity")) browse_program_ = "zenity";
  else if (pipeline::find_program("kdialog")) browse_program_ = "kdialog";
  // Non-reentrant by contract: lr::log holds its mutex while calling the sink, so only push to the queue.
  log::set_sink([this](log::Level level, std::string_view msg) {
    std::string line = level == log::Level::Error ? "[ERROR] " : level == log::Level::Warn ? "[WARN] " : "";
    line += msg;
    log_queue_.on_log(line);
  });
  refresh_from_disk();
}

App::~App() {
  if (sink_) sink_->cancel();
  if (worker_.joinable()) worker_.join();
  if (game_thread_.joinable()) game_thread_.join();
  if (browse_thread_.joinable()) browse_thread_.join();
  log::set_sink({});
}

bool App::animating() const {
  if (installing() || ImGui::GetIO().WantTextInput) return true;
  return std::any_of(stages_.begin(), stages_.end(), [](const StageRow& r) {
    return r.state == StageState::Running && r.shown != r.fraction;
  });
}

void App::refresh_from_disk() {
  const DataPaths p = DataPaths::from_root(settings_.root);
  auto store = pipeline::StateStore::load(p.state_file);
  build_ready_ = false;
  if (!store) {
    add_log("[ERROR] state.toml ilegível: " + store.error().message);
    return;
  }
  for (auto& row : stages_) {
    if (row.state != StageState::Pending) continue;
    if (store.value().complete_fingerprint(row.id)) {
      // Completed by an earlier run (this session or a previous one).
      row.state = StageState::Done;
      row.fraction = 1.0f;
      row.cancelled = false;
      row.message.clear();
    }
  }
  build_ready_ = store.value().complete_fingerprint(pipeline::StageId::Build).has_value() &&
                 ::access((p.bin / "legacyrecomp-game").c_str(), X_OK) == 0;
}

void App::set_iso(const std::string& path) {
  std::snprintf(iso_buf_.data(), iso_buf_.size(), "%s", path.c_str());
  editing_path_ = false;  // a dropped/browsed file replaces a path being typed
}

void App::add_log(std::string line) {
  const LogTone tone = classify_log_line(line);
  log_.push_back({std::move(line), tone});
  while (log_.size() > kMaxLogLines) log_.pop_front();
  log_scroll_ = true;
}

void App::start_install() {
  if (installing()) return;
  if (worker_.joinable()) worker_.join();
  install_error_.reset();
  game_error_.reset();
  install_status_.clear();
  install_done_ = false;
  editing_path_ = false;
  for (auto& row : stages_) row = StageRow(row.id);
  sink_ = std::make_unique<QueueSink>();
  pipeline::Options opts;
  opts.iso = has_iso() ? std::filesystem::path(iso_buf_.data()) : std::filesystem::path();
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
      row.message.clear();
    } else if (p.message == "done" && p.fraction >= 1.0f) {
      row.state = StageState::Done;
      row.fraction = 1.0f;
      row.message.clear();
    } else {
      if (row.state != StageState::Running) {
        row.shown = 0.0f;
        scroll_to_stage_ = static_cast<int>(&row - stages_.data());
      }
      row.state = StageState::Running;
      row.fraction = p.fraction;
      row.message = p.message == "start" ? "Iniciando…" : p.message;
    }
  }
}

void App::handle_finished(const std::optional<Error>& error) {
  install_done_ = true;
  if (error) {
    const bool cancelled = error->code == ErrorCode::Cancelled;
    for (auto& row : stages_)
      if (row.state == StageState::Running) {
        if (!cancelled) scroll_to_stage_ = static_cast<int>(&row - stages_.data());
        row.state = cancelled ? StageState::Pending : StageState::Error;
        row.cancelled = cancelled;
        row.message.clear();  // the state label says "Cancelada" / "Falhou"
      }
    if (cancelled) {
      install_status_ = "Instalação cancelada; as etapas concluídas ficam em cache.";
    } else {
      install_error_ = error;
      install_status_.clear();
      add_log("[ERROR] " + error->message);
      if (!error->hint.empty()) add_log("Dica: " + error->hint);
      log_open_ = true;
    }
  } else {
    install_status_ = "Instalação concluída.";
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
        game_status_ = e.text;
        add_log(e.text);
      } else if (e.kind == QueueSink::Event::Finished) {
        game_running_ = false;
        if (e.error) {
          game_error_ = e.error;
          game_status_.clear();
          add_log("[ERROR] " + e.error->message);
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
  install_error_.reset();
  game_error_.reset();
  game_status_ = "Jogo em execução…";
  game_running_ = true;
  game_sink_ = std::make_unique<QueueSink>();
  QueueSink* sink = game_sink_.get();
  pipeline::Options opts;
  opts.root = settings_.root;
  game_thread_ = std::thread([sink, opts = std::move(opts)]() {
    auto r = pipeline::launch(opts, std::span<const std::string>{});
    if (r && r.value() == 3)
      sink->on_log("M1: o jogo parou na primeira importação de kernel não implementada (esperado neste marco).");
    else if (r)
      sink->on_log("O jogo terminou com código " + std::to_string(r.value()) + ".");
    sink->post_finished(r ? std::nullopt : std::optional<Error>(r.error()));
  });
}

void App::draw_header() {
  const auto& f = theme::fonts();
  const ImVec2 top = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const float mark = px(44);
  widgets::logo_mark(top, mark);

  ImGui::SetCursorScreenPos(ImVec2(top.x + mark + px(14), top.y - px(3)));
  ImGui::BeginGroup();
  widgets::text(f.title, rgb(theme::kText), "LegacyRecomp");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + px(1));
  widgets::text(f.small, rgb(theme::kMuted), "Minecraft: Xbox 360 Edition · nativo no Linux");
  ImGui::EndGroup();
  const float header_h = std::max(mark, ImGui::GetItemRectMax().y - top.y);

  const StatusPill pill =
      derive_status(installing(), (install_error_ && install_error_->code != ErrorCode::Cancelled) || game_error_,
                    build_ready_);
  const ImVec2 ps = widgets::status_pill_size(pill);
  widgets::status_pill(pill, ImVec2(top.x + width - ps.x, top.y + (header_h - ps.y) * 0.5f));
  ImGui::SetCursorScreenPos(top);
  ImGui::Dummy(ImVec2(width, header_h));
}

void App::draw_image_card(bool compact) {
  const auto& f = theme::fonts();
  const bool busy = installing();
  widgets::begin_card("image", ImVec2(0, 0), true);
  widgets::card_title("Imagem do jogo");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));

  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const ImVec2 zone(ImGui::GetContentRegionAvail().x, compact ? px(76) : px(132));

  if (editing_path_ && !busy) {
    // Typed path: an input field centred in the zone.
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + zone.x, pos.y + zone.y), rgb(theme::kBackground, 110), px(10));
    widgets::dashed_rect(dl, ImVec2(pos.x + 0.5f, pos.y + 0.5f), ImVec2(pos.x + zone.x - 0.5f, pos.y + zone.y - 0.5f),
                         rgb(theme::kAccent, 170), px(10), px(1.25f), px(6), px(4));
    const float input_h = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + px(20), pos.y + (zone.y - input_h) * 0.5f - px(10)));
    widgets::text(f.small, rgb(theme::kMuted), "Caminho completo do arquivo .iso");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + px(20), ImGui::GetCursorScreenPos().y - spacing + px(6)));
    ImGui::SetNextItemWidth(zone.x - px(40));
    if (focus_path_) {
      ImGui::SetKeyboardFocusHere();
      focus_path_ = false;
    }
    const bool enter = ImGui::InputTextWithHint("##iso", "/caminho/para/o/jogo.iso", iso_buf_.data(), iso_buf_.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ImGui::IsItemDeactivated()) editing_path_ = false;
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(zone);
  } else {
    std::string primary = "Arraste a ISO aqui ou clique em Procurar";
    std::string secondary = "Imagem .iso do disco original do jogo";
    ImU32 secondary_col = rgb(theme::kMuted);
    if (has_iso()) {
      const std::filesystem::path iso(iso_buf_.data());
      primary = iso.filename().string();
      secondary = home_relative(iso.parent_path());
      std::error_code ec;
      if (!std::filesystem::is_regular_file(iso, ec)) {
        secondary = "Arquivo não encontrado: " + secondary;
        secondary_col = rgb(theme::kWarning);
      }
    } else if (browse_program_.empty()) {
      primary = "Arraste a ISO aqui ou digite o caminho";
    }
    const bool clickable = !busy && !browse_program_.empty() && !browse_running_;
    if (widgets::drop_zone("##dropzone", zone, primary, secondary, has_iso(), clickable, secondary_col)) browse();
    if (has_iso()) widgets::tooltip_for_last_item(iso_buf_.data());
  }

  if (!browse_program_.empty()) {
    if (widgets::button("Procurar…", ImVec2(0, 0), widgets::ButtonKind::Secondary, !busy && !browse_running_)) browse();
    ImGui::SameLine();
  }
  if (widgets::button(editing_path_ ? "Concluir##path" : "Digitar caminho##path", ImVec2(0, 0),
                      widgets::ButtonKind::Ghost, !busy)) {
    editing_path_ = !editing_path_;
    focus_path_ = editing_path_;
  }

  const std::string root = home_relative(settings_.root);
  widgets::text(f.small, rgb(theme::kMuted),
                widgets::fit_text(f.small, "Pasta de dados: " + root, ImGui::GetContentRegionAvail().x, true));
  widgets::tooltip_for_last_item(settings_.root.c_str());
  widgets::end_card();
}

void App::draw_install_card(float height) {
  const auto& f = theme::fonts();
  const bool busy = installing();
  widgets::begin_card("install", ImVec2(0, height));

  const auto done = std::count_if(stages_.begin(), stages_.end(), [](const StageRow& r) {
    return r.state == StageState::Done || r.state == StageState::Cached;
  });
  const std::string note = std::to_string(done) + " de " + std::to_string(stages_.size()) + " etapas concluídas";
  widgets::card_title("Instalação", note.c_str());
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));

  const float button_h = widgets::button_size("Instalar", nullptr).y;
  const float footer_h = button_h + ImGui::GetStyle().ItemSpacing.y + px(6);
  ImGui::BeginChild("steps", ImVec2(0, std::max(px(60), ImGui::GetContentRegionAvail().y - footer_h)),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
  if (install_error_ && install_error_->code != ErrorCode::Cancelled) {
    widgets::error_alert("install_error", "A instalação falhou", install_error_->message, install_error_->hint);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
  }
  widgets::Stepper stepper;
  const float dt = ImGui::GetIO().DeltaTime;
  for (auto& row : stages_) {
    if (scroll_to_stage_ == static_cast<int>(&row - stages_.data())) {
      ImGui::SetScrollHereY(0.25f);
      scroll_to_stage_ = -1;
    }
    row.shown = row.state == StageState::Running ? approach_progress(row.shown, row.fraction, dt) : row.fraction;
    const StageText t = stage_text(row.id);
    stepper.row({t.title, t.description, row.state, row.shown, row.message, row.cancelled});
  }
  ImGui::EndChild();

  // Footer: install status on the left, the action on the right.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
  const float footer_y = ImGui::GetCursorPosY();
  const char* action = busy ? "Cancelar" : "Instalar";
  const float action_w = std::max(px(132), widgets::button_size(action, nullptr).x);
  if (!install_status_.empty()) {
    ImGui::SetCursorPosY(footer_y + (button_h - f.small->FontSize * ImGui::GetIO().FontGlobalScale) * 0.5f);
    const bool ok = install_status_ == "Instalação concluída.";
    widgets::text(f.small, ok ? rgb(theme::kAccent) : rgb(theme::kMuted),
                  widgets::fit_text(f.small, install_status_, ImGui::GetContentRegionAvail().x - action_w - px(12),
                                    false));
    ImGui::SameLine();
  }
  ImGui::SetCursorPosY(footer_y);
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - action_w);
  if (busy) {
    if (widgets::button("Cancelar", ImVec2(action_w, 0), widgets::ButtonKind::Secondary) && sink_) sink_->cancel();
  } else {
    const bool has_progress = std::any_of(stages_.begin(), stages_.end(), [](const StageRow& r) {
      return r.state == StageState::Done || r.state == StageState::Cached;
    });
    const bool can_install = has_iso() || has_progress;
    // Once the game is built, "Jogar" is the one primary action; reinstalling becomes secondary.
    const auto kind = build_ready_ ? widgets::ButtonKind::Secondary : widgets::ButtonKind::Primary;
    if (widgets::button("Instalar", ImVec2(action_w, 0), kind, can_install)) start_install();
    if (!can_install) widgets::tooltip_for_last_item("Escolha a imagem ISO do jogo primeiro.");
  }
  widgets::end_card();
}

void App::draw_play_card(float height) {
  const auto& f = theme::fonts();
  const bool busy = installing();
  widgets::begin_card("play", ImVec2(0, height));
  {
    // Subtle accent glow from the top-left corner.
    const ImVec2 a = ImGui::GetWindowPos();
    const ImVec2 b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
    const bool failed = (install_error_ && install_error_->code != ErrorCode::Cancelled) || game_error_;
    const uint32_t tint = failed ? theme::kError : build_ready_ ? theme::kAccent : theme::kInfo;
    widgets::gradient_rect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), px(11), tint,
                           build_ready_ && !failed ? 0.12f : 0.07f);
  }
  const bool has_error = (install_error_ && install_error_->code != ErrorCode::Cancelled) || game_error_;
  const char* heading = game_running_  ? "Jogo em execução"
                        : busy         ? "Instalação em andamento"
                        : has_error    ? "Algo deu errado"
                        : build_ready_ ? "Tudo pronto"
                                       : "Jogo ainda não instalado";
  // A short card (small window with the log open) keeps only the button and errors.
  const bool tight = height < px(150);
  if (!tight) {
    widgets::text(f.heading, rgb(theme::kText), heading);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
  }
  std::string caption = game_status_;
  if (caption.empty()) {
    if (busy)
      caption = "Acompanhe as etapas ao lado. Se cancelar, as etapas concluídas ficam em cache.";
    else if (build_ready_)
      caption = "O executável nativo está em " + home_relative(DataPaths::from_root(settings_.root).bin) + ".";
    else if (has_error)
      caption = "Veja os detalhes do erro e o registro abaixo.";
    else
      caption = "Escolha a imagem ISO do jogo e clique em Instalar para gerar o executável nativo.";
  }
  if (!tight) widgets::text_wrapped(f.small, rgb(theme::kMuted), caption);
  if (game_error_)
    widgets::error_alert("game_error", "Não foi possível iniciar o jogo", game_error_->message, game_error_->hint);

  // The call to action sits at the bottom of the card; a faint block watermark fills the space above it.
  const float cta_h = px(56);
  const float bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - cta_h;
  {
    const float free_h = bottom - ImGui::GetCursorPosY();
    const float size = std::min(ImGui::GetWindowHeight() * 1.05f, ImGui::GetWindowWidth() * 0.8f);
    if (!tight && free_h > px(60)) {
      // Cropped by the card's right edge (clip to the card rect, away from its rounded corners).
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 we(wp.x + ImGui::GetWindowWidth() - 1.0f, wp.y + ImGui::GetWindowHeight());
      const ImVec2 c(we.x - size * 0.14f, wp.y + ImGui::GetWindowHeight() * 0.5f - size * 0.05f);
      const uint32_t tint = build_ready_ ? theme::kAccent : theme::kMuted;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->PushClipRect(wp, we, false);
      widgets::iso_block(dl, c, size, rgb(tint, 16), rgb(tint, 9), rgb(tint, 5));
      dl->PopClipRect();
    }
  }
  if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
  const bool can_play = !busy && build_ready_ && !game_running_;
  if (widgets::button("Jogar##play", ImVec2(-1, cta_h), widgets::ButtonKind::Primary, can_play,
                      widgets::ButtonIcon::Play, f.heading))
    play();
  if (!can_play) {
    widgets::tooltip_for_last_item(game_running_ ? "O jogo já está em execução."
                                   : busy        ? "Aguarde a instalação terminar."
                                                 : "Instale o jogo primeiro: escolha a ISO e clique em Instalar.");
  }
  widgets::end_card();
}

void App::draw_log_panel(float height) {
  const auto& f = theme::fonts();
  widgets::begin_card("log", ImVec2(0, height), false, ImVec2(16, 8));
  const ImVec2 row = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 copy_size = widgets::button_size("Copiar", f.label);

  // Header row: the whole left part toggles the panel.
  ImGui::InvisibleButton("##toggle", ImVec2(width - copy_size.x - px(8), copy_size.y));
  if (ImGui::IsItemClicked()) log_open_ = !log_open_;
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = row.y + copy_size.y * 0.5f;
  const ImU32 head_col = rgb(hovered ? theme::kText : 0xC4C9D4);
  widgets::chevron(dl, ImVec2(row.x + px(8), cy), px(8), log_open_, head_col);
  const float fs = f.strong->FontSize * ImGui::GetIO().FontGlobalScale;
  dl->AddText(f.strong, fs, ImVec2(row.x + px(22), std::round(cy - fs * 0.5f)), head_col, "Registro");
  const float label_w = f.strong->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Registro").x;
  const std::string count = std::to_string(log_.size()) + (log_.size() == 1 ? " linha" : " linhas");
  const float ss = f.small->FontSize * ImGui::GetIO().FontGlobalScale;
  dl->AddText(f.small, ss, ImVec2(row.x + px(22) + label_w + px(10), std::round(cy - ss * 0.5f)),
              rgb(theme::kMuted), count.c_str());

  ImGui::SameLine(0.0f, px(8));
  if (widgets::button("Copiar", ImVec2(0, 0), widgets::ButtonKind::Ghost, !log_.empty(), widgets::ButtonIcon::None,
                      f.label)) {
    std::string all;
    for (const auto& l : log_) (all += l.text) += '\n';
    ImGui::SetClipboardText(all.c_str());
  }
  widgets::tooltip_for_last_item("Copiar o registro completo para a área de transferência");

  if (log_open_) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(2));
    const ImVec2 sep = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(sep.x - px(16), sep.y), ImVec2(sep.x + width + px(16), sep.y), rgb(theme::kBorder));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
    ImGui::PushFont(f.mono);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(3)));
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (log_.empty()) ImGui::TextDisabled("Nada registrado ainda.");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const LogLine& l = log_[static_cast<size_t>(i)];
        const ImU32 col = l.tone == LogTone::Error     ? rgb(theme::kError)
                          : l.tone == LogTone::Warning ? rgb(theme::kWarning)
                                                       : rgb(0xB4BAC8);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (log_scroll_) {
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(40)) ImGui::SetScrollHereY(1.0f);
      log_scroll_ = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }
  widgets::end_card();
}

void App::frame() {
  pump_events();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("LegacyRecomp", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);

  draw_header();
  vertical_gap(px(20));

  const float gap = region_gap();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float log_collapsed = widgets::button_size("Copiar", theme::fonts().label).y + px(8) * 2.0f + 2.0f;
  const float log_h = log_open_ ? std::max(px(150), std::floor(avail.y * 0.30f)) : log_collapsed;
  const float columns_h = std::max(px(200), avail.y - log_h - gap);
  const float left_w = std::floor((avail.x - gap) * 0.43f);

  // Left column: the ISO card (natural height) above the play card, which fills the rest.
  // Short windows (or an open log) get a compact drop zone; below that the column scrolls.
  const float play_min = px(94);  // just the button (see draw_play_card)
  ImGui::BeginChild("left", ImVec2(left_w, columns_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  draw_image_card(columns_h < px(266) + gap + px(190));
  const float image_h = ImGui::GetItemRectSize().y;
  vertical_gap(gap);
  draw_play_card(std::max(play_min, columns_h - image_h - gap));
  ImGui::EndChild();

  ImGui::SameLine(0.0f, gap);
  draw_install_card(columns_h);

  vertical_gap(gap);
  draw_log_panel(log_h);
  ImGui::End();
}

}  // namespace lr::gui

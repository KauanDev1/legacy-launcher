// SPDX-License-Identifier: MIT
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "launcher/gui/ui_model.h"

using namespace lr::gui;
using lr::pipeline::StageId;

TEST_CASE("abbreviate_home replaces a leading $HOME component with ~", "[unit]") {
  CHECK(abbreviate_home("/home/ana/.local/share/legacyrecomp", "/home/ana") == "~/.local/share/legacyrecomp");
  CHECK(abbreviate_home("/home/ana", "/home/ana") == "~");
  CHECK(abbreviate_home("/home/ana/x", "/home/ana/") == "~/x");
  CHECK(abbreviate_home("/home/anabel/x", "/home/ana") == "/home/anabel/x");  // not a component match
  CHECK(abbreviate_home("/srv/data", "/home/ana") == "/srv/data");
  CHECK(abbreviate_home("/home/ana/x", nullptr) == "/home/ana/x");
  CHECK(abbreviate_home("/home/ana/x", "") == "/home/ana/x");
  CHECK(abbreviate_home("/x", "/") == "/x");
}

TEST_CASE("stage_text gives every stage a Portuguese title and description", "[unit]") {
  CHECK(stage_text(StageId::Doctor).title == "Verificar o sistema");
  CHECK(stage_text(StageId::Patch).title == "Aplicar a atualização");
  CHECK(stage_text(StageId::Recompile).title == "Gerar o código C++");
  CHECK(stage_text(StageId::Build).title == "Compilar o jogo");
  for (auto id : {StageId::Doctor, StageId::Extract, StageId::Verify, StageId::Patch, StageId::Analyse,
                  StageId::Recompile, StageId::Build}) {
    CHECK_FALSE(stage_text(id).title.empty());
    CHECK_FALSE(stage_text(id).description.empty());
  }
}

TEST_CASE("derive_status: installing beats error beats ready", "[unit]") {
  CHECK(derive_status(false, false, false) == StatusPill::NotInstalled);
  CHECK(derive_status(false, false, true) == StatusPill::Ready);
  CHECK(derive_status(false, true, true) == StatusPill::Error);
  CHECK(derive_status(false, true, false) == StatusPill::Error);
  CHECK(derive_status(true, true, true) == StatusPill::Installing);
  CHECK(derive_status(true, false, false) == StatusPill::Installing);
  CHECK(status_label(StatusPill::Ready) == "Pronto para jogar");
  CHECK(status_label(StatusPill::Installing) == "Instalando…");
  CHECK(status_label(StatusPill::Error) == "Erro");
  CHECK(status_label(StatusPill::NotInstalled) == "Não instalado");
}

TEST_CASE("approach_progress eases forward, snaps when close or moving backwards", "[unit]") {
  using Catch::Approx;
  float v = approach_progress(0.0f, 1.0f, 0.016f);
  CHECK(v > 0.0f);
  CHECK(v < 1.0f);
  CHECK(approach_progress(v, 1.0f, 0.016f) > v);  // monotonic towards the target
  CHECK(approach_progress(0.0f, 1.0f, 10.0f) == Approx(1.0f));
  CHECK(approach_progress(0.998f, 1.0f, 0.0f) == 1.0f);  // within half a percent: snap
  CHECK(approach_progress(0.8f, 0.1f, 0.016f) == 0.1f);   // new run: snap back
  CHECK(approach_progress(0.5f, 0.5f, 0.016f) == 0.5f);
}

TEST_CASE("classify_log_line tints errors and warnings", "[unit]") {
  CHECK(classify_log_line("[ERROR] something") == LogTone::Error);
  CHECK(classify_log_line("foo.cpp:1:2: error: bad") == LogTone::Error);
  CHECK(classify_log_line("FAILED: out/a.o") == LogTone::Error);
  CHECK(classify_log_line("CMake Error at x") == LogTone::Error);
  CHECK(classify_log_line("[WARN] careful") == LogTone::Warning);
  CHECK(classify_log_line("warning: low disk") == LogTone::Warning);
  CHECK(classify_log_line("clang -Werror=foo -c x.cpp") == LogTone::Normal);
  CHECK(classify_log_line("[3/10] Building x") == LogTone::Normal);
}

TEST_CASE("ui_scale: no hints means 1, a framebuffer scale > 1 already covers HiDPI", "[unit]") {
  using Catch::Approx;
  CHECK(ui_scale(1.0f, {}) == Approx(1.0f));
  CHECK(ui_scale(2.0f, {}) == Approx(1.0f));
  CHECK(ui_scale(2.0f, {nullptr, "2", "2"}) == Approx(1.0f));  // Wayland/HiDPI: the framebuffer scales
  CHECK(ui_scale(0.0f, {}) == Approx(1.0f));
}

TEST_CASE("ui_scale uses GDK_SCALE / QT_SCALE_FACTOR as hints on a 1x framebuffer", "[unit]") {
  using Catch::Approx;
  CHECK(ui_scale(1.0f, {nullptr, "2", nullptr}) == Approx(2.0f));
  CHECK(ui_scale(1.0f, {nullptr, nullptr, "1.5"}) == Approx(1.5f));
  CHECK(ui_scale(1.0f, {nullptr, "2", "1.5"}) == Approx(2.0f));    // GDK_SCALE first
  CHECK(ui_scale(1.0f, {nullptr, "abc", "1.5"}) == Approx(1.5f));  // invalid GDK_SCALE skipped
  CHECK(ui_scale(1.0f, {nullptr, nullptr, "1.3"}) == Approx(1.25f));  // quarter steps
  CHECK(ui_scale(1.0f, {nullptr, nullptr, "0.5"}) == Approx(1.0f));   // never shrinks below 1
  CHECK(ui_scale(1.0f, {nullptr, "9", nullptr}) == Approx(1.0f));     // out of range
  CHECK(ui_scale(1.0f, {nullptr, "", ""}) == Approx(1.0f));
}

TEST_CASE("ui_scale honours a valid LEGACYRECOMP_UI_SCALE override over everything", "[unit]") {
  using Catch::Approx;
  CHECK(ui_scale(1.0f, {"1.25", nullptr, nullptr}) == Approx(1.25f));
  CHECK(ui_scale(2.0f, {"1.25", nullptr, nullptr}) == Approx(1.25f));
  CHECK(ui_scale(1.0f, {"0.75", "2", nullptr}) == Approx(0.75f));
  for (const char* bad : {"", "abc", "1.5x", "0", "9"}) {
    CAPTURE(bad);
    CHECK(ui_scale(1.0f, {bad, nullptr, nullptr}) == Approx(1.0f));
    CHECK(ui_scale(1.0f, {bad, "2", nullptr}) == Approx(2.0f));
  }
}

TEST_CASE("stage_message translates the pipeline's progress messages", "[unit]") {
  CHECK(stage_message(StageId::Doctor, "start") == "Iniciando…");
  CHECK(stage_message(StageId::Extract, "package: Data/a.arc") == "Extraindo o pacote: Data/a.arc");
  CHECK(stage_message(StageId::Extract, "title update: default.xexp") ==
        "Extraindo a atualização: default.xexp");
  CHECK(stage_message(StageId::Patch, "applying title update") == "Aplicando a atualização do título");
  CHECK(stage_message(StageId::Patch, "writing patched image") == "Gravando o executável atualizado");
  CHECK(stage_message(StageId::Analyse, "XenonAnalyse: scanning for jump tables") ==
        "XenonAnalyse: procurando tabelas de salto");
  CHECK(stage_message(StageId::Analyse, "scanning for jump tables (LegacyRecomp)") ==
        "Procurando tabelas de salto (LegacyRecomp)");
  CHECK(stage_message(StageId::Recompile, "Recompiling functions... 42%") == "Recompilando funções… 42%");
  CHECK(stage_message(StageId::Recompile, "[x] Recompiling functions... 7.5%") == "Recompilando funções… 7.5%");
  CHECK(stage_message(StageId::Build, "wrote 812 import stubs") == "812 stubs de importação gerados");
  CHECK(stage_message(StageId::Build, "configuring the game build") == "Configurando a compilação do jogo");
  CHECK(stage_message(StageId::Build, "compiling with 16 jobs") == "Compilando com 16 processos");
  CHECK(stage_message(StageId::Build, "[12/345]") == "Compilando [12/345]");
}

TEST_CASE("stage_message never passes unknown English through", "[unit]") {
  CHECK(stage_message(StageId::Verify, "hashing default.xex") == "Em andamento…");
  CHECK(stage_message(StageId::Build, "something new") == "Em andamento…");
  CHECK(stage_message(StageId::Build, "") == "Em andamento…");
  CHECK(stage_message(StageId::Build, "wrote x import stubs") == "Em andamento…");
}

// SPDX-License-Identifier: GPL-3.0-or-later
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

// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "pipeline/game_exit.h"

namespace fs = std::filesystem;
using lr::pipeline::describe_game_exit;
using lr::pipeline::ExitTextLanguage;

namespace {
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("game_trace_file is <root>/work/logs/game-trace.log", "[unit]") {
  CHECK(lr::pipeline::game_trace_file("/r") == fs::path("/r/work/logs/game-trace.log"));
  CHECK(lr::pipeline::gpu_trace_file("/r") == fs::path("/r/work/logs/gpu-trace.log"));
}

TEST_CASE("describe_game_exit explains every game exit code", "[unit]") {
  const fs::path trace = "/r/work/logs/game-trace.log";
  for (auto lang : {ExitTextLanguage::English, ExitTextLanguage::Portuguese}) {
    CHECK(describe_game_exit(0, trace, lang).empty());
    for (int code : {2, 3, 4, 5, 6, 7, 137}) {
      const std::string s = describe_game_exit(code, trace, lang);
      CHECK_FALSE(s.empty());
      CHECK_FALSE(has(s, "M1"));
      CHECK(has(s, std::to_string(code)));
    }
    // The import trace hint goes with the codes where the calls before the stop help.
    for (int code : {3, 4, 5, 6}) {
      const std::string s = describe_game_exit(code, trace, lang);
      CHECK(has(s, "legacyrecomp run -- --trace-imports"));
      CHECK(has(s, trace.string()));
    }
    CHECK_FALSE(has(describe_game_exit(2, trace, lang), "--trace-imports"));
  }
}

TEST_CASE("describe_game_exit wording per language", "[unit]") {
  const fs::path trace = "/t.log";
  CHECK(has(describe_game_exit(2, trace, ExitTextLanguage::English), "usage or load error"));
  CHECK(has(describe_game_exit(3, trace, ExitTextLanguage::English), "kernel import that is not implemented"));
  CHECK(has(describe_game_exit(4, trace, ExitTextLanguage::English), "execution fault"));
  CHECK(has(describe_game_exit(5, trace, ExitTextLanguage::English), "fatal guest error"));
  CHECK(has(describe_game_exit(6, trace, ExitTextLanguage::English), "deadlock"));
  CHECK(has(describe_game_exit(137, trace, ExitTextLanguage::English), "killed by signal 9"));
  CHECK(has(describe_game_exit(7, trace, ExitTextLanguage::English), "game exited with code 7"));

  CHECK(has(describe_game_exit(2, trace, ExitTextLanguage::Portuguese), "erro de uso ou de carga"));
  CHECK(has(describe_game_exit(3, trace, ExitTextLanguage::Portuguese), "importação do kernel ainda não implementada"));
  CHECK(has(describe_game_exit(4, trace, ExitTextLanguage::Portuguese), "falha de execução"));
  CHECK(has(describe_game_exit(5, trace, ExitTextLanguage::Portuguese), "erro fatal"));
  CHECK(has(describe_game_exit(6, trace, ExitTextLanguage::Portuguese), "deadlock"));
  CHECK(has(describe_game_exit(137, trace, ExitTextLanguage::Portuguese), "sinal 9"));
  CHECK(has(describe_game_exit(7, trace, ExitTextLanguage::Portuguese), "O jogo terminou com código 7"));
}

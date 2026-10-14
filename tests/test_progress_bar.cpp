// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "launcher/cli/progress_bar.h"

using namespace lr::cli;
using lr::pipeline::Progress;
using lr::pipeline::StageId;

TEST_CASE("non-tty bar prints one line per 10% step", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  bar.on_progress({StageId::Extract, 0.50f, "x"});
  bar.on_progress({StageId::Extract, 0.55f, "x"});
  const std::string s = os.str();
  CHECK(s.find("[extract] 50%") != std::string::npos);
  CHECK(s.find("[extract] 50%") == s.rfind("[extract] 50%"));
  CHECK(s.find("55%") == std::string::npos);
}

TEST_CASE("non-tty bar prints again when the stage changes", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  bar.on_progress({StageId::Extract, 1.0f, "done"});
  bar.on_progress({StageId::Verify, 1.0f, "cached"});
  CHECK(os.str().find("[verify] 100% cached") != std::string::npos);
}

TEST_CASE("tty bar redraws one line with a 40 column bar", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, true);
  bar.on_progress({StageId::Build, 0.45f, "compiling"});
  const std::string s = os.str();
  CHECK(s.front() == '\r');
  CHECK(s.find("[build] [##################......................] 45% compiling") != std::string::npos);
  CHECK(s.find('\n') == std::string::npos);
  bar.on_progress({StageId::Build, 1.0f, "done"});
  bar.on_log("hello");  // clears the line, prints the log, redraws
  CHECK(os.str().find("hello\n") != std::string::npos);
}

TEST_CASE("cancelled() reflects the SIGINT flag", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  ProgressBar::reset_cancel();
  CHECK_FALSE(bar.cancelled());
  ProgressBar::request_cancel();
  CHECK(bar.cancelled());
  ProgressBar::reset_cancel();
}

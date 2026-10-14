// SPDX-License-Identifier: MIT
#include "common/log.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace lr::log {
namespace {

std::mutex g_mutex;
Sink g_sink;

const char* label(Level level) {
  switch (level) {
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}

void emit(Level level, std::string_view msg) {
  std::lock_guard lock(g_mutex);
  if (g_sink) {
    g_sink(level, msg);
    return;
  }
  std::string line = "[";
  line += label(level);
  line += "] ";
  line += msg;
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace

void set_sink(Sink sink) {
  std::lock_guard lock(g_mutex);
  g_sink = std::move(sink);
}

void info(std::string_view msg) { emit(Level::Info, msg); }
void warn(std::string_view msg) { emit(Level::Warn, msg); }
void error(std::string_view msg) { emit(Level::Error, msg); }

}  // namespace lr::log

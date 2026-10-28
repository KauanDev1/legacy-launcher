// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string_view>

namespace lr::log {

enum class Level { Info, Warn, Error };

using Sink = std::function<void(Level, std::string_view)>;

// Replaces the sink and returns the previous one; an empty function restores the default (stderr "[LEVEL] msg").
Sink set_sink(Sink sink);

void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);

}  // namespace lr::log

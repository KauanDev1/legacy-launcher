// SPDX-License-Identifier: MIT
// XAM UI that the game really reaches (M3): the message box is reported on the terminal and answered with the first
// button; a relaunch request ends the process. Argument orders follow Xenia's xam_ui.cc (semantic reference only,
// no code copied).
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "runtime/kernel/guest_printf.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/xam.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
constexpr uint32_t kMaxButtons = 8;
constexpr uint32_t kMaxChars = 1024;

std::string to_utf8(std::u16string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string wide_at(const GuestCall& c, uint32_t addr) {
  return addr == 0 ? std::string() : to_utf8(read_guest_wstring(c.base, addr, kMaxChars));
}

}  // namespace

namespace exports {

// XamShowMessageBoxUI(user, title*, text*, button_count, buttons*, focus, flags, result*, overlapped*): there is no
// dialog; the box is printed and the first button (0) is "pressed". With an overlapped the call completes it and
// returns ERROR_IO_PENDING, as on the console; without one it returns ERROR_SUCCESS.
uint32_t XamShowMessageBoxUI(GuestCall& c, uint32_t /*user*/, uint32_t title, uint32_t text, uint32_t button_count,
                             U32Ptr buttons, uint32_t /*focus*/, uint32_t /*flags*/, U32Ptr result_out,
                             uint32_t overlapped) {
  std::string list;
  const uint32_t n = button_count < kMaxButtons ? button_count : kMaxButtons;
  if (buttons && buttons.valid_range(n * 4)) {
    for (uint32_t i = 0; i < n; ++i) {
      if (i != 0) list += ", ";
      list += wide_at(c, buttons.get(c.base)[i]);
    }
  }
  std::fprintf(stderr, "[game] message box: %s \xE2\x80\x94 %s [%s]\n", wide_at(c, title).c_str(),
               wide_at(c, text).c_str(), list.c_str());
  if (result_out.valid_range(4)) *result_out.get(c.base) = 0;
  if (overlapped == 0) return ERROR_SUCCESS;
  complete_overlapped(overlapped, ERROR_SUCCESS, 0);
  return ERROR_IO_PENDING;
}

// XamLoaderLaunchTitle(image*, flags): relaunching another title (or the dashboard) is not possible here.
void XamLoaderLaunchTitle(uint32_t /*image*/, uint32_t /*flags*/) {
  guest_fatal("the game asked to relaunch itself (XamLoaderLaunchTitle)");
}

}  // namespace exports

LR_KERNEL(XamShowMessageBoxUI);
LR_KERNEL(XamLoaderLaunchTitle);

}  // namespace lr::kernel

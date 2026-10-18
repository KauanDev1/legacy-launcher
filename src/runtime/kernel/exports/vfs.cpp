// SPDX-License-Identifier: MIT
// ObCreateSymbolicLink / ObDeleteSymbolicLink over the VFS (runtime/kernel/vfs.h). Argument order follows Xenia's
// xboxkrnl_ob.cc (semantic reference only): both take pointers to X_ANSI_STRING.
#include <cstdint>
#include <string>

#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace {

// The text of an X_ANSI_STRING in guest memory; false when the descriptor or its buffer is outside guest memory.
bool read_ansi(GuestCall& c, GuestPtr<X_ANSI_STRING> p, std::string& out) {
  if (!p.valid_range(sizeof(X_ANSI_STRING))) return false;
  const X_ANSI_STRING* s = p.get(c.base);
  const uint32_t len = s->length, buf = s->buffer;
  if (len == 0) {
    out.clear();
    return true;
  }
  if (!GuestPtr<char>{buf}.valid_range(len)) return false;
  out.assign(reinterpret_cast<const char*>(c.base + buf), len);
  return true;
}

}  // namespace

namespace exports {

uint32_t ObCreateSymbolicLink(GuestCall& c, GuestPtr<X_ANSI_STRING> name, GuestPtr<X_ANSI_STRING> target) {
  std::string n, t;
  if (!read_ansi(c, name, n) || !read_ansi(c, target, t)) return STATUS_INVALID_PARAMETER;
  return kernel().vfs().create_symlink(n, t);
}

uint32_t ObDeleteSymbolicLink(GuestCall& c, GuestPtr<X_ANSI_STRING> name) {
  std::string n;
  if (!read_ansi(c, name, n)) return STATUS_INVALID_PARAMETER;
  return kernel().vfs().delete_symlink(n);
}

}  // namespace exports

LR_KERNEL(ObCreateSymbolicLink);
LR_KERNEL(ObDeleteSymbolicLink);

}  // namespace lr::kernel

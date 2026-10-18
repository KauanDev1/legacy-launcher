// SPDX-License-Identifier: MIT
// ObCreateSymbolicLink / ObDeleteSymbolicLink over the VFS (runtime/kernel/vfs.h). Argument order follows Xenia's
// xboxkrnl_ob.cc (semantic reference only): both take pointers to X_ANSI_STRING.
#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/kernel/files.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace exports {

uint32_t ObCreateSymbolicLink(GuestCall& c, GuestPtr<X_ANSI_STRING> name, GuestPtr<X_ANSI_STRING> target) {
  std::string n, t;
  if (!read_ansi_string(c.base, name, n) || !read_ansi_string(c.base, target, t)) return STATUS_INVALID_PARAMETER;
  const uint32_t st = kernel().vfs().create_symlink(n, t);
  if (st == STATUS_OBJECT_NAME_COLLISION) {  // Xenia keeps the existing link and reports success
    std::fprintf(stderr, "[kernel] symlink %s already exists, kept\n", n.c_str());
    return STATUS_SUCCESS;
  }
  return st;
}

uint32_t ObDeleteSymbolicLink(GuestCall& c, GuestPtr<X_ANSI_STRING> name) {
  std::string n;
  if (!read_ansi_string(c.base, name, n)) return STATUS_INVALID_PARAMETER;
  return kernel().vfs().delete_symlink(n);
}

}  // namespace exports

LR_KERNEL(ObCreateSymbolicLink);
LR_KERNEL(ObDeleteSymbolicLink);

}  // namespace lr::kernel

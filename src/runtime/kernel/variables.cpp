// SPDX-License-Identifier: MIT
#include "runtime/kernel/variables.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/be.h"
#include "runtime/fault.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/xex_imports.h"

namespace lr::kernel {

namespace {

struct VariableDef {
  uint32_t size;
  VariableInit init;
  ImportState state;
};

// Function-local statics, intentionally leaked: registrations run from static initializers in any order (same
// pattern as the import registry in registry.cpp).
std::mutex& vars_mutex() {
  static auto& m = *new std::mutex;
  return m;
}
std::map<std::string, VariableDef, std::less<>>& vars() {
  static auto& t = *new std::map<std::string, VariableDef, std::less<>>;
  return t;
}

void put_be16(uint8_t* p, uint16_t v) { *reinterpret_cast<runtime::be<uint16_t>*>(p) = v; }
void put_be32(uint8_t* p, uint32_t v) { *reinterpret_cast<runtime::be<uint32_t>*>(p) = v; }

std::string hex16(uint16_t v) {
  char b[8];
  std::snprintf(b, sizeof b, "0x%04X", static_cast<unsigned>(v));
  return b;
}

struct Registrar {
  Registrar(const char* name, uint32_t size, VariableInit init, ImportState state = ImportState::Implemented) {
    register_variable(name, size, init, state);
  }
};

// --- Kernel variables. Contents follow Xenia's xboxkrnl_module.cc (XboxkrnlModule constructor) unless noted. ---

// The executable's module handle: a pointer cell holding its X_LDR_DATA_TABLE_ENTRY address (Xenia writes the
// executable's hmodule_ptr into this 4-byte variable when the executable is set).
const Registrar r_module_handle("XexExecutableModuleHandle", 4,
                                [](uint8_t* p, uint32_t) { put_be32(p, kernel().executable_module()); });

// Command line: zeroed 1 KiB buffer holding "\"default.xex\"" (quotes included, as Xenia).
const Registrar r_command_line("ExLoadedCommandLine", 1024, [](uint8_t* p, uint32_t) {
  static constexpr char kLine[] = "\"default.xex\"";
  std::memcpy(p, kLine, sizeof kLine);
});

// Image name: a zeroed buffer (Xenia allocates kExLoadedImageNameSize bytes and writes nothing).
const Registrar r_image_name("ExLoadedImageName", 256, nullptr);

// KeTimeStampBundle (X_TIME_STAMP_BUNDLE): interrupt time (u64), system time (u64), tick count in ms (u32),
// padding. Filled when allocated; resolve_variable_imports starts the 1 ms updates on the kernel worker only when
// the game imports it (Timers::enable_time_stamp_bundle).
const Registrar r_time_stamp("KeTimeStampBundle", sizeof(X_TIME_STAMP_BUNDLE),
                             [](uint8_t* p, uint32_t) { kernel().timers().fill_time_stamp_bundle(p); });

// XboxHardwareInfo: u32 flags = 0x20, u8 CPU count = 6, rest zero.
const Registrar r_hardware_info("XboxHardwareInfo", 16, [](uint8_t* p, uint32_t) {
  put_be32(p, 0x20);
  p[4] = 6;
});

// XboxKrnlVersion: u16 2, u16 0xFFFF, u16 0xFFFF, u8 0x80, u8 0x00.
const Registrar r_krnl_version("XboxKrnlVersion", 8, [](uint8_t* p, uint32_t) {
  put_be16(p, 2);
  put_be16(p + 2, 0xFFFF);
  put_be16(p + 4, 0xFFFF);
  p[6] = 0x80;
  p[7] = 0x00;
});

// KeDebugMonitorData: a null pointer (Xenia's default, debug monitor off).
const Registrar r_debug_monitor("KeDebugMonitorData", 4, nullptr);

// Object type descriptors (X_OBJECT_TYPE). Only their addresses matter for now: ObReferenceObjectByHandle (Task 15)
// compares the type pointer it is given against them. The pool tags are our choice (informational, ASCII big-endian).
template <uint32_t Tag>
void object_type(uint8_t* p, uint32_t) {
  reinterpret_cast<X_OBJECT_TYPE*>(p)->pool_tag = Tag;
}
const Registrar r_thread_type("ExThreadObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x54687265>);     // 'Thre'
const Registrar r_event_type("ExEventObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x4576656E>);       // 'Even'
const Registrar r_semaphore_type("ExSemaphoreObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x53656D61>);  // 'Sema'
const Registrar r_mutant_type("ExMutantObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x4D757461>);     // 'Muta'
const Registrar r_timer_type("ExTimerObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x54696D65>);       // 'Time'
const Registrar r_file_type("IoFileObjectType", sizeof(X_OBJECT_TYPE), &object_type<0x46696C65>);         // 'File'

// Allocates and fills every registered variable once per kernel.
Result<void> allocate_variables(runtime::GuestMemory& mem) {
  Kernel& k = kernel();
  auto& blocks = k.variable_blocks();
  if (!blocks.empty()) return {};
  std::lock_guard<std::mutex> lock(vars_mutex());
  for (const auto& [name, def] : vars()) {
    const uint32_t a = k.kernel_data_alloc(def.size, 16);
    if (a == 0) return Error{ErrorCode::InsufficientResources, "kernel data area exhausted allocating " + name, ""};
    if (def.init) def.init(mem.base() + a, a);
    blocks.emplace(name, a);
  }
  return {};
}

}  // namespace

void register_variable(const char* name, uint32_t size, VariableInit init, ImportState state) {
  if (state == ImportState::Missing) {
    std::fprintf(stderr, "fatal: kernel variable %s registered as missing\n", name);
    std::abort();
  }
  std::lock_guard<std::mutex> lock(vars_mutex());
  if (!vars().emplace(name, VariableDef{size, init, state}).second) {
    std::fprintf(stderr, "fatal: kernel variable %s registered twice\n", name);
    std::abort();
  }
}

ImportState variable_state(std::string_view name) {
  std::lock_guard<std::mutex> lock(vars_mutex());
  const auto it = vars().find(name);
  return it == vars().end() ? ImportState::Missing : it->second.state;
}

bool is_variable_registered(std::string_view name) {
  std::lock_guard<std::mutex> lock(vars_mutex());
  return vars().find(name) != vars().end();
}

uint32_t variable_address(std::string_view name) {
  auto& blocks = kernel().variable_blocks();
  const auto it = blocks.find(name);
  return it == blocks.end() ? 0 : it->second;
}

Result<void> resolve_variable_imports(runtime::GuestMemory& mem, const runtime::LoadedImage& image,
                                      const formats::Xex2File& xex) {
  Kernel& k = kernel();
  k.set_executable_module(image.ldr_entry, image.header_guest);
  LR_TRY(allocate_variables(mem));

  const uint64_t lo = image.base, hi = uint64_t{image.base} + image.size;
  const runtime::ImageReader read = [&](uint32_t addr, void* out, size_t n) {
    if (addr < lo || addr + n > hi || addr + n > mem.size()) return false;
    std::memcpy(out, mem.base() + addr, n);
    return true;
  };
  LR_TRY_ASSIGN(records, runtime::classify_imports(xex, read));

  runtime::clear_fault_page_notes();
  std::map<std::string, uint32_t> guards;  // one guard page per missing variable
  uint32_t time_stamp_bundle = 0;          // imported KeTimeStampBundle block
  for (const runtime::ImportRecord& r : records) {
    uint32_t target = r.thunk;
    const std::string label = r.name.empty() ? r.library + "@" + hex16(r.ordinal) : r.name;
    if (target == 0 && !r.name.empty()) {
      const auto it = k.variable_blocks().find(r.name);
      if (it != k.variable_blocks().end()) {
        target = it->second;
        if (r.name == "KeTimeStampBundle") time_stamp_bundle = target;
      }
    }
    if (target == 0) {
      auto& page = guards[label];
      if (page == 0) {
        page = k.kernel_data_alloc(0x1000, 0x1000);
        if (page == 0) return Error{ErrorCode::InsufficientResources, "kernel data area exhausted (guard pages)", ""};
        LR_TRY(mem.protect(page, 0x1000, PROT_NONE));
        (void)runtime::note_fault_page(page, "missing kernel variable " + label);
      }
      target = page;
    }
    if (r.thunk != 0) k.import_thunks()[{r.library, r.ordinal}] = r.thunk;
    *mem.translate<runtime::be<uint32_t>>(r.slot) = target;
  }
  // Only a title that reads the bundle pays for the 1 ms update loop.
  if (time_stamp_bundle != 0) k.timers().enable_time_stamp_bundle(time_stamp_bundle);
  return {};
}

}  // namespace lr::kernel

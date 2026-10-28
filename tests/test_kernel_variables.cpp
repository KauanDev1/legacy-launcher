// SPDX-License-Identifier: MIT
// Kernel variable imports (runtime/kernel/variables.h). The images are whole synthetic XEX2 files
// (support/xex2_builder.h) loaded through load_image, so the record slots in guest memory are exactly what
// XenonUtils' Image::ParseImage leaves there: type-0 slots byte-swapped to host order, type-1 slots replaced by the
// nop/nop/nop/blr thunk.
#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "formats/xex2.h"
#include "runtime/be.h"
#include "runtime/fault.h"
#include "runtime/kernel/variables.h"
#include "runtime/loader.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"
#include "support/xex2_builder.h"

using namespace lr;
using namespace lr::runtime;
using lr::testing::KernelFixture;
using lr::testing::Xex2Builder;

namespace {

// xboxkrnl.exe ordinals (XenonUtils xbox/xboxkrnl_table.inc).
constexpr uint32_t kDbgPrint = 0x003;                   // function
constexpr uint32_t kExEventObjectType = 0x00E;          // variable
constexpr uint32_t kXboxHardwareInfo = 0x156;           // variable
constexpr uint32_t kXexExecutableModuleHandle = 0x193;  // variable
constexpr uint32_t kXboxKrnlVersion4Digit = 0x345;      // variable we never register

// Slots: .data (0x82002000) holds the type-0 records, .text (0x82001000) the type-1 thunk records.
constexpr uint32_t kSlotFunctionVar = 0x82002000;  // DbgPrint's type-0 record
constexpr uint32_t kSlotThunk = 0x82001010;        // DbgPrint's type-1 record (the thunk)
constexpr uint32_t kSlotModuleHandle = 0x82002004;
constexpr uint32_t kSlotEventType = 0x82002008;
constexpr uint32_t kSlotHardwareInfo = 0x8200200C;
constexpr uint32_t kSlotMissing = 0x82002010;

struct Loaded {
  LoadedImage img;
  formats::Xex2File xex;
};

Loaded load(KernelFixture& f) {
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  lr::testing::put_be32(text, kSlotThunk - 0x82001000, 0x01000000u | kDbgPrint);
  lr::testing::put_be32(data, kSlotFunctionVar - 0x82002000, kDbgPrint);
  lr::testing::put_be32(data, kSlotModuleHandle - 0x82002000, kXexExecutableModuleHandle);
  lr::testing::put_be32(data, kSlotEventType - 0x82002000, kExEventObjectType);
  lr::testing::put_be32(data, kSlotHardwareInfo - 0x82002000, kXboxHardwareInfo);
  lr::testing::put_be32(data, kSlotMissing - 0x82002000, kXboxKrnlVersion4Digit);
  const auto bytes = Xex2Builder()
                         .entry_point(0x82001000)
                         .image_base(0x82000000)
                         .import_library("xboxkrnl.exe", {kSlotFunctionVar, kSlotThunk, kSlotModuleHandle,
                                                          kSlotEventType, kSlotHardwareInfo, kSlotMissing})
                         .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                         .build();
  auto img = load_image(f.mem, bytes);
  REQUIRE(img.ok());
  auto xex = formats::parse_xex2(bytes);
  REQUIRE(xex.ok());
  auto r = kernel::resolve_variable_imports(f.mem, img.value(), xex.value());
  REQUIRE(r.ok());
  return {img.value(), xex.value()};
}

uint32_t be32_at(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }

}  // namespace

TEST_CASE("variable import gets its block address", "[unit]") {
  KernelFixture f;
  const Loaded l = load(f);

  const uint32_t handle = kernel::variable_address("XexExecutableModuleHandle");
  REQUIRE(handle >= kKernelDataBase);
  REQUIRE(handle < kKernelDataEnd);
  CHECK(be32_at(f, kSlotModuleHandle) == handle);
  CHECK(be32_at(f, handle) == l.img.ldr_entry);  // the variable holds the module handle (its LDR entry)

  const uint32_t event_type = kernel::variable_address("ExEventObjectType");
  REQUIRE(event_type != 0);
  CHECK(be32_at(f, kSlotEventType) == event_type);
  CHECK(event_type != kernel::variable_address("ExThreadObjectType"));

  const uint32_t hw = kernel::variable_address("XboxHardwareInfo");
  CHECK(be32_at(f, kSlotHardwareInfo) == hw);
  CHECK(be32_at(f, hw) == 0x20u);                    // flags (Xenia)
  CHECK(*f.mem.translate<uint8_t>(hw + 4) == 6u);  // CPU count

  for (const char* n : {"ExLoadedCommandLine", "ExLoadedImageName", "KeTimeStampBundle", "XboxKrnlVersion",
                        "KeDebugMonitorData", "ExSemaphoreObjectType", "ExMutantObjectType", "ExTimerObjectType",
                        "IoFileObjectType"}) {
    INFO(n);
    CHECK(kernel::variable_address(n) != 0);
  }
  CHECK(std::string(f.mem.translate<char>(kernel::variable_address("ExLoadedCommandLine"))) == "\"default.xex\"");
  CHECK(kernel::variable_address("NotAKernelVariable") == 0);
}

TEST_CASE("type-0 record of a function gets the thunk address", "[unit]") {
  KernelFixture f;
  load(f);
  CHECK(be32_at(f, kSlotFunctionVar) == kSlotThunk);
  // The thunk itself is what ParseImage wrote: nop, nop, nop, blr.
  CHECK(be32_at(f, kSlotThunk) == 0x60000000u);
  CHECK(be32_at(f, kSlotThunk + 12) == 0x4E800020u);
}

TEST_CASE("unknown variable points to a guard page and fault names it", "[unit]") {
  KernelFixture f;
  load(f);
  const uint32_t page = be32_at(f, kSlotMissing);
  REQUIRE(page >= kKernelDataBase);
  REQUIRE(page < kKernelDataEnd);
  CHECK(page % 0x1000 == 0);

  char buf[256];
  format_fault(buf, sizeof buf, SIGSEGV, f.mem.base(), f.mem.size(), f.mem.base() + page + 8);
  CHECK(std::string(buf).find("missing kernel variable XboxKrnlVersion4Digit") != std::string::npos);

  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    if (!install_fault_handlers(f.mem).ok()) ::_exit(99);
    (void)*f.mem.translate<volatile uint32_t>(page);
    ::_exit(0);  // not reached: the guard page is PROT_NONE
  }
  ::close(fds[1]);
  std::string out;
  char rb[256];
  for (ssize_t n; (n = ::read(fds[0], rb, sizeof rb)) > 0;) out.append(rb, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 4);
  CHECK(out.find("missing kernel variable XboxKrnlVersion4Digit") != std::string::npos);
}

TEST_CASE("kernel variables are registered (whole-archive link of variables.cpp)", "[unit]") {
  for (const char* n : {"XexExecutableModuleHandle", "ExLoadedCommandLine", "ExLoadedImageName", "KeTimeStampBundle",
                        "XboxHardwareInfo", "XboxKrnlVersion", "KeDebugMonitorData", "ExThreadObjectType",
                        "ExEventObjectType", "ExSemaphoreObjectType", "ExMutantObjectType", "ExTimerObjectType",
                        "IoFileObjectType"}) {
    INFO(n);
    CHECK(kernel::is_variable_registered(n));
  }
  CHECK_FALSE(kernel::is_variable_registered("XboxKrnlVersion4Digit"));
  CHECK(kernel::variable_state("XexExecutableModuleHandle") == kernel::ImportState::Implemented);
  CHECK(kernel::variable_state("KeTimeStampBundle") == kernel::ImportState::Implemented);
  CHECK(kernel::variable_state("XboxKrnlVersion4Digit") == kernel::ImportState::Missing);
}

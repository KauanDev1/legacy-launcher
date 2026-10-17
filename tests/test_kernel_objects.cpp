// SPDX-License-Identifier: MIT
// Object table and handle imports (runtime/kernel/objects.h, exports/objects.cpp).
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "runtime/be.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/status.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {
uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }
}  // namespace

TEST_CASE("handles start at 0xF8000000 and step by 4", "[unit][kernel][objects]") {
  KernelFixture f;
  auto& t = kernel().objects();
  const uint32_t a = t.add(std::make_shared<KObject>(ObjectKind::Other));
  const uint32_t b = t.add(std::make_shared<KObject>(ObjectKind::Other));
  CHECK(a == 0xF8000000);
  CHECK(b == 0xF8000004);
  CHECK(t.close(a) == STATUS_SUCCESS);
  CHECK(t.add(std::make_shared<KObject>(ObjectKind::Other)) == a);  // lowest free slot is reused
}

TEST_CASE("double close", "[unit][kernel][objects]") {
  KernelFixture f;
  const uint32_t h = kernel().objects().add(std::make_shared<KObject>(ObjectKind::Other));
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
  CHECK(static_cast<uint32_t>(f.call("NtClose", h)) == STATUS_INVALID_HANDLE);
  CHECK(kernel().objects().get(h) == nullptr);
  CHECK(kernel().objects().get(0x12345678) == nullptr);
  CHECK(kernel().objects().get(kCurrentThread) == nullptr);
}

TEST_CASE("duplicate keeps object alive after original close", "[unit][kernel][objects]") {
  KernelFixture f;
  auto obj = std::make_shared<KObject>(ObjectKind::Other);
  std::weak_ptr<KObject> weak = obj;
  const uint32_t h = kernel().objects().add(std::move(obj));
  const uint32_t out = f.alloc(4);
  CHECK(f.call("NtDuplicateObject", h, out, 0) == STATUS_SUCCESS);
  const uint32_t h2 = rd(f, out);
  CHECK(h2 != h);
  CHECK(kernel().objects().close(h) == STATUS_SUCCESS);
  CHECK_FALSE(weak.expired());
  CHECK(kernel().objects().get(h2) != nullptr);
  CHECK(kernel().objects().close(h2) == STATUS_SUCCESS);
  CHECK(weak.expired());
}

TEST_CASE("NtDuplicateObject DUPLICATE_CLOSE_SOURCE closes the source", "[unit][kernel][objects]") {
  KernelFixture f;
  const uint32_t h = kernel().objects().add(std::make_shared<KObject>(ObjectKind::Other));
  const uint32_t out = f.alloc(4);
  CHECK(f.call("NtDuplicateObject", h, out, 1) == STATUS_SUCCESS);
  CHECK(kernel().objects().get(h) == nullptr);
  CHECK(kernel().objects().get(rd(f, out)) != nullptr);
  CHECK(static_cast<uint32_t>(f.call("NtDuplicateObject", h, out, 0)) == STATUS_INVALID_HANDLE);
}

TEST_CASE("ObReferenceObjectByHandle checks the object type", "[unit][kernel][objects]") {
  KernelFixture f;
  auto& k = kernel();
  const uint32_t event_type = f.alloc(16), sema_type = f.alloc(16);
  k.variable_blocks()["ExEventObjectType"] = event_type;
  k.variable_blocks()["ExSemaphoreObjectType"] = sema_type;
  auto ev = std::make_shared<KObject>(ObjectKind::Event);
  ev->set_guest_header(0x20001000);
  const uint32_t h = k.objects().add(ev);
  const uint32_t out = f.alloc(4);

  CHECK(static_cast<uint32_t>(f.call("ObReferenceObjectByHandle", h, sema_type, out)) == STATUS_OBJECT_TYPE_MISMATCH);
  CHECK(f.call("ObReferenceObjectByHandle", h, event_type, out) == STATUS_SUCCESS);
  CHECK(rd(f, out) == 0x20001000);
  CHECK(f.call("ObReferenceObjectByHandle", h, 0, out) == STATUS_SUCCESS);  // 0 = any type
  CHECK(static_cast<uint32_t>(f.call("ObReferenceObjectByHandle", 0xF8000100, 0, out)) == STATUS_INVALID_HANDLE);

  // Two kernel references keep the object alive after the handle closes; two dereferences release them.
  std::weak_ptr<KObject> weak = ev;
  ev.reset();
  CHECK(k.objects().close(h) == STATUS_SUCCESS);
  CHECK_FALSE(weak.expired());
  f.call("ObDereferenceObject", 0x20001000);
  CHECK_FALSE(weak.expired());
  f.call("ObDereferenceObject", 0x20001000);
  CHECK(weak.expired());
  f.call("ObDereferenceObject", 0x20001000);  // unknown now: ignored
}

TEST_CASE("by_guest_header and ObOpenObjectByPointer", "[unit][kernel][objects]") {
  KernelFixture f;
  auto& t = kernel().objects();
  auto o = std::make_shared<KObject>(ObjectKind::Mutant);
  o->set_guest_header(0x20002000);
  CHECK(t.by_guest_header(0x20002000) == o);
  const uint32_t out = f.alloc(4);
  CHECK(f.call("ObOpenObjectByPointer", 0x20002000, out) == STATUS_SUCCESS);
  CHECK(t.get(rd(f, out)) == o);
  CHECK(static_cast<uint32_t>(f.call("ObOpenObjectByPointer", 0x20009000, out)) == STATUS_INVALID_HANDLE);
  o->set_guest_header(0x20003000);
  CHECK(t.by_guest_header(0x20002000) == nullptr);
  CHECK(t.by_guest_header(0x20003000) == o);
  o.reset();
  (void)t.close(rd(f, out));
  CHECK(t.by_guest_header(0x20003000) == nullptr);
}

TEST_CASE("kCurrentThread resolves through the hook", "[unit][kernel][objects]") {
  KernelFixture f;
  auto th = std::make_shared<KObject>(ObjectKind::Thread);
  kernel().objects().set_current_thread_hook([th] { return th; });
  CHECK(kernel().objects().get(kCurrentThread) == th);
  CHECK(kernel().objects().get(kCurrentProcess) == nullptr);
}

TEST_CASE("ObOpenObjectByName and ObCreateObject are not implemented", "[unit][kernel][objects]") {
  KernelFixture f;
  CHECK(static_cast<uint32_t>(f.call("ObOpenObjectByName", 0, 0, 0, 0, 0, 0, 0)) == STATUS_NOT_IMPLEMENTED);
  CHECK(static_cast<uint32_t>(f.call("ObCreateObject", 0, 0, 0, 0, 0)) == STATUS_NOT_IMPLEMENTED);
}

// SPDX-License-Identifier: MIT
// File imports (exports/files.cpp) over the VFS: NtCreateFile/NtOpenFile, reads and writes, queries, directory
// enumeration and the FileObject guest body.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/status.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kGenericRead = 0x80000000;
constexpr uint32_t kGenericWrite = 0x40000000;
constexpr uint32_t kFileOpen = 1, kFileCreate = 2, kFileOpenIf = 3, kFileOverwriteIf = 5;
constexpr uint32_t kFileDirectoryFile = 0x1;
constexpr uint32_t kFileOpened = 1, kFileCreated = 2, kFileOverwritten = 3;

// The import's r3 as a 32-bit NTSTATUS (marshal sign-extends it into the 64-bit register).
template <class... A>
uint32_t call(KernelFixture& f, const char* name, A... args) {
  return static_cast<uint32_t>(f.call(name, args...));
}

uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }
uint64_t rd64(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint64_t>>(addr); }
void wr64(KernelFixture& f, uint32_t addr, uint64_t v) { *f.mem.translate<be<uint64_t>>(addr) = v; }

void write_host(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}

std::string read_host(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// An X_ANSI_STRING (and its text) in guest memory.
uint32_t ansi(KernelFixture& f, const std::string& s) {
  const uint32_t text = f.alloc(static_cast<uint32_t>(s.size()) + 1);
  std::memcpy(f.mem.translate<char>(text), s.data(), s.size());
  const uint32_t str = f.alloc(sizeof(X_ANSI_STRING));
  auto* a = f.mem.translate<X_ANSI_STRING>(str);
  a->length = static_cast<uint16_t>(s.size());
  a->maximum_length = static_cast<uint16_t>(s.size() + 1);
  a->buffer = text;
  return str;
}

uint32_t attrs(KernelFixture& f, const std::string& path, uint32_t root = 0) {
  const uint32_t oa = f.alloc(sizeof(X_OBJECT_ATTRIBUTES));
  auto* o = f.mem.translate<X_OBJECT_ATTRIBUTES>(oa);
  o->root_directory = root;
  o->name_ptr = ansi(f, path);
  o->attributes = 0;
  return oa;
}

struct Opened {
  uint32_t status, handle, info;
};

Opened create(KernelFixture& f, const std::string& path, uint32_t disposition, uint32_t options = 0,
              uint32_t access = kGenericRead, uint32_t root = 0) {
  const uint32_t out = f.alloc(4), iosb = f.alloc(sizeof(X_IO_STATUS_BLOCK));
  const uint32_t st =
      call(f, "NtCreateFile", out, access, attrs(f, path, root), iosb, 0, 0, 0, disposition, options);
  return {st, rd(f, out), rd(f, iosb + 4)};
}

Opened open(KernelFixture& f, const std::string& path, uint32_t options = 0) {
  const uint32_t out = f.alloc(4), iosb = f.alloc(sizeof(X_IO_STATUS_BLOCK));
  const uint32_t st =
      call(f, "NtOpenFile", out, kGenericRead, attrs(f, path), iosb, 1 /* FILE_SHARE_READ */, options);
  return {st, rd(f, out), rd(f, iosb + 4)};
}

struct Io {
  uint32_t status, iosb_status, bytes;
};

// NtReadFile(handle, event, apc, apc_context, iosb, buffer, length, byte_offset*); offset < 0 passes null.
Io read(KernelFixture& f, uint32_t h, uint32_t buffer, uint32_t length, int64_t offset, uint32_t event = 0) {
  const uint32_t iosb = f.alloc(sizeof(X_IO_STATUS_BLOCK));
  uint32_t off = 0;
  if (offset >= 0) {
    off = f.alloc(8);
    wr64(f, off, static_cast<uint64_t>(offset));
  }
  const auto st = call(f, "NtReadFile", h, event, 0, 0, iosb, buffer, length, off);
  return {st, rd(f, iosb), rd(f, iosb + 4)};
}

std::string guest_str(KernelFixture& f, uint32_t addr, uint32_t n) { return {f.mem.translate<char>(addr), n}; }

}  // namespace

TEST_CASE("open existing file reports FILE_OPENED", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "Data" / "Title.arc", "hello");
  const Opened o = create(f, "game:\\data\\title.ARC", kFileOpen);
  CHECK(o.status == STATUS_SUCCESS);
  CHECK(o.info == kFileOpened);
  CHECK(o.handle >= kHandleBase);
  const Opened o2 = open(f, "\\Device\\Cdrom0\\Data\\Title.arc");
  CHECK(o2.status == STATUS_SUCCESS);
  CHECK(o2.info == kFileOpened);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
  CHECK(call(f, "NtClose", o2.handle) == STATUS_SUCCESS);
}

TEST_CASE("open missing -> STATUS_OBJECT_NAME_NOT_FOUND", "[unit][kernel][files]") {
  KernelFixture f;
  fs::create_directories(f.game_tmp.path / "Data");
  CHECK(create(f, "game:\\Data\\missing.arc", kFileOpen).status == STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(open(f, "game:\\Data\\missing.arc").status == STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(create(f, "game:\\NoDir\\missing.arc", kFileOpen).status == STATUS_OBJECT_PATH_NOT_FOUND);
  CHECK(create(f, "nodevice:\\x", kFileOpen).status == STATUS_OBJECT_PATH_NOT_FOUND);
  CHECK(create(f, "game:\\Data", kFileOpen, 0x40 /* FILE_NON_DIRECTORY_FILE */).status == STATUS_FILE_IS_A_DIRECTORY);
}

TEST_CASE("read at offset and at current position", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "0123456789");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  REQUIRE(o.status == STATUS_SUCCESS);
  const uint32_t buf = f.alloc(16);

  Io r = read(f, o.handle, buf, 4, 3);
  CHECK(r.status == STATUS_SUCCESS);
  CHECK(r.iosb_status == STATUS_SUCCESS);
  CHECK(r.bytes == 4);
  CHECK(guest_str(f, buf, 4) == "3456");

  r = read(f, o.handle, buf, 2, -1);  // null offset: continues after the last read (offset 7)
  CHECK(r.status == STATUS_SUCCESS);
  CHECK(guest_str(f, buf, 2) == "78");

  // FILE_USE_FILE_POINTER_POSITION (high -1, low -2) also reads at the current position.
  const uint32_t iosb = f.alloc(8), off = f.alloc(8);
  wr64(f, off, 0xFFFFFFFFFFFFFFFEull);
  CHECK(call(f, "NtReadFile", o.handle, 0, 0, 0, iosb, buf, 8, off) == STATUS_SUCCESS);
  CHECK(rd(f, iosb + 4) == 1);  // short read: only "9" was left
  CHECK(guest_str(f, buf, 1) == "9");

  // NtSetInformationFile(FilePositionInformation) + NtQueryInformationFile(FilePositionInformation).
  const uint32_t pos = f.alloc(8);
  wr64(f, pos, 1);
  CHECK(call(f, "NtSetInformationFile", o.handle, iosb, pos, 8, 14) == STATUS_SUCCESS);
  wr64(f, pos, 0);
  CHECK(call(f, "NtQueryInformationFile", o.handle, iosb, pos, 8, 14) == STATUS_SUCCESS);
  CHECK(rd64(f, pos) == 1);
  r = read(f, o.handle, buf, 3, -1);
  CHECK(guest_str(f, buf, 3) == "123");
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
}

TEST_CASE("read past end -> STATUS_END_OF_FILE", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t buf = f.alloc(16);
  Io r = read(f, o.handle, buf, 4, 3);
  CHECK(r.status == STATUS_END_OF_FILE);
  CHECK(r.iosb_status == STATUS_END_OF_FILE);
  CHECK(r.bytes == 0);
  r = read(f, o.handle, buf, 4, 100);
  CHECK(r.status == STATUS_END_OF_FILE);
  r = read(f, o.handle, buf, 0, 0);  // zero-length read succeeds
  CHECK(r.status == STATUS_SUCCESS);
  CHECK(r.bytes == 0);
}

TEST_CASE("negative byte offsets and directory reads are refused", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  write_host(f.user_tmp.path / "cache" / "w.bin", "abc");
  fs::create_directories(f.game_tmp.path / "Dir");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t buf = f.alloc(8), iosb = f.alloc(8), off = f.alloc(8);
  for (const uint64_t v : {0xFFFFFFFFFFFFFFFDull, 0x8000000000000000ull}) {  // -3, INT64_MIN
    wr64(f, off, v);
    CHECK(call(f, "NtReadFile", o.handle, 0, 0, 0, iosb, buf, 3, off) == STATUS_INVALID_PARAMETER);
  }
  const Opened w = create(f, "cache:\\w.bin", kFileOpen, 0, kGenericRead | kGenericWrite);
  REQUIRE(w.status == STATUS_SUCCESS);
  CHECK(call(f, "NtWriteFile", w.handle, 0, 0, 0, iosb, buf, 3, off) == STATUS_INVALID_PARAMETER);
  const uint32_t seg = f.alloc(4);
  *f.mem.translate<be<uint32_t>>(seg) = buf;
  CHECK(call(f, "NtReadFileScatter", o.handle, 0, 0, 0, iosb, seg, 3, off) == STATUS_INVALID_PARAMETER);

  const Opened d = create(f, "game:\\Dir", kFileOpen, kFileDirectoryFile);
  REQUIRE(d.status == STATUS_SUCCESS);
  CHECK(read(f, d.handle, buf, 3, 0).status == STATUS_INVALID_DEVICE_REQUEST);
}

TEST_CASE("NtReadFile with null io status block", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t buf = f.alloc(16);
  CHECK(call(f, "NtReadFile", o.handle, 0, 0, 0, 0, buf, 3, 0) == STATUS_INVALID_PARAMETER);
  CHECK(call(f, "NtReadFile", 0xF8000400u, 0, 0, 0, f.alloc(8), buf, 3, 0) == STATUS_INVALID_HANDLE);
}

TEST_CASE("read into buffer crossing 4 GiB -> STATUS_ACCESS_VIOLATION", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", std::string(0x200, 'x'));
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const Io r = read(f, o.handle, 0xFFFFFF00u, 0x200, 0);
  CHECK(r.status == STATUS_ACCESS_VIOLATION);
  CHECK(read(f, o.handle, 0, 4, 0).status == STATUS_ACCESS_VIOLATION);  // null buffer
}

TEST_CASE("query standard information size", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", std::string(5000, 'x'));
  fs::create_directories(f.game_tmp.path / "Dir");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t iosb = f.alloc(8), info = f.alloc(64);
  CHECK(call(f, "NtQueryInformationFile", o.handle, iosb, info, 24, 5) == STATUS_SUCCESS);
  CHECK(rd(f, iosb + 4) == 24);
  CHECK(rd64(f, info + 0) >= 5000);  // allocation size
  CHECK(rd64(f, info + 8) == 5000);  // end of file
  CHECK(rd(f, info + 16) == 1);      // links
  CHECK(f.mem.translate<uint8_t>(info)[21] == 0);  // not a directory
  CHECK(call(f, "NtQueryInformationFile", o.handle, iosb, info, 23, 5) == STATUS_INFO_LENGTH_MISMATCH);
  CHECK(call(f, "NtQueryInformationFile", o.handle, iosb, info, 64, 9) == STATUS_INVALID_PARAMETER);

  CHECK(call(f, "NtQueryInformationFile", o.handle, iosb, info, 56, 34) == STATUS_SUCCESS);
  CHECK(rd64(f, info + 40) == 5000);
  CHECK(rd(f, info + 48) == 0x80);  // FILE_ATTRIBUTE_NORMAL
  CHECK(rd64(f, info + 16) != 0);    // last write time

  const uint32_t attr = f.alloc(56);
  CHECK(call(f, "NtQueryFullAttributesFile", attrs(f, "game:\\A.BIN"), attr) == STATUS_SUCCESS);
  CHECK(rd64(f, attr + 40) == 5000);
  CHECK(call(f, "NtQueryFullAttributesFile", attrs(f, "game:\\Dir"), attr) == STATUS_SUCCESS);
  CHECK(rd(f, attr + 48) == 0x10);  // FILE_ATTRIBUTE_DIRECTORY
  CHECK(call(f, "NtQueryFullAttributesFile", attrs(f, "game:\\nope"), attr) == STATUS_OBJECT_NAME_NOT_FOUND);

  // Volume: FileFsSizeInformation (3) and FileFsAttributeInformation (5).
  CHECK(call(f, "NtQueryVolumeInformationFile", o.handle, iosb, info, 24, 3) == STATUS_SUCCESS);
  CHECK(rd(f, info + 20) == 512);  // bytes per sector
  CHECK(call(f, "NtQueryVolumeInformationFile", o.handle, iosb, info, 64, 5) == STATUS_SUCCESS);
  CHECK(rd(f, info + 8) == 4);
  CHECK(guest_str(f, info + 12, 4) == "FATX");
}

TEST_CASE("directory enumeration with *.arc", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "Data" / "b.ARC", "bb");
  write_host(f.game_tmp.path / "Data" / "a.arc", "a");
  write_host(f.game_tmp.path / "Data" / "c.txt", "c");
  fs::create_directories(f.game_tmp.path / "Data" / "sub.arc");
  const Opened d = create(f, "game:\\Data", kFileOpen, kFileDirectoryFile);
  REQUIRE(d.status == STATUS_SUCCESS);
  const uint32_t iosb = f.alloc(8), info = f.alloc(0x200), pattern = ansi(f, "*.arc");

  std::set<std::string> names;
  uint32_t st = 0;
  for (int i = 0; i < 10; ++i) {
    st = call(f, "NtQueryDirectoryFile", d.handle, 0, 0, 0, iosb, info, 0x200, i == 0 ? pattern : 0, 0);
    if (st != STATUS_SUCCESS) break;
    const uint32_t len = rd(f, info + 0x3C);
    CHECK(rd(f, iosb + 4) == 0x40 + len);
    const std::string name = guest_str(f, info + 0x40, len);
    names.insert(name);
    if (name == "b.ARC") CHECK(rd64(f, info + 0x28) == 2);
    if (name == "sub.arc") CHECK(rd(f, info + 0x38) == 0x10);
  }
  CHECK(st == STATUS_NO_MORE_FILES);
  CHECK(names == std::set<std::string>{"a.arc", "b.ARC", "sub.arc"});

  // Restart with another pattern (`?` matches one character).
  CHECK(call(f, "NtQueryDirectoryFile", d.handle, 0, 0, 0, iosb, info, 0x200, ansi(f, "?.TXT"), 1) == STATUS_SUCCESS);
  CHECK(guest_str(f, info + 0x40, rd(f, info + 0x3C)) == "c.txt");
  CHECK(call(f, "NtQueryDirectoryFile", d.handle, 0, 0, 0, iosb, info, 0x200, 0, 0) == STATUS_NO_MORE_FILES);
  // No match at all on the first call.
  CHECK(call(f, "NtQueryDirectoryFile", d.handle, 0, 0, 0, iosb, info, 0x200, ansi(f, "*.zip"), 1) ==
        STATUS_NO_SUCH_FILE);
  CHECK(call(f, "NtQueryDirectoryFile", d.handle, 0, 0, 0, iosb, info, 71, 0, 1) == STATUS_INFO_LENGTH_MISMATCH);

  // A relative open through the directory handle (OBJECT_ATTRIBUTES.root_directory).
  const Opened rel = create(f, "A.ARC", kFileOpen, 0, kGenericRead, d.handle);
  CHECK(rel.status == STATUS_SUCCESS);
  const uint32_t buf = f.alloc(4);
  CHECK(read(f, rel.handle, buf, 1, 0).status == STATUS_SUCCESS);
  CHECK(guest_str(f, buf, 1) == "a");
}

TEST_CASE("create file in cache: then reopen", "[unit][kernel][files]") {
  KernelFixture f;
  const uint32_t rw = kGenericRead | kGenericWrite;
  Opened o = create(f, "cache:\\Save.dat", kFileCreate, 0, rw);
  REQUIRE(o.status == STATUS_SUCCESS);
  CHECK(o.info == kFileCreated);
  const uint32_t data = f.alloc(8), iosb = f.alloc(8);
  std::memcpy(f.mem.translate<char>(data), "payload!", 8);
  CHECK(call(f, "NtWriteFile", o.handle, 0, 0, 0, iosb, data, 8, 0) == STATUS_SUCCESS);
  CHECK(rd(f, iosb + 4) == 8);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
  CHECK(read_host(f.user_tmp.path / "cache" / "Save.dat") == "payload!");

  CHECK(create(f, "cache:\\save.DAT", kFileCreate, 0, rw).status == STATUS_OBJECT_NAME_COLLISION);
  o = create(f, "cache:\\save.DAT", kFileOpenIf, 0, rw);
  CHECK(o.status == STATUS_SUCCESS);
  CHECK(o.info == kFileOpened);
  const uint32_t buf = f.alloc(8);
  CHECK(read(f, o.handle, buf, 8, 0).bytes == 8);
  CHECK(guest_str(f, buf, 8) == "payload!");
  // EndOfFile (20) truncates; Disposition (13) deletes on close.
  const uint32_t v = f.alloc(8);
  wr64(f, v, 3);
  CHECK(call(f, "NtSetInformationFile", o.handle, iosb, v, 8, 20) == STATUS_SUCCESS);
  CHECK(fs::file_size(f.user_tmp.path / "cache" / "Save.dat") == 3);
  *f.mem.translate<uint8_t>(v) = 1;
  CHECK(call(f, "NtSetInformationFile", o.handle, iosb, v, 1, 13) == STATUS_SUCCESS);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
  CHECK(!fs::exists(f.user_tmp.path / "cache" / "Save.dat"));

  // Overwrite-if on a new name creates it; on an existing one truncates it and reports FILE_OVERWRITTEN.
  o = create(f, "cache:\\x.bin", kFileOverwriteIf, 0, rw);
  CHECK(o.info == kFileCreated);
  CHECK(call(f, "NtWriteFile", o.handle, 0, 0, 0, iosb, data, 8, 0) == STATUS_SUCCESS);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
  o = create(f, "cache:\\x.bin", kFileOverwriteIf, 0, rw);
  CHECK(o.info == kFileOverwritten);
  CHECK(fs::file_size(f.user_tmp.path / "cache" / "x.bin") == 0);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);

  // A directory created with FILE_DIRECTORY_FILE.
  o = create(f, "cache:\\Dir", kFileCreate, kFileDirectoryFile, rw);
  CHECK(o.status == STATUS_SUCCESS);
  CHECK(fs::is_directory(f.user_tmp.path / "cache" / "Dir"));
}

TEST_CASE("write to game: -> STATUS_ACCESS_DENIED", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  const uint32_t rw = kGenericRead | kGenericWrite;
  CHECK(create(f, "game:\\new.bin", kFileCreate, 0, rw).status == STATUS_ACCESS_DENIED);
  CHECK(create(f, "game:\\a.bin", kFileOverwriteIf, 0, rw).status == STATUS_ACCESS_DENIED);
  CHECK(!fs::exists(f.game_tmp.path / "new.bin"));
  // Opening with write access succeeds read-only (as Xenia); writing through it is refused.
  const Opened o = create(f, "game:\\a.bin", kFileOpen, 0, rw);
  REQUIRE(o.status == STATUS_SUCCESS);
  const uint32_t data = f.alloc(4), iosb = f.alloc(8), v = f.alloc(8);
  CHECK(call(f, "NtWriteFile", o.handle, 0, 0, 0, iosb, data, 3, 0) == STATUS_ACCESS_DENIED);
  CHECK(call(f, "NtSetInformationFile", o.handle, iosb, v, 8, 20) == STATUS_ACCESS_DENIED);
  *f.mem.translate<uint8_t>(v) = 1;
  CHECK(call(f, "NtSetInformationFile", o.handle, iosb, v, 1, 13) == STATUS_ACCESS_DENIED);
  CHECK(read_host(f.game_tmp.path / "a.bin") == "abc");
}

TEST_CASE("read signals event", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t ev_out = f.alloc(4);
  REQUIRE(call(f, "NtCreateEvent", ev_out, 0, 0 /* notification */, 0) == STATUS_SUCCESS);
  const uint32_t ev = rd(f, ev_out);
  const uint32_t header = kernel().objects().get(ev)->guest_header();
  CHECK(*f.mem.translate<be<int32_t>>(header + 4) == 0);
  const uint32_t buf = f.alloc(4);
  CHECK(read(f, o.handle, buf, 3, 0, ev).status == STATUS_SUCCESS);
  CHECK(*f.mem.translate<be<int32_t>>(header + 4) == 1);
  CHECK(read(f, o.handle, buf, 3, 0, 0xF8000400u).status == STATUS_INVALID_HANDLE);  // bad event handle
}

TEST_CASE("NtReadFileScatter fills page segments", "[unit][kernel][files]") {
  KernelFixture f;
  std::string data(0x1800, 'a');
  for (size_t i = 0x1000; i < data.size(); ++i) data[i] = 'b';
  write_host(f.game_tmp.path / "a.bin", data);
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t p0 = (f.alloc(0x2000) + 0xFF0) & ~0xFFFu, p1 = (f.alloc(0x2000) + 0xFF0) & ~0xFFFu;
  const uint32_t seg = f.alloc(8), iosb = f.alloc(8);
  *f.mem.translate<be<uint32_t>>(seg) = p0;
  *f.mem.translate<be<uint32_t>>(seg + 4) = p1;
  CHECK(call(f, "NtReadFileScatter", o.handle, 0, 0, 0, iosb, seg, 0x1800, 0) == STATUS_SUCCESS);
  CHECK(rd(f, iosb + 4) == 0x1800);
  CHECK(guest_str(f, p0 + 0xFFF, 1) == "a");
  CHECK(guest_str(f, p1, 1) == "b");
  CHECK(guest_str(f, p1 + 0x7FF, 1) == "b");
}

TEST_CASE("file handles have a guest body for ObReferenceObjectByHandle but stay unwaitable", "[unit][kernel][files]") {
  KernelFixture f;
  write_host(f.game_tmp.path / "a.bin", "abc");
  const Opened o = create(f, "game:\\a.bin", kFileOpen);
  const uint32_t out = f.alloc(4);
  CHECK(call(f, "ObReferenceObjectByHandle", o.handle, 0, out) == STATUS_SUCCESS);
  const uint32_t body = rd(f, out);
  CHECK(body >= 0x10000);
  CHECK(kernel().objects().by_guest_header(body) == kernel().objects().get(o.handle));
  CHECK(kernel().objects().get(o.handle)->guest_header() == 0);
  const uint32_t timeout = f.alloc(8);
  CHECK(call(f, "NtWaitForSingleObjectEx", o.handle, 1, 0, timeout) == STATUS_OBJECT_TYPE_MISMATCH);
  call(f, "ObDereferenceObject", body);
  CHECK(call(f, "NtClose", o.handle) == STATUS_SUCCESS);
}

TEST_CASE("NtDeviceIoControlFile and Fsc cache stubs", "[unit][kernel][files]") {
  KernelFixture f;
  CHECK(call(f, "NtDeviceIoControlFile", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == STATUS_NOT_IMPLEMENTED);
  CHECK(call(f, "FscGetCacheElementCount", 0) == 0);
  CHECK(call(f, "FscSetCacheElementCount", 0, 256) == STATUS_SUCCESS);
}

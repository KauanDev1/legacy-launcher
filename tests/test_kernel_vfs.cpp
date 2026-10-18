// SPDX-License-Identifier: MIT
// Guest path layer (runtime/kernel/vfs.h) and the Ob*SymbolicLink imports.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/vfs.h"
#include "support/kernel_fixture.h"
#include "support/temp_dir.h"

using namespace lr::kernel;
using lr::testing::KernelFixture;
using lr::testing::TempDir;
namespace fs = std::filesystem;

namespace {

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "x";
}

struct Tree {
  TempDir tmp{"lr_vfs"};
  fs::path game, user;
  Vfs vfs;
  explicit Tree(const char* base = "") {
    const fs::path root = *base ? tmp.path / base : tmp.path;
    game = root / "game";
    user = root / "user";
    touch(game / "Data" / "x.arc");
    touch(game / "default.xex");
    vfs.mount_defaults(game, user);
  }
};

}  // namespace

TEST_CASE("resolve handles odd guest paths", "[unit][vfs]") {
  Tree t;
  const fs::path want = t.game / "Data" / "x.arc";
  for (const char* p : {"game:\\Data\\x.arc", "GAME:\\DATA\\X.ARC", "\\??\\game:\\Data\\x.arc", "game:\\\\Data\\\\\\\\x.arc",
                        "game:/Data/x.arc", "d:\\data\\.\\x.arc", "\\Device\\Cdrom0\\Data\\x.arc",
                        "\\DEVICE\\CDROM0\\data\\x.arc", "game:\\Data\\..\\Data\\x.arc", "\\??\\GAME:\\Data\\x.arc"}) {
    INFO(p);
    auto r = t.vfs.resolve(p);
    REQUIRE(r);
    CHECK(r.value().host == want);
    CHECK(r.value().exists);
    CHECK(r.value().read_only);
    CHECK_FALSE(r.value().is_directory);
  }
  SECTION("directories, trailing separator and the mount root") {
    auto d = t.vfs.resolve("game:\\data\\");
    REQUIRE(d);
    CHECK(d.value().host == t.game / "Data");
    CHECK(d.value().is_directory);
    auto root = t.vfs.resolve("game:\\");
    REQUIRE(root);
    CHECK(root.value().host == t.game);
    CHECK(root.value().is_directory);
    CHECK_FALSE(t.vfs.resolve("game:\\Data\\x.arc\\"));  // a file with a trailing separator
  }
  SECTION("escaping the mount is NotFound") {
    for (const char* p : {"game:\\..\\..\\etc\\passwd", "game:\\..", "\\Device\\Cdrom0\\..\\user", "game:\\Data\\..\\..\\x"}) {
      INFO(p);
      auto r = t.vfs.resolve(p);
      REQUIRE_FALSE(r);
      CHECK(r.error().code == lr::ErrorCode::NotFound);
    }
  }
  SECTION("unknown devices, missing directories and control bytes") {
    CHECK(t.vfs.resolve("nope:\\x").error().code == lr::ErrorCode::NotFound);
    CHECK(t.vfs.resolve("game:\\Missing\\x.arc").error().code == lr::ErrorCode::NotFound);
    CHECK(t.vfs.resolve("game:\\default.xex\\x").error().code == lr::ErrorCode::NotFound);  // file as directory
    CHECK(t.vfs.resolve(std::string("game:\\Data\\x.arc\0y", 18)).error().code == lr::ErrorCode::InvalidFormat);
    CHECK(t.vfs.resolve("game:\\Data\\x\x01.arc").error().code == lr::ErrorCode::InvalidFormat);
  }
  SECTION("a missing last component keeps the guest spelling") {
    auto r = t.vfs.resolve("game:\\Data\\New.BIN");
    REQUIRE(r);
    CHECK_FALSE(r.value().exists);
    CHECK(r.value().host == t.game / "Data" / "New.BIN");
  }
  SECTION("relative to an open directory") {
    const fs::path dir = t.game / "Data";
    auto r = t.vfs.resolve("X.ARC", &dir);
    REQUIRE(r);
    CHECK(r.value().host == t.game / "Data" / "x.arc");
    CHECK(r.value().read_only);
    auto up = t.vfs.resolve("..\\DEFAULT.XEX", &dir);
    REQUIRE(up);
    CHECK(up.value().host == t.game / "default.xex");
    CHECK_FALSE(t.vfs.resolve("..\\..\\x", &dir));
  }
}

TEST_CASE("resolve works under a host path with spaces", "[unit][vfs]") {
  Tree t("Jogos Teste");
  auto r = t.vfs.resolve("game:\\DATA\\X.arc");
  REQUIRE(r);
  CHECK(r.value().host == t.game / "Data" / "x.arc");
  CHECK(r.value().host.string().find("Jogos Teste") != std::string::npos);
  CHECK(r.value().exists);
  auto c = t.vfs.resolve("cache:\\Save Game.bin");
  REQUIRE(c);
  CHECK(c.value().host == t.user / "cache" / "Save Game.bin");
}

TEST_CASE("writes to game: are read-only", "[unit][vfs]") {
  Tree t;
  auto existing = t.vfs.resolve("game:\\Data\\x.arc");
  REQUIRE(existing);
  CHECK(existing.value().read_only);
  auto fresh = t.vfs.resolve("game:\\Data\\new.sav");
  REQUIRE(fresh);
  CHECK(fresh.value().read_only);
  CHECK_FALSE(fresh.value().exists);
  CHECK(t.vfs.resolve("\\Device\\Cdrom0\\Data\\x.arc").value().read_only);
}

TEST_CASE("new file under cache: keeps guest spelling", "[unit][vfs]") {
  Tree t;
  auto r = t.vfs.resolve("cache:\\Saves\\World.DAT");
  REQUIRE_FALSE(r);  // Saves does not exist yet
  fs::create_directories(t.user / "cache" / "Saves");
  r = t.vfs.resolve("CACHE:\\Saves\\World.DAT");
  REQUIRE(r);
  CHECK_FALSE(r.value().read_only);
  CHECK_FALSE(r.value().exists);
  CHECK(r.value().host == t.user / "cache" / "Saves" / "World.DAT");
  touch(t.user / "cache" / "Saves" / "World.DAT");
  // The guest now spells it differently: the existing host name wins; writable dirs are not cached.
  r = t.vfs.resolve("cache:\\saves\\WORLD.dat");
  REQUIRE(r);
  CHECK(r.value().exists);
  CHECK(r.value().host == t.user / "cache" / "Saves" / "World.DAT");
  CHECK(t.vfs.resolve("\\Device\\Harddisk0\\Cache0\\SAVES\\world.dat").value().exists);
}

TEST_CASE("symlink create and delete", "[unit][vfs]") {
  Tree t;
  CHECK(t.vfs.create_symlink("\\??\\game:", "\\Device\\Cdrom0") == STATUS_OBJECT_NAME_COLLISION);
  CHECK(t.vfs.create_symlink("GAME", "\\Device\\Cdrom0\\") == STATUS_OBJECT_NAME_COLLISION);  // no colon, case
  CHECK(t.vfs.create_symlink("", "\\Device\\Cdrom0") == STATUS_INVALID_PARAMETER);
  // Chained: media: -> game: -> \Device\Cdrom0.
  CHECK(t.vfs.create_symlink("\\??\\media:", "\\??\\game:") == STATUS_SUCCESS);
  auto r = t.vfs.resolve("MEDIA:\\data\\x.arc");
  REQUIRE(r);
  CHECK(r.value().host == t.game / "Data" / "x.arc");
  // A link to a subdirectory of a device.
  CHECK(t.vfs.create_symlink("data:", "\\Device\\Cdrom0\\Data") == STATUS_SUCCESS);
  CHECK(t.vfs.resolve("data:\\X.ARC").value().host == t.game / "Data" / "x.arc");
  CHECK(t.vfs.delete_symlink("\\??\\DATA:\\") == STATUS_SUCCESS);
  CHECK_FALSE(t.vfs.resolve("data:\\x.arc"));
  CHECK(t.vfs.delete_symlink("data:") == STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(t.vfs.delete_symlink("media") == STATUS_SUCCESS);
  CHECK_FALSE(t.vfs.resolve("media:\\data\\x.arc"));
  // A loop does not hang.
  CHECK(t.vfs.create_symlink("a:", "b:") == STATUS_SUCCESS);
  CHECK(t.vfs.create_symlink("b:", "a:") == STATUS_SUCCESS);
  CHECK_FALSE(t.vfs.resolve("a:\\x"));
}

TEST_CASE("Ob*SymbolicLink imports drive the kernel VFS", "[unit][vfs]") {
  KernelFixture f;
  touch(f.game_tmp.path / "Media" / "a.bin");
  const auto ansi = [&](const char* s) {
    const uint32_t str = f.alloc(8), buf = f.alloc(64);
    std::memcpy(f.mem.base() + buf, s, std::strlen(s));
    auto* x = f.mem.translate<X_ANSI_STRING>(str);
    x->length = static_cast<uint16_t>(std::strlen(s));
    x->maximum_length = 64;
    x->buffer = buf;
    return str;
  };
  CHECK(static_cast<uint32_t>(f.call("ObCreateSymbolicLink", ansi("\\??\\media:"), ansi("\\Device\\Cdrom0\\Media"))) == STATUS_SUCCESS);
  // A duplicate keeps the existing link and succeeds (as Xenia does); the original target still applies.
  CHECK(static_cast<uint32_t>(f.call("ObCreateSymbolicLink", ansi("MEDIA:"), ansi("\\Device\\Cdrom0"))) == STATUS_SUCCESS);
  CHECK(kernel().vfs().resolve("media:\\a.bin").value().exists);
  auto r = kernel().vfs().resolve("media:\\A.BIN");
  REQUIRE(r);
  CHECK(r.value().exists);
  CHECK(static_cast<uint32_t>(f.call("ObCreateSymbolicLink", 0u, ansi("x"))) == STATUS_INVALID_PARAMETER);
  CHECK(static_cast<uint32_t>(f.call("ObDeleteSymbolicLink", ansi("\\??\\media:"))) == STATUS_SUCCESS);
  CHECK(static_cast<uint32_t>(f.call("ObDeleteSymbolicLink", ansi("media:"))) == STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(static_cast<uint32_t>(f.call("ObDeleteSymbolicLink", 0u)) == STATUS_INVALID_PARAMETER);
  // The default mounts come from the kernel config; cache: is created under user_dir.
  CHECK(fs::is_directory(f.user_tmp.path / "cache"));
  CHECK(kernel().vfs().resolve("game:\\Media\\a.bin").value().read_only);
}

TEST_CASE("hard disk partitions share the writable <user>/cache folder", "[unit][vfs]") {
  // Spec section 9: cache: and the disk partitions go to <root>/user/cache.
  Tree t;
  fs::create_directories(t.user / "cache" / "Saves");
  for (const char* dev : {"\\Device\\Harddisk0\\Partition0", "\\Device\\Harddisk0\\Partition1"}) {
    auto r = t.vfs.resolve(std::string(dev) + "\\Saves\\World.dat");
    REQUIRE(r);
    CHECK(r.value().host == t.user / "cache" / "Saves" / "World.dat");
    CHECK_FALSE(r.value().read_only);
  }
  touch(t.user / "cache" / "content.bin");
  CHECK(t.vfs.resolve("\\Device\\Harddisk0\\Partition1\\CONTENT.BIN").value().exists);
  CHECK(t.vfs.resolve("cache:\\content.bin").value().host ==
        t.vfs.resolve("\\Device\\Harddisk0\\Partition0\\content.bin").value().host);
}

TEST_CASE("mount_defaults skips cache: and reports when user/cache cannot be created", "[unit][vfs]") {
  TempDir t;
  touch(t.path / "game" / "a.bin");
  std::ofstream(t.path / "user") << "a regular file blocks the folder";
  Vfs vfs;
  CHECK_FALSE(vfs.mount_defaults(t.path / "game", t.path / "user"));
  CHECK(vfs.resolve("game:\\a.bin").value().exists);  // the game mount still works
  CHECK_FALSE(vfs.resolve("cache:\\x"));
  CHECK_FALSE(vfs.resolve("\\Device\\Harddisk0\\Partition1\\x"));
}

TEST_CASE("a failed directory listing is not cached in the read-only index", "[unit][vfs]") {
  Tree t;
  const fs::path sub = t.game / "Later";
  fs::create_directories(sub);
  fs::permissions(sub, fs::perms::none);
  const bool blocked = !std::ifstream(sub / "nothing").is_open() && ::access(sub.c_str(), R_OK) != 0;
  auto first = t.vfs.resolve("game:\\Later\\File.BIN");
  fs::permissions(sub, fs::perms::owner_all);
  if (!blocked) SKIP("directory permissions are not enforced (running as root)");
  REQUIRE(first);
  CHECK_FALSE(first.value().exists);
  touch(sub / "file.bin");
  auto second = t.vfs.resolve("game:\\Later\\File.BIN");
  REQUIRE(second);
  CHECK(second.value().exists);
  CHECK(second.value().host == sub / "file.bin");
}

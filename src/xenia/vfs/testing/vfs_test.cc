/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/entry.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::vfs::test {

TEST_CASE("STFS Decode date and time", "[stfs_decode]") {
  SECTION("10 June 2022 19:46:00 UTC - Decode") {
    const uint16_t date = 0x54CA;
    const uint16_t time = 0x9DBD;
    const uint64_t result = 132993639580000000;

    const uint64_t timestamp = decode_fat_timestamp(date, time);

    REQUIRE(timestamp == result);
  }
}

TEST_CASE("Host path entry found by its new name after a rename",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-rename-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "item.tmp") << "data";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* entry = device.ResolvePath("item.tmp");
    REQUIRE(entry != nullptr);

    entry->Rename("cache:\\item.ipk");

    REQUIRE(entry->name() == "item.ipk");
    REQUIRE(device.ResolvePath("item.ipk") == entry);
    REQUIRE(device.ResolvePath("item.tmp") == nullptr);
    REQUIRE(std::filesystem::exists(dir / "item.ipk"));
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("Host path entry moved to another directory by a rename",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-move-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir / "data");
  std::ofstream(dir / "setting.tmp") << "data";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* entry = device.ResolvePath("setting.tmp");
    Entry* data = device.ResolvePath("data");
    REQUIRE(entry != nullptr);
    REQUIRE(data != nullptr);

    REQUIRE(entry->Rename("cache:\\data\\setting.txt"));

    REQUIRE(entry->parent() == data);
    REQUIRE(device.ResolvePath("data\\setting.txt") == entry);
    REQUIRE(device.ResolvePath("setting.tmp") == nullptr);
    REQUIRE(std::filesystem::exists(dir / "data" / "setting.txt"));

    // Deleting the moved entry must remove it from its new parent.
    REQUIRE(entry->Delete());
    REQUIRE(device.ResolvePath("data\\setting.txt") == nullptr);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("Host path entry rename replaces an existing file when asked",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-replace-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "banner.tmp") << "new";
  std::ofstream(dir / "banner.bin") << "old";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* tmp = device.ResolvePath("banner.tmp");
    Entry* old_entry = device.ResolvePath("banner.bin");
    REQUIRE(tmp != nullptr);
    REQUIRE(old_entry != nullptr);

    // Without ReplaceIfExists the target must be left untouched.
    X_STATUS status = X_STATUS_SUCCESS;
    REQUIRE_FALSE(tmp->Rename("cache:\\banner.bin", false, &status));
    REQUIRE(status == X_STATUS_OBJECT_NAME_COLLISION);
    REQUIRE(device.ResolvePath("banner.tmp") == tmp);
    REQUIRE(std::filesystem::exists(dir / "banner.tmp"));

    // With ReplaceIfExists (POSIX rename()) the target is overwritten.
    REQUIRE(tmp->Rename("cache:\\banner.bin", true, &status));
    REQUIRE(device.ResolvePath("banner.bin") == tmp);
    REQUIRE(device.ResolvePath("banner.tmp") == nullptr);
    REQUIRE_FALSE(std::filesystem::exists(dir / "banner.tmp"));
    std::ifstream in(dir / "banner.bin");
    std::string content;
    in >> content;
    REQUIRE(content == "new");
    // Exactly one cached entry named banner.bin remains.
    size_t count = 0;
    for (auto& child : device.ResolvePath("")->children()) {
      if (child->name() == "banner.bin") ++count;
    }
    REQUIRE(count == 1);
  }

  std::filesystem::remove_all(dir);
}

}  // namespace xe::vfs::test

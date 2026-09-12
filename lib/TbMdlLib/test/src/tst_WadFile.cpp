/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mdl/WadFile.h"

#include "kd/result.h"

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

std::vector<uint8_t> bytes(const std::initializer_list<int> values)
{
  auto result = std::vector<uint8_t>{};
  for (const auto value : values)
  {
    result.push_back(uint8_t(value));
  }
  return result;
}

/** A directory of its own per test, removed with everything in it afterwards. */
class TempDir
{
private:
  std::filesystem::path m_path;

public:
  TempDir()
    : m_path{
        std::filesystem::temp_directory_path()
        / ("tb_wad_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)))}
  {
    std::filesystem::create_directories(m_path);
  }

  ~TempDir() { std::filesystem::remove_all(m_path); }

  std::filesystem::path file(const std::string& name) const { return m_path / name; }
};

} // namespace

TEST_CASE("WadFile")
{
  const auto tempDir = TempDir{};

  SECTION("lumps survive a round trip")
  {
    const auto path = tempDir.file("round_trip.wad");
    const auto lumps = std::vector<WadLump>{
      WadLump{"first", WadMipTextureType, bytes({1, 2, 3, 4})},
      WadLump{"second", WadMipTextureType, bytes({5, 6})},
      // A name of the full fifteen characters, which is as long as the format keeps.
      WadLump{"123456789012345", WadMipTextureType, bytes({7})},
    };

    REQUIRE(writeWad(path, lumps).is_success());

    const auto read = readWadLumps(path);
    REQUIRE(read.is_success());

    const auto& readLumps = read.value();
    REQUIRE(readLumps.size() == 3u);
    CHECK(readLumps[0].name == "first");
    CHECK(readLumps[0].type == WadMipTextureType);
    CHECK(readLumps[0].data == bytes({1, 2, 3, 4}));
    CHECK(readLumps[1].name == "second");
    CHECK(readLumps[1].data == bytes({5, 6}));
    CHECK(readLumps[2].name == "123456789012345");
    CHECK(readLumps[2].data == bytes({7}));
  }

  SECTION("an empty wad is still a wad")
  {
    const auto path = tempDir.file("empty.wad");
    REQUIRE(writeWad(path, {}).is_success());

    const auto read = readWadLumps(path);
    REQUIRE(read.is_success());
    CHECK(read.value().empty());
  }

  SECTION("a repeated name is written once, keeping the last")
  {
    const auto path = tempDir.file("repeated.wad");
    const auto lumps = std::vector<WadLump>{
      WadLump{"keep", WadMipTextureType, bytes({1})},
      WadLump{"replaced", WadMipTextureType, bytes({2})},
      // The same name in a different case: a wad is read without regard to case, so
      // this replaces the one above rather than sitting beside it.
      WadLump{"REPLACED", WadMipTextureType, bytes({3})},
    };

    REQUIRE(writeWad(path, lumps).is_success());

    const auto read = readWadLumps(path);
    REQUIRE(read.is_success());

    const auto& readLumps = read.value();
    REQUIRE(readLumps.size() == 2u);
    CHECK(readLumps[0].name == "keep");
    CHECK(readLumps[1].data == bytes({3}));
  }

  SECTION("adding to a wad keeps what was already in it")
  {
    const auto path = tempDir.file("appended.wad");
    REQUIRE(
      writeWad(path, {WadLump{"old", WadMipTextureType, bytes({1, 1})}}).is_success());

    auto lumps = readWadLumps(path) | kdl::value();
    lumps.push_back(WadLump{"new", WadMipTextureType, bytes({2, 2})});
    REQUIRE(writeWad(path, lumps).is_success());

    const auto read = readWadLumps(path) | kdl::value();
    REQUIRE(read.size() == 2u);
    CHECK(read[0].name == "old");
    CHECK(read[0].data == bytes({1, 1}));
    CHECK(read[1].name == "new");
  }

  SECTION("reading something that is not a wad fails")
  {
    const auto path = tempDir.file("not_a_wad.txt");
    {
      auto stream = std::ofstream{path, std::ios::binary};
      stream << "this is not a wad file at all";
    }

    CHECK(readWadLumps(path).is_error());
    CHECK(readWadLumps(tempDir.file("missing.wad")).is_error());
  }
}

} // namespace tb::mdl

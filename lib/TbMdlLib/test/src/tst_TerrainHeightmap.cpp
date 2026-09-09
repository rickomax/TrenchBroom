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

#include "mdl/Terrain.h"
#include "mdl/TerrainHeightmap.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

/** An 8 bit height map of the given side length, filled by the given function. */
std::string make8Bit(const size_t size, const auto& sample)
{
  auto data = std::string{};
  for (size_t row = 0; row < size; ++row)
  {
    for (size_t column = 0; column < size; ++column)
    {
      data += char(static_cast<unsigned char>(sample(column, row)));
    }
  }
  return data;
}

Terrain makeTerrain(const size_t columns = 4, const size_t rows = 4)
{
  const auto bounds = vm::bbox3d{
    vm::vec3d{0, 0, 0}, vm::vec3d{double(columns) * 32.0, double(rows) * 32.0, 65.0}};
  return *createTerrain(bounds, 32.0, "some_material");
}

} // namespace

TEST_CASE("TerrainHeightmap")
{
  SECTION("parseRawHeightmap")
  {
    SECTION("reads 8 bit samples when the length is a perfect square")
    {
      const auto data = make8Bit(4, [](auto column, auto) { return column * 85; });

      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());
      CHECK(heightmap.value().size == 4u);
      CHECK(!heightmap.value().sixteenBit);
      CHECK(heightmap.value().samples.size() == 16u);
      CHECK(heightmap.value().samples[0] == vm::approx{0.0});
      CHECK(heightmap.value().samples[3] == vm::approx{1.0});
    }

    SECTION("reads little endian 16 bit samples when the length is twice a square")
    {
      // Two by two 16 bit samples: 0, 65535, 0, 65535.
      const auto data = std::string{"\x00\x00\xff\xff\x00\x00\xff\xff", 8};

      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());
      CHECK(heightmap.value().size == 2u);
      CHECK(heightmap.value().sixteenBit);
      CHECK(heightmap.value().samples.size() == 4u);
      CHECK(heightmap.value().samples[0] == vm::approx{0.0});
      CHECK(heightmap.value().samples[1] == vm::approx{1.0});
    }

    SECTION("a length can never fit both widths")
    {
      // A length that is both a perfect square and twice one would make sqrt(2)
      // rational, so the width always follows from the length alone. This checks the
      // sizes a height map is actually likely to have.
      auto anyAmbiguous = false;
      for (size_t side = 1; side <= 4096 && !anyAmbiguous; ++side)
      {
        // A 16 bit height map's length must not also look like an 8 bit one.
        const auto sixteenBit = 2 * side * side;
        const auto root = size_t(std::llround(std::sqrt(double(sixteenBit))));
        anyAmbiguous = root * root == sixteenBit;
      }
      CHECK(!anyAmbiguous);
    }

    SECTION("rejects lengths that are neither")
    {
      CHECK(parseRawHeightmap("").is_error());
      CHECK(parseRawHeightmap(std::string(5, '\0')).is_error());
      // 18 is even but 9 samples of 16 bits would be a 3x3 square... which is valid.
      CHECK(parseRawHeightmap(std::string(18, '\0')).is_success());
      CHECK(parseRawHeightmap(std::string(20, '\0')).is_error());
    }
  }

  SECTION("applyRawHeightmap")
  {
    SECTION("replaces the heights and keeps the terrain's vertical extent")
    {
      auto terrain = makeTerrain();
      const auto boundsBefore = terrainBounds(terrain);

      // A ramp along X from black to white in steps of 51, which divide 255 exactly.
      const auto data = make8Bit(6, [](auto column, auto) { return column * 51; });
      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());
      REQUIRE(heightmap.value().size == 6u);

      REQUIRE(applyRawHeightmap(terrain, heightmap.value()));

      // The samples span the extent the terrain already had, from just above the base
      // to its old top, so the footprint and the height are unchanged.
      CHECK(terrainBounds(terrain).min == vm::approx{boundsBefore.min});
      CHECK(terrainBounds(terrain).max == vm::approx{boundsBefore.max});
      // Black sits one unit above the base plane and white at the terrain's old top.
      CHECK(terrain.heights[terrainVertexIndex(terrain, 0, 0)] == vm::approx{1.0});
      CHECK(terrain.heights[terrainVertexIndex(terrain, 4, 0)] == vm::approx{65.0});
      // The middle of the ramp lands half way up.
      CHECK(terrain.heights[terrainVertexIndex(terrain, 2, 0)] == vm::approx{33.0});
      // The ramp runs along X only, so every row is the same.
      CHECK(
        terrain.heights[terrainVertexIndex(terrain, 2, 3)]
        == vm::approx{terrain.heights[terrainVertexIndex(terrain, 2, 0)]});
    }

    SECTION("resamples a height map whose resolution differs from the terrain's")
    {
      auto terrain = makeTerrain(2, 2);

      // An 8x8 height map onto a terrain with a 3x3 vertex grid.
      const auto data =
        make8Bit(8, [](auto column, auto) { return column == 0 ? 0 : 255; });
      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());
      REQUIRE(heightmap.value().size == 8u);

      REQUIRE(applyRawHeightmap(terrain, heightmap.value()));

      CHECK(terrain.heights.size() == terrainVertexCount(terrain));
      // The height map's corners land on the terrain's corners whatever its resolution.
      CHECK(terrain.heights[terrainVertexIndex(terrain, 0, 0)] == vm::approx{1.0});
      CHECK(terrain.heights[terrainVertexIndex(terrain, 2, 0)] == vm::approx{65.0});
      CHECK(terrain.heights[terrainVertexIndex(terrain, 2, 2)] == vm::approx{65.0});
    }

    SECTION("no vertex is left below the base plane")
    {
      auto terrain = makeTerrain();
      const auto data = make8Bit(4, [](auto, auto) { return 0; });
      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());

      REQUIRE(applyRawHeightmap(terrain, heightmap.value()));

      // An all black height map still has to leave the prisms some thickness.
      for (const auto height : terrain.heights)
      {
        CHECK(height >= terrain.origin.z() + TerrainMinThickness);
      }
    }

    SECTION("rejects an empty height map")
    {
      auto terrain = makeTerrain();
      CHECK(!applyRawHeightmap(terrain, RawHeightmap{}));
    }
  }
}

} // namespace tb::mdl

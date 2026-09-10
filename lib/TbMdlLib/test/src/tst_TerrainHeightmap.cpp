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
#include <cstdint>
#include <cstring>

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

/** A little endian 32 bit float height map of the given side length. */
std::string makeFloat32(const size_t size, const auto& sample)
{
  auto data = std::string{};
  for (size_t row = 0; row < size; ++row)
  {
    for (size_t column = 0; column < size; ++column)
    {
      const auto value = float(sample(column, row));
      auto bits = uint32_t{};
      std::memcpy(&bits, &value, sizeof(bits));
      for (size_t byte = 0; byte < 4; ++byte)
      {
        data += char(static_cast<unsigned char>((bits >> (8 * byte)) & 0xffu));
      }
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

/** Whether a file of the given length could be read as a square of 8 bit samples. */
bool parseRawHeightmapFitsAsInt8(const size_t length)
{
  const auto root = size_t(std::llround(std::sqrt(double(length))));
  return root * root == length;
}

/** Whether a file of the given length could be read as a square of 32 bit samples. */
bool parseRawHeightmapFitsAsFloat32(const size_t length)
{
  return length % 4 == 0 && parseRawHeightmapFitsAsInt8(length / 4);
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
      CHECK(heightmap.value().format == RawSampleFormat::Int8);
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
      CHECK(heightmap.value().format == RawSampleFormat::Int16);
      CHECK(heightmap.value().samples.size() == 4u);
      CHECK(heightmap.value().samples[0] == vm::approx{0.0});
      CHECK(heightmap.value().samples[1] == vm::approx{1.0});
    }

    SECTION("reads 32 bit floats, whose range comes from the file itself")
    {
      // Heights in metres rather than normalized, which floats are just as likely to be.
      const auto data = makeFloat32(4, [](auto column, auto) { return 100.0 * column; });

      const auto heightmap = parseRawHeightmap(data);
      REQUIRE(heightmap.is_success());
      CHECK(heightmap.value().size == 4u);
      CHECK(heightmap.value().format == RawSampleFormat::Float32);
      // Floats carry no defined range, so the file's own lowest and highest samples
      // become 0 and 1.
      CHECK(heightmap.value().samples[0] == vm::approx{0.0});
      CHECK(heightmap.value().samples[1] == vm::approx{1.0 / 3.0});
      CHECK(heightmap.value().samples[3] == vm::approx{1.0});
    }

    SECTION("a 16 bit length can be nothing else")
    {
      // Twice a perfect square is never a perfect square, nor four times one, as either
      // would make the square root of two rational.
      for (size_t side = 1; side <= 512; ++side)
      {
        const auto length = 2 * side * side;
        CHECK(!parseRawHeightmapFitsAsInt8(length));
        CHECK(!parseRawHeightmapFitsAsFloat32(length));
      }
    }

    SECTION("32 bit floats are told from 8 bit samples by their contents")
    {
      // A square of 32 bit samples is four times a perfect square, which is always a
      // perfect square too, so the length alone cannot decide. A 64x64 float height map
      // is exactly as long as a 128x128 8 bit one.
      const auto floats = makeFloat32(
        64, [](auto column, auto row) { return double(column) + double(row) * 0.5; });
      const auto bytes =
        make8Bit(128, [](auto column, auto row) { return (column * 2 + row) % 256; });
      REQUIRE(floats.size() == bytes.size());

      const auto asFloats = parseRawHeightmap(floats);
      REQUIRE(asFloats.is_success());
      CHECK(asFloats.value().format == RawSampleFormat::Float32);
      CHECK(asFloats.value().size == 64u);

      const auto asBytes = parseRawHeightmap(bytes);
      REQUIRE(asBytes.is_success());
      CHECK(asBytes.value().format == RawSampleFormat::Int8);
      CHECK(asBytes.value().size == 128u);
    }

    SECTION("an explicit format overrides what the contents suggest")
    {
      const auto data = makeFloat32(4, [](auto column, auto) { return double(column); });

      const auto forced = parseRawHeightmap(data, RawSampleFormat::Int8);
      REQUIRE(forced.is_success());
      CHECK(forced.value().format == RawSampleFormat::Int8);
      CHECK(forced.value().size == 8u);

      // A length that does not fit the requested format is refused rather than guessed.
      CHECK(parseRawHeightmap(data, RawSampleFormat::Int16).is_error());
    }

    SECTION("rawSampleFormatForExtension")
    {
      CHECK(rawSampleFormatForExtension("terrain.r8") == RawSampleFormat::Int8);
      CHECK(rawSampleFormatForExtension("terrain.r16") == RawSampleFormat::Int16);
      CHECK(rawSampleFormatForExtension("terrain.R16") == RawSampleFormat::Int16);
      CHECK(rawSampleFormatForExtension("terrain.r32") == RawSampleFormat::Float32);
      CHECK(rawSampleFormatForExtension("terrain.f32") == RawSampleFormat::Float32);
      CHECK(rawSampleFormatForExtension("terrain.flt") == RawSampleFormat::Float32);
      // .raw says nothing, so the file has to be judged by its contents.
      CHECK(rawSampleFormatForExtension("terrain.raw") == std::nullopt);
    }

    SECTION("rejects lengths that fit no format")
    {
      CHECK(parseRawHeightmap("").is_error());
      CHECK(parseRawHeightmap(std::string(5, '\0')).is_error());
      // 18 bytes is nine 16 bit samples, so a 3x3 height map.
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

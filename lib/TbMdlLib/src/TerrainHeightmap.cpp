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

#include "mdl/TerrainHeightmap.h"

#include "mdl/Terrain.h"

#include "vm/scalar.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdint>

namespace tb::mdl
{
namespace
{

/** The integer square root of the given number, or nullopt if it is not a square. */
std::optional<size_t> exactSquareRoot(const size_t value)
{
  if (value == 0)
  {
    return std::nullopt;
  }

  const auto root = size_t(std::llround(std::sqrt(double(value))));
  return root * root == value ? std::optional{root} : std::nullopt;
}

/** A height map sample at fractional coordinates, interpolated from its four neighbours
 * the way the terrain's own resampling does. */
double sampleHeightmap(
  const RawHeightmap& heightmap, const double column, const double row)
{
  const auto lower = [&](const double coordinate) {
    const auto raw = std::llround(std::floor(coordinate));
    return size_t(vm::clamp(raw, 0ll, static_cast<long long>(heightmap.size) - 1));
  };

  const auto column0 = lower(column);
  const auto row0 = lower(row);
  const auto column1 = vm::min(column0 + 1, heightmap.size - 1);
  const auto row1 = vm::min(row0 + 1, heightmap.size - 1);

  const auto u = vm::clamp(column - double(column0), 0.0, 1.0);
  const auto v = vm::clamp(row - double(row0), 0.0, 1.0);

  const auto at = [&](const size_t c, const size_t r) {
    return heightmap.samples[r * heightmap.size + c];
  };

  return (at(column0, row0) * (1.0 - u) + at(column1, row0) * u) * (1.0 - v)
         + (at(column0, row1) * (1.0 - u) + at(column1, row1) * u) * v;
}

} // namespace

Result<RawHeightmap> parseRawHeightmap(const std::string_view data)
{
  // A .raw file has no header at all, so its dimensions can only come from its length.
  // A length cannot be both a perfect square and twice one - that would need sqrt(2) to
  // be rational - so the sample width follows from the length without ambiguity.
  if (const auto size = exactSquareRoot(data.size()))
  {
    auto heightmap = RawHeightmap{*size, false, {}};
    heightmap.samples.reserve(data.size());
    for (const auto c : data)
    {
      heightmap.samples.push_back(double(static_cast<unsigned char>(c)) / 255.0);
    }
    return heightmap;
  }

  if (data.size() % 2 == 0)
  {
    if (const auto size = exactSquareRoot(data.size() / 2))
    {
      // Little endian, which is what the tools that write raw height maps produce.
      auto heightmap = RawHeightmap{*size, true, {}};
      heightmap.samples.reserve(data.size() / 2);
      for (size_t i = 0; i + 1 < data.size(); i += 2)
      {
        const auto low = uint16_t(static_cast<unsigned char>(data[i]));
        const auto high = uint16_t(static_cast<unsigned char>(data[i + 1]));
        heightmap.samples.push_back(double(uint16_t(low | (high << 8))) / 65535.0);
      }
      return heightmap;
    }
  }

  return Error{fmt::format(
    "{} bytes is neither a square of 8 bit samples nor of 16 bit samples; only square "
    "raw height maps can be read",
    data.size())};
}

bool applyRawHeightmap(Terrain& terrain, const RawHeightmap& heightmap)
{
  if (
    !isValidTerrain(terrain) || heightmap.size == 0
    || heightmap.samples.size() != heightmap.size * heightmap.size)
  {
    return false;
  }

  // The samples are spread over the vertical extent the terrain already has, so
  // importing a height map does not resize it; the Scale mode is there for that.
  const auto bounds = terrainBounds(terrain);
  const auto minHeight = bounds.min.z() + TerrainMinThickness;
  const auto range = vm::max(bounds.max.z() - minHeight, 0.0);

  // The height map is resampled onto the terrain's grid, so the two do not have to have
  // the same resolution.
  const auto last = double(heightmap.size - 1);
  for (size_t row = 0; row <= terrain.rows; ++row)
  {
    const auto sourceRow = double(row) / double(terrain.rows) * last;
    for (size_t column = 0; column <= terrain.columns; ++column)
    {
      const auto sourceColumn = double(column) / double(terrain.columns) * last;
      terrain.heights[terrainVertexIndex(terrain, column, row)] =
        minHeight + sampleHeightmap(heightmap, sourceColumn, sourceRow) * range;
    }
  }

  return true;
}

} // namespace tb::mdl

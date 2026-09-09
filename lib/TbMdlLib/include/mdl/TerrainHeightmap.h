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

#pragma once

#include "Result.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tb::mdl
{
struct Terrain;

/**
 * A square height map read from a headerless .raw file.
 *
 * A .raw file is nothing but its samples, so its dimensions have to be deduced from its
 * length. Only square height maps can be deduced, which is what the tools that write
 * them produce.
 */
struct RawHeightmap
{
  /** The number of samples along each side. */
  size_t size = 0;
  /** Whether the samples were 16 bits wide rather than 8. */
  bool sixteenBit = false;
  /** One sample per point, normalized to [0, 1], row by row. */
  std::vector<double> samples;
};

/**
 * Reads a headerless .raw height map.
 *
 * The sample width is deduced from the file's length: a length that is a perfect square
 * is read as 8 bit samples, and one that is twice a perfect square as little endian 16
 * bit samples. No length can be both, so the two never have to be told apart by asking.
 *
 * Returns an error if the length fits neither, which is what a non-square height map or
 * a file that is not a raw height map at all looks like.
 */
Result<RawHeightmap> parseRawHeightmap(std::string_view data);

/**
 * Replaces the terrain's heights with the given height map.
 *
 * The height map is resampled bilinearly onto the terrain's grid, so its resolution does
 * not have to match, and its samples are spread over the terrain's current vertical
 * extent, leaving the terrain the size it already is. Use the Scale mode to make it
 * taller or shorter afterwards.
 *
 * Returns whether the terrain and the height map were usable.
 */
bool applyRawHeightmap(Terrain& terrain, const RawHeightmap& heightmap);

} // namespace tb::mdl

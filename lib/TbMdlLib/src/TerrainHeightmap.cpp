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

#include "kd/string_compare.h"

#include "vm/scalar.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace tb::mdl
{
namespace
{

/** The number of bytes one sample of the given format takes. */
size_t sampleWidth(const RawSampleFormat format)
{
  switch (format)
  {
  case RawSampleFormat::Int16:
    return 2;
  case RawSampleFormat::Float32:
    return 4;
  case RawSampleFormat::Int8:
    break;
  }
  return 1;
}

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

/** The side length a square height map of the given format would have, if the data is
 * exactly the right length for one. */
std::optional<size_t> sideLength(const size_t byteCount, const RawSampleFormat format)
{
  const auto width = sampleWidth(format);
  return byteCount % width == 0 ? exactSquareRoot(byteCount / width) : std::nullopt;
}

uint32_t readLittleEndian(
  const std::string_view data, const size_t offset, const size_t bytes)
{
  auto value = uint32_t(0);
  for (size_t i = 0; i < bytes; ++i)
  {
    value |= uint32_t(static_cast<unsigned char>(data[offset + i])) << (8 * i);
  }
  return value;
}

float readFloat(const std::string_view data, const size_t offset)
{
  const auto bits = readLittleEndian(data, offset, 4);
  auto value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * Whether the data reads as plausible 32 bit floats: nothing but finite, normal values
 * of a magnitude a height could have.
 *
 * This is what tells a 32 bit height map from an 8 bit one, whose length is always the
 * same as some 32 bit height map's. Real 8 bit height map data fails it comfortably,
 * because bytes in the range image data occupies land in the float's exponent and come
 * out as denormals, infinities or absurd magnitudes.
 */
bool looksLikeFloat32(const std::string_view data)
{
  for (size_t offset = 0; offset + 4 <= data.size(); offset += 4)
  {
    const auto value = readFloat(data, offset);
    switch (std::fpclassify(value))
    {
    case FP_NAN:
    case FP_INFINITE:
    case FP_SUBNORMAL:
      return false;
    default:
      break;
    }

    if (std::fabs(value) > 1.0e7f)
    {
      return false;
    }
  }
  return true;
}

/** The format the data must be in, judged by its length and, where the length leaves a
 * choice, by its contents. */
std::optional<RawSampleFormat> deduceFormat(const std::string_view data)
{
  // Twice a perfect square can be nothing but 16 bit samples: it is never a perfect
  // square itself, nor four times one, as either would make the square root of two
  // rational.
  if (sideLength(data.size(), RawSampleFormat::Int16))
  {
    return RawSampleFormat::Int16;
  }

  // A square of 32 bit samples is four times a perfect square, which is a perfect square
  // as well, so it looks exactly like an 8 bit height map of twice the side. Only the
  // bytes can tell them apart.
  if (sideLength(data.size(), RawSampleFormat::Float32) && looksLikeFloat32(data))
  {
    return RawSampleFormat::Float32;
  }

  if (sideLength(data.size(), RawSampleFormat::Int8))
  {
    return RawSampleFormat::Int8;
  }

  return std::nullopt;
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

std::string describeRawSampleFormat(const RawSampleFormat format)
{
  switch (format)
  {
  case RawSampleFormat::Int16:
    return "16 bit";
  case RawSampleFormat::Float32:
    return "32 bit float";
  case RawSampleFormat::Int8:
    break;
  }
  return "8 bit";
}

std::optional<RawSampleFormat> rawSampleFormatForExtension(
  const std::filesystem::path& path)
{
  const auto extension = path.extension().string();
  if (kdl::ci::str_is_equal(extension, ".r8"))
  {
    return RawSampleFormat::Int8;
  }
  if (kdl::ci::str_is_equal(extension, ".r16"))
  {
    return RawSampleFormat::Int16;
  }
  if (
    kdl::ci::str_is_equal(extension, ".r32") || kdl::ci::str_is_equal(extension, ".f32")
    || kdl::ci::str_is_equal(extension, ".flt"))
  {
    return RawSampleFormat::Float32;
  }
  return std::nullopt;
}

Result<RawHeightmap> parseRawHeightmap(
  const std::string_view data, const std::optional<RawSampleFormat> format)
{
  const auto deduced = format ? format : deduceFormat(data);
  if (!deduced)
  {
    return Error{fmt::format(
      "{} bytes is not a square of 8 bit, 16 bit or 32 bit float samples; only square "
      "raw height maps can be read",
      data.size())};
  }

  const auto size = sideLength(data.size(), *deduced);
  if (!size)
  {
    return Error{fmt::format(
      "{} bytes is not a square of {} samples",
      data.size(),
      describeRawSampleFormat(*deduced))};
  }

  auto heightmap = RawHeightmap{*size, *deduced, {}};
  const auto count = *size * *size;
  heightmap.samples.reserve(count);

  switch (*deduced)
  {
  case RawSampleFormat::Int8:
    for (size_t i = 0; i < count; ++i)
    {
      heightmap.samples.push_back(double(readLittleEndian(data, i, 1)) / 255.0);
    }
    break;
  case RawSampleFormat::Int16:
    for (size_t i = 0; i < count; ++i)
    {
      heightmap.samples.push_back(double(readLittleEndian(data, i * 2, 2)) / 65535.0);
    }
    break;
  case RawSampleFormat::Float32: {
    // Floats carry no defined range - they may be normalized, or heights in metres, or
    // anything else - so the file's own range is what gets mapped onto the terrain.
    for (size_t i = 0; i < count; ++i)
    {
      heightmap.samples.push_back(double(readFloat(data, i * 4)));
    }

    const auto [min, max] = std::ranges::minmax(heightmap.samples);
    const auto range = max - min;
    for (auto& sample : heightmap.samples)
    {
      sample = range > 0.0 ? (sample - min) / range : 0.0;
    }
    break;
  }
  }

  return heightmap;
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

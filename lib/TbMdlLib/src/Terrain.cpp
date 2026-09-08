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

#include "kd/reflection_impl.h"

#include "vm/intersection.h"
#include "vm/scalar.h"
#include "vm/vec_ext.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>
#include <limits>
#include <utility>

namespace tb::mdl
{
namespace
{

/**
 * A smooth falloff in [0, 1]: full strength at the brush center, fading to zero at the
 * radius with a cosine curve, so repeated strokes blend instead of leaving edges.
 */
double falloff(const double distance, const double radius)
{
  if (radius <= 0.0 || distance >= radius)
  {
    return 0.0;
  }
  const auto t = distance / radius;
  return 0.5 * (1.0 + std::cos(t * vm::Cd::pi()));
}

/** The XY distance from a grid vertex to the brush center. */
double planarDistance(const vm::vec3d& vertex, const vm::vec3d& center)
{
  const auto dx = vertex.x() - center.x();
  const auto dy = vertex.y() - center.y();
  return std::sqrt(dx * dx + dy * dy);
}

/**
 * The inclusive range of grid vertices covered by a brush of the given radius, clamped
 * to the terrain. Used to avoid visiting the whole height field for a local edit.
 */
struct VertexRange
{
  size_t minColumn;
  size_t maxColumn;
  size_t minRow;
  size_t maxRow;
};

VertexRange vertexRangeInRadius(
  const Terrain& terrain, const vm::vec3d& center, const double radius)
{
  const auto index = [](
                       const double world,
                       const double origin,
                       const double cellSize,
                       const size_t count) {
    const auto raw = std::llround(std::floor((world - origin) / cellSize));
    return size_t(vm::clamp(raw, 0ll, static_cast<long long>(count)));
  };

  return VertexRange{
    index(center.x() - radius, terrain.origin.x(), terrain.cellSize, terrain.columns),
    index(
      center.x() + radius + terrain.cellSize,
      terrain.origin.x(),
      terrain.cellSize,
      terrain.columns),
    index(center.y() - radius, terrain.origin.y(), terrain.cellSize, terrain.rows),
    index(
      center.y() + radius + terrain.cellSize,
      terrain.origin.y(),
      terrain.cellSize,
      terrain.rows)};
}

/**
 * The height of the terrain's surface at fractional grid coordinates, interpolated
 * bilinearly from the four surrounding vertices. Used to resample the height field when
 * the terrain is scaled.
 */
double sampleHeight(const Terrain& terrain, const double column, const double row)
{
  const auto lower = [](const double coordinate, const size_t count) {
    const auto raw = std::llround(std::floor(coordinate));
    return size_t(vm::clamp(raw, 0ll, static_cast<long long>(count) - 1));
  };

  const auto column0 = lower(column, terrain.columns + 1);
  const auto row0 = lower(row, terrain.rows + 1);
  const auto column1 = column0 + 1;
  const auto row1 = row0 + 1;

  const auto u = vm::clamp(column - double(column0), 0.0, 1.0);
  const auto v = vm::clamp(row - double(row0), 0.0, 1.0);

  const auto h00 = terrain.heights[terrainVertexIndex(terrain, column0, row0)];
  const auto h10 = terrain.heights[terrainVertexIndex(terrain, column1, row0)];
  const auto h01 = terrain.heights[terrainVertexIndex(terrain, column0, row1)];
  const auto h11 = terrain.heights[terrainVertexIndex(terrain, column1, row1)];

  return (h00 * (1.0 - u) + h10 * u) * (1.0 - v) + (h01 * (1.0 - u) + h11 * u) * v;
}

/** The average height of a vertex's existing neighbours, used by the smooth brush. */
double neighbourAverage(
  const Terrain& terrain,
  const std::vector<double>& heights,
  const size_t column,
  const size_t row)
{
  auto sum = 0.0;
  auto count = 0;
  for (auto dc = -1; dc <= 1; ++dc)
  {
    for (auto dr = -1; dr <= 1; ++dr)
    {
      if (dc == 0 && dr == 0)
      {
        continue;
      }
      const auto c = std::ptrdiff_t(column) + dc;
      const auto r = std::ptrdiff_t(row) + dr;
      if (
        c < 0 || r < 0 || c > std::ptrdiff_t(terrain.columns)
        || r > std::ptrdiff_t(terrain.rows))
      {
        continue;
      }
      sum += heights[terrainVertexIndex(terrain, size_t(c), size_t(r))];
      ++count;
    }
  }
  return count > 0 ? sum / double(count)
                   : heights[terrainVertexIndex(terrain, column, row)];
}

/**
 * The parameter range in which the given ray overlaps the given bounds, or nullopt if
 * it misses them entirely.
 */
std::optional<std::pair<double, double>> clipRayToSlab(
  const vm::ray3d& ray, const vm::bbox3d& bounds)
{
  auto near = 0.0;
  auto far = std::numeric_limits<double>::max();

  for (size_t axis = 0; axis < 3; ++axis)
  {
    const auto direction = ray.direction[axis];
    const auto origin = ray.origin[axis];
    if (vm::abs(direction) < vm::Cd::almost_zero())
    {
      if (origin < bounds.min[axis] || origin > bounds.max[axis])
      {
        return std::nullopt;
      }
      continue;
    }

    auto t0 = (bounds.min[axis] - origin) / direction;
    auto t1 = (bounds.max[axis] - origin) / direction;
    if (t0 > t1)
    {
      std::swap(t0, t1);
    }

    near = vm::max(near, t0);
    far = vm::min(far, t1);
    if (near > far)
    {
      return std::nullopt;
    }
  }

  return std::pair{near, far};
}

} // namespace

kdl_reflect_impl(Terrain);

std::optional<Terrain> createTerrain(
  const vm::bbox3d& bounds, const double cellSize, std::string defaultMaterial)
{
  if (cellSize <= 0.0)
  {
    return std::nullopt;
  }

  const auto size = bounds.size();
  const auto columns = size_t(std::llround(size.x() / cellSize));
  const auto rows = size_t(std::llround(size.y() / cellSize));
  if (columns < 1 || rows < 1 || columns * rows > TerrainMaxCells)
  {
    return std::nullopt;
  }

  if (size.z() < TerrainMinThickness)
  {
    return std::nullopt;
  }

  auto terrain = Terrain{};
  terrain.origin = vm::vec3d{bounds.min.x(), bounds.min.y(), bounds.min.z()};
  terrain.cellSize = cellSize;
  terrain.columns = columns;
  terrain.rows = rows;
  terrain.defaultMaterial = std::move(defaultMaterial);
  terrain.heights.assign((columns + 1) * (rows + 1), bounds.max.z());
  terrain.materials.assign(columns * rows, std::string{});
  return terrain;
}

bool isValidTerrain(const Terrain& terrain)
{
  return terrain.columns > 0 && terrain.rows > 0 && terrain.cellSize > 0.0
         && terrain.heights.size() == terrainVertexCount(terrain)
         && terrain.materials.size() == terrainCellCount(terrain);
}

size_t terrainVertexCount(const Terrain& terrain)
{
  return (terrain.columns + 1) * (terrain.rows + 1);
}

size_t terrainCellCount(const Terrain& terrain)
{
  return terrain.columns * terrain.rows;
}

size_t terrainVertexIndex(const Terrain& terrain, const size_t column, const size_t row)
{
  return row * (terrain.columns + 1) + column;
}

vm::vec3d terrainVertexPosition(
  const Terrain& terrain, const size_t column, const size_t row)
{
  return vm::vec3d{
    terrain.origin.x() + double(column) * terrain.cellSize,
    terrain.origin.y() + double(row) * terrain.cellSize,
    terrain.heights[terrainVertexIndex(terrain, column, row)]};
}

vm::bbox3d terrainBounds(const Terrain& terrain)
{
  auto minZ = terrain.origin.z();
  auto maxZ = terrain.origin.z();
  for (const auto height : terrain.heights)
  {
    minZ = vm::min(minZ, height);
    maxZ = vm::max(maxZ, height);
  }

  return vm::bbox3d{
    vm::vec3d{terrain.origin.x(), terrain.origin.y(), minZ},
    vm::vec3d{
      terrain.origin.x() + double(terrain.columns) * terrain.cellSize,
      terrain.origin.y() + double(terrain.rows) * terrain.cellSize,
      maxZ}};
}

bool scaleTerrain(Terrain& terrain, const vm::bbox3d& bounds)
{
  if (!isValidTerrain(terrain))
  {
    return false;
  }

  // The cell size is kept, so scaling the footprint changes the terrain's resolution
  // rather than the size of its cells.
  const auto size = bounds.size();
  const auto columns = size_t(std::llround(size.x() / terrain.cellSize));
  const auto rows = size_t(std::llround(size.y() / terrain.cellSize));
  if (
    columns < 1 || rows < 1 || columns * rows > TerrainMaxCells
    || size.z() < TerrainMinThickness)
  {
    return false;
  }

  // Heights are absolute, so scaling in Z means mapping the old height range onto the
  // new one. Sculpting keeps every vertex at least TerrainMinThickness above the base,
  // so the old range is never empty, but a terrain read from a map file might be.
  const auto oldBounds = terrainBounds(terrain);
  const auto oldRangeZ = oldBounds.max.z() - oldBounds.min.z();
  const auto scaleZ = oldRangeZ > 0.0 ? size.z() / oldRangeZ : 0.0;
  const auto minHeight = bounds.min.z() + TerrainMinThickness;

  auto heights = std::vector<double>((columns + 1) * (rows + 1));
  for (size_t row = 0; row <= rows; ++row)
  {
    const auto sourceRow = double(row) / double(rows) * double(terrain.rows);
    for (size_t column = 0; column <= columns; ++column)
    {
      const auto sourceColumn =
        double(column) / double(columns) * double(terrain.columns);
      const auto height = sampleHeight(terrain, sourceColumn, sourceRow);
      const auto scaled = scaleZ > 0.0
                            ? bounds.min.z() + (height - oldBounds.min.z()) * scaleZ
                            : bounds.max.z();
      heights[row * (columns + 1) + column] = vm::max(scaled, minHeight);
    }
  }

  // Materials cannot be interpolated, so every new cell takes the material of the old
  // cell that covers the same part of the footprint.
  auto materials = std::vector<std::string>(columns * rows);
  for (size_t row = 0; row < rows; ++row)
  {
    const auto sourceRow = vm::min(
      terrain.rows - 1, size_t(double(row) / double(rows) * double(terrain.rows)));
    for (size_t column = 0; column < columns; ++column)
    {
      const auto sourceColumn = vm::min(
        terrain.columns - 1,
        size_t(double(column) / double(columns) * double(terrain.columns)));
      materials[row * columns + column] =
        terrain.materials[sourceRow * terrain.columns + sourceColumn];
    }
  }

  terrain.origin = vm::vec3d{bounds.min.x(), bounds.min.y(), bounds.min.z()};
  terrain.columns = columns;
  terrain.rows = rows;
  terrain.heights = std::move(heights);
  terrain.materials = std::move(materials);
  return true;
}

bool sculptTerrain(
  Terrain& terrain,
  const vm::vec3d& center,
  const double radius,
  const double strength,
  const TerrainSculptMode mode)
{
  if (!isValidTerrain(terrain) || radius <= 0.0)
  {
    return false;
  }

  // Smoothing reads the unmodified heights so that the result does not depend on the
  // order in which the vertices are visited; the other modes read each vertex once
  // before writing it, so they can work in place and skip copying the height field.
  const auto snapshot =
    mode == TerrainSculptMode::Smooth ? terrain.heights : std::vector<double>{};
  const auto& original = mode == TerrainSculptMode::Smooth ? snapshot : terrain.heights;
  const auto minHeight = terrain.origin.z() + TerrainMinThickness;

  // Only the vertices inside the brush's bounding square can be affected, so the whole
  // height field does not have to be visited.
  const auto [minColumn, maxColumn, minRow, maxRow] =
    vertexRangeInRadius(terrain, center, radius);

  auto changed = false;
  for (size_t row = minRow; row <= maxRow; ++row)
  {
    for (size_t column = minColumn; column <= maxColumn; ++column)
    {
      const auto index = terrainVertexIndex(terrain, column, row);
      const auto position = terrainVertexPosition(terrain, column, row);
      const auto weight = falloff(planarDistance(position, center), radius);
      if (weight <= 0.0)
      {
        continue;
      }

      const auto height = original[index];
      auto newHeight = height;
      switch (mode)
      {
      case TerrainSculptMode::Raise:
        newHeight = height + strength * weight;
        break;
      case TerrainSculptMode::Lower:
        newHeight = height - strength * weight;
        break;
      case TerrainSculptMode::Flatten:
        // Pull towards the height under the brush center, at a rate given by the
        // strength relative to a nominal step of 32 units.
        newHeight = vm::mix(height, center.z(), vm::min(1.0, strength / 32.0) * weight);
        break;
      case TerrainSculptMode::Smooth:
        newHeight = vm::mix(
          height,
          neighbourAverage(terrain, original, column, row),
          vm::min(1.0, strength / 32.0) * weight);
        break;
      }

      newHeight = vm::max(newHeight, minHeight);
      if (newHeight != height)
      {
        terrain.heights[index] = newHeight;
        changed = true;
      }
    }
  }

  return changed;
}

bool paintTerrain(
  Terrain& terrain,
  const vm::vec3d& center,
  const double radius,
  const std::string& materialName)
{
  if (!isValidTerrain(terrain) || radius <= 0.0)
  {
    return false;
  }

  const auto [minColumn, maxColumn, minRow, maxRow] =
    vertexRangeInRadius(terrain, center, radius);

  auto changed = false;
  for (size_t row = minRow; row < vm::min(maxRow + 1, terrain.rows); ++row)
  {
    for (size_t column = minColumn; column < vm::min(maxColumn + 1, terrain.columns);
         ++column)
    {
      // The cell's center, at the average height of its four corners.
      auto height = 0.0;
      for (const auto& [dc, dr] : {
             std::pair{size_t(0), size_t(0)},
             std::pair{size_t(1), size_t(0)},
             std::pair{size_t(1), size_t(1)},
             std::pair{size_t(0), size_t(1)},
           })
      {
        height += terrain.heights[terrainVertexIndex(terrain, column + dc, row + dr)];
      }

      const auto position = vm::vec3d{
        terrain.origin.x() + (double(column) + 0.5) * terrain.cellSize,
        terrain.origin.y() + (double(row) + 0.5) * terrain.cellSize,
        height * 0.25};

      if (planarDistance(position, center) <= radius)
      {
        auto& material = terrain.materials[row * terrain.columns + column];
        if (material != materialName)
        {
          material = materialName;
          changed = true;
        }
      }
    }
  }

  return changed;
}

const std::string& terrainCellMaterial(
  const Terrain& terrain, const size_t column, const size_t row)
{
  const auto& material = terrain.materials[row * terrain.columns + column];
  return material.empty() ? terrain.defaultMaterial : material;
}

std::optional<vm::vec3d> pickTerrain(const Terrain& terrain, const vm::ray3d& ray)
{
  if (!isValidTerrain(terrain))
  {
    return std::nullopt;
  }

  auto closest = std::optional<double>{};
  const auto consider = [&](const vm::vec3d& a, const vm::vec3d& b, const vm::vec3d& c) {
    if (const auto distance = vm::intersect_ray_triangle(ray, a, b, c))
    {
      if (!closest || *distance < *closest)
      {
        closest = *distance;
      }
    }
  };

  // Restrict the search to the cells the ray can actually cross: clip it to the slab
  // spanned by the terrain's heights and take the bounds of the resulting segment.
  const auto bounds = terrainBounds(terrain);
  const auto entry = clipRayToSlab(ray, bounds);
  if (!entry)
  {
    return std::nullopt;
  }

  const auto [near, far] = *entry;
  const auto p0 = vm::point_at_distance(ray, near);
  const auto p1 = vm::point_at_distance(ray, far);

  const auto cellIndex = [&](
                           const double world, const double origin, const size_t count) {
    const auto raw = std::llround(std::floor((world - origin) / terrain.cellSize));
    return size_t(vm::clamp(raw, 0ll, static_cast<long long>(count) - 1));
  };

  const auto minColumn = cellIndex(
    vm::min(p0.x(), p1.x()) - terrain.cellSize, terrain.origin.x(), terrain.columns);
  const auto maxColumn = cellIndex(
    vm::max(p0.x(), p1.x()) + terrain.cellSize, terrain.origin.x(), terrain.columns);
  const auto minRow = cellIndex(
    vm::min(p0.y(), p1.y()) - terrain.cellSize, terrain.origin.y(), terrain.rows);
  const auto maxRow = cellIndex(
    vm::max(p0.y(), p1.y()) + terrain.cellSize, terrain.origin.y(), terrain.rows);

  // The surface is the triangulated top of the height field; both triangles of every
  // candidate cell are tested against the ray.
  for (size_t row = minRow; row <= maxRow; ++row)
  {
    for (size_t column = minColumn; column <= maxColumn; ++column)
    {
      const auto v00 = terrainVertexPosition(terrain, column, row);
      const auto v10 = terrainVertexPosition(terrain, column + 1, row);
      const auto v11 = terrainVertexPosition(terrain, column + 1, row + 1);
      const auto v01 = terrainVertexPosition(terrain, column, row + 1);

      consider(v00, v10, v11);
      consider(v00, v11, v01);
    }
  }

  return closest ? std::optional{vm::point_at_distance(ray, *closest)} : std::nullopt;
}

} // namespace tb::mdl

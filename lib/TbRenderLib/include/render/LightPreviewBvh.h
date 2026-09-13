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

#include "vm/vec.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace tb::render
{

/**
 * The part of a triangle that ray traversal needs, kept apart from the shading data so
 * that traversal touches as few cache lines as possible. The two edge vectors are stored
 * rather than the remaining two corners because that is the form the intersection test
 * wants.
 */
struct PreviewTrianglePos
{
  vm::vec3f p0;
  vm::vec3f e1;
  vm::vec3f e2;

  vm::vec3f p1() const { return p0 + e1; }
  vm::vec3f p2() const { return p0 + e2; }
};

/**
 * A ray with the reciprocal of its direction precomputed for the slab test.
 *
 * The reciprocal is clamped rather than allowed to become infinite, so that a direction
 * component of zero yields a very large finite number instead of an infinity that would
 * turn into a NaN when multiplied by a zero sized slab.
 */
struct PreviewRay
{
  vm::vec3f origin;
  vm::vec3f direction;
  vm::vec3f inverseDirection;

  PreviewRay(const vm::vec3f& i_origin, const vm::vec3f& i_direction)
    : origin{i_origin}
    , direction{i_direction}
  {
    for (size_t i = 0; i < 3; ++i)
    {
      const auto d = i_direction[i];
      inverseDirection[i] = std::abs(d) > 1.0e-20f ? 1.0f / d
                            : d < 0.0f             ? -1.0e20f
                                                   : 1.0e20f;
    }
  }
};

/**
 * The hit filter of a query that has nothing to turn down, which is every query over
 * geometry the rays cannot see through.
 */
struct AcceptEveryHit
{
  bool operator()(uint32_t, float, float) const { return true; }
};

struct PreviewRayHit
{
  float distance = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
  uint32_t triangleIndex = 0;
  bool frontFace = true;
};

/**
 * One node of the bounding volume hierarchy. Interior nodes store the index of their
 * first child (the second follows immediately) and leaves store a range into the index
 * array; triangleCount tells the two apart.
 *
 * The fields are sized so that a node is exactly half a cache line, which is why the
 * triangle count is only sixteen bits: leaves are capped far below that anyway, and the
 * space buys room for the split axis, which is what lets traversal visit the nearer child
 * first.
 */
struct PreviewBvhNode
{
  vm::vec3f min;
  vm::vec3f max;
  uint32_t firstIndexOrLeftChild = 0;
  uint16_t triangleCount = 0;
  uint8_t axis = 0;
  uint8_t padding = 0;
};

/**
 * A bounding volume hierarchy over the map's triangles, built with a binned surface area
 * heuristic.
 *
 * A BVH rather than a BSP tree: a BSP would suit the axis aligned bulk of a Quake map,
 * but it needs to split the triangles that straddle a plane, and getting that robust
 * costs more than it saves here. A BVH bounds each triangle exactly once, builds in a
 * fraction of the time, and degrades gracefully on the parts of a map that are not axis
 * aligned, which is where a preview spends most of its rays anyway.
 */
class PreviewBvh
{
private:
  std::vector<PreviewBvhNode> m_nodes;
  std::vector<uint32_t> m_indices;

public:
  /**
   * Builds the hierarchy over the given triangles. Any previous contents are discarded.
   */
  void build(const std::vector<PreviewTrianglePos>& triangles);

  void clear();

  bool empty() const { return m_nodes.empty(); }

  size_t nodeCount() const { return m_nodes.size(); }

  const std::vector<uint32_t>& indices() const { return m_indices; }

  /**
   * Finds the closest triangle hit by the given ray within (0, maxDistance].
   *
   * The filter is called with a triangle index and decides whether that triangle takes
   * part in this query; it is how shadow rays ignore surfaces that light passes through.
   * Triangles are hit from either side, since a preview camera can end up behind a face.
   *
   * The hit filter is called only once a triangle has actually been hit, with the point
   * on it, so that a query can turn a hit down for something the triangle index alone
   * cannot answer: whether the texture has a hole there. Refusing a hit leaves the ray
   * to carry on as though the triangle were not in its way.
   */
  template <typename Filter, typename HitFilter = AcceptEveryHit>
  std::optional<PreviewRayHit> intersect(
    const std::vector<PreviewTrianglePos>& triangles,
    const PreviewRay& ray,
    const float maxDistance,
    const Filter& filter,
    const HitFilter& hitFilter = HitFilter{}) const
  {
    auto result = std::optional<PreviewRayHit>{};
    auto closest = maxDistance;

    traverse(ray, closest, [&](const uint32_t first, const uint32_t count) {
      for (uint32_t i = 0; i < count; ++i)
      {
        const auto triangleIndex = m_indices[first + i];
        if (!filter(triangleIndex))
        {
          continue;
        }

        float u = 0.0f;
        float v = 0.0f;
        float det = 0.0f;
        if (const auto distance =
              intersectTriangle(triangles[triangleIndex], ray, closest, u, v, det);
            distance && hitFilter(triangleIndex, u, v))
        {
          closest = *distance;
          result = PreviewRayHit{*distance, u, v, triangleIndex, det > 0.0f};
        }
      }
      return false;
    });

    return result;
  }

  /**
   * Returns whether any triangle accepted by both filters is hit within (0, maxDistance].
   * Traversal stops at the first hit, so this is much cheaper than intersect. See
   * intersect for what the two filters are each for.
   */
  template <typename Filter, typename HitFilter = AcceptEveryHit>
  bool occluded(
    const std::vector<PreviewTrianglePos>& triangles,
    const PreviewRay& ray,
    const float maxDistance,
    const Filter& filter,
    const HitFilter& hitFilter = HitFilter{}) const
  {
    auto found = false;
    auto limit = maxDistance;

    traverse(ray, limit, [&](const uint32_t first, const uint32_t count) {
      for (uint32_t i = 0; i < count; ++i)
      {
        const auto triangleIndex = m_indices[first + i];
        if (!filter(triangleIndex))
        {
          continue;
        }

        float u = 0.0f;
        float v = 0.0f;
        float det = 0.0f;
        if (
          intersectTriangle(triangles[triangleIndex], ray, limit, u, v, det)
          && hitFilter(triangleIndex, u, v))
        {
          found = true;
          return true;
        }
      }
      return false;
    });

    return found;
  }

private:
  /**
   * Walks the hierarchy, calling visitLeaf for every leaf whose box the ray enters.
   * visitLeaf returns true to stop traversal. maxDistance is read on every node test, so
   * a caller that shrinks it as it finds closer hits prunes the rest of the walk.
   */
  template <typename VisitLeaf>
  void traverse(
    const PreviewRay& ray, const float& maxDistance, const VisitLeaf& visitLeaf) const
  {
    if (m_nodes.empty())
    {
      return;
    }

    uint32_t stack[64];
    size_t stackSize = 0;
    uint32_t current = 0;

    while (true)
    {
      const auto& node = m_nodes[current];
      if (intersectBounds(node, ray, maxDistance))
      {
        if (node.triangleCount > 0)
        {
          if (visitLeaf(node.firstIndexOrLeftChild, node.triangleCount))
          {
            return;
          }
        }
        else
        {
          const auto left = node.firstIndexOrLeftChild;
          const auto right = left + 1;

          // Visit the nearer child first, so that the far one is more likely to be culled
          // by a hit found in the near one. The left child holds the smaller coordinates
          // along the split axis, so a ray heading that way should see it first.
          const auto nearFirst = ray.direction[node.axis] >= 0.0f;
          const auto first = nearFirst ? left : right;
          const auto second = nearFirst ? right : left;

          if (stackSize < std::size(stack))
          {
            stack[stackSize++] = second;
          }
          current = first;
          continue;
        }
      }

      if (stackSize == 0)
      {
        return;
      }
      current = stack[--stackSize];
    }
  }

  static bool intersectBounds(
    const PreviewBvhNode& node, const PreviewRay& ray, const float maxDistance)
  {
    auto tNear = 0.0f;
    auto tFar = maxDistance;

    for (size_t i = 0; i < 3; ++i)
    {
      const auto t0 = (node.min[i] - ray.origin[i]) * ray.inverseDirection[i];
      const auto t1 = (node.max[i] - ray.origin[i]) * ray.inverseDirection[i];
      const auto lo = t0 < t1 ? t0 : t1;
      const auto hi = t0 < t1 ? t1 : t0;
      tNear = lo > tNear ? lo : tNear;
      tFar = hi < tFar ? hi : tFar;
    }

    // A slack factor of a few ulps keeps a ray that grazes a shared edge from slipping
    // between two neighbouring boxes.
    return tNear <= tFar * 1.0000004f;
  }

  /**
   * Moeller-Trumbore, without back face culling: a preview camera can end up behind a
   * face, and shadow rays must be blocked by a wall whichever side they approach it from.
   * The sign of the determinant is handed back so that the caller can tell the two cases
   * apart and flip the shading normal to match.
   */
  static std::optional<float> intersectTriangle(
    const PreviewTrianglePos& triangle,
    const PreviewRay& ray,
    const float maxDistance,
    float& u,
    float& v,
    float& det)
  {
    const auto pv = vm::cross(ray.direction, triangle.e2);
    det = vm::dot(triangle.e1, pv);
    if (std::abs(det) < 1.0e-12f)
    {
      return std::nullopt;
    }

    const auto inverseDet = 1.0f / det;
    const auto tv = ray.origin - triangle.p0;

    u = vm::dot(tv, pv) * inverseDet;
    if (u < 0.0f || u > 1.0f)
    {
      return std::nullopt;
    }

    const auto qv = vm::cross(tv, triangle.e1);
    v = vm::dot(ray.direction, qv) * inverseDet;
    if (v < 0.0f || u + v > 1.0f)
    {
      return std::nullopt;
    }

    const auto distance = vm::dot(triangle.e2, qv) * inverseDet;
    if (distance <= 0.0f || distance > maxDistance)
    {
      return std::nullopt;
    }

    return distance;
  }
};

} // namespace tb::render

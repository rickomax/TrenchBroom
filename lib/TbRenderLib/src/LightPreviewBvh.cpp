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

#include "render/LightPreviewBvh.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>

namespace tb::render
{
namespace
{

constexpr auto BinCount = size_t(12);

/** A node this small cannot be improved by splitting it. */
constexpr auto MinTrianglesToSplit = uint32_t(3);

/**
 * The most triangles a leaf may hold.
 *
 * Without a cap, the cost heuristic alone will happily leave thousands of triangles in
 * one leaf: a room's walls are a handful of triangles whose bounds span everything, and
 * next to them no split looks like an improvement, since both halves inherit those same
 * bounds. Every ray then ends up testing that whole leaf. The cap is what keeps a few
 * outsized triangles from flattening the subtree around them.
 */
constexpr auto MaxTrianglesPerLeaf = uint32_t(8);

/**
 * What visiting a node costs relative to testing one triangle, which is what stops the
 * heuristic from splitting when there is nothing to gain.
 */
constexpr auto TraversalCost = 0.5f;

constexpr auto Infinity = std::numeric_limits<float>::infinity();

struct Bounds
{
  vm::vec3f min = vm::vec3f{Infinity, Infinity, Infinity};
  vm::vec3f max = vm::vec3f{-Infinity, -Infinity, -Infinity};

  void add(const vm::vec3f& point)
  {
    for (size_t i = 0; i < 3; ++i)
    {
      min[i] = std::min(min[i], point[i]);
      max[i] = std::max(max[i], point[i]);
    }
  }

  void add(const Bounds& other)
  {
    for (size_t i = 0; i < 3; ++i)
    {
      min[i] = std::min(min[i], other.min[i]);
      max[i] = std::max(max[i], other.max[i]);
    }
  }

  bool valid() const { return min.x() <= max.x(); }

  float surfaceArea() const
  {
    if (!valid())
    {
      return 0.0f;
    }
    const auto size = max - min;
    return 2.0f * (size.x() * size.y() + size.y() * size.z() + size.z() * size.x());
  }
};

Bounds triangleBounds(const PreviewTrianglePos& triangle)
{
  auto result = Bounds{};
  result.add(triangle.p0);
  result.add(triangle.p1());
  result.add(triangle.p2());
  return result;
}

vm::vec3f triangleCentroid(const PreviewTrianglePos& triangle)
{
  return triangle.p0 + (triangle.e1 + triangle.e2) / 3.0f;
}

struct Bin
{
  Bounds bounds;
  uint32_t count = 0;
};

struct BuildTask
{
  uint32_t nodeIndex;
  uint32_t first;
  uint32_t count;
};

} // namespace

void PreviewBvh::clear()
{
  m_nodes.clear();
  m_indices.clear();
}

void PreviewBvh::build(const std::vector<PreviewTrianglePos>& triangles)
{
  clear();

  if (triangles.empty())
  {
    return;
  }

  const auto triangleCount = uint32_t(triangles.size());

  m_indices.resize(triangleCount);
  std::iota(m_indices.begin(), m_indices.end(), uint32_t(0));

  auto centroids = std::vector<vm::vec3f>{};
  auto bounds = std::vector<Bounds>{};
  centroids.reserve(triangleCount);
  bounds.reserve(triangleCount);
  for (const auto& triangle : triangles)
  {
    centroids.push_back(triangleCentroid(triangle));
    bounds.push_back(triangleBounds(triangle));
  }

  // A balanced hierarchy over n triangles needs at most 2n - 1 nodes; reserving that up
  // front keeps the node vector from reallocating while a build task holds an index.
  m_nodes.reserve(size_t(2) * triangleCount);
  m_nodes.push_back(PreviewBvhNode{});

  auto tasks = std::vector<BuildTask>{BuildTask{0, 0, triangleCount}};
  tasks.reserve(64);

  while (!tasks.empty())
  {
    const auto task = tasks.back();
    tasks.pop_back();

    auto nodeBounds = Bounds{};
    auto centroidBounds = Bounds{};
    for (uint32_t i = 0; i < task.count; ++i)
    {
      const auto triangleIndex = m_indices[task.first + i];
      nodeBounds.add(bounds[triangleIndex]);
      centroidBounds.add(centroids[triangleIndex]);
    }

    {
      auto& node = m_nodes[task.nodeIndex];
      node.min = nodeBounds.min;
      node.max = nodeBounds.max;
    }

    const auto makeLeaf = [&]() {
      auto& node = m_nodes[task.nodeIndex];
      node.firstIndexOrLeftChild = task.first;
      node.triangleCount = uint16_t(task.count);
    };

    const auto makeInterior = [&](const uint32_t leftCount, const uint32_t splitAxis) {
      const auto leftChild = uint32_t(m_nodes.size());

      auto& node = m_nodes[task.nodeIndex];
      node.firstIndexOrLeftChild = leftChild;
      node.triangleCount = 0;
      node.axis = uint8_t(splitAxis);

      m_nodes.push_back(PreviewBvhNode{});
      m_nodes.push_back(PreviewBvhNode{});

      tasks.push_back(BuildTask{leftChild, task.first, leftCount});
      tasks.push_back(
        BuildTask{leftChild + 1, task.first + leftCount, task.count - leftCount});
    };

    if (task.count < MinTrianglesToSplit)
    {
      makeLeaf();
      continue;
    }

    const auto centroidSize = centroidBounds.max - centroidBounds.min;
    const auto axis = centroidSize.x() > centroidSize.y()
                        ? (centroidSize.x() > centroidSize.z() ? 0u : 2u)
                        : (centroidSize.y() > centroidSize.z() ? 1u : 2u);

    /**
     * Splits the range down the middle by centroid, which is what the builder falls back
     * on whenever the heuristic cannot separate the triangles.
     */
    const auto splitAtMedian = [&]() {
      const auto middle = task.count / 2u;
      const auto begin = m_indices.begin() + task.first;
      std::nth_element(
        begin,
        begin + middle,
        begin + task.count,
        [&](const uint32_t lhs, const uint32_t rhs) {
          return centroids[lhs][axis] < centroids[rhs][axis];
        });
      makeInterior(middle, axis);
    };

    if (centroidSize[axis] <= 0.0f)
    {
      // Every centroid falls on the same point, so no plane separates them. Halving the
      // range keeps a pile of coincident triangles from ending up in one huge leaf.
      makeInterior(task.count / 2u, axis);
      continue;
    }

    const auto binScale = float(BinCount) / centroidSize[axis];
    const auto binOf = [&](const uint32_t triangleIndex) {
      const auto offset = centroids[triangleIndex][axis] - centroidBounds.min[axis];
      const auto bin = size_t(offset * binScale);
      return std::min(bin, BinCount - 1);
    };

    auto bins = std::array<Bin, BinCount>{};
    for (uint32_t i = 0; i < task.count; ++i)
    {
      const auto triangleIndex = m_indices[task.first + i];
      auto& bin = bins[binOf(triangleIndex)];
      bin.bounds.add(bounds[triangleIndex]);
      ++bin.count;
    }

    // Sweep once from each end so that both sides of every candidate plane are known
    // without rescanning the bins for each of them.
    auto leftArea = std::array<float, BinCount - 1>{};
    auto leftCount = std::array<uint32_t, BinCount - 1>{};
    {
      auto accumulated = Bounds{};
      auto count = uint32_t(0);
      for (size_t i = 0; i < BinCount - 1; ++i)
      {
        accumulated.add(bins[i].bounds);
        count += bins[i].count;
        leftArea[i] = accumulated.surfaceArea();
        leftCount[i] = count;
      }
    }

    auto bestCost = std::numeric_limits<float>::max();
    auto bestSplit = size_t(0);
    {
      auto accumulated = Bounds{};
      auto count = uint32_t(0);
      for (size_t i = BinCount - 1; i > 0; --i)
      {
        accumulated.add(bins[i].bounds);
        count += bins[i].count;

        const auto cost = float(leftCount[i - 1]) * leftArea[i - 1]
                          + float(count) * accumulated.surfaceArea();
        if (cost < bestCost && leftCount[i - 1] > 0 && count > 0)
        {
          bestCost = cost;
          bestSplit = i;
        }
      }
    }

    // Stop splitting once a leaf would be small enough that visiting another node costs
    // more than testing what is already there. A node too large to be a leaf is split
    // whatever the heuristic says, since the alternative is a leaf every ray pays for.
    const auto leafCost = float(task.count) * nodeBounds.surfaceArea();
    const auto splitCost = TraversalCost * nodeBounds.surfaceArea() + bestCost;
    if (task.count <= MaxTrianglesPerLeaf && (bestSplit == 0 || splitCost >= leafCost))
    {
      makeLeaf();
      continue;
    }

    if (bestSplit == 0)
    {
      splitAtMedian();
      continue;
    }

    const auto middle = std::partition(
      m_indices.begin() + task.first,
      m_indices.begin() + task.first + task.count,
      [&](const uint32_t triangleIndex) { return binOf(triangleIndex) < bestSplit; });
    const auto leftCountFinal = uint32_t(middle - (m_indices.begin() + task.first));

    if (leftCountFinal == 0 || leftCountFinal == task.count)
    {
      splitAtMedian();
      continue;
    }

    makeInterior(leftCountFinal, axis);
  }
}

} // namespace tb::render

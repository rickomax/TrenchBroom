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
#include <cmath>
#include <optional>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::render
{
namespace
{

PreviewTrianglePos makeTriangle(
  const vm::vec3f& p0, const vm::vec3f& p1, const vm::vec3f& p2)
{
  return PreviewTrianglePos{p0, p1 - p0, p2 - p0};
}

const auto acceptAll = [](const uint32_t) { return true; };

/**
 * Tests every triangle in turn, spelled out rather than delegated, so that it is an
 * independent answer for the hierarchy to agree with.
 */
std::optional<PreviewRayHit> intersectExhaustively(
  const std::vector<PreviewTrianglePos>& triangles,
  const PreviewRay& ray,
  const float maxDistance)
{
  auto result = std::optional<PreviewRayHit>{};
  auto closest = maxDistance;

  for (uint32_t i = 0; i < uint32_t(triangles.size()); ++i)
  {
    const auto& triangle = triangles[i];

    const auto pv = vm::cross(ray.direction, triangle.e2);
    const auto det = vm::dot(triangle.e1, pv);
    if (std::abs(det) < 1.0e-12f)
    {
      continue;
    }

    const auto inverseDet = 1.0f / det;
    const auto tv = ray.origin - triangle.p0;

    const auto u = vm::dot(tv, pv) * inverseDet;
    if (u < 0.0f || u > 1.0f)
    {
      continue;
    }

    const auto qv = vm::cross(tv, triangle.e1);
    const auto v = vm::dot(ray.direction, qv) * inverseDet;
    if (v < 0.0f || u + v > 1.0f)
    {
      continue;
    }

    const auto distance = vm::dot(triangle.e2, qv) * inverseDet;
    if (distance <= 0.0f || distance > closest)
    {
      continue;
    }

    closest = distance;
    result = PreviewRayHit{distance, u, v, i, det > 0.0f};
  }

  return result;
}

std::vector<PreviewTrianglePos> makeTestGeometry()
{
  auto rng = std::mt19937{0x5eed};
  auto coordinate = std::uniform_real_distribution<float>{-500.0f, 500.0f};
  auto extent = std::uniform_real_distribution<float>{-60.0f, 60.0f};

  auto result = std::vector<PreviewTrianglePos>{};

  for (auto i = 0; i < 400; ++i)
  {
    const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
    result.push_back(makeTriangle(
      origin,
      origin + vm::vec3f{extent(rng), extent(rng), extent(rng)},
      origin + vm::vec3f{extent(rng), extent(rng), extent(rng)}));
  }

  // Axis aligned quads, which is what most of a map is made of and what the slab test has
  // the most trouble with.
  for (auto i = 0; i < 200; ++i)
  {
    const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
    result.push_back(
      makeTriangle(origin, origin + vm::vec3f{64, 0, 0}, origin + vm::vec3f{64, 64, 0}));
    result.push_back(
      makeTriangle(origin, origin + vm::vec3f{64, 64, 0}, origin + vm::vec3f{0, 64, 0}));
  }

  return result;
}

} // namespace

TEST_CASE("PreviewBvh")
{
  SECTION("build")
  {
    SECTION("an empty hierarchy hits nothing")
    {
      auto triangles = std::vector<PreviewTrianglePos>{};
      auto bvh = PreviewBvh{};
      bvh.build(triangles);

      CHECK(bvh.empty());

      const auto ray = PreviewRay{vm::vec3f{0, 0, 0}, vm::vec3f{1, 0, 0}};
      CHECK_FALSE(bvh.intersect(triangles, ray, 100.0f, acceptAll));
      CHECK_FALSE(bvh.occluded(triangles, ray, 100.0f, acceptAll));
    }

    SECTION("every triangle ends up in exactly one leaf")
    {
      const auto triangles = makeTestGeometry();

      auto bvh = PreviewBvh{};
      bvh.build(triangles);

      auto counts = std::vector<int>(triangles.size(), 0);
      for (const auto index : bvh.indices())
      {
        REQUIRE(index < counts.size());
        ++counts[index];
      }

      CHECK(bvh.indices().size() == triangles.size());
      CHECK(std::ranges::all_of(counts, [](const auto count) { return count == 1; }));
    }
  }

  SECTION("intersect")
  {
    const auto triangles = makeTestGeometry();
    auto bvh = PreviewBvh{};
    bvh.build(triangles);

    SECTION("finds the same closest hit as testing every triangle")
    {
      auto rng = std::mt19937{0xf00d};
      auto coordinate = std::uniform_real_distribution<float>{-500.0f, 500.0f};

      auto hits = 0;
      for (auto i = 0; i < 400; ++i)
      {
        const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        const auto target = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        if (vm::squared_length(target - origin) < 1.0f)
        {
          continue;
        }

        const auto ray = PreviewRay{origin, vm::normalize(target - origin)};
        const auto actual = bvh.intersect(triangles, ray, 1.0e6f, acceptAll);
        const auto expected = intersectExhaustively(triangles, ray, 1.0e6f);

        REQUIRE(actual.has_value() == expected.has_value());
        if (actual)
        {
          ++hits;
          CHECK(actual->distance == Catch::Approx(expected->distance).margin(1.0e-3));
        }
      }

      CHECK(hits > 0);
    }

    SECTION("handles rays with a zero direction component")
    {
      auto rng = std::mt19937{0xbeef};
      auto coordinate = std::uniform_real_distribution<float>{-500.0f, 500.0f};

      for (auto i = 0; i < 300; ++i)
      {
        const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};

        auto direction = vm::vec3f{0, 0, 0};
        direction[size_t(i % 3)] = (i % 2) == 0 ? 1.0f : -1.0f;

        const auto ray = PreviewRay{origin, direction};
        const auto actual = bvh.intersect(triangles, ray, 1.0e6f, acceptAll);
        const auto expected = intersectExhaustively(triangles, ray, 1.0e6f);

        REQUIRE(actual.has_value() == expected.has_value());
        if (actual)
        {
          CHECK(actual->distance == Catch::Approx(expected->distance).margin(1.0e-3));
        }
      }
    }

    SECTION("skips the triangles the filter rejects")
    {
      auto rng = std::mt19937{0xcafe};
      auto coordinate = std::uniform_real_distribution<float>{-500.0f, 500.0f};

      const auto evenOnly = [](const uint32_t index) { return index % 2 == 0; };

      for (auto i = 0; i < 300; ++i)
      {
        const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        const auto target = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        if (vm::squared_length(target - origin) < 1.0f)
        {
          continue;
        }

        const auto ray = PreviewRay{origin, vm::normalize(target - origin)};
        if (const auto hit = bvh.intersect(triangles, ray, 1.0e6f, evenOnly))
        {
          CHECK(hit->triangleIndex % 2 == 0);
        }
      }
    }

    SECTION("hits a triangle from either side")
    {
      auto triangle =
        std::vector<PreviewTrianglePos>{makeTriangle({0, 0, 0}, {10, 0, 0}, {0, 10, 0})};

      auto singleBvh = PreviewBvh{};
      singleBvh.build(triangle);

      const auto fromBelow = singleBvh.intersect(
        triangle, PreviewRay{{1, 1, -5}, {0, 0, 1}}, 100.0f, acceptAll);
      const auto fromAbove = singleBvh.intersect(
        triangle, PreviewRay{{1, 1, 5}, {0, 0, -1}}, 100.0f, acceptAll);

      REQUIRE(fromBelow);
      REQUIRE(fromAbove);
      CHECK(fromBelow->distance == Catch::Approx(5.0).margin(1.0e-4));
      CHECK(fromAbove->distance == Catch::Approx(5.0).margin(1.0e-4));
      CHECK(fromBelow->frontFace != fromAbove->frontFace);
    }

    SECTION("misses a triangle behind the ray")
    {
      auto triangle =
        std::vector<PreviewTrianglePos>{makeTriangle({0, 0, 0}, {10, 0, 0}, {0, 10, 0})};

      auto singleBvh = PreviewBvh{};
      singleBvh.build(triangle);

      CHECK_FALSE(singleBvh.intersect(
        triangle, PreviewRay{{1, 1, -5}, {0, 0, -1}}, 100.0f, acceptAll));
    }
  }

  SECTION("occluded")
  {
    const auto triangles = makeTestGeometry();
    auto bvh = PreviewBvh{};
    bvh.build(triangles);

    SECTION("agrees with intersect about whether anything is in the way")
    {
      auto rng = std::mt19937{0xd00d};
      auto coordinate = std::uniform_real_distribution<float>{-500.0f, 500.0f};

      for (auto i = 0; i < 400; ++i)
      {
        const auto origin = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        const auto target = vm::vec3f{coordinate(rng), coordinate(rng), coordinate(rng)};
        if (vm::squared_length(target - origin) < 1.0f)
        {
          continue;
        }

        const auto ray = PreviewRay{origin, vm::normalize(target - origin)};
        const auto hit = bvh.intersect(triangles, ray, 1.0e6f, acceptAll);

        if (hit)
        {
          CHECK(bvh.occluded(triangles, ray, hit->distance * 1.001f, acceptAll));
          CHECK_FALSE(bvh.occluded(triangles, ray, hit->distance * 0.999f, acceptAll));
        }
        else
        {
          CHECK_FALSE(bvh.occluded(triangles, ray, 1.0e6f, acceptAll));
        }
      }
    }
  }
}

} // namespace tb::render

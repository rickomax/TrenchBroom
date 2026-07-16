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

#include "mdl/Spline.h"

#include "vm/approx.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("Spline")
{
  SECTION("sampleSpline")
  {
    SECTION("returns empty vector for fewer than two points")
    {
      CHECK(sampleSpline({}, 8).empty());
      CHECK(sampleSpline({SplinePoint{vm::vec3d{0, 0, 0}}}, 8).empty());
    }

    SECTION("interpolates the control points")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{64, 0, 0}},
        SplinePoint{vm::vec3d{128, 64, 0}},
      };

      const auto samples = sampleSpline(points, 4);
      REQUIRE(samples.size() == 9);

      CHECK(samples.front() == vm::approx{points.front().position});
      CHECK(samples[4] == vm::approx{points[1].position});
      CHECK(samples.back() == vm::approx{points.back().position});
    }

    SECTION("a straight spline samples along the line")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
      };

      const auto samples = sampleSpline(points, 2);
      REQUIRE(samples.size() == 3);
      CHECK(samples[0] == vm::approx{vm::vec3d{0, 0, 0}});
      CHECK(samples[1] == vm::approx{vm::vec3d{64, 0, 0}});
      CHECK(samples[2] == vm::approx{vm::vec3d{128, 0, 0}});
    }
  }

  SECTION("computeSplineFrames")
  {
    SECTION("returns empty vector for fewer than two points")
    {
      CHECK(computeSplineFrames({}, 8).empty());
      CHECK(computeSplineFrames({SplinePoint{vm::vec3d{0, 0, 0}}}, 8).empty());
    }

    SECTION("straight spline along the X axis produces identity frames")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
      };

      const auto frames = computeSplineFrames(points, 4);
      REQUIRE(frames.size() == 5);

      for (const auto& frame : frames)
      {
        CHECK(frame.tangent == vm::approx{vm::vec3d{1, 0, 0}});
        CHECK(frame.normal == vm::approx{vm::vec3d{0, 1, 0}});
        CHECK(frame.binormal == vm::approx{vm::vec3d{0, 0, 1}});
      }

      CHECK(frames.front().arcLength == vm::approx{0.0});
      CHECK(frames.back().arcLength == vm::approx{128.0});
    }

    SECTION("roll rotates the frame around the tangent")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}, 90.0},
        SplinePoint{vm::vec3d{128, 0, 0}, 90.0},
      };

      const auto frames = computeSplineFrames(points, 4);
      REQUIRE(frames.size() == 5);

      for (const auto& frame : frames)
      {
        CHECK(frame.tangent == vm::approx{vm::vec3d{1, 0, 0}});
        CHECK(frame.normal == vm::approx{vm::vec3d{0, 0, 1}});
        CHECK(frame.binormal == vm::approx{vm::vec3d{0, -1, 0}});
      }
    }

    SECTION("frames stay continuous around a bend")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
        SplinePoint{vm::vec3d{256, 128, 0}},
      };

      const auto frames = computeSplineFrames(points, 8);
      REQUIRE(frames.size() == 17);

      for (size_t i = 1; i < frames.size(); ++i)
      {
        // Consecutive normals must not flip.
        CHECK(vm::dot(frames[i - 1].normal, frames[i].normal) > 0.5);
        // Frames must remain orthonormal.
        CHECK(vm::dot(frames[i].tangent, frames[i].normal) == vm::approx{0.0});
        CHECK(vm::length(frames[i].normal) == vm::approx{1.0});
        CHECK(vm::length(frames[i].binormal) == vm::approx{1.0});
      }
    }

    SECTION("arc length increases monotonically")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{64, 64, 32}},
        SplinePoint{vm::vec3d{128, 0, 64}},
      };

      const auto frames = computeSplineFrames(points, 8);
      for (size_t i = 1; i < frames.size(); ++i)
      {
        CHECK(frames[i].arcLength > frames[i - 1].arcLength);
      }

      CHECK(splineLength(frames) == vm::approx{frames.back().arcLength});
    }
  }
}

} // namespace tb::mdl

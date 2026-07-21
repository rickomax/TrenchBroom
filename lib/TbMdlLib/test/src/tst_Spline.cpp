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

    SECTION("a segment between two plane locked points stays in the locked plane")
    {
      // Two XY locked points on a level line, with lifted end points and a sideways
      // curve: the locked segment must stay level (z constant), but keep curving
      // smoothly in the horizontal plane.
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{-64, -64, 512}},
        SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0, SplineLock::XY},
        SplinePoint{vm::vec3d{128, 64, 0}, 0.0, 1.0, SplineLock::XY},
        SplinePoint{vm::vec3d{192, 128, 512}},
      };

      const auto subdivisions = size_t{8};
      const auto samples = sampleSpline(points, subdivisions);
      REQUIRE(samples.size() == 3 * subdivisions + 1);

      // The samples of the middle segment lie between indices 8 and 16 and must stay
      // level despite the lifted end points.
      for (size_t i = subdivisions; i <= 2 * subdivisions; ++i)
      {
        CHECK(samples[i].z() == vm::approx{0.0});
      }

      // The middle segment still curves in the horizontal plane: it deviates from
      // the straight chord between the two locked points (the chord runs along
      // y = x / 2).
      const auto quarterPoint = curvePoint(points, 1, 0.25);
      CHECK(vm::abs(quarterPoint.y() - quarterPoint.x() / 2.0) > 1.0);

      // The curve is C1 continuous at the locked points: both touching segments
      // share the flattened tangent, so the ramps level into the plane.
      CHECK(curveTangent(points, 0, 1.0) == vm::approx{curveTangent(points, 1, 0.0)});
      CHECK(curveTangent(points, 1, 1.0) == vm::approx{curveTangent(points, 2, 0.0)});
      CHECK(curveTangent(points, 1, 0.0).z() == vm::approx{0.0});
      CHECK(curveTangent(points, 1, 1.0).z() == vm::approx{0.0});
    }

    SECTION("a closed spline wraps back around to the first point")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
        SplinePoint{vm::vec3d{128, 128, 0}},
        SplinePoint{vm::vec3d{0, 128, 0}},
      };

      const auto samples = sampleSpline(points, 4, true);
      REQUIRE(samples.size() == 4 * 4 + 1);

      // One segment per control point, ending back on the first point.
      CHECK(samples.front() == vm::approx{points.front().position});
      CHECK(samples[4] == vm::approx{points[1].position});
      CHECK(samples[8] == vm::approx{points[2].position});
      CHECK(samples[12] == vm::approx{points[3].position});
      CHECK(samples.back() == vm::approx{points.front().position});
    }
  }

  SECTION("buildSweepFrames")
  {
    SECTION("returns empty vector for fewer than two points")
    {
      CHECK(buildSweepFrames({}, 64.0).empty());
      CHECK(buildSweepFrames({SplinePoint{vm::vec3d{0, 0, 0}}}, 64.0).empty());
    }

    SECTION("straight spline along the X axis produces upright frames")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
      };

      // One segment of length 128 divided into two spans of the forward size.
      const auto frames = buildSweepFrames(points, 64.0);
      REQUIRE(frames.size() == 3);

      CHECK(frames[0].position == vm::approx{vm::vec3d{0, 0, 0}});
      CHECK(frames[1].position == vm::approx{vm::vec3d{64, 0, 0}});
      CHECK(frames[2].position == vm::approx{vm::vec3d{128, 0, 0}});

      for (const auto& frame : frames)
      {
        CHECK(frame.right == vm::approx{vm::vec3d{0, 1, 0}});
        CHECK(frame.up == vm::approx{vm::vec3d{0, 0, 1}});
        CHECK(frame.scale == vm::approx{1.0});
      }
    }

    SECTION("each segment holds at least one span")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{16, 0, 0}},
        SplinePoint{vm::vec3d{32, 0, 0}},
      };

      // Both segments are much shorter than the forward size, so each still gets
      // one span.
      const auto frames = buildSweepFrames(points, 64.0);
      CHECK(frames.size() == 3);
    }

    SECTION("roll rotates the frame around the tangent")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}, 90.0},
        SplinePoint{vm::vec3d{128, 0, 0}, 90.0},
      };

      const auto frames = buildSweepFrames(points, 64.0);
      REQUIRE(frames.size() == 3);

      for (const auto& frame : frames)
      {
        CHECK(frame.right == vm::approx{vm::vec3d{0, 0, 1}});
        CHECK(frame.up == vm::approx{vm::vec3d{0, -1, 0}});
      }
    }

    SECTION("scale interpolates between the control points")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0},
        SplinePoint{vm::vec3d{128, 0, 0}, 0.0, 0.5},
      };

      const auto frames = buildSweepFrames(points, 64.0);
      REQUIRE(frames.size() == 3);
      CHECK(frames[0].scale == vm::approx{1.0});
      CHECK(frames[1].scale == vm::approx{0.75});
      CHECK(frames[2].scale == vm::approx{0.5});
    }

    SECTION("frames stay orthonormal around a bend")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{128, 0, 0}},
        SplinePoint{vm::vec3d{256, 128, 0}},
      };

      const auto frames = buildSweepFrames(points, 32.0);
      REQUIRE(frames.size() > 2);

      for (size_t i = 1; i < frames.size(); ++i)
      {
        // Consecutive ups must not flip.
        CHECK(vm::dot(frames[i - 1].up, frames[i].up) > 0.5);
        // Frames must remain orthonormal.
        CHECK(vm::dot(frames[i].right, frames[i].up) == vm::approx{0.0});
        CHECK(vm::length(frames[i].right) == vm::approx{1.0});
        CHECK(vm::length(frames[i].up) == vm::approx{1.0});
      }
    }

    SECTION("a closed spline's sweep returns to its starting frame")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{256, 0, 0}},
        SplinePoint{vm::vec3d{256, 256, 0}},
        SplinePoint{vm::vec3d{0, 256, 0}},
      };

      const auto frames = buildSweepFrames(points, 64.0, true);
      REQUIRE(frames.size() > 4);

      // The loop must close seamlessly: the last frame coincides with the first.
      CHECK(frames.back().position == vm::approx{frames.front().position});
      CHECK(frames.back().right == vm::approx{frames.front().right});
      CHECK(frames.back().up == vm::approx{frames.front().up});
      CHECK(frames.back().scale == vm::approx{frames.front().scale});
    }
  }

  SECTION("computeNodeFrames")
  {
    SECTION("a locked point anchors the frame to its upright orientation")
    {
      // The curve rises vertically and then turns horizontal. The rotation
      // minimizing frame transports the initial up (which lies in the bend plane
      // for a vertical start) around the bend, ending up pointing down; locking
      // the last point pins its frame upright instead.
      auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{0, 0, 128}},
        SplinePoint{vm::vec3d{128, 0, 128}},
      };

      const auto unlockedFrames = computeNodeFrames(points);
      REQUIRE(unlockedFrames.size() == 3);
      CHECK(vm::dot(unlockedFrames.back().up, vm::vec3d{0, 0, 1}) < 0.5);

      points.back().locks = SplineLock::Twist;
      const auto lockedFrames = computeNodeFrames(points);
      REQUIRE(lockedFrames.size() == 3);
      CHECK(lockedFrames.back().up == vm::approx{vm::vec3d{0, 0, 1}});
    }
  }
}

} // namespace tb::mdl

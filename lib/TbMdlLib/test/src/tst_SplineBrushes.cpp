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

#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/MapFormat.h"
#include "mdl/Spline.h"
#include "mdl/SplineBrushes.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto worldBounds = vm::bbox3d{8192.0};

Brush makeCuboid(const vm::bbox3d& bounds, const std::string& materialName)
{
  const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
  return builder.createCuboid(bounds, materialName) | kdl::value();
}

vm::bbox3d unitedBounds(const std::vector<Brush>& brushes)
{
  auto bounds = brushes.front().bounds();
  for (const auto& brush : brushes)
  {
    bounds = vm::merge(bounds, brush.bounds());
  }
  return bounds;
}

} // namespace

TEST_CASE("createSplineBrushes")
{
  const auto templateBounds = vm::bbox3d{{0, -16, -16}, {64, 16, 16}};
  const auto templateBrush = makeCuboid(templateBounds, "some_material");
  const auto templateBrushes = std::vector<const Brush*>{&templateBrush};

  SECTION("fails for degenerate input")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    CHECK(createSplineBrushes(
            MapFormat::Standard, worldBounds, {}, templateBrushes, templateBounds)
            .is_error());
    CHECK(
      createSplineBrushes(MapFormat::Standard, worldBounds, points, {}, templateBounds)
        .is_error());
    CHECK(createSplineBrushes(
            MapFormat::Standard,
            worldBounds,
            points,
            templateBrushes,
            vm::bbox3d{{0, 0, 0}, {0, 16, 16}})
            .is_error());
  }

  SECTION("straight spline reproduces the tiled template")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    // Two spans, one template copy each.
    CHECK(brushes.size() == 2);

    const auto bounds = unitedBounds(brushes);
    CHECK(bounds.min == vm::approx{vm::vec3d{0, -16, -16}});
    CHECK(bounds.max == vm::approx{vm::vec3d{128, 16, 16}});

    for (const auto& brush : brushes)
    {
      CHECK(brush.fullySpecified());
      for (const auto& face : brush.faces())
      {
        CHECK(face.attributes().materialName() == "some_material");
      }
    }
  }

  SECTION("each segment is tiled with stretched copies")
  {
    // The template brush covers only the first half of the template bounds, so the
    // generated geometry reveals where each copy starts and how it is stretched.
    const auto halfBrush =
      makeCuboid(vm::bbox3d{{0, -16, -16}, {32, 16, 16}}, "some_material");
    const auto halfBrushes = std::vector<const Brush*>{&halfBrush};

    // One segment of length 96 with a forward size of 64 holds round(1.5) = 2
    // copies, each stretched to span 48 units; the half brush fills the first half
    // of each span.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{96, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, halfBrushes, templateBounds)
      | kdl::value();

    REQUIRE(brushes.size() == 2);

    const auto inSpan = [](const Brush& brush, const double min, const double max) {
      return brush.bounds().min.x() == vm::approx{min}
             && brush.bounds().max.x() == vm::approx{max};
    };
    CHECK((inSpan(brushes[0], 0.0, 24.0) || inSpan(brushes[0], 48.0, 72.0)));
    CHECK((inSpan(brushes[1], 0.0, 24.0) || inSpan(brushes[1], 48.0, 72.0)));
    CHECK(brushes[0].bounds().min.x() != vm::approx{brushes[1].bounds().min.x()});
  }

  SECTION("cross-section scale tapers the sweep")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0},
      SplinePoint{vm::vec3d{128, 0, 0}, 0.0, 0.5},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    REQUIRE(brushes.size() == 2);

    // The frames sit at scales 1.0, 0.75 and 0.5, so the second cell's widest
    // cross-section is 0.75 * 16.
    const auto& lastBrush =
      brushes[0].bounds().min.x() < brushes[1].bounds().min.x() ? brushes[1] : brushes[0];
    CHECK(lastBrush.bounds().max.y() == vm::approx{12.0});
    CHECK(lastBrush.bounds().min.y() == vm::approx{-12.0});

    const auto bounds = unitedBounds(brushes);
    CHECK(bounds.max.y() == vm::approx{16.0});
    CHECK(bounds.min.y() == vm::approx{-16.0});
  }

  SECTION("curved spline produces valid brushes along the curve")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
      SplinePoint{vm::vec3d{256, 128, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    CHECK(brushes.size() > 1);

    for (const auto& brush : brushes)
    {
      CHECK(brush.fullySpecified());
      CHECK(worldBounds.contains(brush.bounds()));
    }
  }
}

} // namespace tb::mdl

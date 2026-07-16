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
    const auto frames = computeSplineFrames(points, 4);

    CHECK(createSplineBrushes(
            MapFormat::Standard, worldBounds, {}, templateBrushes, templateBounds)
            .is_error());
    CHECK(
      createSplineBrushes(MapFormat::Standard, worldBounds, frames, {}, templateBounds)
        .is_error());
    CHECK(createSplineBrushes(
            MapFormat::Standard,
            worldBounds,
            frames,
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
    const auto frames = computeSplineFrames(points, 4);

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, frames, templateBrushes, templateBounds)
      | kdl::value();

    CHECK(!brushes.empty());

    // The united bounds of all brushes must match the extruded template.
    auto bounds = brushes.front().bounds();
    for (const auto& brush : brushes)
    {
      bounds = vm::merge(bounds, brush.bounds());
    }
    CHECK(bounds.min == vm::approx{vm::vec3d{0, -16, -16}});
    CHECK(bounds.max == vm::approx{vm::vec3d{128, 16, 16}});

    // All faces must use the template's material.
    for (const auto& brush : brushes)
    {
      for (const auto& face : brush.faces())
      {
        CHECK(face.attributes().materialName() == "some_material");
      }
    }
  }

  SECTION("repetitions keep the template's natural size")
  {
    // The template brush covers only the first half of the template bounds, so the
    // generated geometry reveals where each repetition starts and how it is scaled.
    const auto halfBrush =
      makeCuboid(vm::bbox3d{{0, -16, -16}, {32, 16, 16}}, "some_material");
    const auto halfBrushes = std::vector<const Brush*>{&halfBrush};

    // The curve is 1.5 template lengths long: the first repetition must keep its
    // natural size, and only the second one is scaled (by 0.5) to fill the rest.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{96, 0, 0}},
    };
    const auto frames = computeSplineFrames(points, 4);

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, frames, halfBrushes, templateBounds)
      | kdl::value();

    CHECK(!brushes.empty());

    for (const auto& brush : brushes)
    {
      // First repetition: natural size, brush occupies [0, 32].
      // Second repetition: starts at 64 with scale 0.5, brush occupies [64, 80].
      const auto inFirst =
        brush.bounds().min.x() >= -0.001 && brush.bounds().max.x() <= 32.001;
      const auto inSecond =
        brush.bounds().min.x() >= 63.999 && brush.bounds().max.x() <= 80.001;
      CHECK((inFirst || inSecond));
    }

    auto bounds = brushes.front().bounds();
    for (const auto& brush : brushes)
    {
      bounds = vm::merge(bounds, brush.bounds());
    }
    CHECK(bounds.min.x() == vm::approx{0.0});
    CHECK(bounds.max.x() == vm::approx{80.0});
  }

  SECTION("curved spline produces valid brushes along the curve")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
      SplinePoint{vm::vec3d{256, 128, 0}},
    };
    const auto frames = computeSplineFrames(points, 8);

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, frames, templateBrushes, templateBounds)
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

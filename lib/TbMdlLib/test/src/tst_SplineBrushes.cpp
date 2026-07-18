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

    // Two spans, one template copy each; every copy is decomposed into tetrahedra
    // (two per quad face of the cuboid).
    CHECK(brushes.size() == 2 * 12);

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

    REQUIRE(!brushes.empty());

    // Each copy is decomposed into tetrahedra, so instead of comparing individual
    // brushes, partition them by span and compare the united bounds per span.
    auto firstCopy = std::vector<Brush>{};
    auto secondCopy = std::vector<Brush>{};
    for (const auto& brush : brushes)
    {
      if (brush.bounds().max.x() <= 24.0)
      {
        firstCopy.push_back(brush);
      }
      else
      {
        REQUIRE(brush.bounds().min.x() >= 48.0);
        secondCopy.push_back(brush);
      }
    }

    REQUIRE(!firstCopy.empty());
    REQUIRE(!secondCopy.empty());
    CHECK(unitedBounds(firstCopy) == vm::bbox3d{{0, -16, -16}, {24, 16, 16}});
    CHECK(unitedBounds(secondCopy) == vm::bbox3d{{48, -16, -16}, {72, 16, 16}});
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

    REQUIRE(!brushes.empty());

    // The frames sit at scales 1.0, 0.75 and 0.5, so the second cell's widest
    // cross-section is 0.75 * 16. The cell consists of the tetrahedra beyond x = 64.
    auto secondCell = std::vector<Brush>{};
    for (const auto& brush : brushes)
    {
      if (brush.bounds().min.x() >= 64.0)
      {
        secondCell.push_back(brush);
      }
    }
    REQUIRE(!secondCell.empty());
    const auto secondCellBounds = unitedBounds(secondCell);
    CHECK(secondCellBounds.max.y() == vm::approx{12.0});
    CHECK(secondCellBounds.min.y() == vm::approx{-12.0});

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

  SECTION("face UV attributes carry over to the generated brushes")
  {
    // Note: offsets cannot be verified here because the test faces have no loaded
    // material, so the alignment lock wraps them modulo a 1x1 texture size.
    auto uvTemplate = makeCuboid(templateBounds, "some_material");
    for (auto& face : uvTemplate.faces())
    {
      auto attributes = face.attributes();
      attributes.setScale(vm::vec2f{2.0f, 2.0f});
      attributes.setRotation(30.0f);
      face.setAttributes(attributes);
    }
    const auto uvTemplates = std::vector<const Brush*>{&uvTemplate};

    // A straight sweep at the template's natural size deforms with the identity
    // transform, so the template's UV attributes must arrive unchanged on the
    // generated faces that correspond to template faces (the axis aligned ones; the
    // tetrahedra's interior faces are invisible).
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, uvTemplates, templateBounds)
      | kdl::value();

    REQUIRE(!brushes.empty());

    auto checkedFaces = 0;
    for (const auto& brush : brushes)
    {
      for (const auto& face : brush.faces())
      {
        const auto& normal = face.normal();
        const auto axisAligned = vm::abs(normal.x()) > 0.999
                                 || vm::abs(normal.y()) > 0.999
                                 || vm::abs(normal.z()) > 0.999;
        if (axisAligned)
        {
          CHECK(face.attributes().scale() == vm::vec2f{2.0f, 2.0f});
          CHECK(face.attributes().rotation() == 30.0f);
          CHECK(face.attributes().materialName() == "some_material");
          ++checkedFaces;
        }
      }
    }
    CHECK(checkedFaces > 0);
  }

  SECTION("vertices are snapped to integer coordinates")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}, 30.0},
      SplinePoint{vm::vec3d{100, 30, 10}, 45.0},
      SplinePoint{vm::vec3d{250, 130, 50}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    CHECK(!brushes.empty());

    for (const auto& brush : brushes)
    {
      for (const auto& vertex : brush.vertexPositions())
      {
        CHECK(vertex == vm::round(vertex));
      }
    }
  }
}

} // namespace tb::mdl

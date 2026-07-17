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
#include "mdl/Entity.h"
#include "mdl/MapFormat.h"
#include "mdl/SplineEntity.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("SplineEntity")
{
  const auto data = SplineEntityData{
    {
      SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0, false},
      SplinePoint{vm::vec3d{64, 32, 16}, 45.0, 2.5, true},
      SplinePoint{vm::vec3d{128, 0, 0}, -90.0, 0.5, false},
    },
    12,
    42,
  };

  SECTION("isSplineEntity")
  {
    CHECK_FALSE(isSplineEntity(Entity{}));
    CHECK(isSplineEntity(writeSplineEntity(Entity{}, data)));
  }

  SECTION("writeSplineEntity and parseSplineEntity round-trip")
  {
    const auto entity = writeSplineEntity(Entity{}, data);
    CHECK(entity.classname() == SplineEntityClassname);

    const auto parsed = parseSplineEntity(entity);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == data);
  }

  SECTION("parseSplineEntity returns nullopt for non-spline entities")
  {
    CHECK(parseSplineEntity(Entity{}) == std::nullopt);
  }

  SECTION("the closed flag round-trips")
  {
    auto closedData = data;
    closedData.closed = true;

    const auto entity = writeSplineEntity(Entity{}, closedData);
    CHECK(entity.property(SplinePropertyKeys::Closed) != nullptr);

    const auto parsed = parseSplineEntity(entity);
    REQUIRE(parsed.has_value());
    CHECK(parsed->closed);

    // Reopening the spline removes the property again.
    const auto reopened = writeSplineEntity(entity, data);
    CHECK(reopened.property(SplinePropertyKeys::Closed) == nullptr);
    CHECK(!parseSplineEntity(reopened)->closed);
  }

  SECTION("writeSplineEntity removes stale point properties")
  {
    const auto entity = writeSplineEntity(Entity{}, data);

    auto shorterData = data;
    shorterData.points.pop_back();
    shorterData.templateGroupId = std::nullopt;

    const auto updatedEntity = writeSplineEntity(entity, shorterData);
    CHECK(updatedEntity.property("_spline_point_2") == nullptr);
    CHECK(updatedEntity.property("_spline_template_group") == nullptr);

    const auto parsed = parseSplineEntity(updatedEntity);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == shorterData);
  }

  SECTION("writeSplineEntity preserves unrelated properties")
  {
    auto entity = Entity{};
    entity.addOrUpdateProperty("angle", "45");

    const auto splineEntity = writeSplineEntity(entity, data);
    REQUIRE(splineEntity.property("angle") != nullptr);
    CHECK(*splineEntity.property("angle") == "45");
  }

  SECTION("template brush snapshot round-trip")
  {
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
    const auto brush =
      builder.createCuboid(vm::bbox3d{{0, -16, -16}, {64, 16, 16}}, "some_material")
      | kdl::value();

    const auto entity = writeSplineTemplateBrushes(Entity{}, {brush});
    CHECK(entity.property("_spline_template_brush_0") != nullptr);

    const auto parsed =
      parseSplineTemplateBrushes(entity, MapFormat::Standard, worldBounds);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().bounds().min == vm::approx{vm::vec3d{0, -16, -16}});
    CHECK(parsed.front().bounds().max == vm::approx{vm::vec3d{64, 16, 16}});
    for (const auto& face : parsed.front().faces())
    {
      CHECK(face.attributes().materialName() == "some_material");
    }

    SECTION("an empty snapshot removes stored brushes")
    {
      const auto clearedEntity = writeSplineTemplateBrushes(entity, {});
      CHECK(clearedEntity.property("_spline_template_brush_0") == nullptr);
      CHECK(parseSplineTemplateBrushes(clearedEntity, MapFormat::Standard, worldBounds)
              .empty());
    }
  }
}

} // namespace tb::mdl

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

#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/Spline.h"
#include "mdl/SplineEntities.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <fmt/format.h>

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

/** A template lattice 64 units long, centered on the origin across its section. */
const auto templateBounds = vm::bbox3d{{0, -16, -16}, {64, 16, 16}};

SplineTemplateEntity makeTemplateEntity(
  const vm::vec3d& origin, std::vector<EntityProperty> extraProperties = {})
{
  auto properties = std::vector<EntityProperty>{
    {EntityPropertyKeys::Classname, "light"},
    {EntityPropertyKeys::Origin,
     fmt::format("{} {} {}", origin.x(), origin.y(), origin.z())},
  };
  for (auto& property : extraProperties)
  {
    properties.push_back(std::move(property));
  }

  auto entity = Entity{std::move(properties)};
  entity.setPointEntity(true);
  return SplineTemplateEntity{
    std::move(entity),
    vm::bbox3d{origin - vm::vec3d{8, 8, 8}, origin + vm::vec3d{8, 8, 8}}};
}

/** A straight spline along the X axis, one template length per segment. */
std::vector<SplinePoint> straightPoints(const size_t segments)
{
  auto points = std::vector<SplinePoint>{};
  for (size_t i = 0; i <= segments; ++i)
  {
    points.push_back(SplinePoint{vm::vec3d{double(i) * 64.0, 0, 0}});
  }
  return points;
}

} // namespace

TEST_CASE("splineTemplateEntityBounds")
{
  CHECK(splineTemplateEntityBounds({}) == std::nullopt);

  const auto entities = std::vector<SplineTemplateEntity>{
    makeTemplateEntity(vm::vec3d{0, 0, 0}),
    makeTemplateEntity(vm::vec3d{32, 0, 0}),
  };
  CHECK(splineTemplateEntityBounds(entities) == vm::bbox3d{{-8, -8, -8}, {40, 8, 8}});
}

TEST_CASE("createSplineEntities")
{
  SECTION("returns nothing for degenerate input")
  {
    const auto entities =
      std::vector<SplineTemplateEntity>{makeTemplateEntity(vm::vec3d{32, 0, 0})};

    CHECK(createSplineEntities(straightPoints(2), {}, templateBounds).empty());
    CHECK(createSplineEntities({}, entities, templateBounds).empty());
    CHECK(
      createSplineEntities({SplinePoint{vm::vec3d{0, 0, 0}}}, entities, templateBounds)
        .empty());
    CHECK(createSplineEntities(
            straightPoints(2), entities, vm::bbox3d{{0, -16, -16}, {0, 16, 16}})
            .empty());
  }

  SECTION("places one copy of every template entity per span")
  {
    const auto entities = std::vector<SplineTemplateEntity>{
      makeTemplateEntity(vm::vec3d{32, 0, 0}),
      makeTemplateEntity(vm::vec3d{32, 0, 16}),
    };

    // Three segments of one template length each, so three spans.
    const auto result = createSplineEntities(straightPoints(3), entities, templateBounds);
    CHECK(result.size() == 6u);
  }

  SECTION("a straight spline lays the copies out along it")
  {
    const auto entities =
      std::vector<SplineTemplateEntity>{makeTemplateEntity(vm::vec3d{32, 0, 0})};

    const auto result = createSplineEntities(straightPoints(3), entities, templateBounds);
    REQUIRE(result.size() == 3u);

    // The template origin sits at the middle of the lattice, so each copy sits at the
    // middle of its span.
    CHECK(result[0].origin() == vm::approx{vm::vec3d{32, 0, 0}});
    CHECK(result[1].origin() == vm::approx{vm::vec3d{96, 0, 0}});
    CHECK(result[2].origin() == vm::approx{vm::vec3d{160, 0, 0}});
  }

  SECTION("an offset from the template's axis is carried through")
  {
    const auto entities =
      std::vector<SplineTemplateEntity>{makeTemplateEntity(vm::vec3d{32, 0, 24})};

    const auto result = createSplineEntities(straightPoints(1), entities, templateBounds);
    REQUIRE(result.size() == 1u);

    // 24 above the lattice center, which is above the section the sweep is made of:
    // still 24 above the curve.
    CHECK(result[0].origin() == vm::approx{vm::vec3d{32, 0, 24}});
  }

  SECTION("the copies follow the curve around a corner")
  {
    const auto entities =
      std::vector<SplineTemplateEntity>{makeTemplateEntity(vm::vec3d{32, 0, 0})};

    // A right angle: out along X, then away along Y.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
      SplinePoint{vm::vec3d{128, 128, 0}},
    };

    const auto result = createSplineEntities(points, entities, templateBounds);
    REQUIRE(result.size() >= 4u);

    // The first copy is on the leg along X and the last on the leg along Y, so the
    // curve has taken the copies around the corner rather than running them straight.
    CHECK(result.front().origin().x() < 64.0);
    CHECK(result.front().origin().y() < 32.0);
    CHECK(result.back().origin().y() > 64.0);
  }

  SECTION("a copy's angle turns with the curve")
  {
    // An entity facing along +X in the template, which is the sweep direction.
    const auto entities = std::vector<SplineTemplateEntity>{
      makeTemplateEntity(vm::vec3d{32, 0, 0}, {{EntityPropertyKeys::Angle, "0"}})};

    SECTION("a straight spline leaves it alone")
    {
      const auto result =
        createSplineEntities(straightPoints(1), entities, templateBounds);
      REQUIRE(result.size() == 1u);
      CHECK(*result[0].property(EntityPropertyKeys::Angle) == "0");
    }

    SECTION("a spline running the other way turns it around")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{-64, 0, 0}},
      };

      const auto result = createSplineEntities(points, entities, templateBounds);
      REQUIRE(result.size() == 1u);
      CHECK(*result[0].property(EntityPropertyKeys::Angle) == "180");
    }

    SECTION("a spline running across turns it across")
    {
      const auto points = std::vector<SplinePoint>{
        SplinePoint{vm::vec3d{0, 0, 0}},
        SplinePoint{vm::vec3d{0, 64, 0}},
      };

      const auto result = createSplineEntities(points, entities, templateBounds);
      REQUIRE(result.size() == 1u);
      CHECK(*result[0].property(EntityPropertyKeys::Angle) == "90");
    }
  }

  SECTION("a copy keeps the template's other properties")
  {
    const auto entities = std::vector<SplineTemplateEntity>{
      makeTemplateEntity(vm::vec3d{32, 0, 0}, {{"light", "200"}, {"_color", "1 0 0"}})};

    const auto result = createSplineEntities(straightPoints(1), entities, templateBounds);
    REQUIRE(result.size() == 1u);
    CHECK(result[0].classname() == "light");
    CHECK(*result[0].property("light") == "200");
    CHECK(*result[0].property("_color") == "1 0 0");
  }

  SECTION("a closed spline sweeps the segment back to the first point as well")
  {
    const auto entities =
      std::vector<SplineTemplateEntity>{makeTemplateEntity(vm::vec3d{32, 0, 0})};
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
      SplinePoint{vm::vec3d{128, 128, 0}},
      SplinePoint{vm::vec3d{0, 128, 0}},
    };

    const auto open = createSplineEntities(points, entities, templateBounds, false);
    const auto closed = createSplineEntities(points, entities, templateBounds, true);
    CHECK(closed.size() > open.size());
  }
}

} // namespace tb::mdl

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
#include "mdl/EntityProperties.h"
#include "mdl/MapFormat.h"
#include "mdl/Spline.h"
#include "mdl/SplineEntities.h"
#include "mdl/SplineEntity.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("SplineEntity")
{
  const auto data = SplineEntityData{"track_0", 12, 42};

  SECTION("isSplineEntity")
  {
    CHECK_FALSE(isSplineEntity(Entity{}));
    CHECK(isSplineEntity(writeSplineEntity(Entity{}, data)));

    // A head is known by its settings and by the point it starts from. Without either
    // it is a plain func_group: what a map that has lost its sidecar holds, or one
    // written before the points were entities of their own.
    auto withoutTarget = writeSplineEntity(Entity{}, data);
    withoutTarget.removeProperty(EntityPropertyKeys::Target);
    CHECK_FALSE(isSplineEntity(withoutTarget));

    auto withoutSettings = writeSplineEntity(Entity{}, data);
    withoutSettings.removeProperty(SplinePropertyKeys::Subdivisions);
    CHECK_FALSE(isSplineEntity(withoutSettings));
  }

  SECTION("writeSplineEntity and parseSplineEntity round-trip")
  {
    const auto entity = writeSplineEntity(Entity{}, data);
    CHECK(entity.classname() == SplineEntityClassname);
    CHECK(*entity.property(EntityPropertyKeys::Target) == "track_0");
    CHECK(parseSplineEntity(entity) == data);

    auto flagged = data;
    flagged.lockUVs = true;
    flagged.keepSize = true;
    CHECK(parseSplineEntity(writeSplineEntity(Entity{}, flagged)) == flagged);
  }

  SECTION("parseSplineEntity returns nullopt for non-spline entities")
  {
    CHECK(parseSplineEntity(Entity{}) == std::nullopt);
  }

  SECTION("writeSplineEntity removes the settings the data no longer has")
  {
    auto flagged = data;
    flagged.lockUVs = true;
    flagged.keepSize = true;
    const auto entity = writeSplineEntity(Entity{}, flagged);

    auto cleared = data;
    cleared.templateGroupId = std::nullopt;
    const auto updated = writeSplineEntity(entity, cleared);
    CHECK(updated.property(SplinePropertyKeys::TemplateGroupId) == nullptr);
    CHECK(updated.property(SplinePropertyKeys::LockUVs) == nullptr);
    CHECK(updated.property(SplinePropertyKeys::KeepSize) == nullptr);
    CHECK(parseSplineEntity(updated) == cleared);
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

  SECTION("template entities round trip")
  {
    auto templateEntity = Entity{{
      {EntityPropertyKeys::Classname, "light"},
      {EntityPropertyKeys::Origin, "32 0 24"},
      {"light", "200"},
      // A value with spaces and one with a quote in it, which is what the packing has
      // to survive.
      {"message", "a room with a view"},
      {"_note", "the \"good\" one"},
    }};
    templateEntity.setPointEntity(true);

    const auto templateEntities = std::vector<SplineTemplateEntity>{
      SplineTemplateEntity{
        std::move(templateEntity), vm::bbox3d{{24, -8, 16}, {40, 8, 32}}},
    };

    const auto entity = writeSplineTemplateEntities(Entity{}, templateEntities);
    CHECK(entity.property("_spline_template_entity_0") != nullptr);

    const auto parsed = parseSplineTemplateEntities(entity);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().bounds == vm::bbox3d{{24, -8, 16}, {40, 8, 32}});
    CHECK(parsed.front().entity.classname() == "light");
    CHECK(parsed.front().entity.origin() == vm::approx{vm::vec3d{32, 0, 24}});
    CHECK(*parsed.front().entity.property("light") == "200");
    CHECK(*parsed.front().entity.property("message") == "a room with a view");
    CHECK(*parsed.front().entity.property("_note") == "the \"good\" one");

    SECTION("an empty snapshot removes stored entities")
    {
      const auto clearedEntity = writeSplineTemplateEntities(entity, {});
      CHECK(clearedEntity.property("_spline_template_entity_0") == nullptr);
      CHECK(parseSplineTemplateEntities(clearedEntity).empty());
    }
  }

  SECTION("template brush entity snapshot round-trip")
  {
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
    const auto makeBrush = [&](const vm::bbox3d& bounds) {
      return builder.createCuboid(bounds, "some_material") | kdl::value();
    };

    auto templateEntity = Entity{};
    templateEntity.addOrUpdateProperty("classname", "func_detail");
    templateEntity.addOrUpdateProperty("_phong", "1");

    const auto brushEntities = std::vector<SplineTemplateBrushEntity>{
      SplineTemplateBrushEntity{
        std::move(templateEntity),
        {makeBrush(vm::bbox3d{{0, -16, -16}, {32, 16, 16}}),
         makeBrush(vm::bbox3d{{32, -16, -16}, {64, 16, 16}})}},
    };

    const auto entity = writeSplineTemplateBrushEntities(Entity{}, brushEntities);
    CHECK(entity.property("_spline_template_solid_0") != nullptr);
    CHECK(entity.property("_spline_template_solid_0_brush_0") != nullptr);
    CHECK(entity.property("_spline_template_solid_0_brush_1") != nullptr);

    const auto parsed =
      parseSplineTemplateBrushEntities(entity, MapFormat::Standard, worldBounds);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().entity.classname() == "func_detail");
    CHECK(*parsed.front().entity.property("_phong") == "1");

    REQUIRE(parsed.front().brushes.size() == 2);
    CHECK(parsed.front().brushes[0].bounds().max.x() == vm::approx{32.0});
    CHECK(parsed.front().brushes[1].bounds().min.x() == vm::approx{32.0});

    SECTION("the brush snapshot is written under its own prefix")
    {
      // "_spline_template_brush_" is a prefix of the key these would have had, so
      // rewriting the plain brush snapshot would have taken them with it.
      const auto both = writeSplineTemplateBrushes(
        entity, {makeBrush(vm::bbox3d{{0, 0, 0}, {16, 16, 16}})});

      CHECK(both.property("_spline_template_brush_0") != nullptr);
      CHECK(
        parseSplineTemplateBrushEntities(both, MapFormat::Standard, worldBounds).size()
        == 1);
    }

    SECTION("an empty snapshot removes the entities and their brushes")
    {
      const auto clearedEntity = writeSplineTemplateBrushEntities(entity, {});
      CHECK(clearedEntity.property("_spline_template_solid_0") == nullptr);
      CHECK(clearedEntity.property("_spline_template_solid_0_brush_0") == nullptr);
      CHECK(
        parseSplineTemplateBrushEntities(clearedEntity, MapFormat::Standard, worldBounds)
          .empty());
    }

    SECTION("an entity whose brushes are all gone contributes nothing")
    {
      auto withoutBrushes = entity;
      withoutBrushes.removeProperty("_spline_template_solid_0_brush_0");
      withoutBrushes.removeProperty("_spline_template_solid_0_brush_1");

      CHECK(
        parseSplineTemplateBrushEntities(withoutBrushes, MapFormat::Standard, worldBounds)
          .empty());
    }
  }

  SECTION("splineEntityId")
  {
    // A spline with no data has nothing to tie generated entities to.
    CHECK(splineEntityId(Entity{}).empty());

    // Writing a spline gives it one, and rewriting it keeps the same one, so the
    // entities it generated are still its own after an edit.
    const auto written = writeSplineEntity(Entity{}, data);
    const auto id = splineEntityId(written);
    CHECK_FALSE(id.empty());
    CHECK(splineEntityId(writeSplineEntity(written, data)) == id);
  }
}

TEST_CASE("SplinePointEntity")
{
  const auto points = std::vector<SplinePoint>{
    SplinePoint{vm::vec3d{0, 0, 0}},
    SplinePoint{vm::vec3d{64, 32, 16}, 45.0, 2.5, SplineLock::Twist},
    SplinePoint{
      vm::vec3d{128, 0, 0},
      -90.0,
      0.5,
      SplineLock::None,
      false,
      vm::vec3d{-16, -8, 4},
      vm::vec3d{24, 12, -6}},
    SplinePoint{vm::vec3d{96, -64, 32}},
  };

  const auto write = [&](const size_t index, const bool closed = false) {
    return writeSplinePointEntity(Entity{}, points, index, closed);
  };

  const auto vectorProperty = [](const Entity& entity, const std::string& key) {
    const auto* value = entity.property(key);
    REQUIRE(value != nullptr);
    const auto vector = vm::parse<double, 3>(*value);
    REQUIRE(vector.has_value());
    return *vector;
  };

  SECTION("isSplinePointEntity")
  {
    CHECK(isSplinePointEntity(write(0)));
    CHECK_FALSE(isSplinePointEntity(Entity{}));
    CHECK_FALSE(isSplinePointEntity(Entity{{{"classname", "path_corner"}}}));
  }

  SECTION("a point round trips through its entity")
  {
    for (size_t i = 0; i < points.size(); ++i)
    {
      CAPTURE(i);
      const auto entity = write(i);
      CHECK(entity.classname() == SplinePointClassname);
      CHECK(parseSplinePointEntity(entity) == points[i]);
    }
  }

  SECTION("the keys a game reads are written for every point")
  {
    const auto automatic = write(1);
    CHECK(*automatic.property(SplinePointPropertyKeys::Roll) == "45");
    CHECK(*automatic.property(SplinePointPropertyKeys::SectionScale) == "2.5");
    CHECK(*automatic.property(SplinePointPropertyKeys::TwistLock) == "1");
    CHECK(*automatic.property(SplinePointPropertyKeys::AutoTangent) == "1");
    // A point whose tangents the editor works out carries them all the same.
    CHECK(automatic.property(SplinePointPropertyKeys::TangentIn) != nullptr);
    CHECK(automatic.property(SplinePointPropertyKeys::TangentOut) != nullptr);

    const auto manual = write(2);
    CHECK(*manual.property(SplinePointPropertyKeys::TangentIn) == "-16 -8 4");
    CHECK(*manual.property(SplinePointPropertyKeys::TangentOut) == "24 12 -6");
    CHECK(manual.property(SplinePointPropertyKeys::TwistLock) == nullptr);
    CHECK(manual.property(SplinePointPropertyKeys::AutoTangent) == nullptr);

    // Quake and Quake 2 discard a key starting with an underscore as they spawn an
    // entity, so none of these can.
    for (const auto* key :
         {SplinePointPropertyKeys::Roll,
          SplinePointPropertyKeys::SectionScale,
          SplinePointPropertyKeys::TangentIn,
          SplinePointPropertyKeys::TangentOut,
          SplinePointPropertyKeys::TwistLock})
    {
      CAPTURE(key);
      CHECK(key[0] != '_');
    }
  }

  SECTION("the curve a game draws through the points is the one the editor sweeps")
  {
    // A game knows nothing about how the tangents were made: it takes each segment as
    // the Bezier curve the keys describe. That has to be the curve the editor sweeps
    // along, whether the tangents are automatic or not and whether the spline is open
    // or closed.
    for (const auto closed : {false, true})
    {
      CAPTURE(closed);

      auto entities = std::vector<Entity>{};
      for (size_t i = 0; i < points.size(); ++i)
      {
        entities.push_back(write(i, closed));
      }

      const auto segments = closed ? points.size() : points.size() - 1;
      for (size_t segment = 0; segment < segments; ++segment)
      {
        const auto& from = entities[segment];
        const auto& to = entities[(segment + 1) % points.size()];

        const auto p0 = from.origin();
        const auto p1 = p0 + vectorProperty(from, SplinePointPropertyKeys::TangentOut);
        const auto p3 = to.origin();
        const auto p2 = p3 + vectorProperty(to, SplinePointPropertyKeys::TangentIn);

        for (const auto t : {0.0, 0.25, 0.5, 0.75, 1.0})
        {
          CAPTURE(segment, t);
          const auto u = 1.0 - t;
          const auto bezier = p0 * (u * u * u) + p1 * (3.0 * u * u * t)
                              + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
          // The keys are written to six significant digits.
          CHECK(bezier == vm::approx{curvePoint(points, segment, t, closed), 0.01});
        }
      }
    }
  }

  SECTION("a key that is missing or cannot be read takes its default")
  {
    const auto bare = Entity{{{"classname", SplinePointClassname}, {"origin", "1 2 3"}}};
    CHECK(parseSplinePointEntity(bare) == SplinePoint{vm::vec3d{1, 2, 3}});

    for (const auto* scale : {"0", "-1", "large"})
    {
      CAPTURE(scale);
      auto entity = bare;
      entity.addOrUpdateProperty(SplinePointPropertyKeys::SectionScale, scale);
      CHECK(parseSplinePointEntity(entity).scale == 1.0);
    }

    // With only one of its tangents, a point has nothing to shape the curve with on the
    // other side, so it falls back to automatic ones.
    auto halfTangent = bare;
    halfTangent.addOrUpdateProperty(SplinePointPropertyKeys::TangentIn, "8 0 0");
    CHECK(parseSplinePointEntity(halfTangent).autoTangent);
  }

  SECTION("writing a point keeps the keys it does not own")
  {
    auto entity = Entity{{
      {"classname", SplinePointClassname},
      {"targetname", "track_1"},
      {"target", "track_2"},
      {"speed", "300"},
    }};

    entity = writeSplinePointEntity(entity, points, 1, false);
    CHECK(*entity.property("targetname") == "track_1");
    CHECK(*entity.property("target") == "track_2");
    CHECK(*entity.property("speed") == "300");

    // What the point no longer is goes away.
    auto changed = points;
    changed[1].locks = SplineLock::None;
    changed[1].autoTangent = false;
    entity = writeSplinePointEntity(entity, changed, 1, false);
    CHECK(entity.property(SplinePointPropertyKeys::TwistLock) == nullptr);
    CHECK(entity.property(SplinePointPropertyKeys::AutoTangent) == nullptr);
  }

  SECTION("transformSplinePointEntity")
  {
    const auto original = write(2);

    SECTION("a rotation turns the tangents and leaves the roll alone")
    {
      auto entity = original;
      transformSplinePointEntity(
        entity,
        vm::translation_matrix(vm::vec3d{100, 200, 300})
          * vm::rotation_matrix(0.0, 0.0, vm::to_radians(90.0)));

      // The tangents are offsets, which a translation does not move.
      CHECK(
        vectorProperty(entity, SplinePointPropertyKeys::TangentIn)
        == vm::approx{vm::vec3d{8, -16, 4}});
      CHECK(
        vectorProperty(entity, SplinePointPropertyKeys::TangentOut)
        == vm::approx{vm::vec3d{-12, 24, -6}});
      CHECK(*entity.property(SplinePointPropertyKeys::Roll) == "-90");
      CHECK(entity.origin() == original.origin());
    }

    SECTION("a scale stretches the tangents")
    {
      auto entity = original;
      transformSplinePointEntity(entity, vm::scaling_matrix(vm::vec3d{2, 1, 0.5}));
      CHECK(
        vectorProperty(entity, SplinePointPropertyKeys::TangentIn)
        == vm::approx{vm::vec3d{-32, -8, 2}});
      CHECK(
        vectorProperty(entity, SplinePointPropertyKeys::TangentOut)
        == vm::approx{vm::vec3d{48, 12, -3}});
    }

    SECTION("a mirror turns the roll the other way")
    {
      auto entity = original;
      transformSplinePointEntity(entity, vm::mirror_matrix<double>(vm::axis::x));
      CHECK(
        vectorProperty(entity, SplinePointPropertyKeys::TangentIn)
        == vm::approx{vm::vec3d{16, -8, 4}});
      CHECK(*entity.property(SplinePointPropertyKeys::Roll) == "90");
    }

    SECTION("which is what keeps a mirrored spline banking into its bends")
    {
      // Mirror every point across a vertical plane, the way transforming the spline's
      // group would, and its frames have to come out as the mirror image of the
      // original's: the up direction a game banks a follower toward is mirrored too.
      const auto mirror = vm::mirror_matrix<double>(vm::axis::x);

      auto mirrored = std::vector<SplinePoint>{};
      for (size_t i = 0; i < points.size(); ++i)
      {
        auto entity = write(i);
        REQUIRE(entity.transform(mirror, false).is_success());
        mirrored.push_back(parseSplinePointEntity(entity));
      }

      const auto frames = computeNodeFrames(points, false);
      const auto mirroredFrames = computeNodeFrames(mirrored, false);
      REQUIRE(frames.size() == mirroredFrames.size());
      for (size_t i = 0; i < frames.size(); ++i)
      {
        CAPTURE(i);
        CHECK(
          mirroredFrames[i].position == vm::approx{mirror * frames[i].position, 0.01});
        CHECK(mirroredFrames[i].up == vm::approx{mirror * frames[i].up, 0.001});
      }
    }
  }
}

} // namespace tb::mdl

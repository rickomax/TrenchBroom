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
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/MapFormat.h"
#include "mdl/MapSidecar.h"
#include "mdl/SplineEntity.h"
#include "mdl/SplineNodes.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto worldBounds = vm::bbox3d{8192.0};

EntityNode* makePoint(
  const std::string& name, const std::string& target, const vm::vec3d& position)
{
  auto entity = writeSplinePointEntity(
    Entity{{{"targetname", name}}}, {SplinePoint{position}}, 0, false);
  if (!target.empty())
  {
    entity.addOrUpdateProperty("target", target);
  }
  return new EntityNode{std::move(entity)};
}

EntityNode* makeHead(const std::string& firstPoint)
{
  return new EntityNode{writeSplineEntity(Entity{}, SplineEntityData{firstPoint})};
}

/** The targetname of the given entity node. */
std::string nameOf(const EntityNode* entityNode)
{
  const auto* name = entityNode->entity().property("targetname");
  return name ? *name : std::string{};
}

std::vector<std::string> namesOf(const SplinePointChain& chain)
{
  auto names = std::vector<std::string>{};
  for (const auto* point : chain.points)
  {
    names.push_back(nameOf(point));
  }
  return names;
}

EntityNode* generatedEntityOf(const GroupNode& groupNode)
{
  for (auto* child : groupNode.children())
  {
    if (auto* entityNode = dynamic_cast<EntityNode*>(child);
        entityNode && isSplineGeneratedEntity(entityNode->entity()))
    {
      return entityNode;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("SplineNodes")
{
  SECTION("findSplinePointChain")
  {
    SECTION("follows the targets from the head, whatever order the points are kept in")
    {
      auto groupNode = GroupNode{Group{"track"}};
      auto* head = makeHead("track_0");
      groupNode.addChildren({
        makePoint("track_2", "", vm::vec3d{128, 0, 0}),
        head,
        makePoint("track_0", "track_1", vm::vec3d{0, 0, 0}),
        makePoint("track_1", "track_2", vm::vec3d{64, 0, 0}),
      });

      const auto chain = findSplinePointChain(*head);
      CHECK(namesOf(chain) == std::vector<std::string>{"track_0", "track_1", "track_2"});
      CHECK_FALSE(chain.closed);

      CHECK(
        parseSplinePoints(chain)
        == std::vector<SplinePoint>{
          SplinePoint{vm::vec3d{0, 0, 0}},
          SplinePoint{vm::vec3d{64, 0, 0}},
          SplinePoint{vm::vec3d{128, 0, 0}},
        });
    }

    SECTION("a spline whose last point targets the first is closed")
    {
      auto* groupNode = createSplineGroupNode("loop", 3, true);
      const auto chain = findSplinePointChain(*findSplineHead(*groupNode));
      CHECK(namesOf(chain) == std::vector<std::string>{"loop_0", "loop_1", "loop_2"});
      CHECK(chain.closed);
      delete groupNode;
    }

    SECTION("a single point targeting itself is a closed spline of one point")
    {
      auto groupNode = GroupNode{Group{"dot"}};
      auto* head = makeHead("dot_0");
      groupNode.addChildren({head, makePoint("dot_0", "dot_0", vm::vec3d{0, 0, 0})});

      const auto chain = findSplinePointChain(*head);
      CHECK(namesOf(chain) == std::vector<std::string>{"dot_0"});
      CHECK(chain.closed);
    }

    SECTION("the chain ends where a target names nothing beside the head")
    {
      auto outside = GroupNode{Group{"elsewhere"}};
      outside.addChild(makePoint("track_2", "", vm::vec3d{128, 0, 0}));

      auto groupNode = GroupNode{Group{"track"}};
      auto* head = makeHead("track_0");
      groupNode.addChildren({
        head,
        makePoint("track_0", "track_1", vm::vec3d{0, 0, 0}),
        makePoint("track_1", "track_2", vm::vec3d{64, 0, 0}),
      });

      const auto chain = findSplinePointChain(*head);
      CHECK(namesOf(chain) == std::vector<std::string>{"track_0", "track_1"});
      CHECK_FALSE(chain.closed);
    }

    SECTION("a loop back into the middle ends the chain without closing it")
    {
      auto groupNode = GroupNode{Group{"knot"}};
      auto* head = makeHead("knot_0");
      groupNode.addChildren({
        head,
        makePoint("knot_0", "knot_1", vm::vec3d{0, 0, 0}),
        makePoint("knot_1", "knot_2", vm::vec3d{64, 0, 0}),
        makePoint("knot_2", "knot_1", vm::vec3d{128, 0, 0}),
      });

      const auto chain = findSplinePointChain(*head);
      CHECK(namesOf(chain) == std::vector<std::string>{"knot_0", "knot_1", "knot_2"});
      CHECK_FALSE(chain.closed);
    }

    SECTION("a stray copy of a point does not take the place of the original")
    {
      auto groupNode = GroupNode{Group{"track"}};
      auto* head = makeHead("track_0");
      auto* original = makePoint("track_1", "", vm::vec3d{64, 0, 0});
      groupNode.addChildren({
        head,
        makePoint("track_0", "track_1", vm::vec3d{0, 0, 0}),
        original,
        makePoint("track_1", "", vm::vec3d{999, 0, 0}),
      });

      const auto chain = findSplinePointChain(*head);
      REQUIRE(chain.points.size() == 2u);
      CHECK(chain.points[1] == original);
    }

    SECTION("a head that is not in the map yet has no points to find")
    {
      auto head = EntityNode{writeSplineEntity(Entity{}, SplineEntityData{"track_0"})};
      CHECK(findSplinePointChain(head).points.empty());
    }
  }

  SECTION("findSplineHead, isSplineGroup and isSplinePointNode")
  {
    auto* splineGroup = createSplineGroupNode("track", 2);
    auto* head = findSplineHead(*splineGroup);
    REQUIRE(head != nullptr);
    CHECK(isSplineEntity(head->entity()));
    CHECK(isSplineGroup(*splineGroup));

    const auto chain = findSplinePointChain(*head);
    REQUIRE(chain.points.size() == 2u);
    CHECK(isSplinePointNode(*chain.points[0]));
    CHECK_FALSE(isSplinePointNode(*generatedEntityOf(*splineGroup)));

    // A point entity in a group that holds no spline is the mapper's own.
    auto plainGroup = GroupNode{Group{"props"}};
    auto* loosePoint = makePoint("loose_0", "", vm::vec3d{0, 0, 0});
    plainGroup.addChild(loosePoint);
    CHECK_FALSE(isSplineGroup(plainGroup));
    CHECK(findSplineHead(plainGroup) == nullptr);
    CHECK_FALSE(isSplinePointNode(*loosePoint));

    delete splineGroup;
  }

  SECTION("findSplineHeadFor")
  {
    auto worldNode = WorldNode{{}, {}, MapFormat::Standard};
    auto* splineGroup = createSplineGroupNode("track", 2);
    worldNode.defaultLayer()->addChild(splineGroup);

    auto* head = findSplineHead(*splineGroup);
    const auto chain = findSplinePointChain(*head);
    auto* generated = generatedEntityOf(*splineGroup);

    CHECK(findSplineHeadFor(*head) == head);
    CHECK(findSplineHeadFor(*splineGroup) == head);
    CHECK(findSplineHeadFor(*chain.points[1]) == head);
    CHECK(findSplineHeadFor(*head->children().front()) == head);
    CHECK(findSplineHeadFor(*generated) == head);

    auto* other = new EntityNode{Entity{{{"classname", "light"}}}};
    worldNode.defaultLayer()->addChild(other);
    CHECK(findSplineHeadFor(*other) == nullptr);
    CHECK(findSplineHeadFor(*worldNode.defaultLayer()) == nullptr);
  }

  SECTION("names")
  {
    CHECK(splinePointName("track", 3) == "track_3");

    auto head = EntityNode{writeSplineEntity(Entity{}, SplineEntityData{"my_track_12"})};
    CHECK(splineName(head) == "my_track");

    // A first point the mapper named without an index is the spline's name whole.
    head.setEntity(writeSplineEntity(head.entity(), SplineEntityData{"start"}));
    CHECK(splineName(head) == "start");
  }

  SECTION("uniqueSplineName")
  {
    CHECK(uniqueSplineName("track", {}) == "track");
    CHECK(uniqueSplineName("track", {"track_0", "track_1"}) == "track2");
    CHECK(uniqueSplineName("track", {"track_0", "track2_0"}) == "track3");
    CHECK(uniqueSplineName("spline1", {"spline1_0"}) == "spline2");

    // Only a name followed by an index is a point name.
    CHECK(uniqueSplineName("track", {"track", "track_a", "track_"}) == "track");
  }
}

} // namespace tb::mdl

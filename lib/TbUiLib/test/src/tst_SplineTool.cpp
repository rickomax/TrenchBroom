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


#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/SplineEntity.h"
#include "mdl/SplineNodes.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/SplineTool.h"

#include "vm/approx.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/** The spline groups in the given map's default layer. */
std::vector<mdl::GroupNode*> splineGroups(mdl::Map& map)
{
  auto groups = std::vector<mdl::GroupNode*>{};
  for (auto* child : map.worldNode().defaultLayer()->children())
  {
    if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(child);
        groupNode && mdl::isSplineGroup(*groupNode))
    {
      groups.push_back(groupNode);
    }
  }
  return groups;
}

/** The one spline in the map's default layer. */
mdl::EntityNode& onlySpline(mdl::Map& map)
{
  const auto groups = splineGroups(map);
  REQUIRE(groups.size() == 1u);
  auto* head = mdl::findSplineHead(*groups.front());
  REQUIRE(head != nullptr);
  return *head;
}

std::vector<std::string> pointNames(const mdl::SplinePointChain& chain)
{
  auto names = std::vector<std::string>{};
  for (const auto* point : chain.points)
  {
    const auto* name = point->entity().property(mdl::EntityPropertyKeys::Targetname);
    names.push_back(name ? *name : std::string{});
  }
  return names;
}

const std::string* targetOf(const mdl::EntityNode& entityNode)
{
  return entityNode.entity().property(mdl::EntityPropertyKeys::Target);
}

} // namespace

TEST_CASE("SplineTool")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  auto tool = SplineTool{document};
  REQUIRE(tool.activate());

  const auto addPoints = [&](const std::vector<vm::vec3d>& positions) {
    for (const auto& position : positions)
    {
      tool.addPoint(position);
    }
  };

  SECTION("adding points makes a spline in a group of its own")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}, {128, 64, 0}});

    const auto groups = splineGroups(map);
    REQUIRE(groups.size() == 1u);
    CHECK(groups.front()->group().name() == "spline1");

    auto& head = onlySpline(map);
    CHECK(head.entity().classname() == mdl::SplineEntityClassname);

    const auto chain = mdl::findSplinePointChain(head);
    CHECK(
      pointNames(chain)
      == std::vector<std::string>{"spline1_0", "spline1_1", "spline1_2"});
    CHECK_FALSE(chain.closed);
    CHECK(*targetOf(head) == "spline1_0");
    CHECK(targetOf(*chain.points.back()) == nullptr);

    REQUIRE(chain.points.size() == 3u);
    CHECK(chain.points[2]->entity().origin() == vm::vec3d{128, 64, 0});
    CHECK(chain.points[2]->entity().classname() == mdl::SplinePointClassname);
    CHECK(tool.splineName() == "spline1");
  }

  SECTION("the selected point's entity is what the map has selected")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}});

    const auto chain = mdl::findSplinePointChain(onlySpline(map));
    REQUIRE(chain.points.size() == 2u);
    CHECK(map.selection().nodes == std::vector<mdl::Node*>{chain.points[1]});

    // Selecting another point's entity selects the point.
    mdl::deselectAll(map);
    mdl::selectNodes(map, {chain.points[0]});
    CHECK(tool.selectedPointIndex() == 0u);

    // And nothing of the spline is left selected for the other tools once the spline
    // tool is put away.
    REQUIRE(tool.deactivate());
    CHECK(map.selection().nodes.empty());
  }

  SECTION("a point is edited in place, keeping the keys the mapper gave it")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}, {128, 0, 0}});
    REQUIRE(tool.selectedPointIndex() == 2u);

    auto* point = mdl::findSplinePointChain(onlySpline(map)).points.back();
    auto entity = point->entity();
    entity.addOrUpdateProperty("speed", "300");
    REQUIRE(
      mdl::updateNodeContents(map, "Set Speed", {{point, mdl::NodeContents{entity}}}));

    tool.moveSelectedPoint(vm::vec3d{0, 0, 32});
    tool.setSelectedPointRoll(30.0);

    const auto chain = mdl::findSplinePointChain(onlySpline(map));
    REQUIRE(chain.points.size() == 3u);
    CHECK(chain.points[2] == point);
    CHECK(point->entity().origin() == vm::vec3d{128, 0, 32});
    CHECK(*point->entity().property(mdl::SplinePointPropertyKeys::Roll) == "30");
    CHECK(*point->entity().property("speed") == "300");
  }

  SECTION("an edit made to a point from outside the tool is picked up")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}});

    auto* point = mdl::findSplinePointChain(onlySpline(map)).points.front();
    auto entity = point->entity();
    entity.addOrUpdateProperty(mdl::SplinePointPropertyKeys::SectionScale, "3");
    REQUIRE(
      mdl::updateNodeContents(map, "Set Scale", {{point, mdl::NodeContents{entity}}}));

    REQUIRE(tool.points().size() == 2u);
    CHECK(tool.points().front().scale == 3.0);
  }

  SECTION("removing a point joins its neighbours")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}, {128, 0, 0}});

    auto chain = mdl::findSplinePointChain(onlySpline(map));
    auto* removed = chain.points[1];
    mdl::deselectAll(map);
    mdl::selectNodes(map, {removed});
    REQUIRE(tool.selectedPointIndex() == 1u);

    tool.removePoint();

    chain = mdl::findSplinePointChain(onlySpline(map));
    CHECK(pointNames(chain) == std::vector<std::string>{"spline1_0", "spline1_2"});
    CHECK(*targetOf(*chain.points[0]) == "spline1_2");
    CHECK_FALSE(removed->isDescendantOf(map.worldNode()));
    CHECK(map.selection().nodes.empty());
  }

  SECTION("a new point takes the first number no other entity has")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}, {128, 0, 0}});

    auto chain = mdl::findSplinePointChain(onlySpline(map));
    mdl::deselectAll(map);
    mdl::selectNodes(map, {chain.points[0]});
    tool.addPoint(vm::vec3d{32, 32, 0});

    chain = mdl::findSplinePointChain(onlySpline(map));
    CHECK(
      pointNames(chain)
      == std::vector<std::string>{"spline1_0", "spline1_3", "spline1_1", "spline1_2"});
  }

  SECTION("a closed spline's last point targets the first")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}, {128, 64, 0}});

    tool.setClosed(true);
    auto chain = mdl::findSplinePointChain(onlySpline(map));
    CHECK(chain.closed);
    CHECK(*targetOf(*chain.points.back()) == "spline1_0");
    CHECK(tool.closed());

    tool.setClosed(false);
    chain = mdl::findSplinePointChain(onlySpline(map));
    CHECK_FALSE(chain.closed);
    CHECK(targetOf(*chain.points.back()) == nullptr);
  }

  SECTION("renaming a spline renames its points in order, and its group")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}});
    auto chain = mdl::findSplinePointChain(onlySpline(map));
    mdl::deselectAll(map);
    mdl::selectNodes(map, {chain.points[0]});
    tool.addPoint(vm::vec3d{32, 32, 0});

    tool.setSplineName("coaster");
    chain = mdl::findSplinePointChain(onlySpline(map));
    CHECK(
      pointNames(chain)
      == std::vector<std::string>{"coaster_0", "coaster_1", "coaster_2"});
    CHECK(*targetOf(onlySpline(map)) == "coaster_0");
    CHECK(splineGroups(map).front()->group().name() == "coaster");
    CHECK(tool.splineName() == "coaster");

    SECTION("whitespace becomes underscores")
    {
      tool.setSplineName(" big dipper ");
      CHECK(tool.splineName() == "big_dipper");
    }

    SECTION("a name something else is made of is given a number")
    {
      mdl::addNodes(
        map,
        {{mdl::parentForNodes(map),
          {new mdl::EntityNode{mdl::Entity{{{"targetname", "taken_0"}}}}}}});
      tool.setSplineName("taken");
      CHECK(tool.splineName() == "taken2");
    }
  }

  SECTION("removing the last point takes the spline's group with it")
  {
    tool.addPoint(vm::vec3d{0, 0, 0});
    REQUIRE(splineGroups(map).size() == 1u);

    tool.removePoint();
    CHECK(splineGroups(map).empty());
    CHECK_FALSE(tool.hasPoints());
  }

  SECTION("undo and redo")
  {
    addPoints({{0, 0, 0}, {64, 0, 0}});
    REQUIRE(tool.points().size() == 2u);

    // Each undo takes back one step at a time, selection changes included, until the
    // second point is gone.
    while (tool.points().size() == 2u)
    {
      REQUIRE(map.undoCommandName() != nullptr);
      map.undoCommand();
    }
    CHECK(tool.points().size() == 1u);
    CHECK(mdl::findSplinePointChain(onlySpline(map)).points.size() == 1u);

    while (!splineGroups(map).empty())
    {
      REQUIRE(map.undoCommandName() != nullptr);
      map.undoCommand();
    }
    CHECK_FALSE(tool.hasPoints());

    while (map.redoCommandName() != nullptr)
    {
      map.redoCommand();
    }
    CHECK(mdl::findSplinePointChain(onlySpline(map)).points.size() == 2u);
    CHECK(tool.points().size() == 2u);
  }

  SECTION("a linked template is swept into the spline's head, and replaced on an edit")
  {
    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{mdl::parentForNodes(map), {brushNode}}});

    addPoints({{0, 0, 0}, {128, 0, 0}});
    mdl::deselectAll(map);
    mdl::selectNodes(map, {brushNode});
    tool.linkTemplate();

    const auto sweptBrushes = [&]() { return onlySpline(map).children().size(); };
    CHECK(sweptBrushes() > 0u);

    tool.addPoint(vm::vec3d{256, 64, 0});
    CHECK(sweptBrushes() > 0u);

    // There is only ever the one head in the spline's group.
    auto heads = size_t(0);
    for (const auto* child : splineGroups(map).front()->children())
    {
      if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(child);
          entityNode && mdl::isSplineEntity(entityNode->entity()))
      {
        ++heads;
      }
    }
    CHECK(heads == 1u);

    SECTION("breaking the spline leaves its brushes beside its group, not in it")
    {
      const auto brushesInLayer = [&]() {
        auto count = size_t(0);
        for (const auto* child : map.worldNode().defaultLayer()->children())
        {
          count += dynamic_cast<const mdl::BrushNode*>(child) ? 1u : 0u;
        }
        return count;
      };

      const auto brushesBefore = brushesInLayer();
      const auto swept = sweptBrushes();
      REQUIRE(tool.canBreakSpline());

      tool.breakSpline();

      CHECK(brushesInLayer() == brushesBefore + swept);
      CHECK_FALSE(tool.hasTemplate());
      // The spline itself stays, points and all.
      CHECK(mdl::findSplinePointChain(onlySpline(map)).points.size() == 3u);
    }
  }
}

} // namespace tb::ui

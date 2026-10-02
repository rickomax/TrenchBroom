/*
 Copyright (C) 2025 Kristian Duske

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

#include "mdl/TestFactory.h"

#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/GameInfo.h" // IWYU pragma: keep
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/PatchNode.h"
#include "mdl/SplineEntity.h"
#include "mdl/SplineNodes.h"
#include "mdl/WorldNode.h"

namespace tb::mdl
{

BrushNode* createBrushNode(
  const Map& map,
  const std::string& materialName,
  const std::function<void(Brush&)>& brushFunc)
{
  const auto& worldNode = map.worldNode();
  auto builder = BrushBuilder{
    worldNode.mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto brush = builder.createCube(32.0, materialName) | kdl::value();
  brushFunc(brush);
  return new BrushNode(std::move(brush));
}

PatchNode* createPatchNode(const std::string& materialName)
{
  // clang-format off
  return new PatchNode{BezierPatch{3, 3, {
    {0, 0, 0}, {1, 0, 1}, {2, 0, 0},
    {0, 1, 1}, {1, 1, 2}, {2, 1, 1},
    {0, 2, 0}, {1, 2, 1}, {2, 2, 0} }, materialName}};
  // clang-format on
}

GroupNode* createSplineGroupNode(
  const std::string& name, const size_t pointCount, const bool closed)
{
  auto* groupNode = new GroupNode{Group{name}};

  auto* head = new EntityNode{
    writeSplineEntity(Entity{}, SplineEntityData{splinePointName(name, 0)})};
  head->addChild(new BrushNode{
    BrushBuilder{MapFormat::Standard, vm::bbox3d{8192.0}}.createCube(32.0, "material")
    | kdl::value()});
  groupNode->addChild(head);

  auto points = std::vector<SplinePoint>{};
  for (size_t i = 0; i < pointCount; ++i)
  {
    points.push_back(SplinePoint{vm::vec3d{double(i) * 64.0, 0, 0}});
  }

  for (size_t i = 0; i < pointCount; ++i)
  {
    auto entity = writeSplinePointEntity(Entity{}, points, i, closed);
    entity.addOrUpdateProperty("targetname", splinePointName(name, i));

    const auto isLast = i + 1 == pointCount;
    if (!isLast || closed)
    {
      entity.addOrUpdateProperty("target", splinePointName(name, (i + 1) % pointCount));
    }
    groupNode->addChild(new EntityNode{std::move(entity)});
  }

  groupNode->addChild(new EntityNode{Entity{{
    {"classname", "light"},
    {SplinePropertyKeys::GeneratedBy, splineEntityId(head->entity())},
  }}});

  return groupNode;
}

} // namespace tb::mdl

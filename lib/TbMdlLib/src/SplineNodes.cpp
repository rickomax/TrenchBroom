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


#include "mdl/SplineNodes.h"

#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/SplineEntity.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <unordered_map>

namespace tb::mdl
{
namespace
{

/** The given name without the digits it ends with. */
std::string_view withoutTrailingDigits(const std::string_view name)
{
  auto end = name.size();
  while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1])))
  {
    --end;
  }
  return name.substr(0, end);
}

/**
 * The spline name the given targetname is a point name of, if it is one: everything
 * before the last underscore, if only digits follow it.
 */
std::optional<std::string_view> pointNamePrefix(const std::string_view name)
{
  const auto underscore = name.rfind('_');
  if (underscore == std::string_view::npos || underscore + 1 == name.size())
  {
    return std::nullopt;
  }

  const auto index = name.substr(underscore + 1);
  if (!std::ranges::all_of(
        index, [](const char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
  {
    return std::nullopt;
  }
  return name.substr(0, underscore);
}

/** Calls the given function for every entity node in the given subtree. */
template <typename F>
void visitEntityNodes(Node& node, const F& f)
{
  node.accept(kdl::overload(
    [](auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
    [](auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
    [](auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
    [&](EntityNode& entityNode) { f(entityNode); },
    [](BrushNode&) {},
    [](PatchNode&) {}));
}

/** The first of the given node's children that is a spline head with the given id, or
 * any spline head if the id is empty. */
EntityNode* headAmongChildren(const Node& node, const std::string& id = {})
{
  for (auto* child : node.children())
  {
    if (auto* entityNode = dynamic_cast<EntityNode*>(child);
        entityNode && isSplineEntity(entityNode->entity())
        && (id.empty() || splineEntityId(entityNode->entity()) == id))
    {
      return entityNode;
    }
  }
  return nullptr;
}

/** The head of the spline that generated the given entity, if it is still beside it. */
EntityNode* generatingHead(const EntityNode& entityNode)
{
  const auto* owner = entityNode.entity().property(SplinePropertyKeys::GeneratedBy);
  const auto* parent = entityNode.parent();
  return owner && !owner->empty() && parent ? headAmongChildren(*parent, *owner)
                                            : nullptr;
}

} // namespace

SplinePointChain findSplinePointChain(const EntityNode& head)
{
  auto chain = SplinePointChain{};

  const auto* parent = head.parent();
  const auto* first = head.entity().property(EntityPropertyKeys::Target);
  if (!parent || !first || first->empty())
  {
    return chain;
  }

  // The first point of each name is the one the chain goes through, so that a stray copy
  // of a point cannot take the place of the one it was copied from.
  auto pointsByName = std::unordered_map<std::string, EntityNode*>{};
  for (auto* child : parent->children())
  {
    if (auto* entityNode = dynamic_cast<EntityNode*>(child);
        entityNode && isSplinePointEntity(entityNode->entity()))
    {
      if (const auto* name =
            entityNode->entity().property(EntityPropertyKeys::Targetname);
          name && !name->empty())
      {
        pointsByName.try_emplace(*name, entityNode);
      }
    }
  }

  for (auto it = pointsByName.find(*first); it != pointsByName.end();)
  {
    auto* point = it->second;
    if (std::ranges::find(chain.points, point) != chain.points.end())
    {
      chain.closed = point == chain.points.front();
      break;
    }
    chain.points.push_back(point);

    const auto* target = point->entity().property(EntityPropertyKeys::Target);
    if (!target || target->empty())
    {
      break;
    }
    it = pointsByName.find(*target);
  }

  return chain;
}

std::vector<SplinePoint> parseSplinePoints(const SplinePointChain& chain)
{
  return chain.points | std::views::transform([](const auto* point) {
           return parseSplinePointEntity(point->entity());
         })
         | kdl::ranges::to<std::vector>();
}

EntityNode* findSplineHead(const GroupNode& groupNode)
{
  return headAmongChildren(groupNode);
}

bool isSplineGroup(const GroupNode& groupNode)
{
  return findSplineHead(groupNode) != nullptr;
}

bool isSplinePointNode(const EntityNode& entityNode)
{
  if (!isSplinePointEntity(entityNode.entity()))
  {
    return false;
  }
  const auto* groupNode = dynamic_cast<const GroupNode*>(entityNode.parent());
  return groupNode && isSplineGroup(*groupNode);
}

EntityNode* findSplineHeadFor(Node& node)
{
  return node.accept(kdl::overload(
    [](WorldNode&) -> EntityNode* { return nullptr; },
    [](LayerNode&) -> EntityNode* { return nullptr; },
    [](GroupNode& groupNode) { return findSplineHead(groupNode); },
    [](EntityNode& entityNode) -> EntityNode* {
      if (isSplineEntity(entityNode.entity()))
      {
        return &entityNode;
      }
      if (isSplinePointEntity(entityNode.entity()))
      {
        return entityNode.parent() ? headAmongChildren(*entityNode.parent()) : nullptr;
      }
      return generatingHead(entityNode);
    },
    [](BrushNode& brushNode) -> EntityNode* {
      auto* entityNode = dynamic_cast<EntityNode*>(brushNode.parent());
      if (!entityNode)
      {
        return nullptr;
      }
      return isSplineEntity(entityNode->entity()) ? entityNode
                                                  : generatingHead(*entityNode);
    },
    [](PatchNode&) -> EntityNode* { return nullptr; }));
}

std::string splinePointName(const std::string_view splineName, const size_t index)
{
  return fmt::format("{}_{}", splineName, index);
}

std::string splineName(const EntityNode& head)
{
  const auto* first = head.entity().property(EntityPropertyKeys::Target);
  if (!first)
  {
    return {};
  }
  return std::string{pointNamePrefix(*first).value_or(*first)};
}

std::unordered_set<std::string> collectTargetnames(const Node& node)
{
  auto targetnames = std::unordered_set<std::string>{};
  visitEntityNodes(const_cast<Node&>(node), [&](const EntityNode& entityNode) {
    if (const auto* name = entityNode.entity().property(EntityPropertyKeys::Targetname);
        name && !name->empty())
    {
      targetnames.insert(*name);
    }
  });
  return targetnames;
}

std::string uniqueSplineName(
  const std::string_view preferred, const std::unordered_set<std::string>& targetnames)
{
  auto taken = std::unordered_set<std::string_view>{};
  for (const auto& name : targetnames)
  {
    if (const auto prefix = pointNamePrefix(name))
    {
      taken.insert(*prefix);
    }
  }

  if (!preferred.empty() && !taken.contains(preferred))
  {
    return std::string{preferred};
  }

  auto stem = withoutTrailingDigits(preferred);
  if (stem.empty())
  {
    stem = "spline";
  }
  for (size_t i = 2;; ++i)
  {
    auto candidate = fmt::format("{}{}", stem, i);
    if (!taken.contains(candidate))
    {
      return candidate;
    }
  }
}

} // namespace tb::mdl

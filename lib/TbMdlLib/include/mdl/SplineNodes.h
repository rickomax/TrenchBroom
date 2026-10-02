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

#pragma once

#include "mdl/Spline.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{
class EntityNode;
class GroupNode;
class Node;
class WorldNode;

/**
 * The control point entities of a spline, in order along the curve.
 */
struct SplinePointChain
{
  std::vector<EntityNode*> points;
  /** Whether the last point targets the first. */
  bool closed = false;
};

/**
 * Follows the chain of control points from the given spline head.
 *
 * The chain starts at the point the head targets and goes on through each point's
 * target. Only the head's siblings are looked at, which is where the spline's points
 * are kept: its group, or whatever holds the head if the group has been dissolved. The
 * chain ends at a point that targets nothing, or nothing among the siblings, or a point
 * already passed; it is closed if that point is the first one.
 */
SplinePointChain findSplinePointChain(const EntityNode& head);

/** The control points the entities of the given chain describe. */
std::vector<SplinePoint> parseSplinePoints(const SplinePointChain& chain);

/** The spline head among the given group's children, or nullptr if there is none. */
EntityNode* findSplineHead(const GroupNode& groupNode);

/** Whether the given group holds a spline. */
bool isSplineGroup(const GroupNode& groupNode);

/**
 * Whether the given entity node is a control point of a spline: a point entity kept
 * beside a spline's head. Only a group is searched for the head, since a spline whose
 * group has been dissolved has its points among everything else in the layer, and
 * looking through all of that every time would cost far more than the answer is worth.
 */
bool isSplinePointNode(const EntityNode& entityNode);

/**
 * The head of the spline the given node belongs to: the node itself if it is a head,
 * the head among the children of a spline's group, the head beside one of the spline's
 * control points, or the head holding a brush the spline generated. Returns nullptr if
 * the node is not part of a spline.
 */
EntityNode* findSplineHeadFor(Node& node);

/** The name of a spline's control point: the spline's name and the given index. */
std::string splinePointName(std::string_view splineName, size_t index);

/**
 * The name a spline's control points are named after: the first point's targetname
 * without the index splinePointName put on it. Returns an empty string if the spline
 * has no points.
 */
std::string splineName(const EntityNode& head);

/** Every targetname used in the given subtree. */
std::unordered_set<std::string> collectTargetnames(const Node& node);

/**
 * A name for a spline that none of the given targetnames is a point name of. The
 * preferred name is taken if it is free; otherwise a number is put on it, replacing any
 * it already ends with, counting up from 2.
 */
std::string uniqueSplineName(
  std::string_view preferred, const std::unordered_set<std::string>& targetnames);

} // namespace tb::mdl

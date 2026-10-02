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

#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace tb::mdl
{
class Brush;
class BrushNode;
class GroupNode;
class Map;
class PatchNode;

BrushNode* createBrushNode(
  const Map& map,
  const std::string& materialName = "material",
  const std::function<void(mdl::Brush&)>& brushFunc = [](mdl::Brush&) {});

PatchNode* createPatchNode(const std::string& materialName = "material");

/**
 * A spline's group the way the spline tool makes one: the head holding a brush the
 * sweep generated, the given number of control points named after the given name and
 * chained in order, and an entity the spline generated.
 */
GroupNode* createSplineGroupNode(
  const std::string& name, size_t pointCount, bool closed = false);

} // namespace tb::mdl

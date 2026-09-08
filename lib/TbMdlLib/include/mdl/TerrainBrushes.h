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

#include "Result.h"
#include "mdl/Terrain.h"

#include "vm/bbox.h"

#include <vector>

namespace tb::mdl
{
class Brush;
enum class MapFormat;

/**
 * Creates the solid geometry of the given terrain.
 *
 * Every cell is extruded from the terrain's base plane up to its four corner heights
 * and decomposed into six tetrahedra: the cell is split into two triangular prisms
 * along a diagonal, and each prism into three tetrahedra. A tetrahedron is a simplex,
 * so each one is a valid convex brush no matter how the corner heights differ, and
 * neighbouring cells share their corner vertices exactly, so the terrain is
 * watertight.
 *
 * Every face receives the cell's material (or the terrain's default material) and the
 * terrain's texture scale.
 *
 * Returns an error if the terrain is degenerate or no brushes could be created.
 */
Result<std::vector<Brush>> createTerrainBrushes(
  MapFormat mapFormat, const vm::bbox3d& worldBounds, const Terrain& terrain);

} // namespace tb::mdl

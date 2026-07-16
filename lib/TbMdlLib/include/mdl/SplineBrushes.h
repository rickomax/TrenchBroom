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
#include "mdl/Spline.h"

#include "vm/bbox.h"

#include <vector>

namespace tb::mdl
{
class Brush;
enum class MapFormat;

/**
 * Creates brushes by deforming the given template brushes along the given spline
 * frames.
 *
 * The template brushes are interpreted in a local coordinate system where the X axis
 * runs along the spline: the X extent of the given template bounds is mapped onto the
 * curve's arc length, and the Y / Z offsets from the bounds center are applied
 * sideways / upwards along the curve's normal and binormal. The template is repeated
 * along the curve as often as it fits, and stretched slightly so that a whole number
 * of repetitions covers the entire curve.
 *
 * To keep the resulting brushes convex, each template brush is cut into slices between
 * consecutive curve samples, and each slice is deformed individually. Face materials
 * and attributes are copied from the template face whose orientation matches best.
 *
 * Returns an error if the template bounds or the spline are degenerate, or if no
 * brushes could be created.
 */
Result<std::vector<Brush>> createSplineBrushes(
  MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplineFrame>& frames,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds);

} // namespace tb::mdl

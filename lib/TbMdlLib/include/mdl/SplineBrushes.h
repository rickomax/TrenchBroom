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
 * Creates brushes by sweeping the given template (profile) brushes along the spline
 * through the given control points.
 *
 * The template brushes are interpreted inside their combined bounds (the lattice),
 * with the X axis as the sweep direction: cross-section frames are placed along the
 * curve so that each control point segment holds round(segmentLength / latticeSizeX)
 * copies of the template (at least one), stretched or squished to fit the segment
 * while keeping the cross-section's proportions.
 *
 * Each span between two consecutive frames holds one copy of every template brush:
 * a vertex's X position inside the lattice interpolates linearly between the span's
 * two cross-sections, and its Y / Z offsets from the lattice center are applied along
 * each frame's right / up direction, scaled by the frame's cross-section scale.
 * Consecutive spans share their end cross-sections, so the swept solid is continuous.
 * Face materials and attributes are copied from the best matching template face.
 *
 * Returns an error if the lattice or the spline is degenerate, or if no brushes
 * could be created.
 */
Result<std::vector<Brush>> createSplineBrushes(
  MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds);

} // namespace tb::mdl

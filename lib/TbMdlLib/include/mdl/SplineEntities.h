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

#include "mdl/Entity.h"
#include "mdl/Spline.h"

#include "kd/reflection_decl.h"

#include "vm/bbox.h"

#include <optional>
#include <vector>

namespace tb::mdl
{

/**
 * A point entity belonging to a spline's template, together with the bounds it
 * occupies in template space. The bounds only matter when the template has no brushes
 * to size the lattice from.
 */
struct SplineTemplateEntity
{
  Entity entity;
  vm::bbox3d bounds;

  kdl_reflect_decl(SplineTemplateEntity, entity, bounds);
};

/**
 * The bounds the given template entities occupy together, or nullopt if there are
 * none. Used to size the lattice for a template made only of point entities.
 */
std::optional<vm::bbox3d> splineTemplateEntityBounds(
  const std::vector<SplineTemplateEntity>& templateEntities);

/**
 * Places a copy of each template point entity along the spline through the given
 * control points, once per span, the same way createSplineBrushes places one copy of
 * every template brush per span.
 *
 * A copy's origin is the template origin carried through the span's free-form
 * deformation, so an entity keeps its place relative to the swept geometry around it,
 * and its angle is turned by the span's orientation, so it faces the same way relative
 * to the curve as it did relative to the template. Entities that do not rotate, and
 * entities whose rotation the map format pins down elsewhere (a spotlight aimed at a
 * target, say), keep the angle they were authored with.
 *
 * Returns an empty vector if the lattice or the spline is degenerate, or if there are
 * no template entities.
 */
std::vector<Entity> createSplineEntities(
  const std::vector<SplinePoint>& points,
  const std::vector<SplineTemplateEntity>& templateEntities,
  const vm::bbox3d& templateBounds,
  bool closed = false);

} // namespace tb::mdl

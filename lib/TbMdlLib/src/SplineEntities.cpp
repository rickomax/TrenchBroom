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

#include "mdl/SplineEntities.h"

#include "mdl/EntityRotation.h"

#include "kd/reflection_impl.h"

#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/mat.h"
#include "vm/vec.h"

#include <vector>

namespace tb::mdl
{

kdl_reflect_impl(SplineTemplateEntity);

std::optional<vm::bbox3d> splineTemplateEntityBounds(
  const std::vector<SplineTemplateEntity>& templateEntities)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto& templateEntity : templateEntities)
  {
    result = result ? vm::merge(*result, templateEntity.bounds) : templateEntity.bounds;
  }
  return result;
}

std::vector<Entity> createSplineEntities(
  const std::vector<SplinePoint>& points,
  const std::vector<SplineTemplateEntity>& templateEntities,
  const vm::bbox3d& templateBounds,
  const bool closed)
{
  if (templateEntities.empty() || points.size() < 2 || templateBounds.size().x() <= 0.0)
  {
    return {};
  }

  // The same frames the swept brushes use, so the copies line up with the geometry
  // around them rather than with a curve of their own.
  const auto forwardSize = vm::max(1.0, templateBounds.size().x());
  const auto frames = buildSweepFrames(points, forwardSize, closed);
  if (frames.size() < 2)
  {
    return {};
  }

  auto entities = std::vector<Entity>{};
  entities.reserve(templateEntities.size() * (frames.size() - 1));

  for (size_t i = 0; i < frames.size() - 1; ++i)
  {
    const auto& a = frames[i];
    const auto& b = frames[i + 1];

    for (const auto& templateEntity : templateEntities)
    {
      const auto origin = templateEntity.entity.origin();

      auto entity = templateEntity.entity;
      // Snapped like the swept vertices are, so that a copy sitting on a surface of
      // the sweep does not end up a fraction of a unit off it.
      entity.setOrigin(vm::round(deformIntoSpan(origin, templateBounds, a, b)));

      if (const auto rotation = spanOrientation(origin, templateBounds, a, b);
          rotation != vm::mat4x4d::identity())
      {
        applyEntityRotation(entity, rotation);
      }

      entities.push_back(std::move(entity));
    }
  }

  return entities;
}

} // namespace tb::mdl

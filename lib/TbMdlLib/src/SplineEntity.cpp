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

#include "mdl/SplineEntity.h"

#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"

#include "kd/reflection_impl.h"
#include "kd/string_utils.h"

#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <string>

namespace tb::mdl
{
namespace
{

std::string pointKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::PointPrefix, index);
}

std::optional<SplinePoint> parsePoint(const std::string& value)
{
  const auto parsed = vm::parse<double, 5>(value);
  if (!parsed)
  {
    return std::nullopt;
  }
  return SplinePoint{
    vm::vec3d{(*parsed)[0], (*parsed)[1], (*parsed)[2]},
    (*parsed)[3],
    (*parsed)[4] != 0.0};
}

std::string formatPoint(const SplinePoint& point)
{
  return fmt::format(
    "{} {} {} {} {}",
    point.position.x(),
    point.position.y(),
    point.position.z(),
    point.roll,
    point.locked ? 1 : 0);
}

} // namespace

kdl_reflect_impl(SplineEntityData);

bool isSplineEntity(const Entity& entity)
{
  return entity.property(pointKey(0)) != nullptr;
}

std::optional<SplineEntityData> parseSplineEntity(const Entity& entity)
{
  if (!isSplineEntity(entity))
  {
    return std::nullopt;
  }

  auto data = SplineEntityData{};

  for (size_t i = 0;; ++i)
  {
    const auto* value = entity.property(pointKey(i));
    if (!value)
    {
      break;
    }
    if (const auto point = parsePoint(*value))
    {
      data.points.push_back(*point);
    }
  }

  if (const auto* subdivisions = entity.property(SplinePropertyKeys::Subdivisions))
  {
    if (const auto value = kdl::str_to_size(*subdivisions); value && *value > 0)
    {
      data.subdivisions = *value;
    }
  }

  if (const auto* groupId = entity.property(SplinePropertyKeys::TemplateGroupId))
  {
    if (const auto value = kdl::str_to_size(*groupId))
    {
      data.templateGroupId = *value;
    }
  }

  return data;
}

Entity writeSplineEntity(const Entity& entity, const SplineEntityData& data)
{
  auto result = entity;

  // Remove all spline properties, including stale point properties beyond the new
  // point count.
  for (const auto& property : entity.properties())
  {
    if (property.hasPrefix(SplinePropertyKeys::PointPrefix))
    {
      result.removeProperty(property.key());
    }
  }
  result.removeProperty(SplinePropertyKeys::Subdivisions);
  result.removeProperty(SplinePropertyKeys::TemplateGroupId);

  result.addOrUpdateProperty(EntityPropertyKeys::Classname, SplineEntityClassname);

  for (size_t i = 0; i < data.points.size(); ++i)
  {
    result.addOrUpdateProperty(pointKey(i), formatPoint(data.points[i]));
  }

  result.addOrUpdateProperty(
    SplinePropertyKeys::Subdivisions, kdl::str_to_string(data.subdivisions));

  if (data.templateGroupId)
  {
    result.addOrUpdateProperty(
      SplinePropertyKeys::TemplateGroupId, kdl::str_to_string(*data.templateGroupId));
  }

  return result;
}

} // namespace tb::mdl

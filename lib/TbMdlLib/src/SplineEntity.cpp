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

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"

#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <array>
#include <sstream>
#include <string>

namespace tb::mdl
{
namespace
{

std::string pointKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::PointPrefix, index);
}

std::string templateBrushKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::TemplateBrushPrefix, index);
}

std::string formatTemplateBrush(const Brush& brush)
{
  auto faces = std::vector<std::string>{};
  faces.reserve(brush.faceCount());
  for (const auto& face : brush.faces())
  {
    const auto& p = face.points();
    faces.push_back(fmt::format(
      "{} {} {} {} {} {} {} {} {} {}",
      p[0].x(),
      p[0].y(),
      p[0].z(),
      p[1].x(),
      p[1].y(),
      p[1].z(),
      p[2].x(),
      p[2].y(),
      p[2].z(),
      face.attributes().materialName()));
  }
  return kdl::str_join(faces, ";");
}

std::optional<Brush> parseTemplateBrush(
  const std::string& value, const MapFormat mapFormat, const vm::bbox3d& worldBounds)
{
  auto faces = std::vector<BrushFace>{};

  for (const auto& faceStr : kdl::str_split(value, ";"))
  {
    auto stream = std::istringstream{faceStr};
    auto coords = std::array<double, 9>{};
    for (auto& coord : coords)
    {
      stream >> coord;
    }
    if (stream.fail())
    {
      return std::nullopt;
    }

    auto materialName = std::string{};
    std::getline(stream, materialName);
    materialName = kdl::str_trim(materialName);

    auto face = BrushFace::create(
      vm::vec3d{coords[0], coords[1], coords[2]},
      vm::vec3d{coords[3], coords[4], coords[5]},
      vm::vec3d{coords[6], coords[7], coords[8]},
      BrushFaceAttributes{materialName},
      mapFormat);
    if (face.is_error())
    {
      return std::nullopt;
    }
    faces.push_back(std::move(face) | kdl::value());
  }

  auto brush = Brush::create(worldBounds, std::move(faces));
  if (brush.is_error())
  {
    return std::nullopt;
  }
  return std::move(brush) | kdl::value();
}

std::optional<SplinePoint> parsePoint(const std::string& value)
{
  // The last component holds the lock flags (see SplineLock). Older splines wrote a
  // boolean locked flag in its place, whose value 1 coincides with the Twist lock.
  if (const auto parsed = vm::parse<double, 6>(value))
  {
    return SplinePoint{
      vm::vec3d{(*parsed)[0], (*parsed)[1], (*parsed)[2]},
      (*parsed)[3],
      (*parsed)[4],
      SplineLock::Type((*parsed)[5])};
  }

  // Older splines were written without the scale component.
  if (const auto parsed = vm::parse<double, 5>(value))
  {
    return SplinePoint{
      vm::vec3d{(*parsed)[0], (*parsed)[1], (*parsed)[2]},
      (*parsed)[3],
      1.0,
      (*parsed)[4] != 0.0 ? SplineLock::Twist : SplineLock::None};
  }

  return std::nullopt;
}

std::string formatPoint(const SplinePoint& point)
{
  return fmt::format(
    "{} {} {} {} {} {}",
    point.position.x(),
    point.position.y(),
    point.position.z(),
    point.roll,
    point.scale,
    point.locks);
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

  if (const auto* closed = entity.property(SplinePropertyKeys::Closed))
  {
    data.closed = *closed != "0";
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
  result.removeProperty(SplinePropertyKeys::Closed);

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

  if (data.closed)
  {
    result.addOrUpdateProperty(SplinePropertyKeys::Closed, "1");
  }

  return result;
}

std::vector<Brush> parseSplineTemplateBrushes(
  const Entity& entity, const MapFormat mapFormat, const vm::bbox3d& worldBounds)
{
  auto brushes = std::vector<Brush>{};

  for (size_t i = 0;; ++i)
  {
    const auto* value = entity.property(templateBrushKey(i));
    if (!value)
    {
      break;
    }
    if (auto brush = parseTemplateBrush(*value, mapFormat, worldBounds))
    {
      brushes.push_back(std::move(*brush));
    }
  }

  return brushes;
}

Entity writeSplineTemplateBrushes(const Entity& entity, const std::vector<Brush>& brushes)
{
  auto result = entity;

  for (const auto& property : entity.properties())
  {
    if (property.hasPrefix(SplinePropertyKeys::TemplateBrushPrefix))
    {
      result.removeProperty(property.key());
    }
  }

  for (size_t i = 0; i < brushes.size(); ++i)
  {
    result.addOrUpdateProperty(templateBrushKey(i), formatTemplateBrush(brushes[i]));
  }

  return result;
}

} // namespace tb::mdl

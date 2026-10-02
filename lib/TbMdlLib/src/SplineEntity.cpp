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
#include "mdl/MapSidecar.h"
#include "mdl/SplineEntities.h"

#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <array>
#include <optional>
#include <sstream>
#include <string>

namespace tb::mdl
{
namespace
{

std::string templateBrushKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::TemplateBrushPrefix, index);
}

std::string templateEntityKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::TemplateEntityPrefix, index);
}

std::string templateSolidKey(const size_t index)
{
  return fmt::format("{}{}", SplinePropertyKeys::TemplateSolidPrefix, index);
}

std::string templateSolidBrushKey(const size_t entityIndex, const size_t brushIndex)
{
  return fmt::format("{}_brush_{}", templateSolidKey(entityIndex), brushIndex);
}

/**
 * An entity's properties as quoted key value pairs.
 *
 * The snapshot is one entity property value holding another entity's properties, so the
 * inner keys and values are quoted the way the map format quotes them.
 */
std::string formatTemplateEntityProperties(const Entity& entity)
{
  auto stream = std::ostringstream{};
  for (const auto& property : entity.properties())
  {
    stream << " " << quoteSidecarString(property.key()) << " "
           << quoteSidecarString(property.value());
  }
  return stream.str();
}

/** The properties of a record written by formatTemplateEntityProperties. */
std::vector<EntityProperty> parseTemplateEntityProperties(const std::string& value)
{
  auto properties = std::vector<EntityProperty>{};

  auto position = value.find('"');
  while (position != std::string::npos)
  {
    const auto key = unquoteSidecarString(value, position);
    if (!key)
    {
      break;
    }

    position = value.find('"', position);
    if (position == std::string::npos)
    {
      break;
    }

    const auto propertyValue = unquoteSidecarString(value, position);
    if (!propertyValue)
    {
      break;
    }

    properties.emplace_back(*key, *propertyValue);
    position = value.find('"', position);
  }

  return properties;
}

std::string formatTemplateEntity(const SplineTemplateEntity& templateEntity)
{
  return fmt::format(
           "{} {} {} {} {} {}",
           templateEntity.bounds.min.x(),
           templateEntity.bounds.min.y(),
           templateEntity.bounds.min.z(),
           templateEntity.bounds.max.x(),
           templateEntity.bounds.max.y(),
           templateEntity.bounds.max.z())
         + formatTemplateEntityProperties(templateEntity.entity);
}

std::optional<SplineTemplateEntity> parseTemplateEntity(const std::string& value)
{
  auto stream = std::istringstream{value};
  auto coords = std::array<double, 6>{};
  for (auto& coord : coords)
  {
    stream >> coord;
  }
  if (stream.fail())
  {
    return std::nullopt;
  }

  auto rest = std::string{};
  std::getline(stream, rest);

  auto entity = Entity{parseTemplateEntityProperties(rest)};
  // A snapshot is only ever taken of a point entity, and the entity it is read back
  // into has no definition to say so until it is put in the map.
  entity.setPointEntity(true);

  return SplineTemplateEntity{
    std::move(entity),
    vm::bbox3d{
      vm::vec3d{coords[0], coords[1], coords[2]},
      vm::vec3d{coords[3], coords[4], coords[5]}}};
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

std::optional<vm::vec3d> vectorProperty(const Entity& entity, const std::string& key)
{
  const auto* value = entity.property(key);
  return value ? vm::parse<double, 3>(*value) : std::nullopt;
}

std::optional<double> numberProperty(const Entity& entity, const std::string& key)
{
  const auto* value = entity.property(key);
  return value ? kdl::str_to_double(*value) : std::nullopt;
}

bool flagProperty(const Entity& entity, const std::string& key)
{
  const auto* value = entity.property(key);
  return value && *value != "0" && !value->empty();
}

void setFlagProperty(Entity& entity, const std::string& key, const bool set)
{
  if (set)
  {
    entity.addOrUpdateProperty(key, "1");
  }
  else
  {
    entity.removeProperty(key);
  }
}

} // namespace

kdl_reflect_impl(SplineEntityData);

bool isSplineEntity(const Entity& entity)
{
  const auto* target = entity.property(EntityPropertyKeys::Target);
  return entity.property(SplinePropertyKeys::Subdivisions) != nullptr && target
         && !target->empty();
}

std::optional<SplineEntityData> parseSplineEntity(const Entity& entity)
{
  if (!isSplineEntity(entity))
  {
    return std::nullopt;
  }

  auto data = SplineEntityData{};
  data.firstPoint = *entity.property(EntityPropertyKeys::Target);

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

  data.lockUVs = flagProperty(entity, SplinePropertyKeys::LockUVs);
  data.keepSize = flagProperty(entity, SplinePropertyKeys::KeepSize);

  return data;
}

Entity writeSplineEntity(const Entity& entity, const SplineEntityData& data)
{
  auto result = entity;

  result.addOrUpdateProperty(EntityPropertyKeys::Classname, SplineEntityClassname);

  // The bulk of this data is written to the map's sidecar file rather than the map
  // itself, so the entity carries an id tying it to its record there. Existing ids are
  // kept so that the link survives an edit.
  if (const auto* id = result.property(SidecarPropertyKeys::DataId); !id || id->empty())
  {
    result.addOrUpdateProperty(SidecarPropertyKeys::DataId, generateSidecarId());
  }

  result.addOrUpdateProperty(EntityPropertyKeys::Target, data.firstPoint);
  result.addOrUpdateProperty(
    SplinePropertyKeys::Subdivisions, kdl::str_to_string(data.subdivisions));

  if (data.templateGroupId)
  {
    result.addOrUpdateProperty(
      SplinePropertyKeys::TemplateGroupId, kdl::str_to_string(*data.templateGroupId));
  }
  else
  {
    result.removeProperty(SplinePropertyKeys::TemplateGroupId);
  }

  setFlagProperty(result, SplinePropertyKeys::LockUVs, data.lockUVs);
  setFlagProperty(result, SplinePropertyKeys::KeepSize, data.keepSize);

  return result;
}

bool isSplinePointEntity(const Entity& entity)
{
  return entity.classname() == SplinePointClassname;
}

SplinePoint parseSplinePointEntity(const Entity& entity)
{
  auto point = SplinePoint{entity.origin()};

  point.roll = numberProperty(entity, SplinePointPropertyKeys::Roll).value_or(0.0);

  // A point squeezed to nothing, or turned inside out, has no cross-section to sweep.
  if (const auto scale = numberProperty(entity, SplinePointPropertyKeys::SectionScale);
      scale && *scale > 0.0)
  {
    point.scale = *scale;
  }

  point.locks = flagProperty(entity, SplinePointPropertyKeys::TwistLock)
                  ? SplineLock::Twist
                  : SplineLock::None;

  const auto tangentIn = vectorProperty(entity, SplinePointPropertyKeys::TangentIn);
  const auto tangentOut = vectorProperty(entity, SplinePointPropertyKeys::TangentOut);
  point.autoTangent = flagProperty(entity, SplinePointPropertyKeys::AutoTangent)
                      || !tangentIn || !tangentOut;
  if (!point.autoTangent)
  {
    point.tangentIn = *tangentIn;
    point.tangentOut = *tangentOut;
  }

  return point;
}

Entity writeSplinePointEntity(
  const Entity& entity,
  const std::vector<SplinePoint>& points,
  const size_t index,
  const bool closed)
{
  const auto& point = points[index];

  auto result = entity;
  result.addOrUpdateProperty(EntityPropertyKeys::Classname, SplinePointClassname);
  result.setOrigin(point.position);
  result.addOrUpdateProperty(
    SplinePointPropertyKeys::Roll, kdl::str_to_string(point.roll));
  result.addOrUpdateProperty(
    SplinePointPropertyKeys::SectionScale, kdl::str_to_string(point.scale));
  result.addOrUpdateProperty(
    SplinePointPropertyKeys::TangentIn,
    kdl::str_to_string(vm::correct(tangentInOffset(points, index, closed))));
  result.addOrUpdateProperty(
    SplinePointPropertyKeys::TangentOut,
    kdl::str_to_string(vm::correct(tangentOutOffset(points, index, closed))));
  setFlagProperty(
    result, SplinePointPropertyKeys::TwistLock, (point.locks & SplineLock::Twist) != 0u);
  setFlagProperty(result, SplinePointPropertyKeys::AutoTangent, point.autoTangent);

  return result;
}

void transformSplinePointEntity(Entity& entity, const vm::mat4x4d& transformation)
{
  const auto linear = vm::strip_translation(transformation);

  for (const auto* key :
       {SplinePointPropertyKeys::TangentIn, SplinePointPropertyKeys::TangentOut})
  {
    if (const auto tangent = vectorProperty(entity, key))
    {
      entity.addOrUpdateProperty(key, kdl::str_to_string(vm::correct(linear * *tangent)));
    }
  }

  // A mirror turns every rotation about the curve the other way.
  if (vm::compute_determinant(linear) < 0.0)
  {
    if (const auto roll = numberProperty(entity, SplinePointPropertyKeys::Roll);
        roll && *roll != 0.0)
    {
      entity.addOrUpdateProperty(
        SplinePointPropertyKeys::Roll, kdl::str_to_string(-*roll));
    }
  }
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

std::vector<SplineTemplateEntity> parseSplineTemplateEntities(const Entity& entity)
{
  auto entities = std::vector<SplineTemplateEntity>{};

  for (size_t i = 0;; ++i)
  {
    const auto* value = entity.property(templateEntityKey(i));
    if (!value)
    {
      break;
    }
    if (auto templateEntity = parseTemplateEntity(*value))
    {
      entities.push_back(std::move(*templateEntity));
    }
  }

  return entities;
}

Entity writeSplineTemplateEntities(
  const Entity& entity, const std::vector<SplineTemplateEntity>& entities)
{
  auto result = entity;

  for (const auto& property : entity.properties())
  {
    if (property.hasPrefix(SplinePropertyKeys::TemplateEntityPrefix))
    {
      result.removeProperty(property.key());
    }
  }

  for (size_t i = 0; i < entities.size(); ++i)
  {
    result.addOrUpdateProperty(templateEntityKey(i), formatTemplateEntity(entities[i]));
  }

  return result;
}

std::vector<SplineTemplateBrushEntity> parseSplineTemplateBrushEntities(
  const Entity& entity, const MapFormat mapFormat, const vm::bbox3d& worldBounds)
{
  auto brushEntities = std::vector<SplineTemplateBrushEntity>{};

  for (size_t i = 0;; ++i)
  {
    const auto* value = entity.property(templateSolidKey(i));
    if (!value)
    {
      break;
    }

    auto brushEntity = SplineTemplateBrushEntity{};
    brushEntity.entity = Entity{parseTemplateEntityProperties(*value)};
    // It holds brushes, whatever the entity it is read back into would assume before it
    // is put in the map.
    brushEntity.entity.setPointEntity(false);

    for (size_t j = 0;; ++j)
    {
      const auto* brushValue = entity.property(templateSolidBrushKey(i, j));
      if (!brushValue)
      {
        break;
      }
      if (auto brush = parseTemplateBrush(*brushValue, mapFormat, worldBounds))
      {
        brushEntity.brushes.push_back(std::move(*brush));
      }
    }

    // An entity that has lost all of its brushes has nothing left to sweep, and would
    // otherwise generate empty entities all along the spline.
    if (!brushEntity.brushes.empty())
    {
      brushEntities.push_back(std::move(brushEntity));
    }
  }

  return brushEntities;
}

Entity writeSplineTemplateBrushEntities(
  const Entity& entity, const std::vector<SplineTemplateBrushEntity>& brushEntities)
{
  auto result = entity;

  // The brushes live under the same prefix as the entity they belong to, so this takes
  // them with it.
  for (const auto& property : entity.properties())
  {
    if (property.hasPrefix(SplinePropertyKeys::TemplateSolidPrefix))
    {
      result.removeProperty(property.key());
    }
  }

  for (size_t i = 0; i < brushEntities.size(); ++i)
  {
    const auto& brushEntity = brushEntities[i];
    result.addOrUpdateProperty(
      templateSolidKey(i), formatTemplateEntityProperties(brushEntity.entity));

    for (size_t j = 0; j < brushEntity.brushes.size(); ++j)
    {
      result.addOrUpdateProperty(
        templateSolidBrushKey(i, j), formatTemplateBrush(brushEntity.brushes[j]));
    }
  }

  return result;
}

bool isSplineGeneratedEntity(const Entity& entity)
{
  const auto* owner = entity.property(SplinePropertyKeys::GeneratedBy);
  return owner && !owner->empty();
}

std::string splineEntityId(const Entity& entity)
{
  const auto* id = entity.property(SidecarPropertyKeys::DataId);
  return id ? *id : std::string{};
}

} // namespace tb::mdl

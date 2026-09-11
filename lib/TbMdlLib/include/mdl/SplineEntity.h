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

#include "mdl/IdType.h"
#include "mdl/Spline.h"
#include "mdl/SplineEntities.h"

#include "kd/reflection_decl.h"

#include "vm/bbox.h"

#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Brush;
class Entity;
enum class MapFormat;

namespace SplinePropertyKeys
{
/** Per point property; the index is appended, e.g. "_spline_point_0". The value has
 * the form "x y z roll scale locks", where locks is a bit mask (see SplineLock). */
constexpr auto PointPrefix = "_spline_point_";
/** Number of curve samples between two control points. */
constexpr auto Subdivisions = "_spline_subdivisions";
/** Present with value "1" if the spline is closed (the last point connects back to
 * the first). */
constexpr auto Closed = "_spline_closed";
/** Persistent ID of the group whose brushes serve as the deformation template. */
constexpr auto TemplateGroupId = "_spline_template_group";
/** Per brush property holding a snapshot of a template brush; the index is appended,
 * e.g. "_spline_template_brush_0". The value contains one face per semicolon separated
 * segment, each of the form "x1 y1 z1 x2 y2 z2 x3 y3 z3 material". Used when the
 * template is a plain brush selection rather than a group. */
constexpr auto TemplateBrushPrefix = "_spline_template_brush_";
/** Per entity property holding a snapshot of a template point entity; the index is
 * appended, e.g. "_spline_template_entity_0". The value is the entity's bounds in
 * template space, "minx miny minz maxx maxy maxz", followed by its properties as
 * quoted key value pairs. Used when the template is a plain entity selection rather
 * than a group. */
constexpr auto TemplateEntityPrefix = "_spline_template_entity_";
/** Carried by the point entities a spline generates, holding the tool data id of the
 * spline that owns them. Under the _tb_ prefix so that it is stripped from exports:
 * a compiler has no use for it, but it has to survive a save so that the spline can
 * still find its entities after the map is reopened. */
constexpr auto GeneratedBy = "_tb_spline_source";
} // namespace SplinePropertyKeys

/**
 * The classname used for spline entities. Spline entities use func_group so that map
 * compilers merge the generated brushes into the world geometry.
 */
constexpr auto SplineEntityClassname = "func_group";

constexpr size_t SplineDefaultSubdivisions = 8;

/**
 * The persistent state of a spline entity: its control points, the number of curve
 * samples per span, and the (optional) template group whose brushes get deformed
 * along the curve.
 */
struct SplineEntityData
{
  std::vector<SplinePoint> points;
  size_t subdivisions = SplineDefaultSubdivisions;
  std::optional<IdType> templateGroupId;
  bool closed = false;

  kdl_reflect_decl(SplineEntityData, points, subdivisions, templateGroupId, closed);
};

/**
 * Returns whether the given entity carries spline data.
 */
bool isSplineEntity(const Entity& entity);

/**
 * Reads the spline data stored in the given entity's properties, or nullopt if the
 * entity is not a spline entity.
 */
std::optional<SplineEntityData> parseSplineEntity(const Entity& entity);

/**
 * Returns an entity carrying the given spline data in its properties. Any spline
 * properties not covered by the given data (e.g. stale point properties) are removed
 * from the given entity's properties.
 */
Entity writeSplineEntity(const Entity& entity, const SplineEntityData& data);

/**
 * Reads the template brush snapshot stored in the given entity's properties. Invalid
 * brushes are skipped; returns an empty vector if no snapshot is stored.
 */
std::vector<Brush> parseSplineTemplateBrushes(
  const Entity& entity, MapFormat mapFormat, const vm::bbox3d& worldBounds);

/**
 * Returns an entity carrying a snapshot of the given brushes in its properties. Any
 * previously stored snapshot is removed; passing an empty vector just removes it.
 */
Entity writeSplineTemplateBrushes(
  const Entity& entity, const std::vector<Brush>& brushes);

/**
 * Reads the template point entity snapshot stored in the given entity's properties.
 * Malformed entries are skipped; returns an empty vector if no snapshot is stored.
 */
std::vector<SplineTemplateEntity> parseSplineTemplateEntities(const Entity& entity);

/**
 * Returns an entity carrying a snapshot of the given point entities in its
 * properties. Any previously stored snapshot is removed; passing an empty vector just
 * removes it.
 */
Entity writeSplineTemplateEntities(
  const Entity& entity, const std::vector<SplineTemplateEntity>& entities);

/**
 * The id tying a spline entity to the point entities it generated, or an empty string
 * if it has none. This is the entity's tool data id, which writeSplineEntity gives
 * every spline.
 */
std::string splineEntityId(const Entity& entity);

} // namespace tb::mdl

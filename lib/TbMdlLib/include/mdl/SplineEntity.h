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
#include "vm/mat.h"

#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Brush;
class Entity;
enum class MapFormat;

/**
 * A spline is kept in the map as a group of its own, which is what lets it be selected,
 * copied and pasted as a whole. The group holds:
 *
 * - The spline's head: a func_group holding the brushes the sweep generates, and the
 *   editor's settings for the spline. Its target names the first control point.
 * - One info_spline_point entity per control point, each carrying its point's position as
 *   its origin and targeting the next point. A closed spline's last point targets the
 *   first; an open spline's last point targets nothing.
 * - The entities the sweep generates, each carrying a marker naming the head.
 *
 * The control points are the part a game reads. They are ordinary entities in the
 * compiled map, so an engine can find a spline by name and follow it from point to
 * point. The segment from a point A to the point B it targets is the cubic Bezier curve
 * whose control points are
 *
 *   A.origin, A.origin + A.tangent_out, B.origin + B.tangent_in, B.origin
 *
 * Every point carries its tangents, including one whose tangents the editor derives from
 * its neighbours, so a game never has to know how they were made.
 */
namespace SplinePropertyKeys
{
/** Number of curve samples between two control points. Its presence is also what marks
 * an entity as a spline's head; it lives in the sidecar file, so a head that has lost
 * its sidecar is a plain func_group again. */
constexpr auto Subdivisions = "_spline_subdivisions";
/** Present with value "1" if the copies keep the template's UV alignment rather than
 * having it realigned onto the geometry the sweep produces. */
constexpr auto LockUVs = "_spline_lock_uvs";
/** Present with value "1" if the copies are placed at the template's own size instead
 * of being stretched to fill their spans. */
constexpr auto KeepSize = "_spline_keep_size";
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
/** Per entity property holding a snapshot of a template brush entity -- a solid
 * entity, in the usual Quake sense -- as quoted key value pairs, with the index
 * appended, e.g. "_spline_template_solid_0". Its brushes are stored alongside it, in
 * "_spline_template_solid_0_brush_0" and so on, in the same form as
 * TemplateBrushPrefix. The prefix deliberately avoids TemplateBrushPrefix, which is a
 * prefix of anything starting "_spline_template_brush_" and would take these with it
 * when the brush snapshot is rewritten. */
constexpr auto TemplateSolidPrefix = "_spline_template_solid_";
/** Carried by the point entities a spline generates, holding the tool data id of the
 * spline that owns them. Under the _tb_ prefix so that it is stripped from exports:
 * a compiler has no use for it, but it has to survive a save so that the spline can
 * still find its entities after the map is reopened. */
constexpr auto GeneratedBy = "_tb_spline_source";
} // namespace SplinePropertyKeys

/**
 * The keys of a spline's control point entities, besides origin, targetname and target.
 *
 * These are for the game as much as for the editor, so none that a game is meant to
 * read begins with an underscore: Quake and Quake 2 throw such keys away as they spawn
 * an entity. Nor is the cross-section scale called "scale", which several engines take
 * as the size to draw an entity's model at.
 */
namespace SplinePointPropertyKeys
{
/** The point's roll around the curve, in degrees. */
constexpr auto Roll = "roll";
/** The cross-section scale at the point; the swept profile tapers between points. */
constexpr auto SectionScale = "section_scale";
/** The offset from the point to the handle the curve arrives from, "x y z". */
constexpr auto TangentIn = "tangent_in";
/** The offset from the point to the handle the curve leaves toward, "x y z". */
constexpr auto TangentOut = "tangent_out";
/** Present with value "1" if the sweep's frame is anchored at the point, so that a
 * twist from rolling other points cannot carry past it (see SplineLock::Twist). */
constexpr auto TwistLock = "twist_lock";
/** Present with value "1" if the editor derives the point's tangents from its
 * neighbours. The tangents are written all the same; this only says the editor is free
 * to rewrite them, so it is for the editor alone. */
constexpr auto AutoTangent = "_auto_tangent";
} // namespace SplinePointPropertyKeys

/**
 * The classname used for a spline's head. It is a func_group so that map compilers
 * merge the generated brushes into the world geometry.
 */
constexpr auto SplineEntityClassname = "func_group";

/** The classname of a spline's control points. */
constexpr auto SplinePointClassname = "info_spline_point";

constexpr size_t SplineDefaultSubdivisions = 8;

/**
 * What a spline's head holds: the targetname of the first control point, the number of
 * curve samples per span, and the (optional) template group whose brushes get deformed
 * along the curve. The points themselves are entities of their own.
 */
struct SplineEntityData
{
  std::string firstPoint;
  size_t subdivisions = SplineDefaultSubdivisions;
  std::optional<IdType> templateGroupId = std::nullopt;
  /** Whether every copy keeps the alignment the template was authored with. */
  bool lockUVs = false;
  /** Whether every copy is placed at the template's own size rather than stretched. */
  bool keepSize = false;

  kdl_reflect_decl(
    SplineEntityData, firstPoint, subdivisions, templateGroupId, lockUVs, keepSize);
};

/**
 * Returns whether the given entity is a spline's head: it carries the spline's settings
 * and names a first point.
 */
bool isSplineEntity(const Entity& entity);

/**
 * Reads the settings stored in a spline head's properties, or nullopt if the entity is
 * not a spline's head.
 */
std::optional<SplineEntityData> parseSplineEntity(const Entity& entity);

/**
 * Returns an entity carrying the given spline settings in its properties, with its
 * target naming the first point. Settings not covered by the given data are removed
 * from the given entity's properties.
 */
Entity writeSplineEntity(const Entity& entity, const SplineEntityData& data);

/** Returns whether the given entity is a spline's control point. */
bool isSplinePointEntity(const Entity& entity);

/**
 * Reads the control point a point entity describes. A key that is missing or cannot be
 * read takes its default, and a point that does not say it has automatic tangents but
 * lacks either tangent is given them, since there is nothing else to shape it with.
 */
SplinePoint parseSplinePointEntity(const Entity& entity);

/**
 * Returns the given entity carrying the control point at the given index of the given
 * points. The tangents written are the ones the curve uses, worked out from the
 * neighbouring points if the point's are automatic. Its other properties are kept,
 * targetname and target included, which are the caller's to set.
 */
Entity writeSplinePointEntity(
  const Entity& entity,
  const std::vector<SplinePoint>& points,
  size_t index,
  bool closed);

/**
 * Applies the given transformation to the tangents a point entity carries, which are
 * directions rather than positions and so are not moved, only turned and scaled. A
 * mirror also turns the point's roll the other way, which is what keeps a curve banking
 * into its bends after it has been mirrored. The origin is the caller's.
 */
void transformSplinePointEntity(Entity& entity, const vm::mat4x4d& transformation);

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
 * Reads the template brush entity snapshot stored in the given entity's properties.
 * Malformed entries and invalid brushes are skipped; returns an empty vector if no
 * snapshot is stored.
 */
std::vector<SplineTemplateBrushEntity> parseSplineTemplateBrushEntities(
  const Entity& entity, MapFormat mapFormat, const vm::bbox3d& worldBounds);

/**
 * Returns an entity carrying a snapshot of the given brush entities in its properties.
 * Any previously stored snapshot is removed; passing an empty vector just removes it.
 */
Entity writeSplineTemplateBrushEntities(
  const Entity& entity, const std::vector<SplineTemplateBrushEntity>& brushEntities);

/**
 * Whether the given entity was generated by a spline, i.e. carries the marker saying
 * which spline owns it.
 *
 * What it holds is regenerated from the spline's template, so editing it by hand would
 * be undone by the next regeneration; the tool's Break is what lets go of it.
 */
bool isSplineGeneratedEntity(const Entity& entity);

/**
 * The id tying a spline entity to the point entities it generated, or an empty string
 * if it has none. This is the entity's tool data id, which writeSplineEntity gives
 * every spline.
 */
std::string splineEntityId(const Entity& entity);

} // namespace tb::mdl

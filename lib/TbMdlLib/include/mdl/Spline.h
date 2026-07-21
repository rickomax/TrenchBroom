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

#include "kd/reflection_decl.h"

#include "vm/vec.h"

#include <cstddef>
#include <vector>

namespace tb::mdl
{

/**
 * Per control point lock flags.
 *
 * Twist anchors the sweep's frame orientation: the rotation minimizing frame is not
 * transported through the point, but reset to the point's own upright frame, so a
 * twist introduced by rotating other points cannot propagate past it.
 */
namespace SplineLock
{
using Type = unsigned;
constexpr Type None = 0u;
constexpr Type Twist = 1u << 0u;
} // namespace SplineLock

/**
 * A single control point of a spline. In addition to its position, a control point
 * carries a roll angle (in degrees) that twists the sweep's frame around the curve's
 * tangent, a cross-section scale that tapers the swept profile, and a set of lock
 * flags (see SplineLock).
 *
 * Each point also owns the curve tangents of the segments touching it, expressed as
 * two handle offsets relative to the point (like the control handles of a Bezier pen
 * tool): the curve arrives from the direction of the in handle and leaves toward the
 * out handle. While autoTangent is set (the default), the handles follow the
 * automatic Catmull-Rom tangents; when cleared, the stored offsets are used and can
 * be edited freely.
 */
struct SplinePoint
{
  vm::vec3d position;
  double roll = 0.0;
  double scale = 1.0;
  SplineLock::Type locks = SplineLock::None;
  bool autoTangent = true;
  vm::vec3d tangentIn = vm::vec3d{0, 0, 0};
  vm::vec3d tangentOut = vm::vec3d{0, 0, 0};

  kdl_reflect_decl(
    SplinePoint, position, roll, scale, locks, autoTangent, tangentIn, tangentOut);
};

/**
 * A cross-section frame along the swept curve: a position on the curve plus a right /
 * up basis spanning the cross-section plane, and the interpolated cross-section
 * scale.
 */
struct SweepFrame
{
  vm::vec3d position;
  vm::vec3d right;
  vm::vec3d up;
  double scale = 1.0;

  kdl_reflect_decl(SweepFrame, position, right, up, scale);
};

/**
 * Evaluates the uniform Catmull-Rom segment between p1 and p2 at parameter t in
 * [0, 1]. p0 and p3 are the neighboring control points.
 */
vm::vec3d evaluateSplineSegment(
  const vm::vec3d& p0,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  double t);

/**
 * The offset of the given point's incoming tangent handle relative to the point: the
 * stored offset if the point has manual tangents, otherwise derived from the
 * automatic Catmull-Rom tangent.
 */
vm::vec3d tangentInOffset(
  const std::vector<SplinePoint>& points, size_t index, bool closed = false);

/**
 * The offset of the given point's outgoing tangent handle relative to the point. See
 * tangentInOffset.
 */
vm::vec3d tangentOutOffset(
  const std::vector<SplinePoint>& points, size_t index, bool closed = false);

/**
 * The point on the curve through the given control points, on the segment starting
 * at the given control point index, at parameter t in [0, 1]. Each segment is a
 * cubic Hermite curve whose tangents come from the two endpoints' tangent handles;
 * with automatic tangents, this is the uniform Catmull-Rom curve. If closed is
 * true, the curve wraps around: the last control point connects back to the first,
 * and neighboring control points are looked up modulo the point count.
 */
vm::vec3d curvePoint(
  const std::vector<SplinePoint>& points, size_t segment, double t, bool closed = false);

/**
 * The (normalized, analytic) tangent of the Catmull-Rom curve through the given
 * control points, on the segment starting at the given control point index, at
 * parameter t in [0, 1]. See curvePoint for the meaning of closed.
 */
vm::vec3d curveTangent(
  const std::vector<SplinePoint>& points, size_t segment, double t, bool closed = false);

/**
 * Samples the Catmull-Rom spline through the given control points for display. Each
 * span between two consecutive control points is subdivided into the given number of
 * steps. For an open spline, the result contains (points.size() - 1) * subdivisions +
 * 1 positions; a closed spline additionally contains the segment from the last point
 * back to the first, ending on the first point's position. Returns an empty vector if
 * fewer than two control points are given.
 */
std::vector<vm::vec3d> sampleSpline(
  const std::vector<SplinePoint>& points, size_t subdivisions, bool closed = false);

/**
 * Builds the ordered cross-section frames of a profile sweep along the curve. Each
 * control point segment is divided into round(segmentLength / forwardSize) spans
 * (at least one), so each span holds one copy of a profile of the given forward
 * size, stretched or squished to fit its segment. A closed spline also sweeps the
 * segment from the last control point back to the first.
 *
 * The base orientation comes from a rotation minimizing frame transported across the
 * control points; locked points anchor the transport to their own upright frame.
 * Within each segment, the frame interpolates between its endpoint orientations, and
 * the control points' roll angles and scales are interpolated on top.
 *
 * Returns an empty vector if fewer than two control points are given.
 */
std::vector<SweepFrame> buildSweepFrames(
  const std::vector<SplinePoint>& points, double forwardSize, bool closed = false);

/**
 * The sweep frame at each control point, for display purposes (reference arrows).
 */
std::vector<SweepFrame> computeNodeFrames(
  const std::vector<SplinePoint>& points, bool closed = false);

} // namespace tb::mdl

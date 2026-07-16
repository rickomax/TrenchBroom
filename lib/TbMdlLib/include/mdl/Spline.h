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
 * A single control point of a spline. In addition to its position, a control point
 * carries a roll angle (in degrees) that twists the spline's coordinate frame around
 * its tangent, and a locked flag. Locked points are not affected by operations that
 * modify multiple points at once (such as rotating the spline).
 */
struct SplinePoint
{
  vm::vec3d position;
  double roll = 0.0;
  bool locked = false;

  kdl_reflect_decl(SplinePoint, position, roll, locked);
};

/**
 * A coordinate frame on the spline curve. The tangent points along the curve, and the
 * normal and binormal span the plane perpendicular to it. The frames returned by
 * computeSplineFrames are continuous along the curve (computed by parallel transport)
 * with the control points' roll angles applied on top.
 */
struct SplineFrame
{
  vm::vec3d position;
  vm::vec3d tangent;
  vm::vec3d normal;
  vm::vec3d binormal;
  /** Accumulated arc length from the start of the curve to this frame. */
  double arcLength = 0.0;

  kdl_reflect_decl(SplineFrame, position, tangent, normal, binormal, arcLength);
};

/**
 * Evaluates a centripetal Catmull-Rom segment between p1 and p2 at parameter t in
 * [0, 1]. p0 and p3 are the neighboring control points.
 */
vm::vec3d evaluateSplineSegment(
  const vm::vec3d& p0,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  double t);

/**
 * Samples the Catmull-Rom spline through the given control points. Each span between
 * two consecutive control points is subdivided into the given number of steps, so the
 * result contains (points.size() - 1) * subdivisions + 1 positions. Returns an empty
 * vector if fewer than two control points are given.
 */
std::vector<vm::vec3d> sampleSpline(
  const std::vector<SplinePoint>& points, size_t subdivisions);

/**
 * Computes continuous coordinate frames along the spline through the given control
 * points. The normal is propagated along the curve by parallel transport to avoid
 * sudden flips, and the control points' roll angles (interpolated linearly along each
 * span) are applied as a rotation around the tangent. Returns an empty vector if fewer
 * than two control points are given.
 */
std::vector<SplineFrame> computeSplineFrames(
  const std::vector<SplinePoint>& points, size_t subdivisions);

/**
 * Returns the total arc length of the sampled spline, i.e. the arc length of the last
 * frame.
 */
double splineLength(const std::vector<SplineFrame>& frames);

} // namespace tb::mdl

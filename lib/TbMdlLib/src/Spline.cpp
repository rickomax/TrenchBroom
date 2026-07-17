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

#include "mdl/Spline.h"

#include "kd/reflection_impl.h"

#include "vm/quat.h"
#include "vm/scalar.h"
#include "vm/vec_ext.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>

namespace tb::mdl
{
namespace
{

/** Number of dense samples used to measure a segment's arc length. */
constexpr size_t SegmentLengthSamples = 24;

vm::vec3d controlPoint(const std::vector<SplinePoint>& points, const std::ptrdiff_t index)
{
  const auto clamped =
    vm::clamp(index, std::ptrdiff_t(0), std::ptrdiff_t(points.size()) - 1);
  return points[size_t(clamped)].position;
}

/**
 * Chooses an initial up direction for the given tangent: upright unless the tangent
 * is nearly vertical, in which case the X axis is used as the reference instead.
 */
vm::vec3d chooseInitialUp(const vm::vec3d& tangent)
{
  const auto reference =
    vm::abs(tangent.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{1, 0, 0};
  const auto up = reference - tangent * vm::dot(reference, tangent);
  return vm::squared_length(up) < 1e-6 ? vm::vec3d{0, 0, 1} : vm::normalize(up);
}

/**
 * Orthonormalizes an up vector against a tangent, falling back to the upright
 * projection if they are (nearly) parallel.
 */
vm::vec3d orthonormalUp(const vm::vec3d& up, const vm::vec3d& tangent)
{
  auto right = vm::cross(up, tangent);
  if (vm::squared_length(right) < 1e-8)
  {
    right = vm::cross(chooseInitialUp(tangent), tangent);
  }
  return vm::normalize(vm::cross(tangent, vm::normalize(right)));
}

/**
 * One double reflection (rotation minimizing) transport step of an up vector from a
 * previous frame across a position step to a new tangent — the minimal rotation
 * continuation of the frame.
 */
vm::vec3d doubleReflectUp(
  const vm::vec3d& prevUp,
  const vm::vec3d& prevTangent,
  const vm::vec3d& step,
  const vm::vec3d& tangent)
{
  const auto c1 = vm::dot(step, step);
  if (c1 < 1e-10)
  {
    return prevUp;
  }
  const auto upL = prevUp - step * (2.0 / c1 * vm::dot(step, prevUp));
  const auto tanL = prevTangent - step * (2.0 / c1 * vm::dot(step, prevTangent));
  const auto v2 = tangent - tanL;
  const auto c2 = vm::dot(v2, v2);
  return c2 < 1e-10 ? upL : upL - v2 * (2.0 / c2 * vm::dot(v2, upL));
}

/**
 * Per control point base up vectors: a rotation minimizing frame transported across
 * the control points, seeded upright. A locked point is an anchor — its up is reset
 * to its own upright frame and the transport continues from there, so a twist cannot
 * propagate past it.
 */
std::vector<vm::vec3d> computeBaseNodeUps(const std::vector<SplinePoint>& points)
{
  const auto n = points.size();
  auto ups = std::vector<vm::vec3d>{};
  ups.resize(n);

  auto tangents = std::vector<vm::vec3d>{};
  tangents.reserve(n);
  for (size_t i = 0; i < n; ++i)
  {
    tangents.push_back(
      i < n - 1 ? curveTangent(points, i, 0.0) : curveTangent(points, i - 1, 1.0));
  }

  ups[0] = orthonormalUp(chooseInitialUp(tangents[0]), tangents[0]);
  for (size_t i = 1; i < n; ++i)
  {
    const auto up = points[i].locked ? chooseInitialUp(tangents[i])
                                     : doubleReflectUp(
                                         ups[i - 1],
                                         tangents[i - 1],
                                         points[i].position - points[i - 1].position,
                                         tangents[i]);
    ups[i] = orthonormalUp(up, tangents[i]);
  }
  return ups;
}

/** Spherical interpolation between two (assumed unit) directions. */
vm::vec3d slerpDirection(const vm::vec3d& a, const vm::vec3d& b, const double t)
{
  const auto d = vm::clamp(vm::dot(a, b), -1.0, 1.0);
  if (d > 0.9995)
  {
    return vm::normalize(vm::mix(a, b, vm::vec3d::fill(t)));
  }
  auto perpendicular = b - a * d;
  if (vm::squared_length(perpendicular) < 1e-12)
  {
    return a;
  }
  perpendicular = vm::normalize(perpendicular);
  const auto theta = std::acos(d) * t;
  return vm::normalize(a * std::cos(theta) + perpendicular * std::sin(theta));
}

/** Per point roll interpolated linearly across the given control point segment. */
double rollAt(
  const std::vector<SplinePoint>& points, const size_t segment, const double t)
{
  const auto a = points[segment].roll;
  const auto b = points[vm::min(segment + 1, points.size() - 1)].roll;
  return vm::mix(a, b, t);
}

/** Per point scale interpolated linearly across the given control point segment. */
double scaleAt(
  const std::vector<SplinePoint>& points, const size_t segment, const double t)
{
  const auto a = points[segment].scale;
  const auto b = points[vm::min(segment + 1, points.size() - 1)].scale;
  return vm::mix(a, b, t);
}

/**
 * One sweep frame at curve parameter (segment, t): interpolate the two endpoint
 * nodes' reference ups across the segment, re-orthogonalize against the actual
 * tangent, then apply the interpolated roll and scale.
 */
SweepFrame sweepFrameAt(
  const std::vector<SplinePoint>& points,
  const std::vector<vm::vec3d>& nodeUps,
  const size_t segment,
  const double t)
{
  const auto position = curvePoint(points, segment, t);
  const auto tangent = curveTangent(points, segment, t);

  auto up = slerpDirection(nodeUps[segment], nodeUps[segment + 1], t);
  auto right = vm::cross(up, tangent);
  if (vm::squared_length(right) < 1e-8)
  {
    up = chooseInitialUp(tangent);
    right = vm::cross(up, tangent);
  }
  right = vm::normalize(right);
  up = vm::normalize(vm::cross(tangent, right));

  const auto roll = rollAt(points, segment, t);
  if (vm::abs(roll) > 1e-4)
  {
    const auto rotation = vm::quatd{tangent, vm::to_radians(roll)};
    right = rotation * right;
    up = rotation * up;
  }

  return SweepFrame{position, right, up, scaleAt(points, segment, t)};
}

/** Measures the arc length of a control point segment by dense sampling. */
double segmentLength(const std::vector<SplinePoint>& points, const size_t segment)
{
  auto length = 0.0;
  auto previous = curvePoint(points, segment, 0.0);
  for (size_t s = 1; s <= SegmentLengthSamples; ++s)
  {
    const auto point =
      curvePoint(points, segment, double(s) / double(SegmentLengthSamples));
    length += vm::length(point - previous);
    previous = point;
  }
  return length;
}

} // namespace

kdl_reflect_impl(SplinePoint);
kdl_reflect_impl(SweepFrame);

vm::vec3d evaluateSplineSegment(
  const vm::vec3d& p0,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  const double t)
{
  const auto t2 = t * t;
  const auto t3 = t2 * t;
  return (p1 * 2.0 + (p2 - p0) * t + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2
          + (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3)
         * 0.5;
}

vm::vec3d curvePoint(
  const std::vector<SplinePoint>& points, const size_t segment, const double t)
{
  return evaluateSplineSegment(
    controlPoint(points, std::ptrdiff_t(segment) - 1),
    controlPoint(points, std::ptrdiff_t(segment)),
    controlPoint(points, std::ptrdiff_t(segment) + 1),
    controlPoint(points, std::ptrdiff_t(segment) + 2),
    t);
}

vm::vec3d curveTangent(
  const std::vector<SplinePoint>& points, const size_t segment, const double t)
{
  const auto p0 = controlPoint(points, std::ptrdiff_t(segment) - 1);
  const auto p1 = controlPoint(points, std::ptrdiff_t(segment));
  const auto p2 = controlPoint(points, std::ptrdiff_t(segment) + 1);
  const auto p3 = controlPoint(points, std::ptrdiff_t(segment) + 2);

  const auto t2 = t * t;
  const auto d = ((p2 - p0) + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * (2.0 * t)
                  + (p1 * 3.0 - p0 - p2 * 3.0 + p3) * (3.0 * t2))
                 * 0.5;
  return vm::squared_length(d) > 1e-10 ? vm::normalize(d) : vm::vec3d{1, 0, 0};
}

std::vector<vm::vec3d> sampleSpline(
  const std::vector<SplinePoint>& points, const size_t subdivisions)
{
  if (points.size() < 2 || subdivisions == 0)
  {
    return {};
  }

  auto result = std::vector<vm::vec3d>{};
  result.reserve((points.size() - 1) * subdivisions + 1);

  for (size_t segment = 0; segment < points.size() - 1; ++segment)
  {
    for (size_t step = 0; step < subdivisions; ++step)
    {
      result.push_back(curvePoint(points, segment, double(step) / double(subdivisions)));
    }
  }
  result.push_back(points.back().position);

  return result;
}

std::vector<SweepFrame> buildSweepFrames(
  const std::vector<SplinePoint>& points, const double forwardSize)
{
  auto frames = std::vector<SweepFrame>{};
  if (points.size() < 2 || forwardSize <= 0.0)
  {
    return frames;
  }

  const auto nodeUps = computeBaseNodeUps(points);

  frames.push_back(sweepFrameAt(points, nodeUps, 0, 0.0));
  for (size_t segment = 0; segment < points.size() - 1; ++segment)
  {
    const auto length = segmentLength(points, segment);
    const auto spanCount = vm::max(size_t(1), size_t(std::llround(length / forwardSize)));
    for (size_t k = 1; k <= spanCount; ++k)
    {
      frames.push_back(
        sweepFrameAt(points, nodeUps, segment, double(k) / double(spanCount)));
    }
  }
  return frames;
}

std::vector<SweepFrame> computeNodeFrames(const std::vector<SplinePoint>& points)
{
  auto frames = std::vector<SweepFrame>{};
  if (points.size() < 2)
  {
    return frames;
  }

  const auto nodeUps = computeBaseNodeUps(points);
  for (size_t i = 0; i < points.size(); ++i)
  {
    frames.push_back(
      i < points.size() - 1 ? sweepFrameAt(points, nodeUps, i, 0.0)
                            : sweepFrameAt(points, nodeUps, points.size() - 2, 1.0));
  }
  return frames;
}

} // namespace tb::mdl

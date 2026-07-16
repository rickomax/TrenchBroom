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

#include <cassert>

namespace tb::mdl
{
namespace
{

/**
 * Computes the parameter increment for the centripetal parameterization between two
 * control points. Coincident points get a small positive increment to keep the
 * parameterization strictly increasing.
 */
double centripetalDelta(const vm::vec3d& a, const vm::vec3d& b)
{
  const auto d = std::sqrt(vm::length(b - a));
  return vm::max(d, 1e-4);
}

vm::vec3d evaluateCentripetal(
  const vm::vec3d& p0,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  const double t)
{
  const auto t0 = 0.0;
  const auto t1 = t0 + centripetalDelta(p0, p1);
  const auto t2 = t1 + centripetalDelta(p1, p2);
  const auto t3 = t2 + centripetalDelta(p2, p3);

  const auto u = vm::mix(t1, t2, t);

  const auto a1 = p0 * ((t1 - u) / (t1 - t0)) + p1 * ((u - t0) / (t1 - t0));
  const auto a2 = p1 * ((t2 - u) / (t2 - t1)) + p2 * ((u - t1) / (t2 - t1));
  const auto a3 = p2 * ((t3 - u) / (t3 - t2)) + p3 * ((u - t2) / (t3 - t2));

  const auto b1 = a1 * ((t2 - u) / (t2 - t0)) + a2 * ((u - t0) / (t2 - t0));
  const auto b2 = a2 * ((t3 - u) / (t3 - t1)) + a3 * ((u - t1) / (t3 - t1));

  return b1 * ((t2 - u) / (t2 - t1)) + b2 * ((u - t1) / (t2 - t1));
}

/**
 * Returns the control point quadruple for the span starting at the given index. The
 * first and last control points are duplicated to clamp the curve to its end points.
 */
std::tuple<vm::vec3d, vm::vec3d, vm::vec3d, vm::vec3d> spanControlPoints(
  const std::vector<SplinePoint>& points, const size_t spanIndex)
{
  const auto& p1 = points[spanIndex].position;
  const auto& p2 = points[spanIndex + 1].position;
  const auto& p0 = spanIndex > 0 ? points[spanIndex - 1].position : p1;
  const auto& p3 = spanIndex + 2 < points.size() ? points[spanIndex + 2].position : p2;
  return {p0, p1, p2, p3};
}

vm::vec3d initialNormal(const vm::vec3d& tangent)
{
  // Prefer a horizontal normal so that untwisted splines keep their template upright;
  // fall back to the X axis if the tangent is (almost) vertical.
  const auto up = vm::abs(vm::dot(tangent, vm::vec3d{0, 0, 1})) < 0.999
                    ? vm::vec3d{0, 0, 1}
                    : vm::vec3d{1, 0, 0};
  return vm::normalize(vm::cross(up, tangent));
}

} // namespace

kdl_reflect_impl(SplinePoint);
kdl_reflect_impl(SplineFrame);

vm::vec3d evaluateSplineSegment(
  const vm::vec3d& p0,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  const double t)
{
  return evaluateCentripetal(p0, p1, p2, p3, t);
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

  for (size_t spanIndex = 0; spanIndex < points.size() - 1; ++spanIndex)
  {
    const auto [p0, p1, p2, p3] = spanControlPoints(points, spanIndex);
    for (size_t step = 0; step < subdivisions; ++step)
    {
      const auto t = double(step) / double(subdivisions);
      result.push_back(evaluateCentripetal(p0, p1, p2, p3, t));
    }
  }
  result.push_back(points.back().position);

  return result;
}

std::vector<SplineFrame> computeSplineFrames(
  const std::vector<SplinePoint>& points, const size_t subdivisions)
{
  const auto positions = sampleSpline(points, subdivisions);
  if (positions.empty())
  {
    return {};
  }

  const auto sampleCount = positions.size();

  // Compute tangents by central differences over the sampled positions.
  auto tangents = std::vector<vm::vec3d>{};
  tangents.reserve(sampleCount);
  for (size_t i = 0; i < sampleCount; ++i)
  {
    const auto& prev = positions[i > 0 ? i - 1 : i];
    const auto& next = positions[i + 1 < sampleCount ? i + 1 : i];
    const auto direction = next - prev;
    tangents.push_back(
      vm::squared_length(direction) > 0.0 ? vm::normalize(direction)
      : i > 0                             ? tangents[i - 1]
                                          : vm::vec3d{1, 0, 0});
  }

  auto frames = std::vector<SplineFrame>{};
  frames.reserve(sampleCount);

  // Propagate the normal along the curve by parallel transport: rotate the previous
  // normal by the rotation that maps the previous tangent onto the current one.
  auto normal = initialNormal(tangents.front());
  auto arcLength = 0.0;

  for (size_t i = 0; i < sampleCount; ++i)
  {
    if (i > 0)
    {
      arcLength += vm::length(positions[i] - positions[i - 1]);

      const auto rotation = vm::quatd{tangents[i - 1], tangents[i]};
      normal = vm::normalize(rotation * normal);
      // Re-orthogonalize to counter numerical drift.
      normal = vm::normalize(normal - tangents[i] * vm::dot(normal, tangents[i]));
    }

    // Interpolate the roll angle linearly along each span and apply it around the
    // tangent.
    const auto splineParam =
      double(i) / double(subdivisions); // in [0, points.size() - 1]
    const auto spanIndex = vm::min(size_t(splineParam), points.size() - 2);
    const auto spanParam = splineParam - double(spanIndex);
    const auto roll = vm::mix(
      points[spanIndex].roll, points[spanIndex + 1].roll, vm::min(spanParam, 1.0));

    const auto rolledNormal = vm::quatd{tangents[i], vm::to_radians(roll)} * normal;

    frames.push_back(SplineFrame{
      positions[i],
      tangents[i],
      vm::normalize(rolledNormal),
      vm::normalize(vm::cross(tangents[i], rolledNormal)),
      arcLength});
  }

  return frames;
}

double splineLength(const std::vector<SplineFrame>& frames)
{
  return frames.empty() ? 0.0 : frames.back().arcLength;
}

} // namespace tb::mdl

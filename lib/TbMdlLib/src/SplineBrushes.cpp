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

#include "mdl/SplineBrushes.h"

#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"

#include "kd/result.h"

#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_ext.h"

#include <vector>

namespace tb::mdl
{
namespace
{

/**
 * Free-form deformation: maps a point inside the lattice onto the span between the
 * cross-section frames a and b. The X position inside the lattice interpolates
 * linearly between the two cross-sections, and the Y / Z offsets from the lattice
 * center are applied along each frame's right / up direction, scaled by the frame's
 * cross-section scale (which tapers the profile without changing its length).
 */
vm::vec3d ffdDeform(
  const vm::vec3d& point,
  const vm::bbox3d& lattice,
  const SweepFrame& a,
  const SweepFrame& b)
{
  const auto sx = vm::max(1e-4, lattice.size().x());
  const auto u = vm::clamp((point.x() - lattice.min.x()) / sx, 0.0, 1.0);
  const auto offY = point.y() - lattice.center().y();
  const auto offZ = point.z() - lattice.center().z();

  const auto csA = a.position + a.right * (offY * a.scale) + a.up * (offZ * a.scale);
  const auto csB = b.position + b.right * (offY * b.scale) + b.up * (offZ * b.scale);
  return vm::mix(csA, csB, vm::vec3d::fill(u));
}

/**
 * Copies the attributes of the best matching template face onto each face of the
 * given brush. Faces are matched by comparing the deformed face's normal, transformed
 * back into template space using the span's frames, with the template faces' normals.
 */
void copyFaceAttributes(
  Brush& brush, const Brush& templateBrush, const SweepFrame& a, const SweepFrame& b)
{
  const auto direction = b.position - a.position;
  const auto tangent = vm::squared_length(direction) > 0.0
                         ? vm::normalize(direction)
                         : vm::normalize(vm::cross(a.right, a.up));
  const auto right = vm::normalize(a.right + b.right);
  const auto up = vm::normalize(a.up + b.up);

  for (auto& face : brush.faces())
  {
    // Transform the world space normal back into template space, where the span's
    // tangent, right and up correspond to the +X, +Y and +Z axes.
    const auto localNormal = vm::vec3d{
      vm::dot(face.normal(), tangent),
      vm::dot(face.normal(), right),
      vm::dot(face.normal(), up)};

    const BrushFace* bestMatch = nullptr;
    auto bestDot = -2.0;
    for (const auto& templateFace : templateBrush.faces())
    {
      const auto d = vm::dot(localNormal, templateFace.normal());
      if (d > bestDot)
      {
        bestDot = d;
        bestMatch = &templateFace;
      }
    }

    if (bestMatch)
    {
      face.setAttributes(*bestMatch);
    }
  }
}

} // namespace

Result<std::vector<Brush>> createSplineBrushes(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds)
{
  if (templateBounds.size().x() <= 0.0)
  {
    return Error{"Spline template must have a non-zero extent along the X axis"};
  }

  if (points.size() < 2)
  {
    return Error{"Spline must have at least two control points"};
  }

  if (templateBrushes.empty())
  {
    return Error{"Spline template contains no brushes"};
  }

  const auto forwardSize = vm::max(1.0, templateBounds.size().x());
  const auto frames = buildSweepFrames(points, forwardSize);
  if (frames.size() < 2)
  {
    return Error{"Spline has zero length"};
  }

  const auto builder = BrushBuilder{mapFormat, worldBounds};

  auto brushes = std::vector<Brush>{};

  // One copy of every template brush per span between two consecutive frames.
  for (size_t i = 0; i < frames.size() - 1; ++i)
  {
    const auto& a = frames[i];
    const auto& b = frames[i + 1];

    for (const auto* templateBrush : templateBrushes)
    {
      auto deformedPoints = std::vector<vm::vec3d>{};
      deformedPoints.reserve(templateBrush->vertexCount());
      for (const auto& vertex : templateBrush->vertexPositions())
      {
        // Snap the deformed vertices to integer coordinates. Adjacent spans compute
        // identical positions for their shared cross section, so both round the same
        // way and snapping cannot open gaps between them.
        deformedPoints.push_back(vm::round(ffdDeform(vertex, templateBounds, a, b)));
      }

      const auto materialName =
        !templateBrush->faces().empty()
          ? templateBrush->faces().front().attributes().materialName()
          : "";

      builder.createBrush(deformedPoints, materialName)
        | kdl::transform([&](Brush brush) {
            copyFaceAttributes(brush, *templateBrush, a, b);
            brushes.push_back(std::move(brush));
          })
        | kdl::transform_error([](const auto&) {
            // Skip degenerate cells, e.g. where the deformation collapsed the brush.
          });
    }
  }

  if (brushes.empty())
  {
    return Error{"Could not create any spline brushes"};
  }

  return brushes;
}

} // namespace tb::mdl

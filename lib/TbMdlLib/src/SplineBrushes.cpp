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
#include "mdl/BrushFaceAttributes.h"

#include "kd/result.h"

#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_ext.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tb::mdl
{
namespace
{

/**
 * Returns an interpolated frame at the given arc length by blending the two nearest
 * sampled frames.
 */
SplineFrame frameAtArcLength(const std::vector<SplineFrame>& frames, const double a)
{
  const auto clamped = vm::clamp(a, 0.0, frames.back().arcLength);

  const auto it = std::lower_bound(
    frames.begin(), frames.end(), clamped, [](const auto& frame, const auto value) {
      return frame.arcLength < value;
    });

  if (it == frames.begin())
  {
    return frames.front();
  }

  const auto& f1 = *it;
  const auto& f0 = *std::prev(it);
  const auto span = f1.arcLength - f0.arcLength;
  if (span <= 0.0)
  {
    return f0;
  }

  const auto t = (clamped - f0.arcLength) / span;
  const auto tangent = vm::normalize(vm::mix(f0.tangent, f1.tangent, vm::vec3d::fill(t)));
  auto normal = vm::mix(f0.normal, f1.normal, vm::vec3d::fill(t));
  normal = vm::normalize(normal - tangent * vm::dot(normal, tangent));

  return SplineFrame{
    vm::mix(f0.position, f1.position, vm::vec3d::fill(t)),
    tangent,
    normal,
    vm::normalize(vm::cross(tangent, normal)),
    clamped};
}

/**
 * Maps a point given in template coordinates onto the curve. The X coordinate selects
 * the frame via the given arc length mapping, and the Y / Z offsets from the template
 * bounds center are applied along the frame's normal and binormal.
 */
vm::vec3d deformPoint(
  const std::vector<SplineFrame>& frames,
  const vm::bbox3d& templateBounds,
  const double arcStart,
  const double arcScale,
  const vm::vec3d& point)
{
  const auto arc = arcStart + (point.x() - templateBounds.min.x()) * arcScale;
  const auto frame = frameAtArcLength(frames, arc);
  const auto center = templateBounds.center();
  return frame.position + frame.normal * (point.y() - center.y())
         + frame.binormal * (point.z() - center.z());
}

/**
 * Creates a face on the axial plane x = offset whose normal points towards positive or
 * negative X, for slicing template brushes into slabs.
 */
Result<BrushFace> createSlabFace(
  const double offset, const bool positive, const MapFormat mapFormat)
{
  const auto p0 = vm::vec3d{offset, 0, 0};
  const auto p1 = positive ? vm::vec3d{offset, 1, 0} : vm::vec3d{offset, 0, 1};
  const auto p2 = positive ? vm::vec3d{offset, 0, 1} : vm::vec3d{offset, 1, 0};

  return BrushFace::create(p0, p1, p2, BrushFaceAttributes{""}, mapFormat)
         | kdl::and_then([&](BrushFace face) -> Result<BrushFace> {
             const auto expected = vm::vec3d{positive ? 1.0 : -1.0, 0, 0};
             if (vm::dot(face.normal(), expected) < 0.9)
             {
               // The point order convention did not produce the expected normal; use
               // the opposite order instead.
               return BrushFace::create(p0, p2, p1, BrushFaceAttributes{""}, mapFormat);
             }
             return face;
           });
}

/**
 * Copies the attributes of the best matching template face onto each face of the
 * given brush. Faces are matched by comparing the deformed face's normal, transformed
 * back into template space using the given frame, with the template faces' normals.
 */
void copyFaceAttributes(
  Brush& brush, const Brush& templateBrush, const SplineFrame& midFrame)
{
  for (auto& face : brush.faces())
  {
    // Transform the world space normal back into template space, where the frame's
    // tangent, normal and binormal correspond to the +X, +Y and +Z axes.
    const auto localNormal = vm::vec3d{
      vm::dot(face.normal(), midFrame.tangent),
      vm::dot(face.normal(), midFrame.normal),
      vm::dot(face.normal(), midFrame.binormal)};

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

/**
 * Cuts the given template brush to the slab between xMin and xMax. Returns nullopt if
 * the brush does not intersect the slab.
 */
std::optional<Brush> sliceBrush(
  const Brush& templateBrush,
  const double xMin,
  const double xMax,
  const vm::bbox3d& worldBounds,
  const MapFormat mapFormat)
{
  constexpr auto epsilon = 0.001;

  if (
    templateBrush.bounds().max.x() <= xMin + epsilon
    || templateBrush.bounds().min.x() >= xMax - epsilon)
  {
    return std::nullopt;
  }

  auto slice = templateBrush;

  if (templateBrush.bounds().min.x() < xMin - epsilon)
  {
    auto result =
      createSlabFace(xMin, false, mapFormat) | kdl::and_then([&](BrushFace face) {
        return slice.clip(worldBounds, std::move(face));
      });
    if (result.is_error())
    {
      return std::nullopt;
    }
  }

  if (templateBrush.bounds().max.x() > xMax + epsilon)
  {
    auto result =
      createSlabFace(xMax, true, mapFormat) | kdl::and_then([&](BrushFace face) {
        return slice.clip(worldBounds, std::move(face));
      });
    if (result.is_error())
    {
      return std::nullopt;
    }
  }

  return slice;
}

} // namespace

Result<std::vector<Brush>> createSplineBrushes(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplineFrame>& frames,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds)
{
  const auto templateLength = templateBounds.size().x();
  if (templateLength <= 0.0)
  {
    return Error{"Spline template must have a non-zero extent along the X axis"};
  }

  if (frames.size() < 2)
  {
    return Error{"Spline must have at least two control points"};
  }

  const auto totalLength = splineLength(frames);
  if (totalLength <= 0.0)
  {
    return Error{"Spline has zero length"};
  }

  if (templateBrushes.empty())
  {
    return Error{"Spline template contains no brushes"};
  }

  // Repeat the template at its natural size as often as it fits along the curve, so
  // that the generated brushes keep the template's proportions. Only the last
  // repetition is scaled to cover the remaining arc length.
  const auto repetitions =
    vm::max(size_t(1), size_t(std::round(totalLength / templateLength)));

  const auto builder = BrushBuilder{mapFormat, worldBounds};

  auto brushes = std::vector<Brush>{};

  for (size_t repetition = 0; repetition < repetitions; ++repetition)
  {
    const auto arcStart = double(repetition) * templateLength;
    const auto tileLength =
      repetition + 1 < repetitions ? templateLength : totalLength - arcStart;
    const auto arcScale = tileLength / templateLength;
    if (tileLength <= 0.0)
    {
      continue;
    }

    for (const auto* templateBrush : templateBrushes)
    {
      // Determine the arc length range covered by this brush and collect the sample
      // arc lengths that fall into it; each interval between consecutive boundaries
      // becomes one slice of the deformed brush.
      const auto brushArcMin =
        arcStart + (templateBrush->bounds().min.x() - templateBounds.min.x()) * arcScale;
      const auto brushArcMax =
        arcStart + (templateBrush->bounds().max.x() - templateBounds.min.x()) * arcScale;

      auto boundaries = std::vector<double>{brushArcMin};
      for (const auto& frame : frames)
      {
        if (frame.arcLength > brushArcMin + 1.0 && frame.arcLength < brushArcMax - 1.0)
        {
          boundaries.push_back(frame.arcLength);
        }
      }
      boundaries.push_back(brushArcMax);

      for (size_t i = 0; i < boundaries.size() - 1; ++i)
      {
        const auto xMin = templateBounds.min.x() + (boundaries[i] - arcStart) / arcScale;
        const auto xMax =
          templateBounds.min.x() + (boundaries[i + 1] - arcStart) / arcScale;

        const auto slice = sliceBrush(*templateBrush, xMin, xMax, worldBounds, mapFormat);
        if (!slice)
        {
          continue;
        }

        auto points = std::vector<vm::vec3d>{};
        points.reserve(slice->vertexCount());
        for (const auto& vertex : slice->vertexPositions())
        {
          points.push_back(
            deformPoint(frames, templateBounds, arcStart, arcScale, vertex));
        }

        const auto materialName =
          !templateBrush->faces().empty()
            ? templateBrush->faces().front().attributes().materialName()
            : "";

        builder.createBrush(points, materialName) | kdl::transform([&](Brush brush) {
          const auto midArc = vm::mix(boundaries[i], boundaries[i + 1], 0.5);
          copyFaceAttributes(brush, *templateBrush, frameAtArcLength(frames, midArc));
          brushes.push_back(std::move(brush));
        }) | kdl::transform_error([](const auto&) {
          // Skip degenerate slices, e.g. where the deformation collapsed the
          // brush.
        });
      }
    }
  }

  if (brushes.empty())
  {
    return Error{"Could not create any spline brushes"};
  }

  return brushes;
}

} // namespace tb::mdl

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
#include "mdl/UVCoordSystem.h"

#include "kd/result.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_ext.h"

#include <optional>
#include <vector>

namespace tb::mdl
{
namespace
{

/**
 * The affine approximation of the free-form deformation over one span, mapping
 * template (lattice) space into the world. The X axis follows the span between the two
 * frame positions, and the cross-section axes are the averaged, scaled right / up
 * directions of the two frames. Used to realign the template faces' UVs onto the
 * deformed geometry via the regular alignment lock.
 */
vm::mat4x4d spanUVTransform(
  const vm::bbox3d& lattice, const SweepFrame& a, const SweepFrame& b)
{
  const auto sx = vm::max(1e-4, lattice.size().x());
  auto xAxis = (b.position - a.position) / sx;
  if (vm::squared_length(xAxis) < 1e-10)
  {
    // Degenerate span: fall back to the frame's forward direction.
    xAxis = vm::cross(a.right, a.up);
  }
  const auto yAxis = (a.right * a.scale + b.right * b.scale) * 0.5;
  const auto zAxis = (a.up * a.scale + b.up * b.scale) * 0.5;

  const auto origin =
    vm::vec3d{lattice.min.x(), lattice.center().y(), lattice.center().z()};
  const auto translation =
    a.position - xAxis * origin.x() - yAxis * origin.y() - zAxis * origin.z();

  return vm::mat4x4d{
    xAxis.x(),
    yAxis.x(),
    zAxis.x(),
    translation.x(),
    xAxis.y(),
    yAxis.y(),
    zAxis.y(),
    translation.y(),
    xAxis.z(),
    yAxis.z(),
    zAxis.z(),
    translation.z(),
    0.0,
    0.0,
    0.0,
    1.0};
}

/**
 * The affine map that takes one template triangle exactly onto the deformed triangle it
 * became.
 *
 * A face's UVs are an affine function of position on its plane, and a triangle has
 * exactly the three points that pin an affine map down, so UVs carried through this map
 * land on the copy precisely where the template had them. The map fitted to a whole span
 * cannot do that: the deformation it stands in for is not affine, so it is only ever an
 * approximation of what happened to any particular triangle. The two normals supply the
 * fourth correspondence a map in space needs.
 *
 * Returns nullopt if either triangle is degenerate, which the caller treats as a cell
 * that produced nothing worth texturing.
 */
std::optional<vm::mat4x4d> triangleTransform(
  const vm::vec3d& t0,
  const vm::vec3d& t1,
  const vm::vec3d& t2,
  const vm::vec3d& w0,
  const vm::vec3d& w1,
  const vm::vec3d& w2)
{
  const auto templateNormal = vm::cross(t1 - t0, t2 - t0);
  const auto worldNormal = vm::cross(w1 - w0, w2 - w0);
  if (
    vm::squared_length(templateNormal) < 1.0e-10
    || vm::squared_length(worldNormal) < 1.0e-10)
  {
    return std::nullopt;
  }

  const auto edges = [](const vm::vec3d& e1, const vm::vec3d& e2, const vm::vec3d& n) {
    return vm::mat3x3d{
      e1.x(), e2.x(), n.x(), e1.y(), e2.y(), n.y(), e1.z(), e2.z(), n.z()};
  };

  const auto fromTemplate = edges(t1 - t0, t2 - t0, vm::normalize(templateNormal));
  const auto toWorld = edges(w1 - w0, w2 - w0, vm::normalize(worldNormal));

  const auto inverse = vm::invert(fromTemplate);
  if (!inverse)
  {
    return std::nullopt;
  }

  const auto linear = toWorld * *inverse;
  const auto embedded = vm::mat4x4d{
    linear[0][0],
    linear[0][1],
    linear[0][2],
    0.0,
    linear[1][0],
    linear[1][1],
    linear[1][2],
    0.0,
    linear[2][0],
    linear[2][1],
    linear[2][2],
    0.0,
    0.0,
    0.0,
    0.0,
    1.0};

  return vm::translation_matrix(w0) * embedded * vm::translation_matrix(-t0);
}

/**
 * The template brush's faces moved into the span's world space, which is what the
 * generated faces take their attributes and alignment from.
 *
 * Whether the UVs come with them is the whole of the difference between the two modes:
 * following carries them through the deformation, which realigns the texture onto the
 * geometry it produces, and locking leaves them where the template had them. A face
 * whose transformation fails is left where it was.
 */
std::vector<BrushFace> placeTemplateFaces(
  const Brush& templateBrush, const vm::mat4x4d& transform, const SplineUVMode uvMode)
{
  auto faces = std::vector<BrushFace>{};
  faces.reserve(templateBrush.faceCount());
  for (const auto& face : templateBrush.faces())
  {
    auto copy = face;
    if (copy.transform(transform, uvMode == SplineUVMode::Follow).is_error())
    {
      copy = face;
    }
    faces.push_back(std::move(copy));
  }
  return faces;
}

/**
 * Copies the attributes and the UV alignment of the best matching template face onto
 * each face of the given brush. Faces are matched by normal in world space, since the
 * template faces have already been moved into the span.
 */
void copyFaceAttributes(
  Brush& brush,
  const std::vector<BrushFace>& templateFaces,
  const std::optional<BrushFace>& lockedFace)
{
  for (auto& face : brush.faces())
  {
    // A locked cell has its own face, carried onto it by the map that produced it, so
    // there is nothing to match: every face of the cell takes its alignment from that
    // one. Only the outer face is ever seen; the rest are inside the solid.
    const BrushFace* bestMatch = lockedFace ? &*lockedFace : nullptr;
    if (bestMatch)
    {
      face.setAttributes(bestMatch->attributes());
      if (const auto snapshot = bestMatch->takeUVCoordSystemSnapshot())
      {
        face.copyUVCoordSystemFromFace(
          *snapshot, bestMatch->attributes(), bestMatch->boundary(), WrapStyle::Rotation);
      }
      continue;
    }

    auto bestDot = -2.0;
    for (const auto& templateFace : templateFaces)
    {
      const auto d = vm::dot(face.normal(), templateFace.normal());
      if (d > bestDot)
      {
        bestDot = d;
        bestMatch = &templateFace;
      }
    }

    if (bestMatch)
    {
      face.setAttributes(bestMatch->attributes());
      if (const auto snapshot = bestMatch->takeUVCoordSystemSnapshot())
      {
        // Wrap the source face's UV coordinate system onto this face's plane; for UV
        // coordinate systems without a snapshot (paraxial), the attributes copied above
        // already carry the alignment.
        face.copyUVCoordSystemFromFace(
          *snapshot,
          bestMatch->attributes(),
          bestMatch->boundary(),
          WrapStyle::Projection);
      }
    }
  }
}

} // namespace

Result<std::vector<Brush>> createSplineBrushes(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds,
  const bool closed,
  const SplineUVMode uvMode)
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
  const auto frames = buildSweepFrames(points, forwardSize, closed);
  if (frames.size() < 2)
  {
    return Error{"Spline has zero length"};
  }

  const auto builder = BrushBuilder{mapFormat, worldBounds};

  auto brushes = std::vector<Brush>{};

  // One copy of every template brush per span between two consecutive frames. Each
  // template brush is decomposed into tetrahedra before deforming: a tetrahedron is a
  // simplex, so its four deformed corners always form a valid convex brush, whereas
  // deforming a larger convex brush as a whole can make its vertex set non-convex on
  // curved spans, and the convex hull would then shave off vertices and distort the
  // shape. The decomposition fans out from the brush's vertex centroid: every face is
  // triangulated, and each triangle forms a tetrahedron with the centroid.
  //
  // All deformed vertices are snapped to integer coordinates. Adjacent spans compute
  // identical positions for their shared cross section, and tetrahedra sharing a face
  // or edge share its deformed vertices, so everything rounds consistently and
  // snapping cannot open gaps.
  for (size_t i = 0; i < frames.size() - 1; ++i)
  {
    const auto& a = frames[i];
    const auto& b = frames[i + 1];

    const auto uvTransform = spanUVTransform(templateBounds, a, b);

    for (const auto* templateBrush : templateBrushes)
    {
      const auto vertices = templateBrush->vertexPositions();
      auto apex = vm::vec3d{};
      for (const auto& vertex : vertices)
      {
        apex = apex + vertex;
      }
      apex = apex / double(vertices.size());
      const auto deformedApex = vm::round(deformIntoSpan(apex, templateBounds, a, b));

      const auto templateFaces = placeTemplateFaces(*templateBrush, uvTransform, uvMode);

      const auto materialName =
        !templateBrush->faces().empty()
          ? templateBrush->faces().front().attributes().materialName()
          : "";

      for (const auto& face : templateBrush->faces())
      {
        const auto faceVertices = face.vertexPositions();
        auto deformedFaceVertices = std::vector<vm::vec3d>{};
        deformedFaceVertices.reserve(faceVertices.size());
        for (const auto& vertex : faceVertices)
        {
          deformedFaceVertices.push_back(
            vm::round(deformIntoSpan(vertex, templateBounds, a, b)));
        }

        for (size_t j = 1; j + 1 < deformedFaceVertices.size(); ++j)
        {
          // Locked, this cell's own triangle says exactly what happened to it, so the
          // template face's UVs are carried across by that rather than by the map
          // fitted to the span, which would leave them a little off.
          auto lockedFace = std::optional<BrushFace>{};
          if (uvMode == SplineUVMode::Lock)
          {
            if (
              const auto transform = triangleTransform(
                faceVertices[0],
                faceVertices[j],
                faceVertices[j + 1],
                deformedFaceVertices[0],
                deformedFaceVertices[j],
                deformedFaceVertices[j + 1]))
            {
              auto copy = face;
              if (copy.transform(*transform, true).is_success())
              {
                lockedFace = std::move(copy);
              }
            }
          }

          builder.createBrush(
            std::vector<vm::vec3d>{
              deformedApex,
              deformedFaceVertices[0],
              deformedFaceVertices[j],
              deformedFaceVertices[j + 1]},
            materialName)
            | kdl::transform([&](Brush brush) {
                copyFaceAttributes(brush, templateFaces, lockedFace);
                brushes.push_back(std::move(brush));
              })
            | kdl::transform_error([](const auto&) {
                // Skip degenerate tetrahedra, e.g. where the deformation or the
                // rounding collapsed the cell.
              });
        }
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

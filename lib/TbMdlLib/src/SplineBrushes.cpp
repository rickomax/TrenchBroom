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
 * Returns copies of the template brush's faces, transformed into the span's world
 * space with alignment lock, so each copy carries the template's UVs realigned to the
 * deformed geometry. Faces whose transformation fails are returned untransformed.
 */
std::vector<BrushFace> transformTemplateFaces(
  const Brush& templateBrush, const vm::mat4x4d& transform)
{
  auto faces = std::vector<BrushFace>{};
  faces.reserve(templateBrush.faceCount());
  for (const auto& face : templateBrush.faces())
  {
    auto copy = face;
    if (copy.transform(transform, true).is_error())
    {
      copy = face;
    }
    faces.push_back(std::move(copy));
  }
  return faces;
}

/**
 * Copies the attributes and the UV alignment of the best matching transformed
 * template face onto each face of the given brush. Faces are matched by normal in
 * world space, since the template faces have already been transformed into the span.
 */
void copyFaceAttributes(Brush& brush, const std::vector<BrushFace>& templateFaces)
{
  for (auto& face : brush.faces())
  {
    const BrushFace* bestMatch = nullptr;
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
        // coordinate systems without a snapshot (paraxial), the attributes realigned
        // by the transformation above already carry the alignment.
        face.copyUVCoordSystemFromFace(
          *snapshot, bestMatch->attributes(), bestMatch->boundary(), WrapStyle::Projection);
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
  const bool closed)
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
      const auto deformedApex = vm::round(ffdDeform(apex, templateBounds, a, b));

      const auto templateFaces = transformTemplateFaces(*templateBrush, uvTransform);

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
            vm::round(ffdDeform(vertex, templateBounds, a, b)));
        }

        for (size_t j = 1; j + 1 < deformedFaceVertices.size(); ++j)
        {
          builder.createBrush(
            std::vector<vm::vec3d>{
              deformedApex,
              deformedFaceVertices[0],
              deformedFaceVertices[j],
              deformedFaceVertices[j + 1]},
            materialName)
            | kdl::transform([&](Brush brush) {
                copyFaceAttributes(brush, templateFaces);
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

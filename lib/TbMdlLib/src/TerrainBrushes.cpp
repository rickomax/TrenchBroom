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

#include "mdl/TerrainBrushes.h"

#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"

#include "kd/result.h"

#include "vm/vec.h"

#include <array>

namespace tb::mdl
{
namespace
{

/**
 * Applies the terrain's material and texture scale to every face of the given brush.
 */
void applyAttributes(
  Brush& brush, const std::string& materialName, const Terrain& terrain)
{
  for (auto& face : brush.faces())
  {
    auto attributes = face.attributes();
    attributes.setMaterialName(materialName);
    attributes.setScale(vm::vec2f{terrain.texScaleX, terrain.texScaleY});
    face.setAttributes(attributes);
  }
}

} // namespace

Result<std::vector<Brush>> createTerrainBrushes(
  const MapFormat mapFormat, const vm::bbox3d& worldBounds, const Terrain& terrain)
{
  if (!isValidTerrain(terrain))
  {
    return Error{"Terrain is degenerate"};
  }

  const auto builder = BrushBuilder{mapFormat, worldBounds};
  const auto baseZ = terrain.origin.z();

  auto brushes = std::vector<Brush>{};
  brushes.reserve(terrainCellCount(terrain) * 6);

  for (size_t row = 0; row < terrain.rows; ++row)
  {
    for (size_t column = 0; column < terrain.columns; ++column)
    {
      const auto& materialName = terrainCellMaterial(terrain, column, row);

      const auto t00 = terrainVertexPosition(terrain, column, row);
      const auto t10 = terrainVertexPosition(terrain, column + 1, row);
      const auto t11 = terrainVertexPosition(terrain, column + 1, row + 1);
      const auto t01 = terrainVertexPosition(terrain, column, row + 1);

      const auto atBase = [&](const vm::vec3d& top) {
        return vm::vec3d{top.x(), top.y(), baseZ};
      };
      const auto b00 = atBase(t00);
      const auto b10 = atBase(t10);
      const auto b11 = atBase(t11);
      const auto b01 = atBase(t01);

      // Split the cell into two triangular prisms along the 00-11 diagonal, and each
      // prism into three tetrahedra. The vertex order below is the standard prism
      // decomposition; using the same diagonal in every cell keeps neighbouring cells
      // conforming.
      const auto prisms = std::array<std::array<vm::vec3d, 6>, 2>{
        // bottom triangle, then the matching top triangle
        std::array<vm::vec3d, 6>{b00, b10, b11, t00, t10, t11},
        std::array<vm::vec3d, 6>{b00, b11, b01, t00, t11, t01},
      };

      for (const auto& p : prisms)
      {
        const auto& a0 = p[0];
        const auto& b0 = p[1];
        const auto& c0 = p[2];
        const auto& a1 = p[3];
        const auto& b1 = p[4];
        const auto& c1 = p[5];

        const auto tetrahedra = std::array<std::array<vm::vec3d, 4>, 3>{
          std::array<vm::vec3d, 4>{a0, b0, c0, c1},
          std::array<vm::vec3d, 4>{a0, b0, c1, b1},
          std::array<vm::vec3d, 4>{a0, b1, c1, a1},
        };

        for (const auto& tetrahedron : tetrahedra)
        {
          builder.createBrush(
            std::vector<vm::vec3d>{
              tetrahedron[0], tetrahedron[1], tetrahedron[2], tetrahedron[3]},
            materialName)
            | kdl::transform([&](Brush brush) {
                applyAttributes(brush, materialName, terrain);
                brushes.push_back(std::move(brush));
              })
            | kdl::transform_error([](const auto&) {
                // Skip degenerate tetrahedra, e.g. where two corner heights coincide
                // with the base plane.
              });
        }
      }
    }
  }

  if (brushes.empty())
  {
    return Error{"Could not create any terrain brushes"};
  }

  return brushes;
}

} // namespace tb::mdl

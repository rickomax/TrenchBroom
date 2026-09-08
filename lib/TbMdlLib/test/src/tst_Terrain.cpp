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

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/Entity.h"
#include "mdl/MapFormat.h"
#include "mdl/Terrain.h"
#include "mdl/TerrainBrushes.h"
#include "mdl/TerrainEntity.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto worldBounds = vm::bbox3d{8192.0};

Terrain makeTerrain(const size_t columns = 4, const size_t rows = 4)
{
  const auto bounds = vm::bbox3d{
    vm::vec3d{0, 0, 0}, vm::vec3d{double(columns) * 32.0, double(rows) * 32.0, 32.0}};
  return *createTerrain(bounds, 32.0, "some_material");
}

} // namespace

TEST_CASE("Terrain")
{
  SECTION("createTerrain")
  {
    SECTION("creates a flat terrain covering the bounds")
    {
      const auto terrain = makeTerrain(4, 4);

      CHECK(terrain.columns == 4);
      CHECK(terrain.rows == 4);
      CHECK(terrainVertexCount(terrain) == 25);
      CHECK(terrainCellCount(terrain) == 16);
      CHECK(isValidTerrain(terrain));

      // Every vertex starts at the top of the bounds, so the terrain is a solid box.
      for (const auto height : terrain.heights)
      {
        CHECK(height == vm::approx{32.0});
      }

      CHECK(terrainBounds(terrain).min == vm::approx{vm::vec3d{0, 0, 0}});
      CHECK(terrainBounds(terrain).max == vm::approx{vm::vec3d{128, 128, 32}});
    }

    SECTION("rejects degenerate and oversized bounds")
    {
      // Smaller than half a cell, so it does not round up to a single cell.
      CHECK(
        createTerrain(vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{8, 8, 32}}, 32.0, "")
        == std::nullopt);
      CHECK(
        createTerrain(vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{128, 128, 0}}, 32.0, "")
        == std::nullopt);
      // More cells than TerrainMaxCells.
      CHECK(
        createTerrain(vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{8192, 8192, 32}}, 32.0, "")
        == std::nullopt);
    }
  }

  SECTION("sculptTerrain")
  {
    SECTION("raise lifts vertices with a falloff towards the radius")
    {
      auto terrain = makeTerrain();
      const auto center = terrainVertexPosition(terrain, 2, 2);

      REQUIRE(sculptTerrain(terrain, center, 64.0, 16.0, TerrainSculptMode::Raise));

      const auto centerHeight = terrain.heights[terrainVertexIndex(terrain, 2, 2)];
      const auto nearHeight = terrain.heights[terrainVertexIndex(terrain, 3, 2)];
      const auto outsideHeight = terrain.heights[terrainVertexIndex(terrain, 0, 2)];

      // Full strength at the center, less further out, nothing beyond the radius.
      CHECK(centerHeight == vm::approx{48.0});
      CHECK(nearHeight > 32.0);
      CHECK(nearHeight < centerHeight);
      CHECK(outsideHeight == vm::approx{32.0});
    }

    SECTION("lower is the inverse of raise")
    {
      auto terrain = makeTerrain();
      const auto center = terrainVertexPosition(terrain, 2, 2);

      REQUIRE(sculptTerrain(terrain, center, 64.0, 8.0, TerrainSculptMode::Raise));
      REQUIRE(sculptTerrain(terrain, center, 64.0, 8.0, TerrainSculptMode::Lower));

      for (const auto height : terrain.heights)
      {
        CHECK(height == vm::approx{32.0});
      }
    }

    SECTION("vertices never sink below the base plane")
    {
      auto terrain = makeTerrain();
      const auto center = terrainVertexPosition(terrain, 2, 2);

      for (auto i = 0; i < 20; ++i)
      {
        sculptTerrain(terrain, center, 128.0, 64.0, TerrainSculptMode::Lower);
      }

      for (const auto height : terrain.heights)
      {
        CHECK(height >= terrain.origin.z() + TerrainMinThickness);
      }
    }

    SECTION("smooth evens out a spike")
    {
      auto terrain = makeTerrain();
      const auto spike = terrainVertexIndex(terrain, 2, 2);
      terrain.heights[spike] = 256.0;

      const auto center = terrainVertexPosition(terrain, 2, 2);
      REQUIRE(sculptTerrain(terrain, center, 96.0, 32.0, TerrainSculptMode::Smooth));

      // The spike drops towards its neighbours, which rise towards it.
      CHECK(terrain.heights[spike] < 256.0);
      CHECK(terrain.heights[terrainVertexIndex(terrain, 3, 2)] > 32.0);
    }

    SECTION("flatten pulls vertices towards the height under the brush")
    {
      auto terrain = makeTerrain();
      terrain.heights[terrainVertexIndex(terrain, 2, 2)] = 128.0;

      auto center = terrainVertexPosition(terrain, 2, 2);
      center = vm::vec3d{center.x(), center.y(), 32.0};

      REQUIRE(sculptTerrain(terrain, center, 64.0, 32.0, TerrainSculptMode::Flatten));
      CHECK(terrain.heights[terrainVertexIndex(terrain, 2, 2)] == vm::approx{32.0});
    }
  }

  SECTION("paintTerrain assigns materials within the radius")
  {
    auto terrain = makeTerrain();
    const auto center = terrainVertexPosition(terrain, 2, 2);

    REQUIRE(paintTerrain(terrain, center, 40.0, "painted"));

    CHECK(terrainCellMaterial(terrain, 1, 1) == "painted");
    // A cell well outside the radius keeps the default material.
    CHECK(terrainCellMaterial(terrain, 0, 0) == "some_material");
  }

  SECTION("pickTerrain finds the surface under a ray")
  {
    const auto terrain = makeTerrain();
    const auto ray = vm::ray3d{vm::vec3d{64, 64, 512}, vm::vec3d{0, 0, -1}};

    const auto hit = pickTerrain(terrain, ray);
    REQUIRE(hit.has_value());
    CHECK(hit->z() == vm::approx{32.0});

    // A ray that misses the terrain entirely.
    CHECK(
      pickTerrain(terrain, vm::ray3d{vm::vec3d{-512, -512, 512}, vm::vec3d{0, 0, -1}})
      == std::nullopt);
  }

  SECTION("createTerrainBrushes")
  {
    SECTION("produces two triangular prisms per cell")
    {
      const auto terrain = makeTerrain(2, 2);
      const auto brushes =
        createTerrainBrushes(MapFormat::Standard, worldBounds, terrain) | kdl::value();

      CHECK(brushes.size() == terrainCellCount(terrain) * TerrainBrushesPerCell);

      for (const auto& brush : brushes)
      {
        // A triangular prism has five faces and six vertices.
        CHECK(brush.faceCount() == 5);
        CHECK(brush.vertexCount() == 6);
        CHECK(brush.fullySpecified());
      }
    }

    SECTION("a cell's brushes can be regenerated on their own")
    {
      const auto terrain = makeTerrain(3, 3);
      const auto all =
        createTerrainBrushes(MapFormat::Standard, worldBounds, terrain) | kdl::value();

      // The cell's brushes appear at its index in row major order, so they can be
      // swapped in place when only that cell changes.
      for (size_t row = 0; row < terrain.rows; ++row)
      {
        for (size_t column = 0; column < terrain.columns; ++column)
        {
          const auto cellBrushes =
            createTerrainCellBrushes(
              MapFormat::Standard, worldBounds, terrain, column, row)
            | kdl::value();
          REQUIRE(cellBrushes.size() == TerrainBrushesPerCell);

          const auto base = (row * terrain.columns + column) * TerrainBrushesPerCell;
          for (size_t i = 0; i < TerrainBrushesPerCell; ++i)
          {
            CHECK(cellBrushes[i].bounds() == all[base + i].bounds());
          }
        }
      }
    }

    SECTION("the brushes fill the terrain's bounds")
    {
      const auto terrain = makeTerrain(2, 2);
      const auto brushes =
        createTerrainBrushes(MapFormat::Standard, worldBounds, terrain) | kdl::value();

      auto bounds = brushes.front().bounds();
      for (const auto& brush : brushes)
      {
        bounds = vm::merge(bounds, brush.bounds());
      }

      CHECK(bounds.min == vm::approx{terrainBounds(terrain).min});
      CHECK(bounds.max == vm::approx{terrainBounds(terrain).max});
    }

    SECTION("faces carry the cell material and the texture scale")
    {
      auto terrain = makeTerrain(2, 2);
      terrain.texScaleX = 2.0f;
      terrain.texScaleY = 4.0f;
      REQUIRE(
        paintTerrain(terrain, terrainVertexPosition(terrain, 0, 0), 40.0, "painted"));

      const auto brushes =
        createTerrainBrushes(MapFormat::Standard, worldBounds, terrain) | kdl::value();

      auto foundPainted = false;
      for (const auto& brush : brushes)
      {
        for (const auto& face : brush.faces())
        {
          CHECK(face.attributes().scale() == vm::vec2f{2.0f, 4.0f});
          const auto& name = face.attributes().materialName();
          CHECK((name == "some_material" || name == "painted"));
          foundPainted = foundPainted || name == "painted";
        }
      }
      CHECK(foundPainted);
    }

    SECTION("a sculpted terrain still produces valid brushes")
    {
      auto terrain = makeTerrain(4, 4);
      sculptTerrain(
        terrain,
        terrainVertexPosition(terrain, 2, 2),
        96.0,
        64.0,
        TerrainSculptMode::Raise);

      const auto brushes =
        createTerrainBrushes(MapFormat::Standard, worldBounds, terrain) | kdl::value();

      REQUIRE(!brushes.empty());
      for (const auto& brush : brushes)
      {
        CHECK(brush.fullySpecified());
        CHECK(worldBounds.contains(brush.bounds()));
      }
    }
  }

  SECTION("TerrainEntity round-trip")
  {
    auto terrain = makeTerrain(4, 4);
    terrain.texScaleX = 0.5f;
    terrain.texScaleY = 2.5f;
    sculptTerrain(
      terrain,
      terrainVertexPosition(terrain, 2, 2),
      96.0,
      24.0,
      TerrainSculptMode::Raise);
    paintTerrain(terrain, terrainVertexPosition(terrain, 1, 1), 40.0, "painted");

    const auto entity = writeTerrainEntity(Entity{}, terrain);
    CHECK(entity.classname() == TerrainEntityClassname);
    CHECK(isTerrainEntity(entity));

    const auto parsed = parseTerrainEntity(entity);
    REQUIRE(parsed.has_value());
    CHECK(parsed->columns == terrain.columns);
    CHECK(parsed->rows == terrain.rows);
    CHECK(parsed->origin == vm::approx{terrain.origin});
    CHECK(parsed->cellSize == vm::approx{terrain.cellSize});
    CHECK(parsed->texScaleX == terrain.texScaleX);
    CHECK(parsed->texScaleY == terrain.texScaleY);
    CHECK(parsed->defaultMaterial == terrain.defaultMaterial);
    CHECK(parsed->materials == terrain.materials);

    REQUIRE(parsed->heights.size() == terrain.heights.size());
    for (size_t i = 0; i < terrain.heights.size(); ++i)
    {
      CHECK(parsed->heights[i] == vm::approx{terrain.heights[i]});
    }

    SECTION("a non-terrain entity does not parse")
    {
      CHECK_FALSE(isTerrainEntity(Entity{}));
      CHECK(parseTerrainEntity(Entity{}) == std::nullopt);
    }

    SECTION("shrinking a terrain removes stale chunks")
    {
      const auto smaller = makeTerrain(2, 2);
      const auto updated = writeTerrainEntity(entity, smaller);

      const auto reparsed = parseTerrainEntity(updated);
      REQUIRE(reparsed.has_value());
      CHECK(reparsed->columns == 2);
      CHECK(reparsed->heights.size() == terrainVertexCount(smaller));
    }
  }
}

} // namespace tb::mdl

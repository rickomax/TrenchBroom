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

#include "mdl/Terrain.h"

#include <optional>

namespace tb::mdl
{
class Entity;

namespace TerrainPropertyKeys
{
/** The terrain's origin, "x y z". */
constexpr auto Origin = "_terrain_origin";
/** The width and depth of a cell, "sx sy". A single number, from before cells could be
 * stretched, is read as square cells. */
constexpr auto CellSize = "_terrain_cell_size";
constexpr auto Columns = "_terrain_columns";
constexpr auto Rows = "_terrain_rows";
/** The texture scale applied to all generated faces, "sx sy". */
constexpr auto TexScale = "_terrain_tex_scale";
constexpr auto DefaultMaterial = "_terrain_material";
/** Vertex heights, split across numbered properties (e.g. "_terrain_heights_0")
 * because a height field easily exceeds a sensible property length. Each value holds
 * TerrainValuesPerChunk space separated heights. */
constexpr auto HeightsPrefix = "_terrain_heights_";
/** Per cell material names, split the same way and separated by semicolons, so that
 * empty names (cells using the default material) round trip. */
constexpr auto MaterialsPrefix = "_terrain_materials_";
} // namespace TerrainPropertyKeys

/** The number of heights or cell materials stored in a single property. */
constexpr size_t TerrainValuesPerChunk = 128;

/**
 * The classname used for terrain entities. Like splines, terrains use func_group so
 * that map compilers merge the generated brushes into the world geometry, and so that
 * editors without terrain support still see ordinary brushes.
 */
constexpr auto TerrainEntityClassname = "func_group";

/** Whether the given entity carries terrain data. */
bool isTerrainEntity(const Entity& entity);

/**
 * Reads the terrain stored in the given entity's properties, or nullopt if the entity
 * is not a terrain entity or its data is inconsistent.
 */
std::optional<Terrain> parseTerrainEntity(const Entity& entity);

/**
 * Returns an entity carrying the given terrain in its properties. Any terrain
 * properties not covered by the given terrain (e.g. stale chunks from a larger
 * terrain) are removed.
 */
Entity writeTerrainEntity(const Entity& entity, const Terrain& terrain);

} // namespace tb::mdl

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

#include <QStringList>

#include <filesystem>

class QWidget;

namespace tb
{
namespace mdl
{
class Map;
}

namespace ui
{

/** Whether the given file is an image the editor can bring in as a texture. */
bool isCustomTextureImage(const QString& pathQStr);

/**
 * Brings the given image files into the map as textures it carries itself, available to
 * faces straight away. Nothing is written to disk: saving the map is what puts them in a
 * wad.
 *
 * Reports the ones that could not be read and brings in the rest. Returns whether any
 * were brought in.
 */
bool importCustomTextures(
  const QStringList& pathQStrs, mdl::Map& map, QWidget* dialogParent);

/**
 * Writes the textures the map carries to its own wad, adds that wad to the map's wad
 * list, and forgets the textures, so that they come back from the wad like any other.
 *
 * The map names its wad in a property of its own. Without that property, or when the wad
 * it names has gone, this asks where to put one. An existing wad is added to rather than
 * replaced, and a texture whose name is already in it takes the place of the old one.
 *
 * Call before saving, with the path the map is about to be saved to, since that is where
 * the wad is offered next to. Returns false only when the user cancels, which is their
 * way of cancelling the save as well.
 */
bool saveCustomTextures(
  mdl::Map& map, const std::filesystem::path& mapPath, QWidget* dialogParent);

} // namespace ui
} // namespace tb

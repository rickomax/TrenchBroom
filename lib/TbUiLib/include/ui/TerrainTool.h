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

#include "Notifier.h"
#include "NotifierConnection.h"
#include "mdl/Brush.h"
#include "mdl/Terrain.h"
#include "mdl/TerrainEntity.h"
#include "ui/Tool.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb
{
namespace mdl
{
class EntityNode;
class Grid;
class Node;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class MapDocument;

/** The terrain tool's editing modes. */
enum class TerrainToolMode
{
  Raise,
  Lower,
  Flatten,
  Smooth,
  Texture,
};

/**
 * A tool for creating and sculpting height field terrains.
 *
 * A terrain is created by dragging out a box, and is then sculpted with a circular
 * brush: raising, lowering, flattening and smoothing its vertices, or painting the
 * materials of its cells. The solid geometry is regenerated as tetrahedra after every
 * change (see mdl::createTerrainBrushes) and kept as the children of the terrain's
 * entity, so maps stay usable in editors without terrain support.
 *
 * The terrain itself is persisted in the entity's properties, so it remains editable
 * across sessions; every change goes through a transaction and is undoable.
 */
class TerrainTool : public Tool
{
public:
  Notifier<> terrainDidChangeNotifier;

private:
  MapDocument& m_document;

  mdl::Terrain m_terrain;
  /** The entity node holding the terrain currently being edited, if any. */
  mdl::EntityNode* m_terrainNode = nullptr;

  /** All other terrains in the map, so they can be shown and picked up while the tool
   * is active. Refreshed whenever the document changes. */
  std::vector<std::pair<mdl::EntityNode*, mdl::Terrain>> m_otherTerrains;

  /** Whether dragging out a box creates a new terrain. */
  bool m_addMode = false;
  TerrainToolMode m_mode = TerrainToolMode::Raise;

  double m_radius = 128.0;
  double m_strength = 16.0;
  double m_cellSize = 32.0;

  /** The position of the sculpting brush on the terrain's surface, if the mouse is
   * over it; drawn as a circle of the current radius. */
  std::optional<vm::vec3d> m_brushPosition;

  /** The terrain as it was before the current stroke started, so that the whole
   * stroke can be undone in one step and cancelled cleanly. */
  std::optional<mdl::Terrain> m_strokeOriginal;

  /** Whether a long running transaction is open for the current stroke. The terrain
   * is updated live inside it, and it becomes a single undoable step when the stroke
   * ends. */
  bool m_strokeTransaction = false;

  /** The brushes of every cell the current stroke has touched, kept so that replaying
   * the stroke after a rollback does not have to rebuild cells that have not changed
   * since they were last generated. */
  std::map<size_t, std::vector<mdl::Brush>> m_strokeBrushes;

  bool m_ignoreNotifications = false;

  NotifierConnection m_notifierConnection;

public:
  explicit TerrainTool(MapDocument& document);
  ~TerrainTool() override;

  const mdl::Grid& grid() const;

  void render(
    render::RenderContext& renderContext, render::RenderBatch& renderBatch) const;

public: // modes and settings
  bool addMode() const;
  void setAddMode(bool addMode);

  TerrainToolMode mode() const;
  void setMode(TerrainToolMode mode);

  /** The mode actually applied, taking the Shift key into account: it swaps Raise
   * with Lower and Flatten with Smooth. */
  TerrainToolMode effectiveMode(bool invert) const;

  double radius() const;
  void setRadius(double radius);

  double strength() const;
  void setStrength(double strength);

  /** The width and height of a cell of newly created terrains. */
  double cellSize() const;
  void setCellSize(double cellSize);

  float texScaleX() const;
  float texScaleY() const;
  /** Rescales the texture coordinates of the whole current terrain. */
  void setTexScale(float x, float y);

public: // terrain management
  bool hasTerrain() const;
  const mdl::Terrain& terrain() const;

  /** Creates a new terrain filling the given bounds and makes it current. Returns
   * whether the bounds were usable. */
  bool createTerrain(const vm::bbox3d& bounds);

  /** Picks up the terrain whose generated geometry is hit for editing. Returns whether
   * a terrain was hit. */
  bool selectTerrainAt(const mdl::PickResult& pickResult);

  /** The point where the given ray enters the current terrain's surface. */
  std::optional<vm::vec3d> pickSurface(const vm::ray3d& ray) const;

  /** Whether the terrain has generated brushes that can be broken out. */
  bool canBreakTerrain() const;
  /**
   * Duplicates the terrain's generated brushes as standard, editable brushes and
   * removes the terrain, so the geometry can be edited by hand.
   */
  void breakTerrain();

public: // the sculpting brush
  const std::optional<vm::vec3d>& brushPosition() const;
  void setBrushPosition(std::optional<vm::vec3d> position);

  /** Starts a stroke; every stroke is committed as a single undoable step. */
  void beginStroke();
  /** Applies the current mode at the given surface position. Returns whether the
   * terrain changed. */
  bool applyStroke(const vm::vec3d& position, bool invert);
  void endStroke();
  void cancelStroke();
  bool strokeActive() const;

private:
  void loadFromSelection();
  void loadTerrainNode(mdl::EntityNode* terrainNode);
  void clearTerrain();
  void refreshOtherTerrains();

  /**
   * Writes the current terrain to the document by replacing the terrain entity (and
   * its generated brushes) in a single undoable transaction.
   */
  void commitTerrain(const std::string& commandName);

  /** Whether the terrain's brushes can be swapped in place, i.e. the terrain node
   * holds exactly the brushes the current terrain generates. */
  bool canUpdateCellsInPlace() const;

  /** The cells whose brushes the sculpting brush at the given position can affect. */
  std::vector<size_t> cellsInRadius(const vm::vec3d& position) const;

  /** Regenerates and caches the brushes of the given cells. Returns false if any cell
   * could not be built. */
  bool regenerateCells(const std::vector<size_t>& cells);

  /**
   * Replays the whole stroke onto the document: rolls the open transaction back to the
   * state before the stroke and swaps in the cached brushes of every touched cell,
   * together with the terrain entity's updated properties.
   */
  bool applyStrokeToDocument();

  /** The name of the undoable step produced by a stroke in the current mode. */
  std::string strokeCommandName() const;

  std::vector<mdl::Node*> createBrushNodes() const;

private:
  bool doActivate() override;
  bool doDeactivate() override;

  QWidget* doCreatePage(QWidget* parent) override;

  void connectObservers();
  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void selectionDidChange();
};

} // namespace ui
} // namespace tb

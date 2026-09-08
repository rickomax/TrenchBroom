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

#include "ui/TerrainTool.h"

#include "Logger.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/TerrainBrushes.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/TerrainToolPage.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/set_temp.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <ranges>

namespace tb::ui
{

TerrainTool::TerrainTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

TerrainTool::~TerrainTool() = default;

const mdl::Grid& TerrainTool::grid() const
{
  return m_document.map().grid();
}

void TerrainTool::render(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};

  // Outline every terrain in the map so they can be found and picked up.
  renderService.setForegroundColor(pref(Preferences::TerrainBoundsColor));
  renderService.setLineWidth(1.0f);
  for (const auto& [entityNode, terrain] : m_otherTerrains)
  {
    renderService.renderBounds(vm::bbox3f{mdl::terrainBounds(terrain)});
  }

  if (hasTerrain())
  {
    renderService.setLineWidth(2.0f);
    renderService.renderBounds(vm::bbox3f{mdl::terrainBounds(m_terrain)});
  }

  // The sculpting brush: a circle of the current radius lying on the terrain surface.
  if (m_brushPosition && !m_addMode)
  {
    renderService.setShowOccludedObjects();
    renderService.setForegroundColor(pref(Preferences::TerrainBrushColor));
    renderService.setLineWidth(2.0f);
    renderService.renderCircle(
      vm::vec3f{*m_brushPosition}, vm::axis::z, 32, float(m_radius));
    renderService.setForegroundColor(pref(Preferences::TerrainBrushFillColor));
    renderService.renderFilledCircle(
      vm::vec3f{*m_brushPosition}, vm::axis::z, 32, float(m_radius));
  }
}

bool TerrainTool::addMode() const
{
  return m_addMode;
}

void TerrainTool::setAddMode(const bool addMode)
{
  if (addMode != m_addMode)
  {
    m_addMode = addMode;
    m_brushPosition = std::nullopt;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

TerrainToolMode TerrainTool::mode() const
{
  return m_mode;
}

void TerrainTool::setMode(const TerrainToolMode mode)
{
  if (mode != m_mode)
  {
    m_mode = mode;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

TerrainToolMode TerrainTool::effectiveMode(const bool invert) const
{
  if (!invert)
  {
    return m_mode;
  }

  switch (m_mode)
  {
  case TerrainToolMode::Raise:
    return TerrainToolMode::Lower;
  case TerrainToolMode::Lower:
    return TerrainToolMode::Raise;
  case TerrainToolMode::Flatten:
    return TerrainToolMode::Smooth;
  case TerrainToolMode::Smooth:
    return TerrainToolMode::Flatten;
  case TerrainToolMode::Texture:
    return TerrainToolMode::Texture;
  }
  return m_mode;
}

double TerrainTool::radius() const
{
  return m_radius;
}

void TerrainTool::setRadius(const double radius)
{
  if (radius > 0.0 && radius != m_radius)
  {
    m_radius = radius;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

double TerrainTool::strength() const
{
  return m_strength;
}

void TerrainTool::setStrength(const double strength)
{
  if (strength > 0.0 && strength != m_strength)
  {
    m_strength = strength;
    terrainDidChangeNotifier();
  }
}

double TerrainTool::cellSize() const
{
  return m_cellSize;
}

void TerrainTool::setCellSize(const double cellSize)
{
  if (cellSize > 0.0 && cellSize != m_cellSize)
  {
    m_cellSize = cellSize;
    terrainDidChangeNotifier();
  }
}

const std::string& TerrainTool::paintMaterial() const
{
  return m_paintMaterial;
}

void TerrainTool::setPaintMaterial(std::string materialName)
{
  m_paintMaterial = std::move(materialName);
  terrainDidChangeNotifier();
}

float TerrainTool::texScaleX() const
{
  return m_terrain.texScaleX;
}

float TerrainTool::texScaleY() const
{
  return m_terrain.texScaleY;
}

void TerrainTool::setTexScale(const float x, const float y)
{
  if (
    !hasTerrain() || x == 0.0f || y == 0.0f
    || (m_terrain.texScaleX == x && m_terrain.texScaleY == y))
  {
    return;
  }

  m_terrain.texScaleX = x;
  m_terrain.texScaleY = y;
  commitTerrain("Change Terrain Texture Scale");
}

bool TerrainTool::hasTerrain() const
{
  return mdl::isValidTerrain(m_terrain);
}

const mdl::Terrain& TerrainTool::terrain() const
{
  return m_terrain;
}

bool TerrainTool::createTerrain(const vm::bbox3d& bounds)
{
  auto& map = m_document.map();

  auto terrain = mdl::createTerrain(bounds, m_cellSize, map.currentMaterialName());
  if (!terrain)
  {
    map.logger().error()
      << "Could not create terrain: the box is too small for a single cell, or needs "
         "more than "
      << mdl::TerrainMaxCells << " cells";
    return false;
  }

  m_terrain = std::move(*terrain);
  m_terrainNode = nullptr;
  commitTerrain("Create Terrain");
  return true;
}

bool TerrainTool::selectTerrainAt(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(mdl::BrushNode::BrushHitType));
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    for (auto* candidate = static_cast<mdl::Node*>(faceHandle->node());
         candidate != nullptr;
         candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_terrainNode
          && mdl::isTerrainEntity(entityNode->entity()))
      {
        loadTerrainNode(entityNode);
        return true;
      }
    }
  }
  return false;
}

std::optional<vm::vec3d> TerrainTool::pickSurface(const vm::ray3d& ray) const
{
  return hasTerrain() ? mdl::pickTerrain(m_terrain, ray) : std::nullopt;
}

bool TerrainTool::canBreakTerrain() const
{
  return m_terrainNode != nullptr && !m_terrainNode->children().empty();
}

void TerrainTool::breakTerrain()
{
  if (!canBreakTerrain())
  {
    return;
  }

  auto& map = m_document.map();

  // Duplicate the generated brushes as standard brushes outside the terrain entity,
  // so they stay behind as editable geometry when the terrain is removed.
  auto duplicates = std::vector<mdl::Node*>{};
  for (auto* child : m_terrainNode->children())
  {
    if (const auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
    {
      duplicates.push_back(new mdl::BrushNode{brushNode->brush()});
    }
  }

  auto* parent = m_terrainNode->parent();

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto transaction = mdl::Transaction{map, "Break Terrain"};
  if (addNodes(map, {{parent, duplicates}}).empty())
  {
    transaction.cancel();
    return;
  }

  removeNodes(map, {m_terrainNode});
  m_terrainNode = nullptr;
  m_terrain = mdl::Terrain{};
  transaction.commit();

  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

const std::optional<vm::vec3d>& TerrainTool::brushPosition() const
{
  return m_brushPosition;
}

void TerrainTool::setBrushPosition(std::optional<vm::vec3d> position)
{
  if (position != m_brushPosition)
  {
    m_brushPosition = std::move(position);
    refreshViews();
  }
}

void TerrainTool::beginStroke()
{
  if (hasTerrain())
  {
    m_strokeOriginal = m_terrain;
  }
}

bool TerrainTool::applyStroke(const vm::vec3d& position, const bool invert)
{
  if (!hasTerrain())
  {
    return false;
  }

  const auto mode = effectiveMode(invert);
  if (mode == TerrainToolMode::Texture)
  {
    if (!mdl::paintTerrain(m_terrain, position, m_radius, m_paintMaterial))
    {
      return false;
    }
  }
  else
  {
    const auto sculptMode =
      mode == TerrainToolMode::Raise     ? mdl::TerrainSculptMode::Raise
      : mode == TerrainToolMode::Lower   ? mdl::TerrainSculptMode::Lower
      : mode == TerrainToolMode::Flatten ? mdl::TerrainSculptMode::Flatten
                                         : mdl::TerrainSculptMode::Smooth;

    if (!mdl::sculptTerrain(m_terrain, position, m_radius, m_strength, sculptMode))
    {
      return false;
    }
  }

  refreshViews();
  return true;
}

void TerrainTool::endStroke()
{
  if (!m_strokeOriginal)
  {
    return;
  }

  const auto original = std::move(*m_strokeOriginal);
  m_strokeOriginal = std::nullopt;

  if (original.heights == m_terrain.heights && original.materials == m_terrain.materials)
  {
    return;
  }

  const auto commandName = [&]() -> std::string {
    switch (m_mode)
    {
    case TerrainToolMode::Flatten:
      return "Flatten Terrain";
    case TerrainToolMode::Smooth:
      return "Smooth Terrain";
    case TerrainToolMode::Texture:
      return "Paint Terrain";
    case TerrainToolMode::Raise:
    case TerrainToolMode::Lower:
      break;
    }
    return "Sculpt Terrain";
  }();

  // A stroke only touches the cells under the brush, so regenerating the whole terrain
  // would be far more work than the edit itself; swap just those cells' brushes.
  if (!commitChangedCells(commandName, original))
  {
    commitTerrain(commandName);
  }
}

std::vector<size_t> TerrainTool::changedCells(const mdl::Terrain& original) const
{
  auto result = std::vector<size_t>{};
  if (
    original.columns != m_terrain.columns || original.rows != m_terrain.rows
    || original.cellSize != m_terrain.cellSize || original.origin != m_terrain.origin
    || original.texScaleX != m_terrain.texScaleX
    || original.texScaleY != m_terrain.texScaleY
    || original.defaultMaterial != m_terrain.defaultMaterial)
  {
    // The terrain's structure changed, so every cell has to be rebuilt.
    return result;
  }

  for (size_t row = 0; row < m_terrain.rows; ++row)
  {
    for (size_t column = 0; column < m_terrain.columns; ++column)
    {
      const auto cell = row * m_terrain.columns + column;
      auto cellChanged = original.materials[cell] != m_terrain.materials[cell];
      for (size_t dr = 0; dr <= 1 && !cellChanged; ++dr)
      {
        for (size_t dc = 0; dc <= 1 && !cellChanged; ++dc)
        {
          const auto vertex = mdl::terrainVertexIndex(m_terrain, column + dc, row + dr);
          cellChanged = original.heights[vertex] != m_terrain.heights[vertex];
        }
      }

      if (cellChanged)
      {
        result.push_back(cell);
      }
    }
  }
  return result;
}

bool TerrainTool::commitChangedCells(
  const std::string& commandName, const mdl::Terrain& original)
{
  auto& map = m_document.map();

  const auto expectedBrushes =
    mdl::terrainCellCount(m_terrain) * mdl::TerrainBrushesPerCell;
  if (!m_terrainNode || m_terrainNode->childCount() != expectedBrushes)
  {
    return false;
  }

  const auto cells = changedCells(original);
  if (cells.empty())
  {
    return false;
  }

  // Swapping the contents of the existing nodes keeps the brush order (and therefore
  // the mapping from cells to brushes) intact, and avoids adding and removing
  // thousands of nodes for every stroke.
  auto nodesToSwap = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  nodesToSwap.reserve(cells.size() * mdl::TerrainBrushesPerCell + 1);

  const auto& children = m_terrainNode->children();
  for (const auto cell : cells)
  {
    const auto column = cell % m_terrain.columns;
    const auto row = cell / m_terrain.columns;

    auto cellBrushes = mdl::createTerrainCellBrushes(
      map.worldNode().mapFormat(), map.worldBounds(), m_terrain, column, row);
    if (cellBrushes.is_error())
    {
      return false;
    }

    auto brushes = std::move(cellBrushes) | kdl::value();
    if (brushes.size() != mdl::TerrainBrushesPerCell)
    {
      return false;
    }

    for (size_t i = 0; i < mdl::TerrainBrushesPerCell; ++i)
    {
      auto* node = children[cell * mdl::TerrainBrushesPerCell + i];
      if (!dynamic_cast<mdl::BrushNode*>(node))
      {
        return false;
      }
      nodesToSwap.emplace_back(node, mdl::NodeContents{std::move(brushes[i])});
    }
  }

  // The height field itself lives in the entity's properties, so it is updated in the
  // same transaction.
  nodesToSwap.emplace_back(
    m_terrainNode,
    mdl::NodeContents{mdl::writeTerrainEntity(m_terrainNode->entity(), m_terrain)});

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  if (!updateNodeContents(map, commandName, std::move(nodesToSwap)))
  {
    return false;
  }

  refreshViews();
  terrainDidChangeNotifier();
  return true;
}

void TerrainTool::cancelStroke()
{
  if (m_strokeOriginal)
  {
    m_terrain = std::move(*m_strokeOriginal);
    m_strokeOriginal = std::nullopt;
    refreshViews();
  }
}

bool TerrainTool::strokeActive() const
{
  return m_strokeOriginal.has_value();
}

void TerrainTool::loadFromSelection()
{
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && mdl::isTerrainEntity(entityNode->entity()))
      {
        loadTerrainNode(entityNode);
        return;
      }
    }
  }
}

void TerrainTool::loadTerrainNode(mdl::EntityNode* terrainNode)
{
  if (auto terrain = mdl::parseTerrainEntity(terrainNode->entity()))
  {
    m_terrainNode = terrainNode;
    m_terrain = std::move(*terrain);
    m_strokeOriginal = std::nullopt;
    m_brushPosition = std::nullopt;

    refreshOtherTerrains();
    refreshViews();
    terrainDidChangeNotifier();
  }
}

void TerrainTool::clearTerrain()
{
  m_terrainNode = nullptr;
  m_terrain = mdl::Terrain{};
  m_strokeOriginal = std::nullopt;
  m_brushPosition = std::nullopt;
  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

void TerrainTool::refreshOtherTerrains()
{
  m_otherTerrains.clear();

  m_document.map().worldNode().accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::GroupNode& groupNode) {
      groupNode.visitChildren(thisLambda);
    },
    [&](mdl::EntityNode& entityNode) {
      if (&entityNode != m_terrainNode && mdl::isTerrainEntity(entityNode.entity()))
      {
        if (auto terrain = mdl::parseTerrainEntity(entityNode.entity()))
        {
          m_otherTerrains.emplace_back(&entityNode, std::move(*terrain));
        }
      }
    },
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));
}

void TerrainTool::commitTerrain(const std::string& commandName)
{
  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto& map = m_document.map();

  if (!hasTerrain())
  {
    if (m_terrainNode)
    {
      auto transaction = mdl::Transaction{map, commandName};
      removeNodes(map, {m_terrainNode});
      m_terrainNode = nullptr;
      transaction.commit();
    }
    refreshViews();
    terrainDidChangeNotifier();
    return;
  }

  auto entity = mdl::writeTerrainEntity(
    m_terrainNode ? m_terrainNode->entity() : mdl::Entity{}, m_terrain);

  // Give the entity an origin so that it has a sensible position while it has no
  // brushes yet.
  entity.addOrUpdateProperty(
    "origin",
    fmt::format(
      "{:g} {:g} {:g}",
      m_terrain.origin.x(),
      m_terrain.origin.y(),
      m_terrain.origin.z()));

  auto* newNode = new mdl::EntityNode{std::move(entity)};
  newNode->addChildren(createBrushNodes());

  auto* parent = m_terrainNode ? m_terrainNode->parent() : parentForNodes(map, {});

  auto transaction = mdl::Transaction{map, commandName};
  if (m_terrainNode)
  {
    removeNodes(map, {m_terrainNode});
  }
  const auto addedNodes = addNodes(map, {{parent, {newNode}}});
  if (addedNodes.empty())
  {
    transaction.cancel();
    m_terrainNode = nullptr;
  }
  else
  {
    m_terrainNode = newNode;
    transaction.commit();
  }

  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

std::vector<mdl::Node*> TerrainTool::createBrushNodes() const
{
  auto& map = m_document.map();
  return mdl::createTerrainBrushes(
           map.worldNode().mapFormat(), map.worldBounds(), m_terrain)
         | kdl::transform([](auto brushes) {
             return brushes | std::views::transform([](auto& brush) {
                      return static_cast<mdl::Node*>(
                        new mdl::BrushNode{std::move(brush)});
                    })
                    | kdl::ranges::to<std::vector>();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not create terrain brushes: " << e.msg;
             return std::vector<mdl::Node*>{};
           })
         | kdl::value();
}

bool TerrainTool::doActivate()
{
  connectObservers();

  // Always start with add mode disabled so that switching to the tool never creates
  // terrains accidentally; the user enables it explicitly on the tool page.
  m_addMode = false;
  m_brushPosition = std::nullopt;
  loadFromSelection();
  refreshOtherTerrains();

  terrainDidChangeNotifier();
  return true;
}

bool TerrainTool::doDeactivate()
{
  m_notifierConnection.disconnect();
  clearTerrain();
  m_otherTerrains.clear();
  return true;
}

QWidget* TerrainTool::doCreatePage(QWidget* parent)
{
  return new TerrainToolPage{m_document, *this, parent};
}

void TerrainTool::connectObservers()
{
  auto& map = m_document.map();
  m_notifierConnection += map.nodesWereAddedNotifier.connect(
    [this](const auto& nodes) { nodesWereAdded(nodes); });
  m_notifierConnection += map.nodesWereRemovedNotifier.connect(
    [this](const auto& nodes) { nodesWereRemoved(nodes); });
  m_notifierConnection += map.nodesDidChangeNotifier.connect(
    [this](const auto& nodes) { nodesDidChange(nodes); });
  m_notifierConnection +=
    map.selectionDidChangeNotifier.connect([this](const auto&) { selectionDidChange(); });
}

void TerrainTool::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherTerrains();

  if (m_terrainNode != nullptr)
  {
    return;
  }

  // Adopt a terrain entity that reappears, e.g. when a terrain edit is undone.
  for (auto* node : nodes)
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && mdl::isTerrainEntity(entityNode->entity()))
    {
      loadTerrainNode(entityNode);
      return;
    }
  }
}

void TerrainTool::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  if (m_terrainNode && std::ranges::find(nodes, m_terrainNode) != nodes.end())
  {
    clearTerrain();
  }
  else
  {
    refreshOtherTerrains();
  }
}

void TerrainTool::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherTerrains();

  if (
    m_terrainNode
    && std::ranges::find(nodes, static_cast<mdl::Node*>(m_terrainNode)) != nodes.end())
  {
    loadTerrainNode(m_terrainNode);
  }
}

void TerrainTool::selectionDidChange()
{
  if (m_ignoreNotifications)
  {
    return;
  }

  // Switch to a newly selected terrain entity, but keep editing the current terrain if
  // the selection does not contain one.
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_terrainNode
          && mdl::isTerrainEntity(entityNode->entity()))
      {
        loadTerrainNode(entityNode);
        return;
      }
    }
  }
}

} // namespace tb::ui

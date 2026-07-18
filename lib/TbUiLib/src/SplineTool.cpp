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

#include "ui/SplineTool.h"

#include "Logger.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
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
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/SplineBrushes.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/SplineToolPage.h"

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

namespace
{
/** Spline point handles are drawn and picked larger than regular point handles. */
constexpr auto SplinePointHandleScale = 5.0;
} // namespace

const mdl::HitType::Type SplineTool::PointHitType = mdl::HitType::freeType();

SplineTool::SplineTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

SplineTool::~SplineTool() = default;

const mdl::Grid& SplineTool::grid() const
{
  return m_document.map().grid();
}

void SplineTool::pick(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult)
{
  // The hit target identifies the spline (by its entity node, null for a new,
  // uncommitted spline) and the point index.
  using Target = std::pair<mdl::EntityNode*, size_t>;

  const auto handleRadius =
    SplinePointHandleScale * double(pref(Preferences::HandleRadius));

  for (size_t i = 0; i < m_points.size(); ++i)
  {
    if (
      const auto distance =
        camera.pickPointHandle(pickRay, m_points[i].position, handleRadius))
    {
      const auto hitPoint = vm::point_at_distance(pickRay, *distance);
      pickResult.addHit(
        mdl::Hit{PointHitType, *distance, hitPoint, Target{m_splineNode, i}});
    }
  }

  for (const auto& [entityNode, data] : m_otherSplines)
  {
    for (size_t i = 0; i < data.points.size(); ++i)
    {
      if (
        const auto distance =
          camera.pickPointHandle(pickRay, data.points[i].position, handleRadius))
      {
        const auto hitPoint = vm::point_at_distance(pickRay, *distance);
        pickResult.addHit(
          mdl::Hit{PointHitType, *distance, hitPoint, Target{entityNode, i}});
      }
    }
  }
}

void SplineTool::render(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult)
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setPointHandleScale(float(SplinePointHandleScale));
  renderService.setShowOccludedObjects();

  // Draw all other splines so they can be picked up for editing.
  for (const auto& [entityNode, data] : m_otherSplines)
  {
    if (data.points.size() > 1)
    {
      const auto samples =
        mdl::sampleSpline(data.points, data.subdivisions, data.closed);
      const auto vertices =
        samples
        | std::views::transform([](const auto& point) { return vm::vec3f{point}; })
        | kdl::ranges::to<std::vector>();

      renderService.setForegroundColor(pref(Preferences::SplineLineColor));
      renderService.setLineWidth(1.0f);
      renderService.renderLineStrip(vertices);
    }

    renderService.setForegroundColor(pref(Preferences::OccludedHandleColor));
    for (const auto& point : data.points)
    {
      renderService.renderHandle(vm::vec3f{point.position});
    }
  }

  if (m_points.empty())
  {
    renderHighlight(renderService, pickResult);
    return;
  }

  if (m_points.size() > 1)
  {
    const auto samples = mdl::sampleSpline(m_points, m_subdivisions, m_closed);
    const auto vertices =
      samples | std::views::transform([](const auto& point) { return vm::vec3f{point}; })
      | kdl::ranges::to<std::vector>();

    renderService.setForegroundColor(pref(Preferences::SplineLineColor));
    renderService.setLineWidth(2.0f);
    renderService.renderLineStrip(vertices);
  }

  // Visualize each point's sweep frame with a reference arrow along its up
  // direction; locked points (frame anchors) are drawn in a different color. The
  // selected point additionally shows its right direction.
  if (m_points.size() > 1)
  {
    const auto frames = mdl::computeNodeFrames(m_points, m_closed);
    const auto axisLength = 24.0;

    renderService.setLineWidth(2.0f);
    for (size_t i = 0; i < frames.size() && i < m_points.size(); ++i)
    {
      const auto& frame = frames[i];
      renderService.setForegroundColor(
        m_points[i].locked ? pref(Preferences::SelectedHandleColor)
                           : pref(Preferences::ZAxisColor));
      renderService.renderLine(
        vm::vec3f{frame.position}, vm::vec3f{frame.position + frame.up * axisLength});

      if (m_selectedIndex == i)
      {
        renderService.setForegroundColor(pref(Preferences::YAxisColor));
        renderService.renderLine(
          vm::vec3f{frame.position},
          vm::vec3f{frame.position + frame.right * axisLength});
      }
    }
  }

  for (size_t i = 0; i < m_points.size(); ++i)
  {
    const auto& point = m_points[i];

    renderService.setForegroundColor(
      m_selectedIndex == i ? pref(Preferences::SelectedHandleColor)
      : point.locked       ? pref(Preferences::SplineLockedHandleColor)
                           : pref(Preferences::HandleColor));
    renderService.renderHandle(vm::vec3f{point.position});
  }

  renderHighlight(renderService, pickResult);

  if (m_selectedIndex && *m_selectedIndex < m_points.size())
  {
    const auto& point = m_points[*m_selectedIndex];
    renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
    renderService.renderHandleHighlight(vm::vec3f{point.position});
    renderService.setBackgroundColor(pref(Preferences::InfoOverlayBackgroundColor));
    renderService.renderString(
      fmt::format(
        "Point {} | Roll {:g} | Scale {:g}{}",
        *m_selectedIndex,
        point.roll,
        point.scale,
        point.locked ? " | Locked" : ""),
      vm::vec3f{point.position});
  }
}

void SplineTool::renderHighlight(
  render::RenderService& renderService, const mdl::PickResult& pickResult) const
{
  // Highlight the hovered point; while add point mode is active, clicks add points
  // instead of selecting, so no highlight is shown.
  if (m_addPointMode)
  {
    return;
  }

  using namespace mdl::HitFilters;
  const auto& hit = pickResult.first(type(PointHitType));
  if (hit.isMatch())
  {
    const auto [entityNode, index] = hit.target<std::pair<mdl::EntityNode*, size_t>>();
    const auto* points = &m_points;
    if (entityNode != m_splineNode)
    {
      for (const auto& [otherNode, otherData] : m_otherSplines)
      {
        if (otherNode == entityNode)
        {
          points = &otherData.points;
          break;
        }
      }
    }
    if (index < points->size())
    {
      renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
      renderService.renderHandleHighlight(vm::vec3f{(*points)[index].position});
    }
  }
}

void SplineTool::renderFeedback(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const vm::vec3d& point) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setPointHandleScale(float(SplinePointHandleScale));
  renderService.setShowOccludedObjects();
  renderService.setForegroundColor(pref(Preferences::HandleColor));
  renderService.renderHandle(vm::vec3f{point});

  if (!m_points.empty())
  {
    renderService.setForegroundColor(pref(Preferences::SplineLineColor));
    renderService.renderLine(
      vm::vec3f{m_points[addPointAnchorIndex()].position}, vm::vec3f{point});
  }
}

bool SplineTool::addPointMode() const
{
  return m_addPointMode;
}

void SplineTool::setAddPointMode(const bool addPointMode)
{
  if (addPointMode != m_addPointMode)
  {
    m_addPointMode = addPointMode;
    refreshViews();
    splineDidChangeNotifier();
  }
}

bool SplineTool::hasPoints() const
{
  return !m_points.empty();
}

const std::vector<mdl::SplinePoint>& SplineTool::points() const
{
  return m_points;
}

std::optional<vm::vec3d> SplineTool::lastPointPosition() const
{
  return !m_points.empty() ? std::optional{m_points[addPointAnchorIndex()].position}
                           : std::nullopt;
}

size_t SplineTool::addPointAnchorIndex() const
{
  // If a point between two other points is selected, new points are inserted after
  // it; otherwise they are appended after the last point.
  return m_selectedIndex && *m_selectedIndex + 1 < m_points.size() ? *m_selectedIndex
                                                                   : m_points.size() - 1;
}

void SplineTool::addPoint(const vm::vec3d& point)
{
  const auto index = m_points.empty() ? 0 : addPointAnchorIndex() + 1;
  m_points.insert(
    m_points.begin() + std::ptrdiff_t(index), mdl::SplinePoint{point});
  m_selectedIndex = index;
  commitSpline("Add Spline Point");
}

bool SplineTool::canRemovePoint() const
{
  return !m_points.empty();
}

void SplineTool::removePoint()
{
  if (m_points.empty())
  {
    return;
  }

  const auto index = m_selectedIndex ? *m_selectedIndex : m_points.size() - 1;
  m_points.erase(std::next(m_points.begin(), std::ptrdiff_t(index)));
  m_selectedIndex = std::nullopt;
  commitSpline("Remove Spline Point");
}

bool SplineTool::selectPoint(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(PointHitType));
  if (!hit.isMatch())
  {
    return false;
  }

  // Hitting another spline's point picks up that spline for editing first.
  const auto [entityNode, index] = hit.target<std::pair<mdl::EntityNode*, size_t>>();
  if (entityNode != m_splineNode)
  {
    if (!entityNode)
    {
      return false;
    }
    loadSplineNode(entityNode);
  }

  if (index >= m_points.size())
  {
    return false;
  }

  m_selectedIndex = index;
  refreshViews();
  splineDidChangeNotifier();
  return true;
}

bool SplineTool::selectSpline(const mdl::PickResult& pickResult)
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
          entityNode && entityNode != m_splineNode
          && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return true;
      }
    }
  }
  return false;
}

void SplineTool::deselectPoint()
{
  m_selectedIndex = std::nullopt;
  refreshViews();
  splineDidChangeNotifier();
}

std::optional<size_t> SplineTool::selectedPointIndex() const
{
  return m_selectedIndex;
}

std::optional<std::tuple<vm::vec3d, vm::vec3d>> SplineTool::beginDragPoint(
  const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(PointHitType));
  if (!hit.isMatch())
  {
    return std::nullopt;
  }

  // Hitting another spline's point picks up that spline for editing first.
  const auto [entityNode, index] = hit.target<std::pair<mdl::EntityNode*, size_t>>();
  if (entityNode != m_splineNode)
  {
    if (!entityNode)
    {
      return std::nullopt;
    }
    loadSplineNode(entityNode);
  }

  if (index >= m_points.size())
  {
    return std::nullopt;
  }

  m_selectedIndex = index;
  m_dragState = DragState{index, m_points[index]};
  splineDidChangeNotifier();
  return {{m_points[index].position, hit.hitPoint()}};
}

bool SplineTool::dragPoint(const vm::vec3d& newPosition)
{
  if (!m_dragState)
  {
    return false;
  }

  m_points[m_dragState->index].position = newPosition;
  refreshViews();
  return true;
}

void SplineTool::endDragPoint()
{
  m_dragState = std::nullopt;
  commitSpline("Move Spline Point");
}

void SplineTool::cancelDragPoint()
{
  if (m_dragState)
  {
    m_points[m_dragState->index] = m_dragState->originalPoint;
    m_dragState = std::nullopt;
    refreshViews();
  }
}

double SplineTool::selectedPointRoll() const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
           ? m_points[*m_selectedIndex].roll
           : 0.0;
}

void SplineTool::setSelectedPointRoll(const double roll)
{
  if (
    m_selectedIndex && *m_selectedIndex < m_points.size()
    && m_points[*m_selectedIndex].roll != roll)
  {
    m_points[*m_selectedIndex].roll = roll;
    commitSpline("Rotate Spline Point");
  }
}

double SplineTool::selectedPointScale() const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
           ? m_points[*m_selectedIndex].scale
           : 1.0;
}

void SplineTool::setSelectedPointScale(const double scale)
{
  if (
    m_selectedIndex && *m_selectedIndex < m_points.size() && scale > 0.0
    && m_points[*m_selectedIndex].scale != scale)
  {
    m_points[*m_selectedIndex].scale = scale;
    commitSpline("Scale Spline Point");
  }
}

bool SplineTool::selectedPointLocked() const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
         && m_points[*m_selectedIndex].locked;
}

void SplineTool::toggleSelectedPointLocked()
{
  if (m_selectedIndex && *m_selectedIndex < m_points.size())
  {
    m_points[*m_selectedIndex].locked = !m_points[*m_selectedIndex].locked;
    commitSpline(
      m_points[*m_selectedIndex].locked ? "Lock Spline Point" : "Unlock Spline Point");
  }
}

void SplineTool::moveSelectedPoint(const vm::vec3d& delta)
{
  if (m_selectedIndex && *m_selectedIndex < m_points.size())
  {
    m_points[*m_selectedIndex].position = m_points[*m_selectedIndex].position + delta;
    commitSpline("Move Spline Point");
  }
}

bool SplineTool::closed() const
{
  return m_closed;
}

void SplineTool::setClosed(const bool closed)
{
  if (closed != m_closed)
  {
    m_closed = closed;
    commitSpline(closed ? "Close Spline" : "Open Spline");
  }
}

size_t SplineTool::subdivisions() const
{
  return m_subdivisions;
}

void SplineTool::setSubdivisions(const size_t subdivisions)
{
  if (subdivisions > 0 && subdivisions != m_subdivisions)
  {
    m_subdivisions = subdivisions;
    commitSpline("Change Spline Subdivisions");
  }
}

bool SplineTool::canLinkTemplate() const
{
  return selectedGroup() != nullptr || !m_document.map().selection().brushes.empty();
}

void SplineTool::linkTemplate()
{
  if (const auto* groupNode = selectedGroup())
  {
    if (groupNode->persistentId() && groupNode->persistentId() != m_templateGroupId)
    {
      m_templateGroupId = groupNode->persistentId();
      m_templateBrushes.clear();
      commitSpline("Link Spline Template");
    }
    return;
  }

  const auto& selectedBrushes = m_document.map().selection().brushes;
  if (!selectedBrushes.empty())
  {
    // Individually selected brushes cannot be referenced persistently, so take a
    // snapshot of their current state instead.
    m_templateBrushes.clear();
    m_templateBrushes.reserve(selectedBrushes.size());
    for (const auto* brushNode : selectedBrushes)
    {
      m_templateBrushes.push_back(brushNode->brush());
    }
    m_templateGroupId = std::nullopt;
    commitSpline("Link Spline Template");
  }
}

bool SplineTool::hasTemplate() const
{
  return m_templateGroupId.has_value() || !m_templateBrushes.empty();
}

void SplineTool::unlinkTemplate()
{
  if (hasTemplate())
  {
    m_templateGroupId = std::nullopt;
    m_templateBrushes.clear();
    commitSpline("Unlink Spline Template");
  }
}

bool SplineTool::canBreakSpline() const
{
  return m_splineNode != nullptr && !m_splineNode->children().empty();
}

void SplineTool::breakSpline()
{
  if (!canBreakSpline())
  {
    return;
  }

  auto& map = m_document.map();

  // Duplicate the generated brushes as standard brushes outside the spline entity, so
  // they stay behind as editable geometry when the spline lets go of them.
  auto duplicates = std::vector<mdl::Node*>{};
  for (auto* child : m_splineNode->children())
  {
    if (const auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
    {
      duplicates.push_back(new mdl::BrushNode{brushNode->brush()});
    }
  }

  auto* parent = m_splineNode->parent();

  auto transaction = mdl::Transaction{map, "Break Spline"};
  if (addNodes(map, {{parent, duplicates}}).empty())
  {
    transaction.cancel();
    return;
  }

  // Unlink the template so the spline stops generating brushes.
  m_templateGroupId = std::nullopt;
  m_templateBrushes.clear();
  commitSpline("Break Spline");
  transaction.commit();
}

std::string SplineTool::templateName() const
{
  if (m_templateGroupId)
  {
    const auto* groupNode = findTemplateGroup();
    return groupNode ? groupNode->group().name() : "";
  }
  if (!m_templateBrushes.empty())
  {
    return m_templateBrushes.size() == 1
             ? std::string{"1 brush"}
             : fmt::format("{} brushes", m_templateBrushes.size());
  }
  return "";
}

mdl::GroupNode* SplineTool::findTemplateGroup() const
{
  if (!m_templateGroupId)
  {
    return nullptr;
  }

  mdl::GroupNode* result = nullptr;
  m_document.map().worldNode().accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
      if (groupNode.persistentId() == m_templateGroupId)
      {
        result = &groupNode;
      }
      else
      {
        groupNode.visitChildren(thisLambda);
      }
    },
    [](mdl::EntityNode&) {},
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));

  return result;
}

mdl::GroupNode* SplineTool::selectedGroup() const
{
  const auto& groups = m_document.map().selection().groups;
  return groups.size() == 1 ? groups.front() : nullptr;
}

void SplineTool::refreshOtherSplines()
{
  m_otherSplines.clear();

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
      if (&entityNode != m_splineNode && mdl::isSplineEntity(entityNode.entity()))
      {
        if (const auto data = mdl::parseSplineEntity(entityNode.entity()))
        {
          m_otherSplines.emplace_back(&entityNode, *data);
        }
      }
    },
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));
}

void SplineTool::loadFromSelection()
{
  for (auto* node : m_document.map().selection().nodes)
  {
    // Look for a selected spline entity, or a selected brush belonging to one.
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return;
      }
    }
  }
}

void SplineTool::loadSplineNode(mdl::EntityNode* splineNode)
{
  if (const auto data = mdl::parseSplineEntity(splineNode->entity()))
  {
    auto& map = m_document.map();

    m_splineNode = splineNode;
    m_points = data->points;
    m_subdivisions = data->subdivisions;
    m_templateGroupId = data->templateGroupId;
    m_closed = data->closed;
    m_templateBrushes = mdl::parseSplineTemplateBrushes(
      splineNode->entity(), map.worldNode().mapFormat(), map.worldBounds());
    m_selectedIndex = std::nullopt;
    m_dragState = std::nullopt;

    // Picking up an existing spline is usually done to edit its points, so disable
    // add point mode to prevent accidentally appending new points.
    m_addPointMode = false;

    refreshOtherSplines();
    refreshViews();
    splineDidChangeNotifier();
  }
}

void SplineTool::clearSpline()
{
  m_splineNode = nullptr;
  m_points.clear();
  m_subdivisions = mdl::SplineDefaultSubdivisions;
  m_closed = false;
  m_templateGroupId = std::nullopt;
  m_templateBrushes.clear();
  m_selectedIndex = std::nullopt;
  m_dragState = std::nullopt;
  refreshOtherSplines();
  refreshViews();
  splineDidChangeNotifier();
}

void SplineTool::commitSpline(const std::string& commandName)
{
  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto& map = m_document.map();

  if (m_points.empty())
  {
    if (m_splineNode)
    {
      auto transaction = mdl::Transaction{map, commandName};
      removeNodes(map, {m_splineNode});
      m_splineNode = nullptr;
      transaction.commit();
    }
    refreshViews();
    splineDidChangeNotifier();
    return;
  }

  const auto data =
    mdl::SplineEntityData{m_points, m_subdivisions, m_templateGroupId, m_closed};
  auto entity = mdl::writeSplineTemplateBrushes(
    mdl::writeSplineEntity(m_splineNode ? m_splineNode->entity() : mdl::Entity{}, data),
    m_templateBrushes);

  // Give the entity an origin so that it has a sensible position while it has no
  // brushes yet.
  entity.addOrUpdateProperty(
    "origin",
    fmt::format(
      "{:g} {:g} {:g}",
      m_points.front().position.x(),
      m_points.front().position.y(),
      m_points.front().position.z()));

  auto* newNode = new mdl::EntityNode{std::move(entity)};
  newNode->addChildren(createBrushNodes());

  auto* parent = m_splineNode ? m_splineNode->parent() : parentForNodes(map, {});

  auto transaction = mdl::Transaction{map, commandName};
  if (m_splineNode)
  {
    removeNodes(map, {m_splineNode});
  }
  const auto addedNodes = addNodes(map, {{parent, {newNode}}});
  if (addedNodes.empty())
  {
    transaction.cancel();
    m_splineNode = nullptr;
  }
  else
  {
    m_splineNode = newNode;
    transaction.commit();
  }

  refreshOtherSplines();
  refreshViews();
  splineDidChangeNotifier();
}

std::vector<mdl::Node*> SplineTool::createBrushNodes() const
{
  if (m_points.size() < 2)
  {
    return {};
  }

  auto templateBrushes = std::vector<const mdl::Brush*>{};
  if (const auto* groupNode = findTemplateGroup())
  {
    groupNode->visitChildren(kdl::overload(
      [](auto&& thisLambda, mdl::GroupNode& nestedGroup) {
        nestedGroup.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) { templateBrushes.push_back(&brushNode.brush()); },
      [](mdl::WorldNode&) {},
      [](mdl::LayerNode&) {},
      [](mdl::PatchNode&) {}));
  }
  else
  {
    for (const auto& brush : m_templateBrushes)
    {
      templateBrushes.push_back(&brush);
    }
  }

  if (templateBrushes.empty())
  {
    return {};
  }

  auto templateBounds = templateBrushes.front()->bounds();
  for (const auto* brush : templateBrushes)
  {
    templateBounds = vm::merge(templateBounds, brush->bounds());
  }

  auto& map = m_document.map();
  return mdl::createSplineBrushes(
           map.worldNode().mapFormat(),
           map.worldBounds(),
           m_points,
           templateBrushes,
           templateBounds,
           m_closed)
         | kdl::transform([](auto brushes) {
             return brushes | std::views::transform([](auto& brush) {
                      return static_cast<mdl::Node*>(
                        new mdl::BrushNode{std::move(brush)});
                    })
                    | kdl::ranges::to<std::vector>();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not create spline brushes: " << e.msg;
             return std::vector<mdl::Node*>{};
           })
         | kdl::value();
}

bool SplineTool::doActivate()
{
  connectObservers();

  // Always start with add point mode disabled so that switching to the tool never
  // creates points accidentally; the user enables it explicitly on the tool page.
  m_addPointMode = false;
  loadFromSelection();
  refreshOtherSplines();

  splineDidChangeNotifier();
  return true;
}

bool SplineTool::doDeactivate()
{
  m_notifierConnection.disconnect();
  clearSpline();
  m_otherSplines.clear();
  return true;
}

QWidget* SplineTool::doCreatePage(QWidget* parent)
{
  return new SplineToolPage{m_document, *this, parent};
}

void SplineTool::connectObservers()
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

void SplineTool::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherSplines();

  if (m_splineNode != nullptr)
  {
    return;
  }

  // Adopt a spline entity that reappears, e.g. when a spline edit is undone.
  for (auto* node : nodes)
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && mdl::isSplineEntity(entityNode->entity()))
    {
      loadSplineNode(entityNode);
      return;
    }
  }
}

void SplineTool::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  if (m_splineNode && std::ranges::find(nodes, m_splineNode) != nodes.end())
  {
    clearSpline();
  }
  else
  {
    refreshOtherSplines();
  }
}

void SplineTool::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherSplines();

  if (
    m_splineNode
    && std::ranges::find(nodes, static_cast<mdl::Node*>(m_splineNode)) != nodes.end())
  {
    loadSplineNode(m_splineNode);
  }
}

void SplineTool::selectionDidChange()
{
  if (m_ignoreNotifications)
  {
    return;
  }

  // Switch to a newly selected spline entity, but keep editing the current spline if
  // the selection does not contain one.
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_splineNode
          && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return;
      }
    }
  }

  splineDidChangeNotifier();
}

} // namespace tb::ui

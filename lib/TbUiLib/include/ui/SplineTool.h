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
#include "mdl/HitType.h"
#include "mdl/Spline.h"
#include "mdl/SplineEntity.h"
#include "ui/Tool.h"

#include "vm/ray.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tb
{
namespace gl
{
class Camera;
}

namespace mdl
{
class Brush;
class BrushNode;
class EntityNode;
class Grid;
class GroupNode;
class Node;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
class RenderService;
} // namespace render

namespace ui
{
class MapDocument;

/**
 * A tool for creating and editing splines. A spline is a curve through a sequence of
 * control points; each point can be moved, rotated (rolled around the curve) and
 * locked. A spline can be linked to an entity group, in which case the group's
 * brushes are used as a template that is deformed along the curve, and the resulting
 * brushes are kept as children of the spline's entity.
 *
 * The spline is persisted in the map as a func_group entity carrying the control
 * points in its properties, so that it remains editable across sessions and its
 * brushes are merged into the world geometry by map compilers.
 */
class SplineTool : public Tool
{
public:
  static const mdl::HitType::Type PointHitType;

  Notifier<> splineDidChangeNotifier;

private:
  MapDocument& m_document;

  std::vector<mdl::SplinePoint> m_points;
  size_t m_subdivisions = mdl::SplineDefaultSubdivisions;

  /** The template is either a group (referenced by its persistent ID) or a snapshot
   * of individually linked brushes; at most one of these is set. */
  std::optional<mdl::IdType> m_templateGroupId;
  std::vector<mdl::Brush> m_templateBrushes;

  /** Whether clicking empty space appends new points. */
  bool m_addPointMode = false;

  /** The entity node holding the spline currently being edited, if any. */
  mdl::EntityNode* m_splineNode = nullptr;

  /** All other splines in the map, so they can be shown and picked up while the tool
   * is active. Refreshed whenever the document changes. */
  std::vector<std::pair<mdl::EntityNode*, std::vector<mdl::SplinePoint>>> m_otherSplines;

  std::optional<size_t> m_selectedIndex;

  struct DragState
  {
    size_t index;
    mdl::SplinePoint originalPoint;
  };
  std::optional<DragState> m_dragState;

  bool m_ignoreNotifications = false;

  NotifierConnection m_notifierConnection;

public:
  explicit SplineTool(MapDocument& document);
  ~SplineTool() override;

  const mdl::Grid& grid() const;

  void pick(
    const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult);

  void render(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult);

  void renderFeedback(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const vm::vec3d& point) const;

private:
  void renderHighlight(
    render::RenderService& renderService, const mdl::PickResult& pickResult) const;

public: // add point mode
  /**
   * While add point mode is enabled, clicking empty space appends a new point to the
   * spline; while it is disabled, clicks only select existing points. The mode is
   * always disabled when the tool is activated and must be enabled explicitly on the
   * tool page.
   */
  bool addPointMode() const;
  void setAddPointMode(bool addPointMode);

public: // point management
  bool hasPoints() const;
  const std::vector<mdl::SplinePoint>& points() const;

  /** Returns the position new points are created relative to: the last point, if any. */
  std::optional<vm::vec3d> lastPointPosition() const;

  void addPoint(const vm::vec3d& point);

  bool canRemovePoint() const;
  /** Removes the selected point, or the last point if none is selected. */
  void removePoint();

  /** Selects the point hit by the given pick result. Returns whether a point was hit. */
  bool selectPoint(const mdl::PickResult& pickResult);

  /**
   * Picks up the spline whose generated geometry is hit by the given pick result for
   * editing. Since generated brushes cannot be selected, this is how an existing
   * spline is chosen while the tool is active. Returns whether a different spline
   * was hit and loaded.
   */
  bool selectSpline(const mdl::PickResult& pickResult);
  void deselectPoint();
  std::optional<size_t> selectedPointIndex() const;

public: // dragging
  std::optional<std::tuple<vm::vec3d, vm::vec3d>> beginDragPoint(
    const mdl::PickResult& pickResult);
  bool dragPoint(const vm::vec3d& newPosition);
  void endDragPoint();
  void cancelDragPoint();

public: // rotation, scale and locking
  double selectedPointRoll() const;
  void setSelectedPointRoll(double roll);

  double selectedPointScale() const;
  void setSelectedPointScale(double scale);

  bool selectedPointLocked() const;
  void toggleSelectedPointLocked();

  /** Adds the given roll delta to all points that are not locked. */
  void rotateUnlockedPoints(double deltaRoll);

public: // template group linkage
  size_t subdivisions() const;
  void setSubdivisions(size_t subdivisions);

  /** Whether the current selection contains a group or brushes that can be linked. */
  bool canLinkTemplate() const;
  /**
   * Links the current selection as the spline's deformation template. A selected
   * group is linked by reference, so later changes to it are picked up when the
   * spline is regenerated; a plain brush selection is linked by taking a snapshot of
   * the selected brushes.
   */
  void linkTemplate();
  bool hasTemplate() const;
  void unlinkTemplate();
  /** A user facing description of the linked template. */
  std::string templateName() const;

private:
  mdl::GroupNode* findTemplateGroup() const;
  mdl::GroupNode* selectedGroup() const;

  void loadFromSelection();
  void loadSplineNode(mdl::EntityNode* splineNode);
  void clearSpline();
  void refreshOtherSplines();

  /**
   * Writes the current spline state to the document by replacing the spline entity
   * (and its generated brushes) in a single undoable transaction.
   */
  void commitSpline(const std::string& commandName);

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

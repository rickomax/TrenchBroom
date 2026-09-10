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

#include "ui/SplineToolController.h"

#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/PickResult.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/MoveHandleDragTracker.h"
#include "ui/SplineTool.h"

#include "vm/intersection.h"
#include "vm/plane.h"
#include "vm/vec.h"

#include <memory>
#include <optional>

namespace tb::ui
{
namespace
{

/**
 * Encapsulates the differences between the 2D and 3D views: how a mouse position maps
 * to a new point position, and on which plane existing points are dragged.
 */
class PartDelegateBase
{
protected:
  SplineTool& m_tool;

public:
  explicit PartDelegateBase(SplineTool& tool)
    : m_tool{tool}
  {
  }

  virtual ~PartDelegateBase() = default;

  SplineTool& tool() const { return m_tool; }

  virtual std::optional<vm::vec3d> newPointPosition(
    const InputState& inputState) const = 0;

  void renderFeedback(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const
  {
    if (
      m_tool.addPointMode() && inputState.mouseButtons() == MouseButtons::None
      && !inputState.anyToolDragging())
    {
      if (const auto position = newPointPosition(inputState))
      {
        m_tool.renderFeedback(renderContext, renderBatch, *position);
      }
    }
  }

protected:
  /** The plane in which new points are placed if the pick ray hits nothing else. */
  vm::vec3d referencePoint() const
  {
    return m_tool.lastPointPosition().value_or(vm::vec3d{0, 0, 0});
  }
};

class PartDelegate2D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  std::optional<vm::vec3d> newPointPosition(const InputState& inputState) const override
  {
    const auto& camera = inputState.camera();
    const auto viewDir = vm::get_abs_max_component_axis(vm::vec3d{camera.direction()});
    const auto& pickRay = inputState.pickRay();

    if (
      const auto distance =
        vm::intersect_ray_plane(pickRay, vm::plane3d{referencePoint(), viewDir}))
    {
      const auto hitPoint = vm::point_at_distance(pickRay, *distance);
      return m_tool.grid().snap(hitPoint);
    }
    return std::nullopt;
  }
};

class PartDelegate3D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  std::optional<vm::vec3d> newPointPosition(const InputState& inputState) const override
  {
    using namespace mdl::HitFilters;

    const auto& hit = inputState.pickResult().first(type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      return m_tool.grid().snap(hit.hitPoint(), faceHandle->face().boundary());
    }

    const auto& pickRay = inputState.pickRay();
    if (
      const auto distance = vm::intersect_ray_plane(
        pickRay, vm::plane3d{referencePoint(), vm::vec3d{0, 0, 1}}))
    {
      const auto hitPoint = vm::point_at_distance(pickRay, *distance);
      return m_tool.grid().snap(hitPoint);
    }
    return std::nullopt;
  }
};

class PartBase
{
protected:
  std::unique_ptr<PartDelegateBase> m_delegate;

  explicit PartBase(std::unique_ptr<PartDelegateBase> delegate)
    : m_delegate{std::move(delegate)}
  {
  }

public:
  virtual ~PartBase() = default;
};

/**
 * Implements the usual move semantics for dragging a spline point: dragging on a
 * horizontal plane in the 3D view, with the Alt key switching to a vertical move and
 * Shift constricting the move to one axis, and dragging on the view plane in the 2D
 * views.
 */
class MoveSplinePointDragDelegate : public MoveHandleDragTrackerDelegate
{
private:
  SplineTool& m_tool;

public:
  explicit MoveSplinePointDragDelegate(SplineTool& tool)
    : m_tool{tool}
  {
  }

  DragStatus move(
    const InputState&, const DragState&, const vm::vec3d& proposedHandlePosition) override
  {
    return m_tool.dragPoint(proposedHandlePosition) ? DragStatus::Continue
                                                    : DragStatus::Deny;
  }

  void end(const InputState&, const DragState&) override { m_tool.endDragPoint(); }

  void cancel(const DragState&) override { m_tool.cancelDragPoint(); }

  DragHandleSnapper makeDragHandleSnapper(
    const InputState&, const SnapMode snapMode) const override
  {
    // Control points sit on the grid, so their drags snap the movement delta;
    // tangent handles start at arbitrary offsets, so a relative snap would keep them
    // off the grid forever -- snap their positions absolutely instead (with the
    // usual modifier still toggling to the other mode).
    auto effectiveSnapMode = snapMode;
    if (m_tool.draggingTangentHandle())
    {
      effectiveSnapMode =
        snapMode == SnapMode::Relative ? SnapMode::Absolute : SnapMode::Relative;
    }
    return effectiveSnapMode == SnapMode::Relative
             ? makeRelativeHandleSnapper(m_tool.grid())
             : makeAbsoluteHandleSnapper(m_tool.grid());
  }
};

class MoveSplinePointPart : public ToolController, protected PartBase
{
public:
  explicit MoveSplinePointPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    // Alt switches to a vertical move and CtrlCmd toggles the snap mode; both may
    // already be held when the drag starts.
    if (
      inputState.mouseButtons() != MouseButtons::Left
      || (!inputState.modifierKeysPressed(ModifierKeys::None)
          && !inputState.modifierKeysPressed(ModifierKeys::Alt)
          && !inputState.modifierKeysPressed(ModifierKeys::CtrlCmd)
          && !inputState.modifierKeysPressed(ModifierKeys::CtrlCmd | ModifierKeys::Alt))
      || m_delegate->tool().addPointMode())
    {
      return nullptr;
    }

    const auto initialHandlePositionAndHitPoint =
      m_delegate->tool().beginDragPoint(inputState.pickResult());
    if (!initialHandlePositionAndHitPoint)
    {
      return nullptr;
    }

    const auto [initialHandlePosition, hitPoint] = *initialHandlePositionAndHitPoint;
    return createMoveHandleDragTracker(
      MoveSplinePointDragDelegate{m_delegate->tool()},
      inputState,
      initialHandlePosition,
      hitPoint);
  }

  bool cancel() override { return false; }
};

class AddSplinePointPart : public ToolController, protected PartBase
{
public:
  explicit AddSplinePointPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  bool mouseClick(const InputState& inputState) override
  {
    if (
      !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return false;
    }

    // While add point mode is enabled, every click appends a new point; selection is
    // disabled. Otherwise, clicking a point selects it (picking up its spline if it
    // belongs to a different one), and clicking a spline's generated geometry picks
    // up that spline for editing.
    if (m_delegate->tool().addPointMode())
    {
      if (const auto position = m_delegate->newPointPosition(inputState))
      {
        m_delegate->tool().addPoint(*position);
        return true;
      }
      return false;
    }

    if (m_delegate->tool().selectPoint(inputState.pickResult()))
    {
      return true;
    }

    return m_delegate->tool().selectSpline(inputState.pickResult());
  }

  bool mouseDoubleClick(const InputState& inputState) override
  {
    // Double clicking a point toggles its twist lock (not in add point mode, where
    // clicks append points).
    if (
      !m_delegate->tool().addPointMode()
      && inputState.mouseButtonsPressed(MouseButtons::Left)
      && m_delegate->tool().selectPoint(inputState.pickResult()))
    {
      m_delegate->tool().toggleSelectedPointLock(mdl::SplineLock::Twist);
      return true;
    }
    return false;
  }

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override
  {
    m_delegate->renderFeedback(inputState, renderContext, renderBatch);
  }

  bool cancel() override { return false; }
};

} // namespace

SplineToolControllerBase::SplineToolControllerBase(SplineTool& tool)
  : m_tool{tool}
{
}

SplineToolControllerBase::~SplineToolControllerBase() = default;

Tool& SplineToolControllerBase::tool()
{
  return m_tool;
}

const Tool& SplineToolControllerBase::tool() const
{
  return m_tool;
}

void SplineToolControllerBase::pick(
  const InputState& inputState, mdl::PickResult& pickResult)
{
  m_tool.pick(inputState.pickRay(), inputState.camera(), pickResult);
}

void SplineToolControllerBase::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch, inputState.pickResult());
  ToolControllerGroup::render(inputState, renderContext, renderBatch);
}

bool SplineToolControllerBase::cancel()
{
  if (m_tool.addPointMode())
  {
    m_tool.setAddPointMode(false);
    return true;
  }
  if (m_tool.tangentEditMode())
  {
    m_tool.setTangentEditMode(false);
    return true;
  }
  if (m_tool.selectedPointIndex())
  {
    m_tool.deselectPoint();
    return true;
  }
  return false;
}

SplineToolController2D::SplineToolController2D(SplineTool& tool)
  : SplineToolControllerBase{tool}
{
  addController(
    std::make_unique<MoveSplinePointPart>(std::make_unique<PartDelegate2D>(tool)));
  addController(
    std::make_unique<AddSplinePointPart>(std::make_unique<PartDelegate2D>(tool)));
}

SplineToolController3D::SplineToolController3D(SplineTool& tool)
  : SplineToolControllerBase{tool}
{
  addController(
    std::make_unique<MoveSplinePointPart>(std::make_unique<PartDelegate3D>(tool)));
  addController(
    std::make_unique<AddSplinePointPart>(std::make_unique<PartDelegate3D>(tool)));
}

} // namespace tb::ui

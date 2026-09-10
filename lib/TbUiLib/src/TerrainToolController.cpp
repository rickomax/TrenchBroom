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

#include "ui/TerrainToolController.h"

#include <QTimer>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/PickResult.h"
#include "render/RenderService.h"
#include "ui/GestureTracker.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/ScaleTool.h"
#include "ui/TerrainTool.h"

#include "vm/intersection.h"
#include "vm/line.h"
#include "vm/plane.h"
#include "vm/polygon.h"
#include "vm/segment.h"
#include "vm/vec.h"

#include <memory>
#include <optional>
#include <utility>

namespace tb::ui
{
namespace
{

/**
 * Encapsulates the difference between the 2D and 3D views: the plane a new terrain's
 * box is dragged out on.
 */
class PartDelegateBase
{
protected:
  TerrainTool& m_tool;

public:
  explicit PartDelegateBase(TerrainTool& tool)
    : m_tool{tool}
  {
  }

  virtual ~PartDelegateBase() = default;

  TerrainTool& tool() const { return m_tool; }

  /** The plane new terrains are dragged out on. */
  virtual vm::plane3d creationPlane(const InputState& inputState) const = 0;

  /**
   * Picks the handles of the terrain's bounding box the way the scale tool picks the
   * selection's, which differs between the 2D and 3D views. The hits carry the scale
   * tool's handle types and targets, so its bbox maths can be applied to them.
   */
  virtual void pickScaleHandles(
    const InputState& inputState,
    const vm::bbox3d& bounds,
    mdl::PickResult& pickResult) const = 0;

  /** The point on the given plane under the mouse, snapped to the grid. */
  std::optional<vm::vec3d> pointOnPlane(
    const InputState& inputState, const vm::plane3d& plane) const
  {
    const auto& pickRay = inputState.pickRay();
    if (const auto distance = vm::intersect_ray_plane(pickRay, plane))
    {
      return m_tool.grid().snap(vm::point_at_distance(pickRay, *distance));
    }
    return std::nullopt;
  }
};

/**
 * Falls back to the side of the box facing away from the camera that comes closest to
 * the pick ray, so that dragging works even when no handle is under the mouse. This is
 * what the scale tool does for the selection.
 */
void pickScaleBackSide(
  const InputState& inputState, const vm::bbox3d& bounds, mdl::PickResult& pickResult)
{
  if (pickResult.empty())
  {
    const auto& pickRay = inputState.pickRay();
    const auto result = pickBackSideOfBox(pickRay, inputState.camera(), bounds);
    if (result.pickedSideNormal != vm::vec3d{0, 0, 0})
    {
      pickResult.addHit(mdl::Hit{
        ScaleTool::SideHitType,
        result.distAlongRay,
        vm::point_at_distance(pickRay, result.distAlongRay),
        BBoxSide{result.pickedSideNormal}});
    }
  }
}

class PartDelegate2D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  vm::plane3d creationPlane(const InputState& inputState) const override
  {
    // In a 2D view the box is dragged out on the view plane through the origin.
    const auto& camera = inputState.camera();
    const auto normal = vm::get_abs_max_component_axis(vm::vec3d{camera.direction()});
    return vm::plane3d{vm::vec3d{0, 0, 0}, normal};
  }

  void pickScaleHandles(
    const InputState& inputState,
    const vm::bbox3d& bounds,
    mdl::PickResult& pickResult) const override
  {
    const auto& pickRay = inputState.pickRay();
    const auto& camera = inputState.camera();
    if (bounds.contains(pickRay.origin))
    {
      return;
    }

    auto localPickResult = mdl::PickResult{};

    // Only the corners of the edges pointing at the camera can be grabbed in a 2D view,
    // because the other handles are hidden behind the box.
    for (const auto& edge : allEdges())
    {
      const auto points = pointsForBBoxEdge(bounds, edge);
      if (!vm::is_parallel(points.direction(), vm::vec3d{camera.direction()}))
      {
        continue;
      }

      for (const auto& point : {points.start(), points.end()})
      {
        if (
          const auto distance = camera.pickPointHandle(
            pickRay, point, double(pref(Preferences::HandleRadius))))
        {
          localPickResult.addHit(mdl::Hit{
            ScaleTool::EdgeHitType,
            *distance,
            vm::point_at_distance(pickRay, *distance),
            edge});
        }
      }
    }

    pickScaleBackSide(inputState, bounds, localPickResult);
    if (!localPickResult.empty())
    {
      pickResult.addHit(localPickResult.all().front());
    }
  }
};

class PartDelegate3D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  vm::plane3d creationPlane(const InputState& inputState) const override
  {
    using namespace mdl::HitFilters;

    // Prefer the surface under the mouse so terrains can be placed on existing
    // geometry, and fall back to the horizontal plane through the origin.
    const auto& hit = inputState.pickResult().first(type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      const auto& boundary = faceHandle->face().boundary();
      if (vm::abs(boundary.normal.z()) > 0.5)
      {
        return vm::plane3d{hit.hitPoint(), vm::vec3d{0, 0, 1}};
      }
    }
    return vm::plane3d{vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}};
  }

  void pickScaleHandles(
    const InputState& inputState,
    const vm::bbox3d& bounds,
    mdl::PickResult& pickResult) const override
  {
    const auto& pickRay = inputState.pickRay();
    const auto& camera = inputState.camera();
    if (bounds.contains(pickRay.origin))
    {
      return;
    }

    auto localPickResult = mdl::PickResult{};

    // The corner handles are made larger than the edge handles so that they win where
    // the two overlap.
    for (const auto& corner : allCorners())
    {
      const auto point = pointForBBoxCorner(bounds, corner);
      if (
        const auto distance = camera.pickPointHandle(
          pickRay, point, double(pref(Preferences::HandleRadius)) * 2.0))
      {
        localPickResult.addHit(mdl::Hit{
          ScaleTool::CornerHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          corner});
      }
    }

    for (const auto& edge : allEdges())
    {
      if (
        const auto distance = camera.pickLineSegmentHandle(
          pickRay,
          pointsForBBoxEdge(bounds, edge),
          double(pref(Preferences::HandleRadius))))
      {
        localPickResult.addHit(mdl::Hit{
          ScaleTool::EdgeHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          edge});
      }
    }

    for (const auto& side : allSides())
    {
      const auto polygon = polygonForBBoxSide(bounds, side);
      if (
        const auto distance = vm::intersect_ray_polygon(
          pickRay, polygon.vertices().begin(), polygon.vertices().end()))
      {
        localPickResult.addHit(mdl::Hit{
          ScaleTool::SideHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          side});
      }
    }

    pickScaleBackSide(inputState, bounds, localPickResult);
    if (!localPickResult.empty())
    {
      pickResult.addHit(localPickResult.all().front());
    }
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
 * Drags out the footprint of a new terrain. The box is as tall as one grid step, so
 * the new terrain starts out as a thin slab that can immediately be sculpted.
 */
class CreateTerrainTracker : public GestureTracker
{
private:
  TerrainTool& m_tool;
  PartDelegateBase& m_delegate;
  /** The plane is captured when the drag starts so that it cannot shift while the
   * mouse moves over other geometry. */
  vm::plane3d m_plane;
  vm::vec3d m_start;
  std::optional<vm::bbox3d> m_bounds;

public:
  CreateTerrainTracker(
    TerrainTool& tool,
    PartDelegateBase& delegate,
    const vm::plane3d& plane,
    const vm::vec3d& start)
    : m_tool{tool}
    , m_delegate{delegate}
    , m_plane{plane}
    , m_start{start}
  {
  }

  bool update(const InputState& inputState) override
  {
    if (const auto current = m_delegate.pointOnPlane(inputState, m_plane))
    {
      const auto thickness = vm::max(8.0, double(m_tool.grid().actualSize()));
      const auto min = vm::vec3d{
        vm::min(m_start.x(), current->x()),
        vm::min(m_start.y(), current->y()),
        m_start.z()};
      const auto max = vm::vec3d{
        vm::max(m_start.x(), current->x()),
        vm::max(m_start.y(), current->y()),
        m_start.z() + thickness};
      m_bounds = vm::bbox3d{min, max};
    }
    return true;
  }

  void end(const InputState&) override
  {
    if (m_bounds)
    {
      m_tool.createTerrain(*m_bounds);
    }
  }

  void cancel() override { m_bounds = std::nullopt; }

  void render(
    const InputState&,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const override
  {
    if (m_bounds)
    {
      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setForegroundColor(pref(Preferences::TerrainBoundsColor));
      renderService.renderBounds(vm::bbox3f{*m_bounds});
    }
  }
};

/**
 * What a sculpting drag reports back to the part that owns the stroke. The stroke is
 * started on mouse down rather than by the drag, so the drag only moves it around.
 */
class SculptHandler
{
public:
  virtual ~SculptHandler() = default;

  virtual void sculptMoved(const InputState& inputState) = 0;
  virtual void sculptEnded() = 0;
  virtual void sculptCancelled() = 0;
};

/** Follows the mouse while it is dragged over the terrain. */
class SculptTracker : public GestureTracker
{
private:
  SculptHandler& m_handler;

public:
  explicit SculptTracker(SculptHandler& handler)
    : m_handler{handler}
  {
  }

  bool update(const InputState& inputState) override
  {
    m_handler.sculptMoved(inputState);
    return true;
  }

  void end(const InputState&) override { m_handler.sculptEnded(); }

  void cancel() override { m_handler.sculptCancelled(); }
};

/** Creates new terrains while add mode is enabled. */
class CreateTerrainPart : public ToolController, protected PartBase
{
public:
  explicit CreateTerrainPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    if (
      !m_delegate->tool().addMode() || inputState.mouseButtons() != MouseButtons::Left
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return nullptr;
    }

    const auto plane = m_delegate->creationPlane(inputState);
    if (const auto start = m_delegate->pointOnPlane(inputState, plane))
    {
      return std::make_unique<CreateTerrainTracker>(
        m_delegate->tool(), *m_delegate, plane, *start);
    }
    return nullptr;
  }

  bool cancel() override { return false; }
};

/**
 * Sculpts and paints the current terrain while add mode is disabled.
 *
 * The stroke starts on mouse down and lasts until the button is released, so a click
 * applies the brush just like a drag does. It is applied repeatedly for as long as the
 * button is held: moving the mouse applies it at the new position, and a repeat timer
 * keeps applying it at the last position while the mouse stands still, so holding the
 * button down in one spot keeps building the terrain up there.
 */
class SculptPart : public ToolController, protected PartBase, private SculptHandler
{
private:
  /** How often the brush is applied while the mouse is held down without moving. */
  static constexpr auto RepeatIntervalMs = 33;

  std::unique_ptr<QTimer> m_repeatTimer;

  /** The position the brush is applied at while the button is held. It stays put while
   * the mouse does, which also keeps Flatten levelling towards the height that was
   * clicked. */
  std::optional<vm::vec3d> m_position;
  bool m_invert = false;
  /** Whether the button is down and a stroke is being applied. */
  bool m_sculpting = false;

public:
  explicit SculptPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
    , m_repeatTimer{std::make_unique<QTimer>()}
  {
    m_repeatTimer->setInterval(RepeatIntervalMs);
    // The timer is owned by this part and is also the connection's context, so the
    // lambda cannot outlive it.
    QObject::connect(
      m_repeatTimer.get(), &QTimer::timeout, m_repeatTimer.get(), [this]() {
        applyBrush();
      });
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  void applyBrush()
  {
    if (m_sculpting && m_position)
    {
      m_delegate->tool().applyStroke(*m_position, m_invert);
      // Applying restarts the wait, so a moving mouse never doubles the rate.
      m_repeatTimer->start();
    }
  }

  /** Whether the given input starts or continues a sculpting stroke. */
  bool acceptsSculpting(const InputState& inputState) const
  {
    // With no sculpting mode selected the tool only selects terrains.
    return m_delegate->tool().sculpting()
           && (inputState.modifierKeysPressed(ModifierKeys::None) || inputState.modifierKeysPressed(ModifierKeys::Shift));
  }

  void endSculpting()
  {
    if (m_sculpting)
    {
      m_repeatTimer->stop();
      m_sculpting = false;
      m_delegate->tool().endStroke();
    }
  }

  void mouseDown(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (
      m_sculpting || !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !acceptsSculpting(inputState)
      // Clicking another terrain picks that one up instead of sculpting this one.
      || tool.canSelectTerrainAt(inputState.pickResult()))
    {
      return;
    }

    m_position = tool.pickSurface(inputState.pickRay());
    if (!m_position)
    {
      return;
    }

    m_invert = inputState.modifierKeysDown(ModifierKeys::Shift);
    m_sculpting = true;
    tool.setBrushPosition(*m_position);
    tool.beginStroke();
    applyBrush();
  }

  void mouseUp(const InputState&) override { endSculpting(); }

  void mouseMove(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    // The brush is only shown while a sculpting mode is selected.
    tool.setBrushPosition(
      tool.sculpting() ? tool.pickSurface(inputState.pickRay()) : std::nullopt);
  }

  bool mouseClick(const InputState& inputState) override
  {
    // The click already started a stroke on mouse down, which mouse up will end.
    if (m_sculpting)
    {
      return true;
    }

    auto& tool = m_delegate->tool();
    if (
      tool.addMode() || !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return false;
    }

    // Clicking a terrain's geometry picks it up for editing.
    return tool.selectTerrainAt(inputState.pickResult());
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    // The stroke is already running; the drag only moves it and ends it.
    // Constructed here rather than with make_unique so that the conversion to the
    // privately inherited handler interface is allowed.
    return m_sculpting && inputState.mouseButtons() == MouseButtons::Left
             ? std::unique_ptr<GestureTracker>{new SculptTracker{*this}}
             : nullptr;
  }

  void sculptMoved(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (const auto position = tool.pickSurface(inputState.pickRay()))
    {
      m_position = *position;
      m_invert = inputState.modifierKeysDown(ModifierKeys::Shift);
      tool.setBrushPosition(*position);
      applyBrush();
    }
  }

  void sculptEnded() override { endSculpting(); }

  void sculptCancelled() override
  {
    if (m_sculpting)
    {
      m_repeatTimer->stop();
      m_sculpting = false;
      m_delegate->tool().cancelStroke();
    }
  }

  bool cancel() override { return false; }
};

/**
 * Scales the current terrain by dragging the handles of its bounding box, the way the
 * scale tool scales brushes.
 *
 * Resampling the terrain means rebuilding every brush, so the drag only draws the box it
 * would produce and the terrain is resampled once when it ends.
 */
class TerrainScaleDragDelegate : public HandleDragTrackerDelegate
{
private:
  TerrainTool& m_tool;
  mdl::Hit m_dragStartHit;
  vm::bbox3d m_boundsAtDragStart;
  vm::vec3d m_cumulativeDelta;
  vm::bbox3d m_currentBounds;

public:
  TerrainScaleDragDelegate(
    TerrainTool& tool, mdl::Hit dragStartHit, const vm::bbox3d& boundsAtDragStart)
    : m_tool{tool}
    , m_dragStartHit{std::move(dragStartHit)}
    , m_boundsAtDragStart{boundsAtDragStart}
    , m_currentBounds{boundsAtDragStart}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& /* initialHandlePosition */,
    const vm::vec3d& handleOffset) override
  {
    return makeProposer(inputState, handleOffset);
  }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState& inputState, const DragState& dragState) override
  {
    return UpdateDragConfig{
      makeProposer(inputState, dragState.handleOffset), ResetInitialHandlePosition::Keep};
  }

  DragStatus update(
    const InputState& inputState,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    m_cumulativeDelta =
      m_cumulativeDelta + (proposedHandlePosition - dragState.currentHandlePosition);

    const auto [anchor, proportional] = modifierSettings(inputState);
    const auto bounds = moveBBoxForHit(
      m_boundsAtDragStart, m_dragStartHit, m_cumulativeDelta, proportional, anchor);

    // An empty box means the drag collapsed or inverted the terrain, so the last usable
    // bounds are kept until the mouse comes back.
    if (!bounds.is_empty())
    {
      m_currentBounds = bounds;
      m_tool.setScalePreview(bounds);
    }
    return DragStatus::Continue;
  }

  void end(const InputState&, const DragState&) override
  {
    m_tool.setScalePreview(std::nullopt);
    if (!vm::is_zero(m_cumulativeDelta, vm::Cd::almost_zero()))
    {
      m_tool.applyScale(m_currentBounds);
    }
  }

  void cancel(const DragState&) override { m_tool.setScalePreview(std::nullopt); }

private:
  /** The anchor and proportional axes the modifier keys ask for, matching the scale
   * tool: Alt scales about the center, Shift scales every axis. */
  static std::pair<AnchorPos, ProportionalAxes> modifierSettings(
    const InputState& inputState)
  {
    const auto anchor = inputState.modifierKeysDown(ModifierKeys::Alt)
                          ? AnchorPos::Center
                          : AnchorPos::Opposite;

    auto proportional = ProportionalAxes::None();
    if (inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      proportional = ProportionalAxes::All();

      const auto& camera = inputState.camera();
      if (camera.orthographicProjection())
      {
        // In a 2D view there is nothing to scale along the camera's axis.
        proportional.setAxisProportional(
          vm::find_abs_max_component(camera.direction()), false);
      }
    }

    return {anchor, proportional};
  }

  HandlePositionProposer makeProposer(
    const InputState& inputState, const vm::vec3d& handleOffset) const
  {
    const auto& grid = m_tool.grid();
    if (
      m_dragStartHit.type() == ScaleTool::EdgeHitType
      && inputState.camera().orthographicProjection()
      && !inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      // A corner handle in a 2D view moves freely in the view plane.
      const auto plane = vm::plane3d{
        m_dragStartHit.hitPoint() + handleOffset,
        vm::vec3d{inputState.camera().direction()} * -1.0};
      return makeHandlePositionProposer(
        makePlaneHandlePicker(plane, handleOffset), makeRelativeHandleSnapper(grid));
    }

    const auto handleLine = handleLineForHit(m_boundsAtDragStart, m_dragStartHit);
    return makeHandlePositionProposer(
      makeLineHandlePicker(handleLine, handleOffset),
      makeAbsoluteLineHandleSnapper(grid, handleLine));
  }
};

/** Scales the current terrain while the Scale mode is selected. */
class TerrainScalePart : public ToolController, protected PartBase
{
public:
  explicit TerrainScalePart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  void pick(const InputState& inputState, mdl::PickResult& pickResult) override
  {
    if (m_delegate->tool().scaling())
    {
      m_delegate->pickScaleHandles(inputState, bounds(), pickResult);
    }
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    using namespace mdl::HitFilters;

    auto& tool = m_delegate->tool();
    if (!tool.scaling() || !inputState.mouseButtonsPressed(MouseButtons::Left))
    {
      return nullptr;
    }

    const auto& hit = inputState.pickResult().first(type(ScaleTool::AnyHitType));
    if (!hit.isMatch())
    {
      return nullptr;
    }

    const auto boundsAtDragStart = bounds();
    const auto handleLine = handleLineForHit(boundsAtDragStart, hit);
    return createHandleDragTracker(
      TerrainScaleDragDelegate{tool, hit, boundsAtDragStart},
      inputState,
      handleLine.get_origin(),
      hit.hitPoint());
  }

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override
  {
    using namespace mdl::HitFilters;

    auto& tool = m_delegate->tool();
    if (!tool.scaling())
    {
      return;
    }

    auto renderService = render::RenderService{renderContext, renderBatch};

    // While dragging, the box the terrain would be resampled into is drawn instead of
    // the handles; the terrain itself only changes when the drag ends.
    if (const auto& preview = tool.scalePreview())
    {
      renderService.setForegroundColor(pref(Preferences::TerrainPreviewColor));
      renderService.setLineWidth(2.0f);
      renderService.renderBounds(vm::bbox3f{*preview});
      return;
    }

    renderService.setForegroundColor(pref(Preferences::ScaleHandleColor));
    for (const auto& corner : allCorners())
    {
      renderService.renderHandle(vm::vec3f{pointForBBoxCorner(bounds(), corner)});
    }

    // Highlight the side the mouse is over, so it is clear what a drag would move.
    const auto& hit = inputState.pickResult().first(type(ScaleTool::AnyHitType));
    if (hit.isMatch() && hit.type() == ScaleTool::SideHitType)
    {
      auto highlight = render::RenderService{renderContext, renderBatch};
      highlight.setShowBackfaces();
      highlight.setForegroundColor(pref(Preferences::ScaleFillColor));
      highlight.renderFilledPolygon(
        vm::polygon3f{polygonForBBoxSide(bounds(), hit.target<BBoxSide>())}.vertices());
    }
  }

  bool cancel() override { return false; }

  vm::bbox3d bounds() const { return mdl::terrainBounds(m_delegate->tool().terrain()); }
};

} // namespace

TerrainToolControllerBase::TerrainToolControllerBase(TerrainTool& tool)
  : m_tool{tool}
{
}

TerrainToolControllerBase::~TerrainToolControllerBase() = default;

Tool& TerrainToolControllerBase::tool()
{
  return m_tool;
}

const Tool& TerrainToolControllerBase::tool() const
{
  return m_tool;
}

void TerrainToolControllerBase::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch);
  ToolControllerGroup::render(inputState, renderContext, renderBatch);
}

bool TerrainToolControllerBase::cancel()
{
  if (m_tool.addMode())
  {
    m_tool.setAddMode(false);
    return true;
  }
  return false;
}

TerrainToolController2D::TerrainToolController2D(TerrainTool& tool)
  : TerrainToolControllerBase{tool}
{
  addController(
    std::make_unique<CreateTerrainPart>(std::make_unique<PartDelegate2D>(tool)));
  addController(
    std::make_unique<TerrainScalePart>(std::make_unique<PartDelegate2D>(tool)));
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate2D>(tool)));
}

TerrainToolController3D::TerrainToolController3D(TerrainTool& tool)
  : TerrainToolControllerBase{tool}
{
  addController(
    std::make_unique<CreateTerrainPart>(std::make_unique<PartDelegate3D>(tool)));
  addController(
    std::make_unique<TerrainScalePart>(std::make_unique<PartDelegate3D>(tool)));
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate3D>(tool)));
}

} // namespace tb::ui

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
#include "ui/InputState.h"
#include "ui/TerrainTool.h"

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

/** Applies the sculpting brush for as long as the mouse is dragged over the terrain. */
class SculptTracker : public GestureTracker
{
private:
  TerrainTool& m_tool;

public:
  explicit SculptTracker(TerrainTool& tool)
    : m_tool{tool}
  {
  }

  bool update(const InputState& inputState) override
  {
    if (const auto position = m_tool.pickSurface(inputState.pickRay()))
    {
      m_tool.setBrushPosition(*position);
      m_tool.applyStroke(*position, inputState.modifierKeysDown(ModifierKeys::Shift));
    }
    return true;
  }

  void end(const InputState&) override { m_tool.endStroke(); }

  void cancel() override { m_tool.cancelStroke(); }
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

/** Sculpts and paints the current terrain while add mode is disabled. */
class SculptPart : public ToolController, protected PartBase
{
public:
  explicit SculptPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  void mouseMove(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (tool.addMode())
    {
      tool.setBrushPosition(std::nullopt);
      return;
    }
    tool.setBrushPosition(tool.pickSurface(inputState.pickRay()));
  }

  bool mouseClick(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (
      tool.addMode() || !inputState.mouseButtonsPressed(MouseButtons::Left)
      || (!inputState.modifierKeysPressed(ModifierKeys::None) && !inputState.modifierKeysPressed(ModifierKeys::Shift)))
    {
      return false;
    }

    // Clicking another terrain's geometry picks it up for editing.
    if (
      inputState.modifierKeysPressed(ModifierKeys::None)
      && tool.selectTerrainAt(inputState.pickResult()))
    {
      return true;
    }

    // Pressing and releasing the mouse in the same place never becomes a drag, so the
    // brush is applied once here; otherwise clicking without moving would do nothing.
    if (const auto position = tool.pickSurface(inputState.pickRay()))
    {
      tool.beginStroke();
      tool.setBrushPosition(*position);
      tool.applyStroke(*position, inputState.modifierKeysDown(ModifierKeys::Shift));
      tool.endStroke();
      return true;
    }
    return false;
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (
      tool.addMode() || inputState.mouseButtons() != MouseButtons::Left
      || (!inputState.modifierKeysPressed(ModifierKeys::None) && !inputState.modifierKeysPressed(ModifierKeys::Shift)))
    {
      return nullptr;
    }

    const auto position = tool.pickSurface(inputState.pickRay());
    if (!position)
    {
      return nullptr;
    }

    tool.beginStroke();
    tool.setBrushPosition(*position);
    tool.applyStroke(*position, inputState.modifierKeysDown(ModifierKeys::Shift));
    return std::make_unique<SculptTracker>(tool);
  }

  bool cancel() override { return false; }
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
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate2D>(tool)));
}

TerrainToolController3D::TerrainToolController3D(TerrainTool& tool)
  : TerrainToolControllerBase{tool}
{
  addController(
    std::make_unique<CreateTerrainPart>(std::make_unique<PartDelegate3D>(tool)));
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate3D>(tool)));
}

} // namespace tb::ui

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

#include "ui/ToolController.h"

namespace tb
{
namespace mdl
{
class PickResult;
}

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class SplineTool;

class SplineToolControllerBase : public ToolControllerGroup
{
protected:
  SplineTool& m_tool;

protected:
  explicit SplineToolControllerBase(SplineTool& tool);
  ~SplineToolControllerBase() override;

private:
  Tool& tool() override;
  const Tool& tool() const override;

  void pick(const InputState& inputState, mdl::PickResult& pickResult) override;

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override;

  bool cancel() override;
};

/**
 * The spline tool controller for the 3D view. New points are placed on brushes under
 * the mouse, or on a horizontal plane through the last point, and points are dragged
 * on a horizontal plane.
 */
class SplineToolController3D : public SplineToolControllerBase
{
public:
  explicit SplineToolController3D(SplineTool& tool);
};

/**
 * The spline tool controller for the 2D views. New points are placed on the view
 * plane through the last point, and points are dragged on the view plane.
 */
class SplineToolController2D : public SplineToolControllerBase
{
public:
  explicit SplineToolController2D(SplineTool& tool);
};

} // namespace ui
} // namespace tb

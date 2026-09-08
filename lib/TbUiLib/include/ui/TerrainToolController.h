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

namespace tb::ui
{
class TerrainTool;

/**
 * Handles the terrain tool's input: dragging out a box to create a terrain while add
 * mode is enabled, dragging the sculpting brush over a terrain otherwise, and clicking
 * a terrain's geometry to pick it up for editing.
 */
class TerrainToolControllerBase : public ToolControllerGroup
{
private:
  TerrainTool& m_tool;

public:
  explicit TerrainToolControllerBase(TerrainTool& tool);
  ~TerrainToolControllerBase() override;

private:
  Tool& tool() override;
  const Tool& tool() const override;

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override;

  bool cancel() override;
};

class TerrainToolController2D : public TerrainToolControllerBase
{
public:
  explicit TerrainToolController2D(TerrainTool& tool);
};

class TerrainToolController3D : public TerrainToolControllerBase
{
public:
  explicit TerrainToolController3D(TerrainTool& tool);
};

} // namespace tb::ui

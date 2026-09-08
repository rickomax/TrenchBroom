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

#include "ui/TerrainToolPage.h"

#include <QBoxLayout>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

#include "mdl/Map.h"
#include "ui/MapDocument.h"
#include "ui/TerrainTool.h"

namespace tb::ui
{
TerrainToolPage::TerrainToolPage(
  MapDocument& document, TerrainTool& tool, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
  , m_tool{tool}
{
  createGui();
  connectObservers();
  updateControls();
}

void TerrainToolPage::createGui()
{
  // None of these controls take focus: focusing a widget outside the map views
  // deactivates them, and the next click in a map view would be swallowed as the
  // activation click instead of reaching the tool.
  const auto makeToggle = [](const QString& label, const QString& toolTip) {
    auto* button = new QPushButton{label};
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(toolTip);
    return button;
  };

  m_addTerrain = makeToggle(
    tr("Add"),
    tr("While enabled, dragging out a box in a map view creates a new terrain of that "
       "size; disable it to sculpt the terrain instead"));

  m_cellSize = new QDoubleSpinBox{};
  m_cellSize->setRange(1.0, 1024.0);
  m_cellSize->setSingleStep(8.0);
  m_cellSize->setToolTip(
    tr("The width and height of one cell of a newly created terrain; smaller cells "
       "give a finer terrain and more brushes"));

  m_radius = new QDoubleSpinBox{};
  m_radius->setRange(1.0, 8192.0);
  m_radius->setSingleStep(16.0);
  m_radius->setToolTip(tr("The radius of the sculpting brush"));

  m_strength = new QDoubleSpinBox{};
  m_strength->setRange(0.1, 1024.0);
  m_strength->setSingleStep(4.0);
  m_strength->setToolTip(
    tr("How much one stroke of the sculpting brush changes the "
       "terrain"));

  m_raise = makeToggle(
    tr("Raise"), tr("Raise the terrain under the brush (hold Shift to lower)"));
  m_lower = makeToggle(
    tr("Lower"), tr("Lower the terrain under the brush (hold Shift to raise)"));
  m_flatten = makeToggle(
    tr("Flatten"),
    tr("Flatten the terrain under the brush towards the height where you clicked "
       "(hold Shift to smooth)"));
  m_smooth = makeToggle(
    tr("Smooth"), tr("Even out the terrain under the brush (hold Shift to flatten)"));
  m_texture = makeToggle(
    tr("Texture"),
    tr("Paint the material selected in the material browser onto the terrain cells "
       "under the brush"));

  m_texScaleX = new QDoubleSpinBox{};
  m_texScaleX->setRange(-64.0, 64.0);
  m_texScaleX->setSingleStep(0.25);
  m_texScaleX->setToolTip(
    tr("Horizontal texture scale of the whole terrain; changing it rescales all of "
       "the terrain's texture coordinates"));

  m_texScaleY = new QDoubleSpinBox{};
  m_texScaleY->setRange(-64.0, 64.0);
  m_texScaleY->setSingleStep(0.25);
  m_texScaleY->setToolTip(
    tr("Vertical texture scale of the whole terrain; changing it rescales all of the "
       "terrain's texture coordinates"));

  m_removeButton = new QPushButton{tr("Remove")};
  m_removeButton->setFocusPolicy(Qt::NoFocus);
  m_removeButton->setToolTip(tr("Delete the current terrain and its brushes"));

  m_breakButton = new QPushButton{tr("Break")};
  m_breakButton->setFocusPolicy(Qt::NoFocus);
  m_breakButton->setToolTip(
    tr("Turn the terrain into standard, editable brushes and remove the terrain"));

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);

  layout->addWidget(m_addTerrain);
  layout->addWidget(new QLabel{tr("Cell:")});
  layout->addWidget(m_cellSize);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Radius:")});
  layout->addWidget(m_radius);
  layout->addWidget(new QLabel{tr("Strength:")});
  layout->addWidget(m_strength);
  layout->addSpacing(12);
  layout->addWidget(m_raise);
  layout->addWidget(m_lower);
  layout->addWidget(m_flatten);
  layout->addWidget(m_smooth);
  layout->addWidget(m_texture);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Scale X:")});
  layout->addWidget(m_texScaleX);
  layout->addWidget(new QLabel{tr("Y:")});
  layout->addWidget(m_texScaleY);
  layout->addSpacing(12);
  layout->addWidget(m_removeButton);
  layout->addWidget(m_breakButton);
  layout->addStretch();

  setLayout(layout);

  connect(m_addTerrain, &QPushButton::toggled, this, [this](const bool checked) {
    if (!m_updatingControls)
    {
      m_tool.setAddMode(checked);
    }
  });

  const auto connectMode = [this](QPushButton* button, const TerrainToolMode mode) {
    connect(button, &QPushButton::clicked, this, [this, mode]() {
      if (!m_updatingControls)
      {
        // The modes are mutually exclusive, so re-selecting the active one keeps it.
        m_tool.setMode(mode);
        updateControls();
      }
    });
  };
  connectMode(m_raise, TerrainToolMode::Raise);
  connectMode(m_lower, TerrainToolMode::Lower);
  connectMode(m_flatten, TerrainToolMode::Flatten);
  connectMode(m_smooth, TerrainToolMode::Smooth);
  connectMode(m_texture, TerrainToolMode::Texture);

  connect(
    m_cellSize,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [this](const double value) {
      if (!m_updatingControls)
      {
        m_tool.setCellSize(value);
      }
    });
  connect(
    m_radius,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [this](const double value) {
      if (!m_updatingControls)
      {
        m_tool.setRadius(value);
      }
    });
  connect(
    m_strength,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [this](const double value) {
      if (!m_updatingControls)
      {
        m_tool.setStrength(value);
      }
    });

  const auto applyTexScale = [this]() {
    if (!m_updatingControls)
    {
      m_tool.setTexScale(float(m_texScaleX->value()), float(m_texScaleY->value()));
    }
  };
  connect(
    m_texScaleX,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [applyTexScale](double) { applyTexScale(); });
  connect(
    m_texScaleY,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [applyTexScale](double) { applyTexScale(); });

  connect(
    m_removeButton, &QPushButton::clicked, this, [this]() { m_tool.removeTerrain(); });
  connect(
    m_breakButton, &QPushButton::clicked, this, [this]() { m_tool.breakTerrain(); });
}

void TerrainToolPage::connectObservers()
{
  m_notifierConnection +=
    m_tool.terrainDidChangeNotifier.connect([this]() { updateControls(); });
  m_notifierConnection += m_document.map().currentMaterialNameDidChangeNotifier.connect(
    [this]() { updateControls(); });
}

void TerrainToolPage::updateControls()
{
  m_updatingControls = true;

  m_addTerrain->setChecked(m_tool.addMode());
  m_cellSize->setValue(m_tool.cellSize());
  m_radius->setValue(m_tool.radius());
  m_strength->setValue(m_tool.strength());

  // While add mode is on no sculpting mode is active, so none of the mode toggles are
  // shown as selected; clicking one of them leaves add mode again.
  const auto addMode = m_tool.addMode();
  const auto mode = m_tool.mode();
  m_raise->setChecked(!addMode && mode == TerrainToolMode::Raise);
  m_lower->setChecked(!addMode && mode == TerrainToolMode::Lower);
  m_flatten->setChecked(!addMode && mode == TerrainToolMode::Flatten);
  m_smooth->setChecked(!addMode && mode == TerrainToolMode::Smooth);
  m_texture->setChecked(!addMode && mode == TerrainToolMode::Texture);

  const auto hasTerrain = m_tool.hasTerrain();
  m_texScaleX->setEnabled(hasTerrain);
  m_texScaleY->setEnabled(hasTerrain);
  if (hasTerrain)
  {
    m_texScaleX->setValue(double(m_tool.texScaleX()));
    m_texScaleY->setValue(double(m_tool.texScaleY()));
  }

  m_removeButton->setEnabled(m_tool.canRemoveTerrain());
  m_breakButton->setEnabled(m_tool.canBreakTerrain());

  m_updatingControls = false;
}

} // namespace tb::ui

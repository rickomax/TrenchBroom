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
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>

#include "mdl/Map.h"
#include "mdl/TerrainHeightmap.h"
#include "ui/FileDialogDefaultDir.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"
#include "ui/TerrainTool.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace tb::ui
{
namespace
{

using ModeItem = std::pair<std::optional<TerrainToolMode>, QString>;

/** The entries of the mode drop down, in the order they are shown. */
const std::array<ModeItem, 7>& modeItems()
{
  static const auto items = std::array<ModeItem, 7>{
    ModeItem{std::nullopt, QObject::tr("None")},
    ModeItem{TerrainToolMode::Raise, QObject::tr("Raise")},
    ModeItem{TerrainToolMode::Lower, QObject::tr("Lower")},
    ModeItem{TerrainToolMode::Flatten, QObject::tr("Flatten")},
    ModeItem{TerrainToolMode::Smooth, QObject::tr("Smooth")},
    ModeItem{TerrainToolMode::Texture, QObject::tr("Texture")},
    ModeItem{TerrainToolMode::Scale, QObject::tr("Scale")},
  };
  return items;
}

} // namespace

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
    tr("Create"),
    tr("While enabled, dragging out a box in a map view creates a new terrain of that "
       "size; disable it to sculpt or select terrains instead"));

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
  m_strength->setRange(0.01, 1024.0);
  m_strength->setDecimals(2);
  m_strength->setSingleStep(0.1);
  m_strength->setToolTip(
    tr("How much each application of the sculpting brush changes the terrain; the "
       "brush keeps being applied while the mouse is held down"));

  // The modes are mutually exclusive, so one drop down takes far less of the toolbar
  // than a row of toggles. "None" leaves the tool only selecting terrains.
  m_mode = new QComboBox{};
  m_mode->setFocusPolicy(Qt::NoFocus);
  for (const auto& [mode, label] : modeItems())
  {
    m_mode->addItem(label);
  }
  m_mode->setToolTip(
    tr("What the sculpting brush does: Raise and Lower move the terrain under the "
       "brush, Flatten levels it towards the height where you clicked, Smooth evens it "
       "out, and Texture paints the material selected in the material browser onto the "
       "cells under the brush. Holding Shift swaps Raise with Lower and Flatten with "
       "Smooth. Scale drags the handles of the terrain's bounding box: scaling it in X "
       "or Y changes how many cells it has, and scaling it in Z scales its heights. "
       "With None selected, clicking a terrain picks it up for editing."));

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

  m_importButton = new QPushButton{tr("Import")};
  m_importButton->setFocusPolicy(Qt::NoFocus);
  m_importButton->setToolTip(
    tr("Replace the terrain's heights with a raw height map. The file's dimensions are "
       "taken from its size, which must be a square of 8 bit or 16 bit samples, and the "
       "heights are spread over the terrain's current height"));

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
  layout->addWidget(new QLabel{tr("Mode:")});
  layout->addWidget(m_mode);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Scale X:")});
  layout->addWidget(m_texScaleX);
  layout->addWidget(new QLabel{tr("Y:")});
  layout->addWidget(m_texScaleY);
  layout->addSpacing(12);
  layout->addWidget(m_importButton);
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

  connect(
    m_mode,
    QOverload<int>::of(&QComboBox::currentIndexChanged),
    this,
    [this](const int index) {
      if (!m_updatingControls && index >= 0 && size_t(index) < modeItems().size())
      {
        m_tool.setMode(modeItems()[size_t(index)].first);
        updateControls();
      }
    });

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

  connect(m_importButton, &QPushButton::clicked, this, [this]() { importHeightmap(); });
  connect(
    m_removeButton, &QPushButton::clicked, this, [this]() { m_tool.removeTerrain(); });
  connect(
    m_breakButton, &QPushButton::clicked, this, [this]() { m_tool.breakTerrain(); });
}

void TerrainToolPage::importHeightmap()
{
  // A .raw file carries no header, so the sample format normally has to be deduced from
  // its length and contents. Picking one of the explicit filters says it outright, which
  // is worth having because a file's length can fit both 8 bit and 32 bit float samples.
  const auto autoFilter = tr("Raw height map (*.raw *.r8 *.r16 *.r32 *.f32 *.flt)");
  const auto int8Filter = tr("Raw height map, 8 bit (*.raw *.r8)");
  const auto int16Filter = tr("Raw height map, 16 bit (*.raw *.r16)");
  const auto float32Filter = tr("Raw height map, 32 bit float (*.raw *.r32 *.f32 *.flt)");
  const auto filters = QStringList{autoFilter, int8Filter, int16Filter, float32Filter}
                       << tr("All files (*.*)");

  auto selectedFilter = autoFilter;
  const auto path = QFileDialog::getOpenFileName(
    this,
    tr("Import Height Map"),
    fileDialogDefaultDirectory(FileDialogDir::Map),
    filters.join(";;"),
    &selectedFilter);

  if (path.isEmpty())
  {
    return;
  }

  const auto format =
    selectedFilter == int8Filter      ? std::optional{mdl::RawSampleFormat::Int8}
    : selectedFilter == int16Filter   ? std::optional{mdl::RawSampleFormat::Int16}
    : selectedFilter == float32Filter ? std::optional{mdl::RawSampleFormat::Float32}
                                      : std::nullopt;

  updateFileDialogDefaultDirectoryWithFilename(FileDialogDir::Map, path);
  if (!m_tool.importHeightmap(pathFromQString(path), format))
  {
    QMessageBox::critical(
      this,
      tr("Import Height Map"),
      tr("The height map could not be imported. See the map's issue log for the reason. "
         "If the file's size fits more than one sample format, choose the format "
         "explicitly in the file dialog."));
  }
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

  // With None selected the tool just picks up the terrain that is clicked; selecting a
  // mode leaves creation mode, and vice versa.
  const auto& items = modeItems();
  const auto item = std::ranges::find(items, m_tool.mode(), &ModeItem::first);
  m_mode->setCurrentIndex(item != items.end() ? int(item - items.begin()) : 0);

  const auto hasTerrain = m_tool.hasTerrain();
  m_texScaleX->setEnabled(hasTerrain);
  m_texScaleY->setEnabled(hasTerrain);
  if (hasTerrain)
  {
    m_texScaleX->setValue(double(m_tool.texScaleX()));
    m_texScaleY->setValue(double(m_tool.texScaleY()));
  }

  m_importButton->setEnabled(m_tool.hasTerrain());
  m_removeButton->setEnabled(m_tool.canRemoveTerrain());
  m_breakButton->setEnabled(m_tool.canBreakTerrain());

  m_updatingControls = false;
}

} // namespace tb::ui

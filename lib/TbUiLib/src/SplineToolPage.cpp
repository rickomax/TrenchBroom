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

#include "ui/SplineToolPage.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

#include "mdl/Map.h"
#include "ui/MapDocument.h"
#include "ui/SplineTool.h"

namespace tb::ui
{

SplineToolPage::SplineToolPage(MapDocument& document, SplineTool& tool, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
  , m_tool{tool}
{
  createGui();
  connectObservers();
  updateControls();
}

void SplineToolPage::createGui()
{
  m_templateLabel = new QLabel{tr("<none>")};
  m_linkButton = new QPushButton{tr("Link Group")};
  m_linkButton->setToolTip(
    tr("Use the selected group's brushes as the spline's template"));
  m_unlinkButton = new QPushButton{tr("Unlink")};

  m_roll = new QDoubleSpinBox{};
  m_roll->setRange(-360.0, 360.0);
  m_roll->setSingleStep(15.0);
  m_roll->setToolTip(tr("Rotation of the selected point around the spline"));

  m_locked = new QCheckBox{tr("Locked")};
  m_locked->setToolTip(
    tr("Locked points are not moved or affected by rotating other points"));

  m_removePointButton = new QPushButton{tr("Remove Point")};

  m_rotateAll = new QDoubleSpinBox{};
  m_rotateAll->setRange(-360.0, 360.0);
  m_rotateAll->setSingleStep(15.0);
  m_rotateAll->setValue(15.0);
  m_rotateAllButton = new QPushButton{tr("Rotate Unlocked")};
  m_rotateAllButton->setToolTip(
    tr("Add the given angle to the rotation of all unlocked points"));

  m_subdivisions = new QSpinBox{};
  m_subdivisions->setRange(1, 64);
  m_subdivisions->setToolTip(tr("Number of curve segments between two points"));

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);

  layout->addWidget(new QLabel{tr("Template:")});
  layout->addWidget(m_templateLabel);
  layout->addWidget(m_linkButton);
  layout->addWidget(m_unlinkButton);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Roll:")});
  layout->addWidget(m_roll);
  layout->addWidget(m_locked);
  layout->addWidget(m_removePointButton);
  layout->addSpacing(12);
  layout->addWidget(m_rotateAll);
  layout->addWidget(m_rotateAllButton);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Subdivisions:")});
  layout->addWidget(m_subdivisions);
  layout->addStretch();

  setLayout(layout);

  connect(
    m_linkButton, &QPushButton::clicked, this, [this]() { m_tool.linkTemplateGroup(); });
  connect(m_unlinkButton, &QPushButton::clicked, this, [this]() {
    m_tool.unlinkTemplateGroup();
  });
  connect(
    m_roll,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [this](const double value) {
      if (!m_updatingControls)
      {
        m_tool.setSelectedPointRoll(value);
      }
    });
  connect(m_locked, &QCheckBox::toggled, this, [this](const bool checked) {
    if (!m_updatingControls && checked != m_tool.selectedPointLocked())
    {
      m_tool.toggleSelectedPointLocked();
    }
  });
  connect(
    m_removePointButton, &QPushButton::clicked, this, [this]() { m_tool.removePoint(); });
  connect(m_rotateAllButton, &QPushButton::clicked, this, [this]() {
    m_tool.rotateUnlockedPoints(m_rotateAll->value());
  });
  connect(
    m_subdivisions,
    QOverload<int>::of(&QSpinBox::valueChanged),
    this,
    [this](const int value) {
      if (!m_updatingControls && value > 0)
      {
        m_tool.setSubdivisions(size_t(value));
      }
    });
}

void SplineToolPage::connectObservers()
{
  m_notifierConnection +=
    m_tool.splineDidChangeNotifier.connect([this]() { updateControls(); });
  m_notifierConnection += m_document.map().selectionDidChangeNotifier.connect(
    [this](const auto&) { updateControls(); });
}

void SplineToolPage::updateControls()
{
  m_updatingControls = true;

  const auto templateName = m_tool.templateGroupName();
  m_templateLabel->setText(
    m_tool.hasTemplateGroup()
      ? (!templateName.empty() ? QString::fromStdString(templateName) : tr("<missing>"))
      : tr("<none>"));

  m_linkButton->setEnabled(m_tool.canLinkTemplateGroup());
  m_unlinkButton->setEnabled(m_tool.hasTemplateGroup());

  const auto hasSelectedPoint = m_tool.selectedPointIndex().has_value();
  m_roll->setEnabled(hasSelectedPoint && !m_tool.selectedPointLocked());
  m_roll->setValue(m_tool.selectedPointRoll());
  m_locked->setEnabled(hasSelectedPoint);
  m_locked->setChecked(m_tool.selectedPointLocked());
  m_removePointButton->setEnabled(m_tool.canRemovePoint());
  m_rotateAllButton->setEnabled(m_tool.hasPoints());

  m_subdivisions->setValue(int(m_tool.subdivisions()));

  m_updatingControls = false;
}

} // namespace tb::ui

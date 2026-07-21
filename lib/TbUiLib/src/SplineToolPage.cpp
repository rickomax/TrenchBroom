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
  m_addPoints = new QPushButton{tr("Add")};
  m_addPoints->setCheckable(true);
  // Don't take focus when clicked: focusing a widget outside the map views deactivates
  // them, causing the next click in a map view to be swallowed as the activation click
  // instead of adding a point.
  m_addPoints->setFocusPolicy(Qt::NoFocus);
  m_addPoints->setToolTip(
    tr("While enabled, clicking empty space appends new points to the spline; "
       "disable it to select and edit points without adding new ones"));

  m_templateLabel = new QLabel{tr("<none>")};
  m_linkButton = new QPushButton{tr("Link")};
  m_linkButton->setToolTip(
    tr("Use the selected group or the selected brushes as the spline's template"));
  m_linkButton->setFocusPolicy(Qt::NoFocus);
  m_unlinkButton = new QPushButton{tr("Unlink")};
  m_unlinkButton->setFocusPolicy(Qt::NoFocus);
  m_breakButton = new QPushButton{tr("Break")};
  m_breakButton->setToolTip(
    tr("Duplicate the generated brushes as standard, editable brushes and unlink "
       "the spline's template"));
  m_breakButton->setFocusPolicy(Qt::NoFocus);

  m_roll = new QDoubleSpinBox{};
  m_roll->setRange(-360.0, 360.0);
  m_roll->setSingleStep(15.0);
  m_roll->setToolTip(tr("Rotation of the selected point around the spline"));

  m_scale = new QDoubleSpinBox{};
  m_scale->setRange(0.01, 100.0);
  m_scale->setSingleStep(0.1);
  m_scale->setValue(1.0);
  m_scale->setToolTip(
    tr("Cross-section scale at the selected point; the swept profile tapers "
       "between points"));

  m_locked = new QCheckBox{tr("Locked")};
  m_locked->setFocusPolicy(Qt::NoFocus);
  m_locked->setToolTip(
    tr("A locked point anchors the sweep's orientation and the curve's shape: twists "
       "and slopes caused by points beyond it cannot propagate past it, so the curve "
       "between two locked points is shaped only by the points between them"));

  m_removePointButton = new QPushButton{tr("Remove")};
  m_removePointButton->setToolTip(tr("Remove the selected point from the spline"));
  m_removePointButton->setFocusPolicy(Qt::NoFocus);

  m_closed = new QCheckBox{tr("Closed")};
  m_closed->setToolTip(
    tr("Close the spline: the last point connects back to the first, and brushes "
       "are created on that segment as well"));
  m_closed->setFocusPolicy(Qt::NoFocus);

  m_subdivisions = new QSpinBox{};
  m_subdivisions->setRange(1, 64);
  m_subdivisions->setToolTip(tr("Number of curve segments between two points"));

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);

  layout->addWidget(m_addPoints);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Template:")});
  layout->addWidget(m_templateLabel);
  layout->addWidget(m_linkButton);
  layout->addWidget(m_unlinkButton);
  layout->addWidget(m_breakButton);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Roll:")});
  layout->addWidget(m_roll);
  layout->addWidget(new QLabel{tr("Scale:")});
  layout->addWidget(m_scale);
  layout->addWidget(m_locked);
  layout->addWidget(m_closed);
  layout->addWidget(m_removePointButton);
  layout->addSpacing(12);
  layout->addWidget(new QLabel{tr("Subdivisions:")});
  layout->addWidget(m_subdivisions);
  layout->addStretch();

  setLayout(layout);

  connect(m_addPoints, &QPushButton::toggled, this, [this](const bool checked) {
    if (!m_updatingControls)
    {
      m_tool.setAddPointMode(checked);
    }
  });
  connect(m_linkButton, &QPushButton::clicked, this, [this]() { m_tool.linkTemplate(); });
  connect(
    m_unlinkButton, &QPushButton::clicked, this, [this]() { m_tool.unlinkTemplate(); });
  connect(
    m_breakButton, &QPushButton::clicked, this, [this]() { m_tool.breakSpline(); });
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
  connect(
    m_scale,
    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
    this,
    [this](const double value) {
      if (!m_updatingControls)
      {
        m_tool.setSelectedPointScale(value);
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
  connect(m_closed, &QCheckBox::toggled, this, [this](const bool checked) {
    if (!m_updatingControls)
    {
      m_tool.setClosed(checked);
    }
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

  m_addPoints->setChecked(m_tool.addPointMode());

  const auto templateName = m_tool.templateName();
  m_templateLabel->setText(
    m_tool.hasTemplate()
      ? (!templateName.empty() ? QString::fromStdString(templateName) : tr("<missing>"))
      : tr("<none>"));

  m_linkButton->setEnabled(m_tool.canLinkTemplate());
  m_unlinkButton->setEnabled(m_tool.hasTemplate());
  m_breakButton->setEnabled(m_tool.canBreakSpline());

  const auto hasSelectedPoint = m_tool.selectedPointIndex().has_value();
  m_roll->setEnabled(hasSelectedPoint);
  m_roll->setValue(m_tool.selectedPointRoll());
  m_scale->setEnabled(hasSelectedPoint);
  m_scale->setValue(m_tool.selectedPointScale());
  m_locked->setEnabled(hasSelectedPoint);
  m_locked->setChecked(m_tool.selectedPointLocked());
  m_removePointButton->setEnabled(m_tool.canRemovePoint());
  m_closed->setEnabled(m_tool.hasPoints());
  m_closed->setChecked(m_tool.closed());

  m_subdivisions->setValue(int(m_tool.subdivisions()));

  m_updatingControls = false;
}

} // namespace tb::ui

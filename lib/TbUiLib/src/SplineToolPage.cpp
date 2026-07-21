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

#include <utility>

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

  const auto createLockCheckBox = [this](const QString& label, const QString& toolTip) {
    auto* checkBox = new QCheckBox{label};
    checkBox->setFocusPolicy(Qt::NoFocus);
    checkBox->setToolTip(toolTip);
    return checkBox;
  };
  const auto planeToolTip = tr(
    "Lock the curve to this plane at the selected point: the point's tangent is "
    "flattened into the plane, and a segment whose two end points share the lock "
    "stays entirely in the plane while still curving smoothly within it");
  m_lockXY = createLockCheckBox(tr("XY"), planeToolTip);
  m_lockXZ = createLockCheckBox(tr("XZ"), planeToolTip);
  m_lockYZ = createLockCheckBox(tr("YZ"), planeToolTip);
  m_lockTwist = createLockCheckBox(
    tr("Twist"),
    tr("Anchor the sweep's orientation at the selected point, so a twist caused by "
       "rotating other points cannot propagate past it"));

  m_removePointButton = new QPushButton{tr("Remove")};
  m_removePointButton->setToolTip(tr("Remove the selected point from the spline"));
  m_removePointButton->setFocusPolicy(Qt::NoFocus);

  m_closed = new QCheckBox{tr("Closed")};
  m_closed->setToolTip(
    tr("Close the spline: the last point connects back to the first, and brushes "
       "are created on that segment as well"));
  m_closed->setFocusPolicy(Qt::NoFocus);

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
  layout->addWidget(new QLabel{tr("Lock:")});
  layout->addWidget(m_lockXY);
  layout->addWidget(m_lockXZ);
  layout->addWidget(m_lockYZ);
  layout->addWidget(m_lockTwist);
  layout->addWidget(m_closed);
  layout->addWidget(m_removePointButton);
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
  const auto connectLockCheckBox = [this](QCheckBox* checkBox,
                                          const mdl::SplineLock::Type lock) {
    connect(checkBox, &QCheckBox::toggled, this, [this, lock](const bool checked) {
      if (!m_updatingControls)
      {
        m_tool.setSelectedPointLock(lock, checked);
      }
    });
  };
  connectLockCheckBox(m_lockXY, mdl::SplineLock::XY);
  connectLockCheckBox(m_lockXZ, mdl::SplineLock::XZ);
  connectLockCheckBox(m_lockYZ, mdl::SplineLock::YZ);
  connectLockCheckBox(m_lockTwist, mdl::SplineLock::Twist);
  connect(
    m_removePointButton, &QPushButton::clicked, this, [this]() { m_tool.removePoint(); });
  connect(m_closed, &QCheckBox::toggled, this, [this](const bool checked) {
    if (!m_updatingControls)
    {
      m_tool.setClosed(checked);
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
  for (const auto& [checkBox, lock] : {
         std::pair{m_lockXY, mdl::SplineLock::XY},
         std::pair{m_lockXZ, mdl::SplineLock::XZ},
         std::pair{m_lockYZ, mdl::SplineLock::YZ},
         std::pair{m_lockTwist, mdl::SplineLock::Twist},
       })
  {
    checkBox->setEnabled(hasSelectedPoint);
    checkBox->setChecked(m_tool.selectedPointLock(lock));
  }
  m_removePointButton->setEnabled(m_tool.canRemovePoint());
  m_closed->setEnabled(m_tool.hasPoints());
  m_closed->setChecked(m_tool.closed());

  m_updatingControls = false;
}

} // namespace tb::ui

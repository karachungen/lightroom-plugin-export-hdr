#include "gui/quick_edit_dock.h"

#include <QButtonGroup>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace uhdr_repack {

QuickEditDock::QuickEditDock(GainMapCanvas* canvas, QWidget* parent)
    : QDockWidget(tr("Adjustments"), parent), canvas_(canvas) {
  setObjectName(QStringLiteral("adjustmentsDock"));
  setFeatures(QDockWidget::NoDockWidgetFeatures);
  setMinimumWidth(276);

  auto* root = new QWidget(this);
  auto* layout = new QVBoxLayout(root);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(14);

  auto* display_group = new QGroupBox(tr("Display"), root);
  auto* display_layout = new QVBoxLayout(display_group);
  display_status_ = new QLabel(tr("Detecting HDR output…"), display_group);
  display_status_->setObjectName(QStringLiteral("displayStatus"));
  display_status_->setWordWrap(true);
  display_layout->addWidget(display_status_);
  layout->addWidget(display_group);

  auto* tools_group = new QGroupBox(tr("Gain map tools"), root);
  auto* tools_layout = new QHBoxLayout(tools_group);
  auto* tool_buttons = new QButtonGroup(this);
  tool_buttons->setExclusive(true);
  auto add_tool = [&](const QString& label, GainTool tool, const QKeySequence& shortcut,
                      bool checked = false) {
    auto* btn = new QToolButton(tools_group);
    btn->setText(label);
    btn->setShortcut(shortcut);
    btn->setCheckable(true);
    btn->setChecked(checked);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tool_buttons->addButton(btn);
    connect(btn, &QToolButton::clicked, this, [this, tool]() {
      if (canvas_) {
        canvas_->setTool(tool);
      }
      emit toolChanged(tool);
    });
    tools_layout->addWidget(btn);
  };

  add_tool(tr("Brush"), GainTool::kBrush, QKeySequence(Qt::Key_B), true);
  add_tool(tr("Erase"), GainTool::kEraser, QKeySequence(Qt::Key_E));
  add_tool(tr("Smooth"), GainTool::kSmooth, QKeySequence(Qt::Key_S));
  layout->addWidget(tools_group);

  auto* brush_group = new QGroupBox(tr("Brush"), root);
  auto* brush_form = new QFormLayout(brush_group);
  radius_slider_ = new QSlider(Qt::Horizontal, brush_group);
  radius_slider_->setRange(4, 250);
  radius_slider_->setValue(24);
  radius_value_ = new QLabel(tr("24 px"), brush_group);
  brush_form->addRow(tr("Size"), radius_slider_);
  brush_form->addRow(QString(), radius_value_);
  opacity_slider_ = new QSlider(Qt::Horizontal, brush_group);
  opacity_slider_->setRange(1, 100);
  opacity_slider_->setValue(50);
  opacity_value_ = new QLabel(tr("50%"), brush_group);
  brush_form->addRow(tr("Strength"), opacity_slider_);
  brush_form->addRow(QString(), opacity_value_);
  layout->addWidget(brush_group);

  auto* hdr_group = new QGroupBox(tr("HDR mapping"), root);
  auto* form = new QFormLayout(hdr_group);
  boost_slider_ = new QSlider(Qt::Horizontal, root);
  boost_slider_->setRange(10, 10000);
  boost_slider_->setValue(320);
  boost_value_ = new QLabel(tr("32.0×"), hdr_group);
  form->addRow(tr("Maximum boost"), boost_slider_);
  form->addRow(QString(), boost_value_);

  peak_slider_ = new QSlider(Qt::Horizontal, root);
  peak_slider_->setRange(400, 4000);
  peak_slider_->setValue(1000);
  peak_value_ = new QLabel(tr("1000 nits"), hdr_group);
  form->addRow(tr("Target peak"), peak_slider_);
  form->addRow(QString(), peak_value_);
  gain_stats_ = new QLabel(tr("Gain map not loaded"), hdr_group);
  gain_stats_->setObjectName(QStringLiteral("secondaryText"));
  form->addRow(gain_stats_);
  layout->addWidget(hdr_group);

  auto* output_group = new QGroupBox(tr("Output"), root);
  auto* output_layout = new QVBoxLayout(output_group);
  output_info_ = new QLabel(output_group);
  output_info_->setObjectName(QStringLiteral("secondaryText"));
  paths_info_ = new QLabel(output_group);
  paths_info_->setObjectName(QStringLiteral("secondaryText"));
  paths_info_->setWordWrap(true);
  paths_info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  output_layout->addWidget(output_info_);
  output_layout->addWidget(paths_info_);
  layout->addWidget(output_group);

  auto* undo_row = new QHBoxLayout();
  auto* undo_btn = new QPushButton(tr("Undo"), root);
  auto* redo_btn = new QPushButton(tr("Redo"), root);
  undo_row->addWidget(undo_btn);
  undo_row->addWidget(redo_btn);
  layout->addLayout(undo_row);

  if (canvas_) {
    connect(undo_btn, &QPushButton::clicked, canvas_->undoStack(), &QUndoStack::undo);
    connect(redo_btn, &QPushButton::clicked, canvas_->undoStack(), &QUndoStack::redo);
  }

  connect(boost_slider_, &QSlider::valueChanged, this, [this](int v) {
    boost_value_->setText(tr("%1×").arg(v / 10.0, 0, 'f', 1));
    if (syncing_) return;
    const float max_boost = static_cast<float>(v) / 10.0f;
    emit contentBoostChanged(1.0f, max_boost);
    if (canvas_) {
      canvas_->setContentBoost(1.0f, max_boost);
    }
  });

  connect(peak_slider_, &QSlider::valueChanged, this, [this](int v) {
    peak_value_->setText(tr("%1 nits").arg(v));
    if (syncing_) return;
    emit displayPeakChanged(static_cast<float>(v));
  });
  connect(radius_slider_, &QSlider::valueChanged, this, [this](int v) {
    radius_value_->setText(tr("%1 px").arg(v));
    if (canvas_) canvas_->setBrushRadius(v);
  });
  connect(opacity_slider_, &QSlider::valueChanged, this, [this](int v) {
    opacity_value_->setText(tr("%1%").arg(v));
    if (canvas_) canvas_->setBrushOpacity(v / 100.0f);
  });

  layout->addStretch();
  setWidget(root);
}

void QuickEditDock::setEncodeOptions(const EncodeOptions& options) {
  syncing_ = true;
  boost_slider_->setValue(
      std::clamp(static_cast<int>(std::lround(options.max_content_boost * 10.0f)), 10, 10000));
  peak_slider_->setValue(std::clamp(static_cast<int>(std::lround(options.target_display_peak_nits)),
                                    400, 4000));
  syncing_ = false;
}

void QuickEditDock::setGainStats(float min_gain, float max_gain, int width, int height) {
  gain_stats_->setText(tr("%1 × %2  ·  %3×–%4×")
                           .arg(width)
                           .arg(height)
                           .arg(min_gain, 0, 'f', 2)
                           .arg(max_gain, 0, 'f', 1));
}

void QuickEditDock::setDisplayStatus(const QString& status, bool hdr_active) {
  display_status_->setText((hdr_active ? tr("HDR active\n") : tr("SDR fallback\n")) + status);
  display_status_->setProperty("hdrActive", hdr_active);
  display_status_->style()->unpolish(display_status_);
  display_status_->style()->polish(display_status_);
}

void QuickEditDock::setItemDetails(const QString& source, const QString& output, int base_quality,
                                   int gain_quality) {
  output_info_->setText(
      tr("JPEG %1  ·  gain map %2").arg(base_quality).arg(gain_quality));
  paths_info_->setText(tr("Source  %1\nOutput  %2").arg(source, output));
}

}  // namespace uhdr_repack

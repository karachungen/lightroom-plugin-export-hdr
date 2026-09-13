#include "gui/gainmap_canvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTabletEvent>
#include <QUndoCommand>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace uhdr_repack {

class GainMapEditCommand final : public QUndoCommand {
 public:
  GainMapEditCommand(GainMapCanvas* canvas, std::vector<float> before, std::vector<float> after)
      : QUndoCommand(QObject::tr("Gain map stroke")), canvas_(canvas), before_(std::move(before)),
        after_(std::move(after)) {}

  void undo() override {
    if (canvas_) canvas_->applyMapFromUndo(before_);
  }

  void redo() override {
    if (canvas_) canvas_->applyMapFromUndo(after_);
  }

 private:
  GainMapCanvas* canvas_;
  std::vector<float> before_;
  std::vector<float> after_;
};

namespace {

float sample_gain(const std::vector<float>& map, int w, int h, int x, int y) {
  x = std::clamp(x, 0, w - 1);
  y = std::clamp(y, 0, h - 1);
  return map[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
}

}  // namespace

GainMapCanvas::GainMapCanvas(QWidget* parent) : QWidget(parent) {
  setMinimumSize(320, 240);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
}

void GainMapCanvas::setBaseSdr(const QImage& sdr) {
  sdr_image_ = sdr;
  gain_w_ = sdr.width();
  gain_h_ = sdr.height();
  const size_t n = static_cast<size_t>(gain_w_) * static_cast<size_t>(gain_h_);
  gain_map_.assign(n, 1.0f);
  auto_gain_map_.assign(n, 1.0f);
  update();
}

void GainMapCanvas::setAutoGainMap(const std::vector<float>& gain, int width, int height) {
  gain_w_ = width;
  gain_h_ = height;
  gain_map_ = gain;
  auto_gain_map_ = gain;
  undo_stack_.clear();
  if (!gain.empty()) {
    const auto minmax = std::minmax_element(gain.begin(), gain.end());
    visualization_min_ = std::max(0.0001f, *minmax.first);
    visualization_max_ = std::max(visualization_min_ + 0.0001f, *minmax.second);
  }
  fitToView();
  update();
}

std::vector<float> GainMapCanvas::gainMapData() const {
  return gain_map_;
}

void GainMapCanvas::setTool(GainTool tool) {
  tool_ = tool;
}

void GainMapCanvas::setBrushRadius(int px) {
  brush_radius_ = std::max(1, px);
}

void GainMapCanvas::setBrushOpacity(float opacity) {
  brush_opacity_ = std::clamp(opacity, 0.0f, 1.0f);
}

void GainMapCanvas::setContentBoost(float min_boost, float max_boost) {
  min_boost_ = min_boost;
  max_boost_ = max_boost;
}

void GainMapCanvas::setZoom(float zoom) {
  zoom_ = std::clamp(zoom, 0.1f, 16.0f);
  update();
}

void GainMapCanvas::fitToView() {
  zoom_ = 1.0f;
  pan_ = {};
  update();
}

QRectF GainMapCanvas::imageRect() const {
  if (sdr_image_.isNull() || width() <= 0 || height() <= 0) return {};
  const QSizeF fit = sdr_image_.size().scaled(size(), Qt::KeepAspectRatio);
  const QSizeF scaled = fit * zoom_;
  const QPointF center(width() * 0.5 + pan_.x(), height() * 0.5 + pan_.y());
  return QRectF(center.x() - scaled.width() * 0.5, center.y() - scaled.height() * 0.5,
                scaled.width(), scaled.height());
}

QPoint GainMapCanvas::mapToGain(const QPoint& widget_pos) const {
  if (sdr_image_.isNull() || gain_w_ <= 0 || gain_h_ <= 0) {
    return QPoint(0, 0);
  }
  const QRectF target = imageRect();
  const int gx = static_cast<int>((widget_pos.x() - target.left()) / target.width() * gain_w_);
  const int gy = static_cast<int>((widget_pos.y() - target.top()) / target.height() * gain_h_);
  return QPoint(gx, gy);
}

void GainMapCanvas::applyBrushAt(const QPoint& pos, float pressure) {
  if (gain_map_.empty() || gain_w_ <= 0 || gain_h_ <= 0) {
    return;
  }
  const int radius = std::max(1, static_cast<int>(brush_radius_ * std::max(0.2f, pressure)));
  const float strength = brush_opacity_ * std::max(0.2f, pressure);
  const float target = max_boost_;

  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = pos.x() + dx;
      const int y = pos.y() + dy;
      if (x < 0 || y < 0 || x >= gain_w_ || y >= gain_h_) {
        continue;
      }
      const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (dist > static_cast<float>(radius)) {
        continue;
      }
      const float t = 1.0f - dist / static_cast<float>(radius);
      const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(gain_w_) + static_cast<size_t>(x);
      if (tool_ == GainTool::kEraser) {
        gain_map_[idx] = auto_gain_map_[idx];
      } else {
        const float cur = gain_map_[idx];
        gain_map_[idx] = cur + (target - cur) * strength * t;
      }
    }
  }
  update();
  emit gainMapEdited();
}

void GainMapCanvas::smoothAt(const QPoint& pos) {
  if (gain_map_.empty()) {
    return;
  }
  const int radius = brush_radius_;
  std::vector<float> copy = gain_map_;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = pos.x() + dx;
      const int y = pos.y() + dy;
      if (x < 0 || y < 0 || x >= gain_w_ || y >= gain_h_) {
        continue;
      }
      float sum = 0.0f;
      int count = 0;
      for (int sy = -1; sy <= 1; ++sy) {
        for (int sx = -1; sx <= 1; ++sx) {
          sum += sample_gain(copy, gain_w_, gain_h_, x + sx, y + sy);
          ++count;
        }
      }
      const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(gain_w_) + static_cast<size_t>(x);
      gain_map_[idx] = sum / static_cast<float>(count);
    }
  }
  update();
  emit gainMapEdited();
}

void GainMapCanvas::beginStroke() {
  if (!stroke_before_.empty() || gain_map_.empty()) return;
  stroke_before_ = gain_map_;
}

void GainMapCanvas::endStroke() {
  if (stroke_before_.empty()) return;
  if (stroke_before_ != gain_map_) {
    undo_stack_.push(
        new GainMapEditCommand(this, std::move(stroke_before_), gain_map_));
  }
  stroke_before_.clear();
}

void GainMapCanvas::applyMapFromUndo(const std::vector<float>& map) {
  if (map.size() != gain_map_.size()) return;
  gain_map_ = map;
  update();
  emit gainMapEdited();
}

QImage GainMapCanvas::heatmapImage() const {
  if (gain_w_ <= 0 || gain_h_ <= 0 || gain_map_.empty()) {
    return {};
  }

  const float data_min = std::max(visualization_min_, 0.0001f);
  const float data_max = std::max(visualization_max_, data_min + 1e-4f);

  QImage img(gain_w_, gain_h_, QImage::Format_RGB32);
  const float log_min = std::log(data_min);
  const float log_max = std::log(data_max);
  const float log_span = std::max(log_max - log_min, 1e-4f);

  for (int y = 0; y < gain_h_; ++y) {
    auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < gain_w_; ++x) {
      const float g = gain_map_[static_cast<size_t>(y) * static_cast<size_t>(gain_w_) +
                                     static_cast<size_t>(x)];
      const float t = (std::log(std::clamp(g, data_min, data_max)) - log_min) / log_span;
      // Blue (low gain) → cyan → green → yellow → red (high gain)
      const int r = static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 255.0f);
      const int gch = static_cast<int>(std::clamp(1.0f - std::abs(t - 0.5f) * 2.0f, 0.0f, 1.0f) * 200.0f);
      const int b = static_cast<int>(std::clamp(1.0f - t, 0.0f, 1.0f) * 255.0f);
      line[x] = qRgb(r, gch, b);
    }
  }
  return img;
}

void GainMapCanvas::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(16, 16, 20));

  QImage heat = heatmapImage();
  if (!heat.isNull()) {
    const QRectF target = imageRect();
    p.drawImage(target, sdr_image_);
    p.setOpacity(0.76);
    p.drawImage(target, heat);
    p.setOpacity(1.0);
  } else if (!sdr_image_.isNull()) {
    p.drawImage(imageRect(), sdr_image_);
  }

  p.setPen(QColor(238, 240, 246));
  QString tool_name = tr("Brush");
  if (tool_ == GainTool::kEraser) tool_name = tr("Eraser");
  if (tool_ == GainTool::kSmooth) tool_name = tr("Smooth");
  float gmin = 1.f;
  float gmax = 1.f;
  if (!gain_map_.empty()) {
    gmin = gain_map_[0];
    gmax = gain_map_[0];
    for (float g : gain_map_) {
      gmin = std::min(gmin, g);
      gmax = std::max(gmax, g);
    }
  }
  p.drawText(16, 24, tr("%1  ·  %2×–%3×")
                          .arg(tool_name)
                          .arg(gmin, 0, 'f', 2)
                          .arg(gmax, 0, 'f', 1));

  const QRectF target = imageRect();
  if (target.contains(hover_pos_)) {
    const float radius_on_screen =
        brush_radius_ * target.width() / std::max(1, gain_w_);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 210), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(hover_pos_), radius_on_screen, radius_on_screen);
  }
}

void GainMapCanvas::mousePressEvent(QMouseEvent* event) {
  last_pointer_ = event->pos();
  hover_pos_ = event->pos();
  if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
    panning_ = true;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  painting_ = true;
  beginStroke();
  const QPoint g = mapToGain(event->pos());
  if (tool_ == GainTool::kSmooth) {
    smoothAt(g);
  } else {
    applyBrushAt(g, 1.0f);
  }
}

void GainMapCanvas::mouseMoveEvent(QMouseEvent* event) {
  hover_pos_ = event->pos();
  if (panning_) {
    pan_ += event->pos() - last_pointer_;
    last_pointer_ = event->pos();
    update();
    return;
  }
  if (!painting_) {
    update();
    return;
  }
  const QPoint g = mapToGain(event->pos());
  if (tool_ == GainTool::kSmooth) {
    smoothAt(g);
  } else {
    applyBrushAt(g, 1.0f);
  }
}

void GainMapCanvas::mouseReleaseEvent(QMouseEvent*) {
  if (panning_) {
    panning_ = false;
    unsetCursor();
  }
  if (painting_) endStroke();
  painting_ = false;
}

void GainMapCanvas::tabletEvent(QTabletEvent* event) {
  if (event->type() == QEvent::TabletPress) {
    painting_ = true;
    beginStroke();
  } else if (event->type() == QEvent::TabletRelease) {
    painting_ = false;
    endStroke();
    event->accept();
    return;
  }
  const QPoint g = mapToGain(event->position().toPoint());
  const float pressure = static_cast<float>(event->pressure());
  if (tool_ == GainTool::kSmooth) {
    smoothAt(g);
  } else {
    applyBrushAt(g, pressure);
  }
  event->accept();
}

void GainMapCanvas::wheelEvent(QWheelEvent* event) {
  const float factor = event->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
  const float old_zoom = zoom_;
  const QPointF before = (event->position() - QPointF(width() * 0.5, height() * 0.5) - pan_) /
                         std::max(old_zoom, 0.0001f);
  setZoom(zoom_ * factor);
  pan_ -= before * (zoom_ - old_zoom);
  event->accept();
}

float GainMapCanvas::gainValueAt(int gx, int gy) const {
  if (gain_map_.empty() || gain_w_ <= 0 || gain_h_ <= 0) {
    return 1.0f;
  }
  gx = std::clamp(gx, 0, gain_w_ - 1);
  gy = std::clamp(gy, 0, gain_h_ - 1);
  return gain_map_[static_cast<size_t>(gy) * static_cast<size_t>(gain_w_) + static_cast<size_t>(gx)];
}

void GainMapCanvas::strokeAtGainCoords(int gx, int gy, float pressure) {
  beginStroke();
  applyBrushAt(QPoint(gx, gy), pressure);
  endStroke();
}

void GainMapCanvas::smoothAtGainCoords(int gx, int gy) {
  beginStroke();
  smoothAt(QPoint(gx, gy));
  endStroke();
}

QImage GainMapCanvas::renderHeatmap() const {
  return heatmapImage();
}

}  // namespace uhdr_repack

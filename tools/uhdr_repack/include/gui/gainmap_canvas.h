#pragma once

#include "gainmap_edit.h"

#include <QImage>
#include <QObject>
#include <QUndoStack>
#include <QWidget>

#include <memory>
#include <vector>

namespace uhdr_repack {

class GainMapEditCommand;

class GainMapCanvas : public QWidget {
  Q_OBJECT

 public:
  explicit GainMapCanvas(QWidget* parent = nullptr);

  void setBaseSdr(const QImage& sdr);
  void setAutoGainMap(const std::vector<float>& gain, int width, int height);
  std::vector<float> gainMapData() const;
  int gainMapWidth() const { return gain_w_; }
  int gainMapHeight() const { return gain_h_; }

  void setTool(GainTool tool);
  void setBrushRadius(int px);
  void setBrushOpacity(float opacity);
  void setContentBoost(float min_boost, float max_boost);
  void setZoom(float zoom);
  float zoom() const { return zoom_; }
  void fitToView();

  QUndoStack* undoStack() { return &undo_stack_; }

  /** Test hooks (used by --self-test). Coordinates are gain-map pixel space. */
  float gainValueAt(int gx, int gy) const;
  void strokeAtGainCoords(int gx, int gy, float pressure = 1.0f);
  void smoothAtGainCoords(int gx, int gy);
  QImage renderHeatmap() const;

 signals:
  void gainMapEdited();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void tabletEvent(QTabletEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  void applyBrushAt(const QPoint& pos, float pressure);
  void smoothAt(const QPoint& pos);
  QPoint mapToGain(const QPoint& widget_pos) const;
  QRectF imageRect() const;
  QImage heatmapImage() const;
  void beginStroke();
  void endStroke();
  void applyMapFromUndo(const std::vector<float>& map);

  QImage sdr_image_;
  std::vector<float> gain_map_;
  std::vector<float> auto_gain_map_;
  int gain_w_ = 0;
  int gain_h_ = 0;

  GainTool tool_ = GainTool::kBrush;
  int brush_radius_ = 24;
  float brush_opacity_ = 0.5f;
  float min_boost_ = 1.0f;
  float max_boost_ = 1000.0f;
  bool painting_ = false;
  bool panning_ = false;
  QPoint last_pointer_;
  QPointF pan_;
  float zoom_ = 1.0f;
  QPoint hover_pos_;
  std::vector<float> stroke_before_;
  float visualization_min_ = 1.0f;
  float visualization_max_ = 2.0f;
  QUndoStack undo_stack_;

  friend class GainMapEditCommand;
};

}  // namespace uhdr_repack

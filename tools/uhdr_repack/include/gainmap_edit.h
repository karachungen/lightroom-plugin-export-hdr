#pragma once

#include <QImage>

#include <array>
#include <vector>

namespace uhdr_repack {

enum class GainTool { kBrush, kEraser, kSmooth };

/** Widget-free gain map editing used by web bridge and self-test. */
class GainMapEditor {
 public:
  std::vector<float> gain_map;
  std::vector<float> auto_gain_map;
  int width = 0;
  int height = 0;

  GainTool tool = GainTool::kBrush;
  int brush_radius = 24;
  float brush_opacity = 0.5f;
  float min_boost = 1.0f;
  float max_boost = 1000.0f;
  float visualization_min = 1.0f;
  float visualization_max = 2.0f;

  void setAutoGainMap(const std::vector<float>& gain, int w, int h);
  void setContentBoost(float min_boost, float max_boost);
  void setTool(GainTool tool);
  void setBrushRadius(int px);
  void setBrushOpacity(float opacity);

  void applyBrushAt(int gx, int gy, float pressure = 1.0f);
  void smoothAt(int gx, int gy);

  /** Normalized image coordinates in [0,1] with optional pressure. */
  void applyNormalizedStroke(const std::vector<std::array<float, 3>>& points);

  float valueAt(int gx, int gy) const;
  QImage renderHeatmap() const;

  void pushUndo();
  bool undo();
  bool redo();
  void clearUndoHistory();

 private:
  std::vector<std::vector<float>> undo_stack_;
  std::vector<std::vector<float>> redo_stack_;
};

}  // namespace uhdr_repack

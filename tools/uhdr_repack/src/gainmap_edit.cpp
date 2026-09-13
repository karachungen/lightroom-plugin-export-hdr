#include "gainmap_edit.h"

#include <algorithm>
#include <cmath>

namespace uhdr_repack {

namespace {

float sample_gain(const std::vector<float>& map, int w, int h, int x, int y) {
  x = std::clamp(x, 0, w - 1);
  y = std::clamp(y, 0, h - 1);
  return map[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
}

}  // namespace

void GainMapEditor::setAutoGainMap(const std::vector<float>& gain, int w, int h) {
  width = w;
  height = h;
  gain_map = gain;
  auto_gain_map = gain;
  clearUndoHistory();
  if (!gain.empty()) {
    const auto minmax = std::minmax_element(gain.begin(), gain.end());
    visualization_min = std::max(0.0001f, *minmax.first);
    visualization_max = std::max(visualization_min + 0.0001f, *minmax.second);
  }
}

void GainMapEditor::setContentBoost(float min_b, float max_b) {
  min_boost = min_b;
  max_boost = max_b;
}

void GainMapEditor::setTool(GainTool t) {
  tool = t;
}

void GainMapEditor::setBrushRadius(int px) {
  brush_radius = std::max(1, px);
}

void GainMapEditor::setBrushOpacity(float opacity) {
  brush_opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void GainMapEditor::applyBrushAt(int gx, int gy, float pressure) {
  if (gain_map.empty() || width <= 0 || height <= 0) {
    return;
  }
  const int radius = std::max(1, static_cast<int>(brush_radius * std::max(0.2f, pressure)));
  const float strength = brush_opacity * std::max(0.2f, pressure);
  const float target = max_boost;

  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = gx + dx;
      const int y = gy + dy;
      if (x < 0 || y < 0 || x >= width || y >= height) {
        continue;
      }
      const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (dist > static_cast<float>(radius)) {
        continue;
      }
      const float t = 1.0f - dist / static_cast<float>(radius);
      const size_t idx =
          static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
      if (tool == GainTool::kEraser) {
        gain_map[idx] = auto_gain_map[idx];
      } else {
        const float cur = gain_map[idx];
        gain_map[idx] = cur + (target - cur) * strength * t;
      }
    }
  }
}

void GainMapEditor::smoothAt(int gx, int gy) {
  if (gain_map.empty()) {
    return;
  }
  const int radius = brush_radius;
  std::vector<float> copy = gain_map;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = gx + dx;
      const int y = gy + dy;
      if (x < 0 || y < 0 || x >= width || y >= height) {
        continue;
      }
      float sum = 0.0f;
      int count = 0;
      for (int sy = -1; sy <= 1; ++sy) {
        for (int sx = -1; sx <= 1; ++sx) {
          sum += sample_gain(copy, width, height, x + sx, y + sy);
          ++count;
        }
      }
      const size_t idx =
          static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
      gain_map[idx] = sum / static_cast<float>(count);
    }
  }
}

void GainMapEditor::applyNormalizedStroke(const std::vector<std::array<float, 3>>& points) {
  if (points.empty()) {
    return;
  }
  pushUndo();
  for (const auto& pt : points) {
    const int gx = static_cast<int>(std::clamp(pt[0], 0.0f, 0.9999f) * width);
    const int gy = static_cast<int>(std::clamp(pt[1], 0.0f, 0.9999f) * height);
    const float pressure = pt[2] > 0.0f ? pt[2] : 1.0f;
    if (tool == GainTool::kSmooth) {
      smoothAt(gx, gy);
    } else {
      applyBrushAt(gx, gy, pressure);
    }
  }
}

float GainMapEditor::valueAt(int gx, int gy) const {
  if (gain_map.empty() || width <= 0 || height <= 0) {
    return 1.0f;
  }
  gx = std::clamp(gx, 0, width - 1);
  gy = std::clamp(gy, 0, height - 1);
  return gain_map[static_cast<size_t>(gy) * static_cast<size_t>(width) + static_cast<size_t>(gx)];
}

QImage GainMapEditor::renderHeatmap() const {
  if (width <= 0 || height <= 0 || gain_map.empty()) {
    return {};
  }

  const float data_min = std::max(visualization_min, 0.0001f);
  const float data_max = std::max(visualization_max, data_min + 1e-4f);

  QImage img(width, height, QImage::Format_RGB32);
  const float log_min = std::log(data_min);
  const float log_max = std::log(data_max);
  const float log_span = std::max(log_max - log_min, 1e-4f);

  for (int y = 0; y < height; ++y) {
    auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < width; ++x) {
      const float g =
          gain_map[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
      const float t = (std::log(std::clamp(g, data_min, data_max)) - log_min) / log_span;
      const int r = static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 255.0f);
      const int gch =
          static_cast<int>(std::clamp(1.0f - std::abs(t - 0.5f) * 2.0f, 0.0f, 1.0f) * 200.0f);
      const int b = static_cast<int>(std::clamp(1.0f - t, 0.0f, 1.0f) * 255.0f);
      line[x] = qRgb(r, gch, b);
    }
  }
  return img;
}

void GainMapEditor::pushUndo() {
  if (gain_map.empty()) {
    return;
  }
  undo_stack_.push_back(gain_map);
  redo_stack_.clear();
  if (undo_stack_.size() > 64) {
    undo_stack_.erase(undo_stack_.begin());
  }
}

bool GainMapEditor::undo() {
  if (undo_stack_.empty() || gain_map.empty()) {
    return false;
  }
  redo_stack_.push_back(gain_map);
  gain_map = undo_stack_.back();
  undo_stack_.pop_back();
  return true;
}

bool GainMapEditor::redo() {
  if (redo_stack_.empty()) {
    return false;
  }
  undo_stack_.push_back(gain_map);
  gain_map = redo_stack_.back();
  redo_stack_.pop_back();
  return true;
}

void GainMapEditor::clearUndoHistory() {
  undo_stack_.clear();
  redo_stack_.clear();
}

}  // namespace uhdr_repack

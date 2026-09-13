#pragma once

#include "encode_engine.h"
#include "gui/gainmap_canvas.h"

#include <QDockWidget>
#include <QLabel>
#include <QSlider>

namespace uhdr_repack {

class QuickEditDock : public QDockWidget {
  Q_OBJECT

 public:
  explicit QuickEditDock(GainMapCanvas* canvas, QWidget* parent = nullptr);
  void setEncodeOptions(const EncodeOptions& options);
  void setGainStats(float min_gain, float max_gain, int width, int height);
  void setDisplayStatus(const QString& status, bool hdr_active);
  void setItemDetails(const QString& source, const QString& output, int base_quality,
                      int gain_quality);

 signals:
  void displayPeakChanged(float nits);
  void contentBoostChanged(float min_boost, float max_boost);
  void toolChanged(GainTool tool);

 private:
  GainMapCanvas* canvas_ = nullptr;
  QSlider* boost_slider_ = nullptr;
  QSlider* peak_slider_ = nullptr;
  QSlider* radius_slider_ = nullptr;
  QSlider* opacity_slider_ = nullptr;
  QLabel* boost_value_ = nullptr;
  QLabel* peak_value_ = nullptr;
  QLabel* radius_value_ = nullptr;
  QLabel* opacity_value_ = nullptr;
  QLabel* gain_stats_ = nullptr;
  QLabel* display_status_ = nullptr;
  QLabel* output_info_ = nullptr;
  QLabel* paths_info_ = nullptr;
  bool syncing_ = false;
};

}  // namespace uhdr_repack

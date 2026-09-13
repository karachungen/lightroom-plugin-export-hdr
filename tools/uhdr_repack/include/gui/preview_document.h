#pragma once

#include "gui/ultrahdr_preview_service.h"
#include "session.h"

#include <QImage>
#include <QObject>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace uhdr_repack {

struct PreviewItemState {
  QImage sdr;
  std::vector<float> gain;
  int gain_width = 0;
  int gain_height = 0;
  float gain_min = 1.0f;
  float gain_max = 1.0f;
  DecodedHdrFrame final_hdr;
  bool loading = false;
  bool loaded = false;
  bool modified = false;
  /** True when the user (or session gainmap_in) replaced libultrahdr’s native map. */
  bool gainmap_override = false;
  bool final_dirty = true;
  QString error;
};

/** Session-backed source of truth for preview state and asynchronous work. */
class PreviewDocument final : public QObject {
  Q_OBJECT

 public:
  explicit PreviewDocument(PreviewSession session, QObject* parent = nullptr);
  ~PreviewDocument() override;

  int itemCount() const;
  const PreviewSession& session() const;
  PreviewSession& mutableSession();
  const SessionItem& item(int index) const;
  const PreviewItemState& state(int index) const;

  void requestItem(int index);
  void requestGainMap(int index);
  void requestFinalPreview(int index);
  void setHdrTiff(int index, const std::string& path);
  void updateGainMap(int index, const std::vector<float>& gain, int width, int height,
                     bool as_override = false);
  bool saveGainMap(int index, QString* error = nullptr);
  void setItemSkipped(int index, bool skipped);
  void setEncodeOptions(int index, const EncodeOptions& options);
  void setItemInstagram(int index, SliceAspect aspect, float crop_offset, unsigned slice_count,
                        unsigned preview_slice_index, unsigned output_width = 0,
                        unsigned output_height = 0);
  /** Copy current item aspect and requested output size onto every other queue item. */
  int copyFeedCropToOthers(int from_index);
  /** Inspector-wide encode settings applied to the whole queue. */
  void setSharedEncodeOptions(const EncodeOptions& options);
  void setDestDir(const std::string& path);

 signals:
  void itemLoading(int index);
  void itemReady(int index);
  void itemFailed(int index, const QString& message);
  void finalPreviewReady(int index);
  void finalPreviewFailed(int index, const QString& message);
  void itemChanged(int index);

 private:
  struct LoadResult;
  struct FinalResult;
  class Private;
  std::unique_ptr<Private> d_;
};

}  // namespace uhdr_repack

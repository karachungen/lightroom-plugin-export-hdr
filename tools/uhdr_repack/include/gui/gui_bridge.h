#pragma once

#include "gainmap_edit.h"
#include "gui/hdr_rhi_viewport.h"
#include "gui/hdr_tiff_client.h"
#include "gui/preview_document.h"
#include "session.h"

#include <QObject>
#include <QRect>
#include <QString>
#include <QVector>
#include <vector>

class QTimer;

namespace uhdr_repack {

class WebChrome;

/** Translates web JSON messages to PreviewDocument / gain editor / HDR viewport. */
class GuiBridge : public QObject {
  Q_OBJECT

 public:
  GuiBridge(PreviewDocument* document, HdrRhiViewport* viewport, WebChrome* chrome,
              QObject* parent = nullptr);
  ~GuiBridge() override;

  void sendSession();
  void sendQueueMeta(int synced_count = -1);
  void sendItemReady(int index);
  void sendSlices(int index);
  void sendDisplayStatus(const HdrViewportStatus& status);
  void setBusy(bool busy, const QString& message = {});
  void sendDestDir();
  void setDestDir(const QString& path);
  void ensureHdrTiffs(const std::vector<int>& indices, HdrTiffClient::Finished finished);

  bool userApproved() const { return approved_; }
  void markApproved() { approved_ = true; }

  GainMapEditor* editorForIndex(int index);

 public slots:
  void handleMessage(const QString& json);
  void onItemLoading(int index);
  void onItemReady(int index);
  void onItemFailed(int index, const QString& message);
  void onFinalPreviewReady(int index);
  void onFinalPreviewFailed(int index, const QString& message);
  void onViewportStatusChanged(const HdrViewportStatus& status);

 signals:
  void applyRequested();
  void cancelRequested();
  void chooseDestRequested();
  void previewOverlayChanged(const QRect& rect, bool visible);
  void sliceGuidesChanged(const QVector<QRect>& rects);

 private:
  void handleSelectItem(const QString& id);
  void handleSetSettingsJson(const QString& json_text);
  void handleSyncToOthers();
  void emitOverlay();
  void emitSliceGuides();
  void applyLiveSettings(bool send_slices, bool send_heatmap);
  void sendHeatmap(int index);
  void sendHdrLoading(bool loading, bool ready = false, const QString& phase = {});
  void sendItemLoading(int index);
  void syncGainToViewport(int index);
  void saveCurrentGain(int index);
  void scheduleFinalPreview();
  void ensureHdrThenPreview(int index);
  void preloadAllHdrTiffs();
  void preferHdrTiff(int index);
  bool hasEncodedHdr() const;
  bool needsEncodedPreview() const;
  void syncGainVisualization();
  PreviewMode parsePreviewMode(const std::string& mode) const;
  int indexForId(const QString& id) const;

  PreviewDocument* document_ = nullptr;
  HdrRhiViewport* viewport_ = nullptr;
  WebChrome* chrome_ = nullptr;
  HdrTiffClient* hdr_client_ = nullptr;
  QTimer* final_preview_timer_ = nullptr;
  int current_index_ = 0;
  PreviewMode preview_mode_ = PreviewMode::kSdr;
  bool approved_ = false;
  SliceAspect slice_aspect_ = SliceAspect::kNone;
  QRect preview_rect_;

  std::vector<GainMapEditor> editors_;
};

}  // namespace uhdr_repack

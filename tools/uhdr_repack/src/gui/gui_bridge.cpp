#include "gui/gui_bridge.h"

#include "activity_log.h"
#include "gui/hdr_tiff_client.h"
#include "gui/sdr_preview_image.h"
#include "gui/web_chrome.h"
#include "slice_plan.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QRect>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace uhdr_repack {

namespace {

using json = nlohmann::json;

QString imageToDataUrl(const QImage& image, const char* format = "PNG") {
  if (image.isNull()) {
    return {};
  }
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, format, 80);
  const QString mime = QStringLiteral("data:image/%1;base64,")
                           .arg(QLatin1String(format) == QLatin1String("JPEG") ? "jpeg" : "png");
  return mime + QString::fromLatin1(bytes.toBase64());
}

QString jpegBytesToDataUrl(const std::vector<uint8_t>& jpeg) {
  if (jpeg.empty()) {
    return {};
  }
  const QByteArray bytes(reinterpret_cast<const char*>(jpeg.data()), static_cast<int>(jpeg.size()));
  return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

QString thumbDataUrl(const QImage& image) {
  if (image.isNull()) {
    return {};
  }
  return imageToDataUrl(image.scaled(192, 144, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                        "JPEG");
}

struct SdrFileProbe {
  qint64 bytes = 0;
  int width = 0;
  int height = 0;
};

SdrFileProbe probeSdrFile(const std::string& path) {
  SdrFileProbe info;
  const QString qpath = QString::fromStdString(path);
  info.bytes = QFileInfo(qpath).size();
  int width = 0;
  int height = 0;
  std::string error;
  if (sdr_preview_size(path, &width, &height, &error)) {
    info.width = width;
    info.height = height;
  }
  return info;
}

SliceAspect parseSliceAspectString(const std::string& s) {
  SliceAspect aspect = SliceAspect::kNone;
  parse_slice_aspect(s, &aspect);
  return aspect;
}

json itemQueueMetaJson(PreviewDocument* document, int index) {
  json j;
  if (!document || index < 0 || index >= document->itemCount()) return j;
  const SessionItem& item = document->item(index);
  const PreviewItemState& st = document->state(index);
  j["id"] = item.id;
  const SliceAspect aspect = effective_slice_aspect(document->session(), item);
  j["sliceAspect"] = slice_aspect_label(aspect);
  unsigned master_w = 0;
  unsigned master_h = 0;
  if (st.loaded && st.sdr.width() >= 2 && st.sdr.height() >= 2) {
    master_w = static_cast<unsigned>(st.sdr.width());
    master_h = static_cast<unsigned>(st.sdr.height());
  } else {
    const SdrFileProbe probe = probeSdrFile(item.sdr);
    master_w = probe.width > 0 ? static_cast<unsigned>(probe.width) : 0;
    master_h = probe.height > 0 ? static_cast<unsigned>(probe.height) : 0;
  }
  unsigned ow = 0;
  unsigned oh = 0;
  if (master_w >= 2 && master_h >= 2 &&
      resolve_item_export_size(master_w, master_h, aspect, item.output_width, item.output_height,
                               &ow, &oh)) {
    j["outputWidth"] = ow;
    j["outputHeight"] = oh;
  } else {
    j["outputWidth"] = 0;
    j["outputHeight"] = 0;
  }
  return j;
}

void downsampleGain(const std::vector<float>& src, int width, int height, int scale,
                    std::vector<float>* out, int* out_w, int* out_h) {
  if (!out || !out_w || !out_h || src.empty() || width <= 0 || height <= 0) {
    return;
  }
  const int step = std::max(1, scale);
  *out_w = std::max(1, width / step);
  *out_h = std::max(1, height / step);
  out->assign(static_cast<size_t>(*out_w * *out_h), 1.0f);
  for (int y = 0; y < *out_h; ++y) {
    for (int x = 0; x < *out_w; ++x) {
      const int sx = std::min(width - 1, x * step + step / 2);
      const int sy = std::min(height - 1, y * step + step / 2);
      (*out)[static_cast<size_t>(y * *out_w + x)] =
          src[static_cast<size_t>(sy * width + sx)];
    }
  }
}

bool encode_inspector_equal(const EncodeOptions& a, const EncodeOptions& b) {
  return a.base_quality == b.base_quality && a.gainmap_quality == b.gainmap_quality &&
         a.gainmap_scale == b.gainmap_scale && a.min_content_boost == b.min_content_boost &&
         a.max_content_boost == b.max_content_boost &&
         a.target_display_peak_nits == b.target_display_peak_nits &&
         a.monochrome_gainmap == b.monochrome_gainmap;
}

QRect mapCropToLocal(const CropRect& crop, int img_w, int img_h, const QRect& image_local) {
  if (img_w < 1 || img_h < 1) return {};
  const int x = image_local.x() + static_cast<int>(std::lround(
                                      static_cast<double>(crop.x) / img_w * image_local.width()));
  const int y = image_local.y() + static_cast<int>(std::lround(
                                      static_cast<double>(crop.y) / img_h * image_local.height()));
  const int w =
      static_cast<int>(std::lround(static_cast<double>(crop.w) / img_w * image_local.width()));
  const int h =
      static_cast<int>(std::lround(static_cast<double>(crop.h) / img_h * image_local.height()));
  return QRect(x, y, std::max(1, w), std::max(1, h));
}

}  // namespace

GuiBridge::GuiBridge(PreviewDocument* document, HdrRhiViewport* viewport, WebChrome* chrome,
                     QObject* parent)
    : QObject(parent), document_(document), viewport_(viewport), chrome_(chrome) {
  if (document_) {
    editors_.assign(static_cast<size_t>(document_->itemCount()), GainMapEditor{});
    slice_aspect_ = document_->session().default_slice_aspect;
  }
  hdr_client_ = new HdrTiffClient(document_, this);
  if (document_) {
    activity_log_configure_session(document_->session());
    activity_log_append("", "session", "editor " + activity_log_primary_path());
  }
  final_preview_timer_ = new QTimer(this);
  final_preview_timer_->setSingleShot(true);
  final_preview_timer_->setInterval(400);
  connect(final_preview_timer_, &QTimer::timeout, this, [this]() { scheduleFinalPreview(); });
}

GuiBridge::~GuiBridge() { activity_log_set_hook({}); }

GainMapEditor* GuiBridge::editorForIndex(int index) {
  if (index < 0 || index >= static_cast<int>(editors_.size())) {
    return nullptr;
  }
  return &editors_[static_cast<size_t>(index)];
}

int GuiBridge::indexForId(const QString& id) const {
  if (!document_) return -1;
  for (int i = 0; i < document_->itemCount(); ++i) {
    if (QString::fromStdString(document_->item(i).id) == id) {
      return i;
    }
  }
  return -1;
}

void GuiBridge::sendSession() {
  if (!chrome_ || !document_) return;
  json root;
  root["type"] = "session";
  root["items"] = json::array();
  std::string dest = document_->session().dest_dir;
  if (dest.empty()) {
    dest = inferred_dest_dir(document_->session());
    if (!dest.empty()) {
      document_->setDestDir(dest);
    }
  }
  root["destDir"] = dest;
  for (int i = 0; i < document_->itemCount(); ++i) {
    const auto& item = document_->item(i);
    json j;
    j["id"] = item.id;
    j["label"] = item.label;
    j["skipped"] = item.skipped;
    const SdrFileProbe probe = probeSdrFile(item.sdr);
    j["bytes"] = probe.bytes;
    j["imageWidth"] = probe.width;
    j["imageHeight"] = probe.height;
    json meta = itemQueueMetaJson(document_, i);
    j["sliceAspect"] = meta.value("sliceAspect", "none");
    j["outputWidth"] = meta.value("outputWidth", 0);
    j["outputHeight"] = meta.value("outputHeight", 0);
    root["items"].push_back(std::move(j));
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::sendQueueMeta(int synced_count) {
  if (!chrome_ || !document_) return;
  json root;
  root["type"] = "queueMeta";
  root["items"] = json::array();
  if (synced_count >= 0) {
    root["syncedCount"] = synced_count;
  }
  for (int i = 0; i < document_->itemCount(); ++i) {
    root["items"].push_back(itemQueueMetaJson(document_, i));
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::sendDestDir() {
  if (!chrome_ || !document_) return;
  json root;
  root["type"] = "destDir";
  root["path"] = document_->session().dest_dir;
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::sendItemLoading(int index) {
  if (!chrome_ || !document_ || index < 0 || index >= document_->itemCount()) return;
  const auto& item = document_->item(index);
  const SdrFileProbe probe = probeSdrFile(item.sdr);
  json root;
  root["type"] = "itemLoading";
  root["id"] = item.id;
  root["label"] = item.label;
  root["bytes"] = probe.bytes;
  root["width"] = probe.width;
  root["height"] = probe.height;
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::setDestDir(const QString& path) {
  if (!document_) return;
  document_->setDestDir(path.trimmed().toStdString());
  sendDestDir();
}

void GuiBridge::sendItemReady(int index) {
  if (!chrome_ || !document_ || index < 0 || index >= document_->itemCount()) return;
  const auto& item = document_->item(index);
  const auto& st = document_->state(index);
  if (!st.loaded) return;

  auto* editor = editorForIndex(index);
  const EncodeOptions opt = effective_encode_options(document_->session(), item);
  if (editor && editor->gain_map.empty() && !st.gain.empty()) {
    editor->setAutoGainMap(st.gain, st.gain_width, st.gain_height);
  }
  if (editor) {
    editor->setRgbGainMap(st.gain_rgb);
    editor->setContentBoost(opt.min_content_boost, opt.max_content_boost);
  }

  json root;
  root["type"] = "itemReady";
  root["id"] = item.id;
  root["sdrDataUrl"] = imageToDataUrl(st.sdr, "JPEG").toStdString();
  root["thumb"] = thumbDataUrl(st.sdr).toStdString();
  root["imageWidth"] = st.sdr.width();
  root["imageHeight"] = st.sdr.height();
  json stats;
  stats["min"] = st.gain_min;
  stats["max"] = st.gain_max;
  stats["w"] = st.gain_width;
  stats["h"] = st.gain_height;
  root["gainStats"] = stats;
  root["sliceAspect"] = slice_aspect_label(effective_slice_aspect(document_->session(), item));
  root["hasSliceOverride"] = item.has_slice_override;
  root["cropOffset"] = effective_crop_offset(item);
  root["sliceCount"] = static_cast<int>(item.slice_count);
  root["previewSlice"] = static_cast<int>(item.preview_slice_index < 1 ? 1 : item.preview_slice_index);
  root["outputWidth"] = static_cast<int>(item.output_width);
  root["outputHeight"] = static_cast<int>(item.output_height);
  if (editor && !editor->gain_map.empty()) {
    const bool encoded = !st.final_dirty && !st.final_hdr.gainmap_jpeg.empty();
    if (encoded) {
      const QString heat = jpegBytesToDataUrl(st.final_hdr.gainmap_jpeg);
      if (!heat.isEmpty()) root["heatmapDataUrl"] = heat.toStdString();
    }
    root["encoded"] = encoded;
    root["gainmapChannels"] = opt.monochrome_gainmap ? "luma" : "rgb";
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
  sendSlices(index);
}

void GuiBridge::sendSlices(int index) {
  if (!chrome_ || !document_ || index < 0 || index >= document_->itemCount()) return;
  const auto& st = document_->state(index);
  const auto& item = document_->item(index);
  const SliceAspect aspect = effective_slice_aspect(document_->session(), item);
  const float offset = effective_crop_offset(item);
  const unsigned slice_count = effective_slice_count(item);
  if (!st.loaded || aspect == SliceAspect::kNone) {
    json root;
    root["type"] = "slices";
    root["id"] = item.id;
    root["rects"] = json::array();
    root["error"] = "";
    root["sliceCount"] = 1;
    root["maxSliceCount"] = 0;
    unsigned ow = 0;
    unsigned oh = 0;
    if (st.loaded && st.sdr.width() >= 2 && st.sdr.height() >= 2 &&
        clamp_encode_output_size(aspect, static_cast<unsigned>(st.sdr.width()),
                                 static_cast<unsigned>(st.sdr.height()), item.output_width,
                                 item.output_height, &ow, &oh)) {
      root["outputWidth"] = ow;
      root["outputHeight"] = oh;
    }
    chrome_->postToPage(QString::fromStdString(root.dump()));
    emitSliceGuides();
    return;
  }

  std::vector<CropRect> slices;
  std::string err;
  const unsigned max_count =
      max_slice_count(static_cast<unsigned>(st.sdr.width()), static_cast<unsigned>(st.sdr.height()),
                      aspect);
  compute_slices(static_cast<unsigned>(st.sdr.width()), static_cast<unsigned>(st.sdr.height()),
                 aspect, &slices, &err, offset, slice_count);

  json root;
  root["type"] = "slices";
  root["id"] = item.id;
  root["rects"] = json::array();
  root["error"] = slices.empty() ? err : "";
  root["cropOffset"] = offset;
  root["sliceCount"] = static_cast<int>(slices.empty() ? 1 : slices.size());
  root["maxSliceCount"] = static_cast<int>(max_count);
  root["previewSlice"] = static_cast<int>(item.preview_slice_index < 1 ? 1 : item.preview_slice_index);
  unsigned ow = 0;
  unsigned oh = 0;
  unsigned crop_w = st.sdr.width() > 0 ? static_cast<unsigned>(st.sdr.width()) : 0;
  unsigned crop_h = st.sdr.height() > 0 ? static_cast<unsigned>(st.sdr.height()) : 0;
  if (!slices.empty()) {
    crop_w = slices[0].w;
    crop_h = slices[0].h;
  }
  if (crop_w >= 2 && crop_h >= 2 &&
      clamp_encode_output_size(aspect, crop_w, crop_h, item.output_width, item.output_height, &ow,
                               &oh)) {
    root["outputWidth"] = ow;
    root["outputHeight"] = oh;
  }
  for (const auto& crop : slices) {
    json r;
    r["x"] = static_cast<float>(crop.x) / std::max(1, st.sdr.width());
    r["y"] = static_cast<float>(crop.y) / std::max(1, st.sdr.height());
    r["w"] = static_cast<float>(crop.w) / std::max(1, st.sdr.width());
    r["h"] = static_cast<float>(crop.h) / std::max(1, st.sdr.height());
    root["rects"].push_back(std::move(r));
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
  emitSliceGuides();
}

void GuiBridge::sendHeatmap(int index) {
  if (!chrome_ || !document_ || index < 0 || index >= document_->itemCount()) return;
  const auto& st = document_->state(index);
  const EncodeOptions opt = effective_encode_options(document_->session(), document_->item(index));
  const bool encoded = !st.final_dirty && !st.final_hdr.gainmap_jpeg.empty();

  json root;
  root["type"] = "heatmap";
  root["id"] = document_->item(index).id;
  json stats;
  stats["min"] = st.gain_min;
  stats["max"] = st.gain_max;
  stats["w"] = st.gain_width;
  stats["h"] = st.gain_height;
  root["gainStats"] = stats;
  root["encoded"] = encoded;
  root["gainmapChannels"] = opt.monochrome_gainmap ? "luma" : "rgb";
  if (encoded) {
    const QString data_url = jpegBytesToDataUrl(st.final_hdr.gainmap_jpeg);
    if (!data_url.isEmpty()) root["heatmapDataUrl"] = data_url.toStdString();
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::sendDisplayStatus(const HdrViewportStatus& status) {
  if (!chrome_) return;
  json root;
  root["type"] = "displayStatus";
  root["text"] = (status.backend + " · " + status.swapchain_format).toStdString();
  root["hdrActive"] = status.hdr_active;
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::sendHdrLoading(bool loading, bool ready, const QString& phase) {
  if (!chrome_) return;
  json root;
  root["type"] = "hdrLoading";
  root["loading"] = loading;
  root["ready"] = ready;
  if (!phase.isEmpty()) root["phase"] = phase.toStdString();
  if (document_ && current_index_ >= 0 && current_index_ < document_->itemCount()) {
    const auto& st = document_->state(current_index_);
    const auto& frame = st.final_hdr;
    if (!st.final_dirty && frame.width > 0 && frame.height > 0) {
      root["encodedWidth"] = frame.width;
      root["encodedHeight"] = frame.height;
      if (frame.jpeg_bytes > 0) root["encodedBytes"] = frame.jpeg_bytes;
    }
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
  if (loading) {
    json status;
    status["type"] = "status";
    QString text;
    if (phase == QLatin1String("wait_tiff") || phase == QLatin1String("render")) {
      text = tr("HDR TIFF · waiting on Lightroom");
    } else if (phase == QLatin1String("gainmap")) {
      text = tr("Gain map · computing");
    } else if (phase == QLatin1String("decode")) {
      text = tr("Preview decode");
    } else {
      text = tr("Preview encode");
    }
    status["text"] = text.toStdString();
    chrome_->postToPage(QString::fromStdString(status.dump()));
  }
}

bool GuiBridge::hasEncodedHdr() const {
  if (!document_ || current_index_ < 0) return false;
  const auto& st = document_->state(current_index_);
  return st.loaded && !st.final_dirty && !st.final_hdr.rgba_half.empty();
}

bool GuiBridge::needsEncodedPreview() const {
  return preview_mode_ == PreviewMode::kFinalHdr || preview_mode_ == PreviewMode::kGainMap;
}

void GuiBridge::syncGainVisualization() {
  if (!document_ || current_index_ < 0 || current_index_ >= document_->itemCount()) return;
  const auto& st = document_->state(current_index_);
  if (!st.loaded) return;
  if (viewport_) {
    viewport_->setGainVisualizationRange(st.gain_min, st.gain_max);
  }
}

PreviewMode GuiBridge::parsePreviewMode(const std::string& mode) const {
  if (mode == "sdr") return PreviewMode::kSdr;
  if (mode == "gain") return PreviewMode::kGainMap;
  if (mode == "hdr" || mode == "live" || mode == "final") return PreviewMode::kFinalHdr;
  return PreviewMode::kSdr;
}

void GuiBridge::setBusy(bool busy, const QString& message) {
  if (!chrome_) return;
  json root;
  root["type"] = "busy";
  root["busy"] = busy;
  if (!message.isEmpty()) {
    json status;
    status["type"] = "status";
    status["text"] = message.toStdString();
    chrome_->postToPage(QString::fromStdString(status.dump()));
  }
  chrome_->postToPage(QString::fromStdString(root.dump()));
}

void GuiBridge::saveCurrentGain(int index) {
  auto* editor = editorForIndex(index);
  if (!editor || !document_) return;
  const bool edited = !editor->auto_gain_map.empty() && editor->gain_map != editor->auto_gain_map;
  document_->updateGainMap(index, editor->gain_map, editor->width, editor->height, edited);
  document_->saveGainMap(index, nullptr);
}

void GuiBridge::syncGainToViewport(int index) {
  auto* editor = editorForIndex(index);
  if (!editor || !viewport_) return;
  const int scale = document_ ? std::max(1, document_->session().default_encode_options.gainmap_scale)
                              : 1;
  std::vector<float> scaled;
  int sw = editor->width;
  int sh = editor->height;
  downsampleGain(editor->gain_map, editor->width, editor->height, scale, &scaled, &sw, &sh);
  viewport_->setGainMap(scaled, sw, sh);
}

void GuiBridge::applyLiveSettings(bool send_slices, bool send_heatmap) {
  if (!document_) return;
  const EncodeOptions& opt = document_->session().default_encode_options;
  if (current_index_ >= 0) {
    auto* editor = editorForIndex(current_index_);
    if (editor) {
      editor->setContentBoost(opt.min_content_boost, opt.max_content_boost);
      if (send_heatmap && preview_mode_ == PreviewMode::kGainMap) sendHeatmap(current_index_);
    }
  }
  if (viewport_) {
    viewport_->setContentBoost(opt.min_content_boost, opt.max_content_boost);
    viewport_->setTargetDisplayPeak(opt.target_display_peak_nits);
    syncGainVisualization();
    if (current_index_ >= 0) {
      syncGainToViewport(current_index_);
    }
  }
  if (send_slices && current_index_ >= 0) {
    sendSlices(current_index_);
  }
}

void GuiBridge::emitSliceGuides() {
  if (preview_mode_ != PreviewMode::kFinalHdr) {
    emit sliceGuidesChanged({}, false, 1, 0, 0.5f);
    return;
  }
  QVector<QRect> local;
  bool axis_x = true;
  int slack_px = 0;
  sliceGuideLayout(&local, &axis_x, &slack_px);
  float offset = 0.5f;
  if (document_ && current_index_ >= 0) {
    offset = effective_crop_offset(document_->item(current_index_));
  }
  emit sliceGuidesChanged(local, slack_px >= 2, axis_x ? 1 : 0, slack_px, offset);
}

void GuiBridge::ensureHdrTiffs(const std::vector<int>& indices, HdrTiffClient::Finished finished) {
  if (!hdr_client_) {
    if (finished) finished(false, tr("HDR client is missing"));
    return;
  }
  if (!indices.empty()) preferHdrTiff(indices[0]);
  hdr_client_->ensure(indices, std::move(finished));
}

void GuiBridge::preferHdrTiff(int index) {
  if (!hdr_client_ || !document_ || index < 0 || index >= document_->itemCount()) return;
  hdr_client_->setPriority(document_->item(index).id);
}

void GuiBridge::scheduleFinalPreview() {
  if (!needsEncodedPreview() || current_index_ < 0 || !document_) return;
  ensureHdrThenPreview(current_index_);
}

void GuiBridge::ensureHdrThenPreview(int index) {
  if (!needsEncodedPreview() || index != current_index_ || !document_) return;
  const std::string id = document_->item(index).id;
  const auto& st = document_->state(index);
  if (!st.loaded) {
    activity_log_append(id, "encode", "waiting for SDR (hdr)");
    return;
  }
  if (hasEncodedHdr()) {
    activity_log_append(id, "encode",
                      preview_mode_ == PreviewMode::kGainMap ? "preview cache hit (gain)"
                                                             : "preview cache hit (hdr)");
    const auto& frame = document_->state(index).final_hdr;
    if (viewport_ && preview_mode_ == PreviewMode::kFinalHdr) {
      viewport_->setFinalHdr(frame.rgba_half, frame.width, frame.height, frame.color_gamut);
      viewport_->setPreviewMode(PreviewMode::kFinalHdr);
    }
    if (preview_mode_ == PreviewMode::kGainMap) {
      sendHeatmap(index);
    }
    sendHdrLoading(false, true);
    emitOverlay();
    emitSliceGuides();
    return;
  }
  if (document_->item(index).hdr_tiff.empty()) {
    sendHdrLoading(true, false, QStringLiteral("wait_tiff"));
    emitOverlay();
    emitSliceGuides();
    const int requested = index;
    ensureHdrTiffs({index}, [this, requested](bool ok, const QString& error) {
      if (requested != current_index_ || !needsEncodedPreview()) return;
      if (!ok) {
        activity_log_append(document_->item(requested).id, "wait_tiff",
                            "pipeline fail " + error.toStdString());
        onFinalPreviewFailed(requested, error);
        return;
      }
      ensureHdrThenPreview(requested);
    });
    return;
  }
  sendHdrLoading(true, false, QStringLiteral("encode"));
  emitOverlay();
  emitSliceGuides();
  document_->requestFinalPreview(index);
}

void GuiBridge::ensureGainHeatmap(int index) {
  if (preview_mode_ != PreviewMode::kGainMap || index != current_index_ || !document_) return;
  scheduleFinalPreview();
}

void GuiBridge::handleSyncToOthers() {
  if (!document_ || current_index_ < 0 || current_index_ >= document_->itemCount()) return;
  document_->setSharedEncodeOptions(document_->session().default_encode_options);
  const int copied = document_->copyFeedCropToOthers(current_index_);
  sendQueueMeta(copied);
}

void GuiBridge::handleSelectItem(const QString& id) {
  const int index = indexForId(id);
  if (index < 0) return;
  if (current_index_ >= 0) {
    saveCurrentGain(current_index_);
  }
  current_index_ = index;
  preferHdrTiff(index);
  document_->requestItem(index);
}

void GuiBridge::handleSetSettingsJson(const QString& json_text) {
  if (!document_) return;
  json msg;
  try {
    msg = json::parse(json_text.toStdString());
  } catch (...) {
    return;
  }
  PreviewSession& session = document_->mutableSession();
  EncodeOptions next = session.default_encode_options;
  try {
    if (msg.contains("baseQuality") && msg["baseQuality"].is_number()) {
      next.base_quality = static_cast<int>(msg["baseQuality"].get<double>());
    }
    if (msg.contains("gainmapQuality") && msg["gainmapQuality"].is_number()) {
      next.gainmap_quality = static_cast<int>(msg["gainmapQuality"].get<double>());
    }
    if (msg.contains("gainmapScale") && msg["gainmapScale"].is_number()) {
      next.gainmap_scale = static_cast<int>(msg["gainmapScale"].get<double>());
    }
    if (msg.contains("minBoost") && msg["minBoost"].is_number()) {
      next.min_content_boost = static_cast<float>(msg["minBoost"].get<double>());
    }
    if (msg.contains("maxBoost") && msg["maxBoost"].is_number()) {
      next.max_content_boost = static_cast<float>(msg["maxBoost"].get<double>());
    }
    if (next.min_content_boost > next.max_content_boost) {
      std::swap(next.min_content_boost, next.max_content_boost);
    }
    if (msg.contains("displayPeak") && msg["displayPeak"].is_number()) {
      next.target_display_peak_nits = static_cast<float>(msg["displayPeak"].get<double>());
    }
    if (msg.contains("monochromeGainmap") && msg["monochromeGainmap"].is_boolean()) {
      next.monochrome_gainmap = msg["monochromeGainmap"].get<bool>();
    }
  } catch (...) {
    return;
  }
  const bool encode_changed = !encode_inspector_equal(session.default_encode_options, next);
  if (encode_changed) {
    document_->setSharedEncodeOptions(next);
  }

  bool instagram_changed = false;
  bool slice_geometry_changed = false;
  if (current_index_ >= 0) {
    const SessionItem& item = document_->item(current_index_);
    SliceAspect aspect = effective_slice_aspect(session, item);
    float offset = effective_crop_offset(item);
    unsigned slice_count = effective_slice_count(item);
    unsigned preview_slice = item.preview_slice_index < 1 ? 1u : item.preview_slice_index;
    unsigned output_width = item.output_width;
    unsigned output_height = item.output_height;
    try {
      if (msg.contains("sliceAspect") && msg["sliceAspect"].is_string()) {
        aspect = parseSliceAspectString(msg["sliceAspect"].get<std::string>());
      }
      if (msg.contains("cropOffset") && msg["cropOffset"].is_number()) {
        offset = static_cast<float>(msg["cropOffset"].get<double>());
      }
      if (msg.contains("sliceCount") && msg["sliceCount"].is_number()) {
        const int n = static_cast<int>(msg["sliceCount"].get<double>());
        slice_count = n < 0 ? 0u : static_cast<unsigned>(n);
      }
      if (msg.contains("previewSlice") && msg["previewSlice"].is_number()) {
        const int idx = static_cast<int>(msg["previewSlice"].get<double>());
        preview_slice = idx < 1 ? 1u : static_cast<unsigned>(idx);
      }
      if (msg.contains("outputWidth") && msg["outputWidth"].is_number()) {
        const int w = static_cast<int>(msg["outputWidth"].get<double>());
        output_width = w < 0 ? 0u : static_cast<unsigned>(w);
      }
      if (msg.contains("outputHeight") && msg["outputHeight"].is_number()) {
        const int h = static_cast<int>(msg["outputHeight"].get<double>());
        output_height = h < 0 ? 0u : static_cast<unsigned>(h);
      }
    } catch (...) {
      return;
    }
    const SliceAspect cur_aspect = effective_slice_aspect(session, document_->item(current_index_));
    const float cur_offset = effective_crop_offset(document_->item(current_index_));
    const unsigned cur_count = effective_slice_count(document_->item(current_index_));
    const unsigned cur_preview =
        document_->item(current_index_).preview_slice_index < 1
            ? 1u
            : document_->item(current_index_).preview_slice_index;
    const unsigned cur_ow = document_->item(current_index_).output_width;
    const unsigned cur_oh = document_->item(current_index_).output_height;
    slice_geometry_changed =
        offset != cur_offset || slice_count != cur_count || preview_slice != cur_preview;
    instagram_changed = aspect != cur_aspect || slice_geometry_changed || output_width != cur_ow ||
                        output_height != cur_oh;
    if (instagram_changed) {
      document_->setItemInstagram(current_index_, aspect, offset, slice_count, preview_slice,
                                  output_width, output_height);
      slice_aspect_ = aspect;
      sendQueueMeta();
    }
  }

  applyLiveSettings(instagram_changed, false);
  if (instagram_changed) {
    emitOverlay();
    emitSliceGuides();
  }
  if (needsEncodedPreview() && encode_changed) {
    emitOverlay();
    emitSliceGuides();
    final_preview_timer_->start();
  }
}

void GuiBridge::emitOverlay() {
  const QRect hole = selectedHdrHole();
  const bool visible = preview_mode_ == PreviewMode::kFinalHdr && hasEncodedHdr() &&
                       hole.width() >= 8 && hole.height() >= 8;
  emit previewOverlayChanged(preview_rect_, hole, visible);
}

QRect GuiBridge::letterboxedImageRect(QRect* local) const {
  if (preview_rect_.width() < 8 || preview_rect_.height() < 8 || !document_ || current_index_ < 0) {
    return {};
  }
  const QImage& sdr = document_->state(current_index_).sdr;
  if (sdr.isNull() || sdr.width() < 2 || sdr.height() < 2) return {};
  const double scale = std::min(preview_rect_.width() / static_cast<double>(sdr.width()),
                                preview_rect_.height() / static_cast<double>(sdr.height()));
  const int dw = std::max(1, static_cast<int>(std::lround(sdr.width() * scale)));
  const int dh = std::max(1, static_cast<int>(std::lround(sdr.height() * scale)));
  const int ox = (preview_rect_.width() - dw) / 2;
  const int oy = (preview_rect_.height() - dh) / 2;
  if (local) *local = QRect(ox, oy, dw, dh);
  return QRect(preview_rect_.x() + ox, preview_rect_.y() + oy, dw, dh);
}

QRect GuiBridge::selectedHdrHole() const {
  return letterboxedImageRect();
}

void GuiBridge::sliceGuideLayout(QVector<QRect>* local, bool* axis_x, int* slack_px) const {
  if (local) local->clear();
  if (axis_x) *axis_x = true;
  if (slack_px) *slack_px = 0;
  QRect image_local;
  if (!letterboxedImageRect(&image_local).isValid() || !document_ || current_index_ < 0) return;
  const auto& item = document_->item(current_index_);
  const auto& st = document_->state(current_index_);
  const SliceAspect aspect = effective_slice_aspect(document_->session(), item);
  if (aspect == SliceAspect::kNone || st.sdr.isNull()) return;
  std::vector<CropRect> slices;
  std::string err;
  compute_slices(static_cast<unsigned>(st.sdr.width()), static_cast<unsigned>(st.sdr.height()),
                 aspect, &slices, &err, effective_crop_offset(item), effective_slice_count(item));
  if (slices.empty()) return;
  if (local) {
    for (const auto& crop : slices) {
      local->push_back(mapCropToLocal(crop, st.sdr.width(), st.sdr.height(), image_local));
    }
  }
  const bool horizontal = slices[0].h + 1 >= static_cast<unsigned>(st.sdr.height());
  if (axis_x) *axis_x = horizontal;
  const unsigned count = static_cast<unsigned>(slices.size());
  int slack = 0;
  if (horizontal) {
    slack = st.sdr.width() - static_cast<int>(count * slices[0].w);
  } else {
    slack = st.sdr.height() - static_cast<int>(count * slices[0].h);
  }
  if (slack < 0) slack = 0;
  if (slack_px) {
    if (horizontal) {
      *slack_px = static_cast<int>(
          std::lround(static_cast<double>(slack) / st.sdr.width() * image_local.width()));
    } else {
      *slack_px = static_cast<int>(
          std::lround(static_cast<double>(slack) / st.sdr.height() * image_local.height()));
    }
  }
}

void GuiBridge::onCropDragStarted() {
  crop_dragging_ = true;
  emitOverlay();
}

void GuiBridge::onCropOffsetChanged(float offset) {
  if (!document_ || current_index_ < 0) return;
  document_->setLiveCropOffset(current_index_, offset);
  sendSlices(current_index_);
  emitOverlay();
}

void GuiBridge::onCropDragFinished(float offset) {
  if (document_ && current_index_ >= 0) {
    document_->setLiveCropOffset(current_index_, offset);
  }
  crop_dragging_ = false;
  emitOverlay();
  emitSliceGuides();
}

void GuiBridge::handleMessage(const QString& json_text) {
  json msg;
  try {
    msg = json::parse(json_text.toStdString());
  } catch (...) {
    return;
  }
  const std::string type = msg.value("type", "");
  if (type == "ready") {
    sendSession();
    const auto& opt = document_->session().default_encode_options;
    json settings;
    settings["type"] = "settings";
    settings["baseQuality"] = opt.base_quality;
    settings["gainmapQuality"] = opt.gainmap_quality;
    settings["gainmapScale"] = opt.gainmap_scale;
    settings["minBoost"] = opt.min_content_boost;
    settings["maxBoost"] = opt.max_content_boost;
    settings["displayPeak"] = opt.target_display_peak_nits;
    settings["monochromeGainmap"] = opt.monochrome_gainmap;
    SliceAspect aspect = document_->session().default_slice_aspect;
    float crop_offset = 0.5f;
    unsigned slice_count = 1;
    unsigned preview_slice = 1;
    if (document_->itemCount() > 0) {
      const auto& first = document_->item(0);
      aspect = effective_slice_aspect(document_->session(), first);
      crop_offset = effective_crop_offset(first);
      slice_count = effective_slice_count(first);
      preview_slice = first.preview_slice_index < 1 ? 1u : first.preview_slice_index;
    }
    settings["sliceAspect"] = slice_aspect_label(aspect);
    settings["hasSliceOverride"] = document_->itemCount() > 0 && document_->item(0).has_slice_override;
    settings["cropOffset"] = crop_offset;
    settings["sliceCount"] = static_cast<int>(slice_count);
    settings["previewSlice"] = static_cast<int>(preview_slice);
    if (document_->itemCount() > 0) {
      settings["outputWidth"] = static_cast<int>(document_->item(0).output_width);
      settings["outputHeight"] = static_cast<int>(document_->item(0).output_height);
    }
    chrome_->postToPage(QString::fromStdString(settings.dump()));
    sendDestDir();
    if (document_->itemCount() > 0) {
      current_index_ = 0;
      document_->requestItem(0);
    }
    return;
  }
  if (type == "selectItem") {
    handleSelectItem(QString::fromStdString(msg.value("id", "")));
    return;
  }
  if (type == "setSettings") {
    handleSetSettingsJson(json_text);
    return;
  }
  if (type == "syncToOthers") {
    handleSyncToOthers();
    return;
  }
  if (type == "setMode") {
    preview_mode_ = parsePreviewMode(msg.value("mode", "sdr"));
    if (viewport_) {
      viewport_->setPreviewMode(preview_mode_ == PreviewMode::kFinalHdr ? PreviewMode::kFinalHdr
                                                                        : preview_mode_);
    }
    applyLiveSettings(false, preview_mode_ == PreviewMode::kGainMap);
    emitOverlay();
    emitSliceGuides();
    if (preview_mode_ == PreviewMode::kGainMap && current_index_ >= 0) {
      ensureGainHeatmap(current_index_);
    } else if (needsEncodedPreview() && current_index_ >= 0) {
      scheduleFinalPreview();
    } else {
      sendHdrLoading(false, false);
    }
    return;
  }
  if (type == "previewRect") {
    const auto num = [](const json& j, const char* key) {
      if (!j.contains(key) || !j[key].is_number()) return 0;
      return static_cast<int>(j[key].get<double>());
    };
    preview_rect_ = QRect(num(msg, "x"), num(msg, "y"), num(msg, "w"), num(msg, "h"));
    const std::string mode = msg.value("mode", "");
    if (!mode.empty()) {
      preview_mode_ = parsePreviewMode(mode);
      if (viewport_ && preview_mode_ != PreviewMode::kFinalHdr) {
        viewport_->setPreviewMode(preview_mode_);
      }
    }
    emitOverlay();
    emitSliceGuides();
    return;
  }
  if (type == "setSkipped") {
    const int index = indexForId(QString::fromStdString(msg.value("id", "")));
    if (index >= 0) {
      document_->setItemSkipped(index, msg.value("skipped", false));
    }
    return;
  }
  if (type == "applyAll") {
    if (current_index_ >= 0) saveCurrentGain(current_index_);
    emit applyRequested();
    return;
  }
  if (type == "chooseDest") {
    emit chooseDestRequested();
    return;
  }
  if (type == "setDestDir") {
    setDestDir(QString::fromStdString(msg.value("path", "")));
    return;
  }
  if (type == "cancel") {
    emit cancelRequested();
  }
}

void GuiBridge::onItemLoading(int index) {
  if (index == current_index_) {
    sendItemLoading(index);
  }
}

void GuiBridge::onItemReady(int index) {
  if (index == current_index_) {
    sendItemReady(index);
    const auto& st = document_->state(index);
    if (viewport_) {
      viewport_->setSdrImage(st.sdr);
      if (preview_mode_ != PreviewMode::kFinalHdr) {
        viewport_->setPreviewMode(preview_mode_);
      }
    }
    applyLiveSettings(false, preview_mode_ == PreviewMode::kGainMap);
    if (preview_mode_ == PreviewMode::kGainMap) {
      ensureGainHeatmap(index);
    } else if (needsEncodedPreview()) {
      scheduleFinalPreview();
    } else {
      emitOverlay();
      emitSliceGuides();
    }
  }
}

void GuiBridge::onItemFailed(int index, const QString& message) {
  if (index == current_index_) {
    json root;
    root["type"] = "itemFailed";
    root["id"] = document_ ? document_->item(index).id : "";
    root["text"] = message.toStdString();
    if (chrome_) chrome_->postToPage(QString::fromStdString(root.dump()));
    setBusy(false, message);
  }
}

void GuiBridge::onFinalPreviewReady(int index) {
  if (index != current_index_) return;
  const auto& frame = document_->state(index).final_hdr;
  if (viewport_ && preview_mode_ == PreviewMode::kFinalHdr) {
    viewport_->setFinalHdr(frame.rgba_half, frame.width, frame.height, frame.color_gamut);
    viewport_->setPreviewMode(PreviewMode::kFinalHdr);
  }
  if (preview_mode_ == PreviewMode::kGainMap) sendHeatmap(index);
  sendHdrLoading(false, true);
  emitOverlay();
  emitSliceGuides();
  if (chrome_ && preview_mode_ == PreviewMode::kFinalHdr) {
    json status;
    status["type"] = "status";
    status["text"] = QStringLiteral("Encoded Ultra HDR").toStdString();
    chrome_->postToPage(QString::fromStdString(status.dump()));
  }
}

void GuiBridge::onFinalPreviewFailed(int index, const QString& message) {
  if (index != current_index_) return;
  sendHdrLoading(false, false);
  emitOverlay();
  emitSliceGuides();
  setBusy(false, message.isEmpty() ? tr("Encoded preview failed") : message);
}

void GuiBridge::onViewportStatusChanged(const HdrViewportStatus& status) {
  sendDisplayStatus(status);
}

}  // namespace uhdr_repack

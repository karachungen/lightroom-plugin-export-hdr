#include "gui/gui_bridge.h"

#include "activity_log.h"
#include "gui/hdr_tiff_client.h"
#include "gui/web_chrome.h"
#include "slice_plan.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QRect>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

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

QString thumbDataUrl(const QImage& image) {
  if (image.isNull()) {
    return {};
  }
  return imageToDataUrl(image.scaled(192, 144, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                        "JPEG");
}

QImage previewSized(const QImage& image, int max_edge = 1600) {
  if (image.isNull()) return {};
  if (image.width() <= max_edge && image.height() <= max_edge) return image;
  return image.scaled(max_edge, max_edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
  QImageReader reader(qpath);
  const QSize sz = reader.size();
  info.width = sz.width();
  info.height = sz.height();
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
  if (editor && editor->gain_map.empty()) {
    editor->setAutoGainMap(st.gain, st.gain_width, st.gain_height);
    EncodeOptions opt = effective_encode_options(document_->session(), item);
    editor->setContentBoost(opt.min_content_boost, opt.max_content_boost);
  }

  json root;
  root["type"] = "itemReady";
  root["id"] = item.id;
  root["sdrDataUrl"] = imageToDataUrl(previewSized(st.sdr), "JPEG").toStdString();
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
  if (editor) {
    const bool encoded = !st.final_dirty && !st.final_hdr.gainmap_jpeg.empty();
    root["heatmapDataUrl"] = imageToDataUrl(previewSized(editor->renderHeatmap(), 1024), "JPEG")
                                 .toStdString();
    root["encoded"] = encoded;
    root["gainmapChannels"] = encoded && st.final_hdr.gainmap_is_rgb ? "rgb" : "luma";
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
  json root;
  root["type"] = "heatmap";
  root["id"] = document_->item(index).id;
  const auto& st = document_->state(index);
  json stats;
  stats["min"] = st.gain_min;
  stats["max"] = st.gain_max;
  stats["w"] = st.gain_width;
  stats["h"] = st.gain_height;
  root["gainStats"] = stats;
  auto* editor = editorForIndex(index);
  if (!editor) return;
  const bool encoded = !st.final_dirty && !st.final_hdr.gainmap_jpeg.empty();
  root["heatmapDataUrl"] = imageToDataUrl(previewSized(editor->renderHeatmap(), 1024), "JPEG")
                               .toStdString();
  root["encoded"] = encoded;
  root["gainmapChannels"] = encoded && st.final_hdr.gainmap_is_rgb ? "rgb" : "luma";
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
  if (ready && document_ && current_index_ >= 0 && current_index_ < document_->itemCount()) {
    const auto& frame = document_->state(current_index_).final_hdr;
    if (frame.width > 0 && frame.height > 0) {
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
  QVector<QRect> out;
  emit sliceGuidesChanged(out);
}

void GuiBridge::ensureHdrTiffs(const std::vector<int>& indices, HdrTiffClient::Finished finished) {
  if (!hdr_client_) {
    if (finished) finished(false, tr("HDR client is missing"));
    return;
  }
  if (!indices.empty()) preferHdrTiff(indices[0]);
  hdr_client_->ensure(indices, std::move(finished));
}

void GuiBridge::preloadAllHdrTiffs() {
  if (!hdr_client_ || !document_) return;
  std::vector<int> all;
  all.reserve(static_cast<size_t>(document_->itemCount()));
  for (int i = 0; i < document_->itemCount(); ++i) all.push_back(i);
  hdr_client_->requestMissing(all);
  if (document_->itemCount() > 0) preferHdrTiff(0);
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
  const char* mode = preview_mode_ == PreviewMode::kGainMap ? "gain" : "hdr";
  const auto& st = document_->state(index);
  if (!st.loaded) {
    activity_log_append(id, "encode", std::string("waiting for SDR (") + mode + ")");
    return;
  }
  if (hasEncodedHdr()) {
    activity_log_append(id, "encode", std::string("preview cache hit (") + mode + ")");
    sendHdrLoading(false, preview_mode_ == PreviewMode::kFinalHdr);
    if (preview_mode_ == PreviewMode::kGainMap) sendHeatmap(current_index_);
    emitOverlay();
    emitSliceGuides();
    document_->requestFinalPreview(current_index_);
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
  if (preview_mode_ == PreviewMode::kGainMap && st.gain.empty()) {
    sendHdrLoading(true, false, QStringLiteral("gainmap"));
    emitOverlay();
    emitSliceGuides();
    document_->requestGainMap(index);
    return;
  }
  sendHdrLoading(true, false, QStringLiteral("encode"));
  emitOverlay();
  emitSliceGuides();
  document_->requestFinalPreview(index);
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
    instagram_changed = aspect != cur_aspect || offset != cur_offset || slice_count != cur_count ||
                        preview_slice != cur_preview || output_width != cur_ow ||
                        output_height != cur_oh;
    if (instagram_changed) {
      document_->setItemInstagram(current_index_, aspect, offset, slice_count, preview_slice,
                                  output_width, output_height);
      slice_aspect_ = aspect;
      sendQueueMeta();
    }
  }

  applyLiveSettings(instagram_changed, false);
  if (needsEncodedPreview() && (encode_changed || instagram_changed)) {
    emitOverlay();
    emitSliceGuides();
    final_preview_timer_->start();
  }
}

void GuiBridge::emitOverlay() {
  const bool visible = preview_mode_ == PreviewMode::kFinalHdr && hasEncodedHdr();
  emit previewOverlayChanged(preview_rect_, visible);
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
    preloadAllHdrTiffs();
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
    if (needsEncodedPreview() && current_index_ >= 0) {
      scheduleFinalPreview();
    } else {
      sendHdrLoading(false, false);
      emitOverlay();
      emitSliceGuides();
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
    if (needsEncodedPreview()) {
      scheduleFinalPreview();
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
  sendHeatmap(index);
  sendHdrLoading(false, preview_mode_ == PreviewMode::kFinalHdr);
  emitOverlay();
  emitSliceGuides();
  if (chrome_) {
    json status;
    status["type"] = "status";
    status["text"] = preview_mode_ == PreviewMode::kGainMap
                         ? (frame.gainmap_is_rgb ? "Encoded RGB gain map" : "Encoded luma gain map")
                         : "Encoded Ultra HDR";
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

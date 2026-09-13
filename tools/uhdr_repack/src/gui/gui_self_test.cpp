#include "gui_app.h"

#include "activity_log.h"
#include "color_primaries.h"
#include "encode_engine.h"
#include "gainmap_compute.h"
#include "gainmap_edit.h"
#include "gui/gainmap_canvas.h"
#include "gui/hdr_rhi_viewport.h"
#include "gui/preview_document.h"
#include "gui/hdr_tiff_client.h"
#include "gui/ultrahdr_preview_service.h"
#include "half_float.h"
#include "hdr_bridge.h"
#include "session.h"
#include "slice_plan.h"
#include "tiff_input.h"
#include "verify.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QImageReader>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace uhdr_repack {

namespace {

int fail(const std::string& msg) {
  std::cerr << "SELF_TEST_FAIL: " << msg << "\n";
  return 1;
}

bool gainmap_stats(const std::vector<float>& gain, float* min_out, float* max_out,
                   float* spread_out) {
  if (gain.empty()) {
    return false;
  }
  float mn = gain[0];
  float mx = gain[0];
  for (float g : gain) {
    mn = std::min(mn, g);
    mx = std::max(mx, g);
  }
  *min_out = mn;
  *max_out = mx;
  *spread_out = mx - mn;
  return true;
}

bool test_session_item_gainmap(const SessionItem& item, int* checks) {
  if (!fs::exists(item.sdr) || !fs::exists(item.hdr_tiff)) {
    std::cout << "SKIP gainmap (missing files): " << item.id << "\n";
    return true;
  }

  std::vector<float> gain;
  int w = 0;
  int h = 0;
  std::string err;
  if (!compute_auto_gainmap(item.sdr, item.hdr_tiff, &gain, &w, &h, &err)) {
    std::cerr << item.id << ": " << err << "\n";
    return false;
  }

  float mn = 0;
  float mx = 0;
  float spread = 0;
  gainmap_stats(gain, &mn, &mx, &spread);
  std::cout << "OK gainmap " << item.id << " " << w << "x" << h << " range [" << mn << ", " << mx
            << "] spread=" << spread << "\n";
  if (spread < 0.05f) {
    return false;
  }
  (*checks)++;

  QImage sdr(QString::fromStdString(item.sdr));
  if (sdr.isNull()) {
    return false;
  }

  GainMapCanvas canvas;
  canvas.resize(640, 480);
  canvas.setBaseSdr(sdr);
  canvas.setAutoGainMap(gain, w, h);
  canvas.setBrushRadius(32);
  canvas.setBrushOpacity(0.8f);
  canvas.setContentBoost(1.0f, 1000.0f);

  const int cx = w / 2;
  const int cy = h / 2;
  const float before = canvas.gainValueAt(cx, cy);
  canvas.setTool(GainTool::kBrush);
  canvas.strokeAtGainCoords(cx, cy, 1.0f);
  const float after_brush = canvas.gainValueAt(cx, cy);
  if (after_brush <= before + 0.01f) {
    std::cerr << "brush did not increase gain at center (" << before << " -> " << after_brush
              << ")\n";
    return false;
  }
  std::cout << "OK brush " << item.id << " " << before << " -> " << after_brush << "\n";
  (*checks)++;

  canvas.undoStack()->undo();
  if (std::abs(canvas.gainValueAt(cx, cy) - before) > 0.5f) {
    std::cerr << "undo did not restore pre-stroke gain\n";
    return false;
  }
  canvas.undoStack()->redo();
  if (std::abs(canvas.gainValueAt(cx, cy) - after_brush) > 0.5f) {
    std::cerr << "redo did not restore brush stroke\n";
    return false;
  }
  std::cout << "OK undo/redo " << item.id << "\n";
  (*checks)++;

  canvas.setTool(GainTool::kEraser);
  canvas.strokeAtGainCoords(cx, cy, 1.0f);
  const float after_eraser = canvas.gainValueAt(cx, cy);
  if (std::abs(after_eraser - before) > 0.5f) {
    std::cerr << "eraser did not restore auto gain (" << before << " vs " << after_eraser << ")\n";
    return false;
  }
  std::cout << "OK eraser " << item.id << "\n";
  (*checks)++;

  canvas.setTool(GainTool::kBrush);
  canvas.strokeAtGainCoords(cx, cy, 1.0f);
  const float brushed = canvas.gainValueAt(cx, cy);
  canvas.setTool(GainTool::kSmooth);
  canvas.smoothAtGainCoords(cx, cy);
  const float smoothed = canvas.gainValueAt(cx, cy);
  if (std::abs(smoothed - brushed) < 1e-4f) {
    std::cerr << "smooth had no effect\n";
    return false;
  }
  std::cout << "OK smooth " << item.id << "\n";
  (*checks)++;

  QImage heat = canvas.renderHeatmap();
  if (heat.isNull() || heat.width() != w || heat.height() != h) {
    std::cerr << "heatmap render size mismatch\n";
    return false;
  }
  GainMapEditor scaled;
  scaled.setAutoGainMap(gain, w, h);
  const float data_max = scaled.visualization_max;
  scaled.setContentBoost(1.0f, 1000.0f);
  if (scaled.visualization_max > data_max + 1.0f) {
    std::cerr << "heatmap scale followed inspector max boost instead of data range\n";
    return false;
  }
  QImage editor_heat = scaled.renderHeatmap();
  int hot = 0;
  for (int y = 0; y < editor_heat.height(); y += std::max(1, editor_heat.height() / 32)) {
    for (int x = 0; x < editor_heat.width(); x += std::max(1, editor_heat.width() / 32)) {
      const QRgb px = editor_heat.pixel(x, y);
      if (qRed(px) > 80) ++hot;
    }
  }
  if (hot < 1) {
    std::cerr << "data-range heatmap is uniformly blue\n";
    return false;
  }
  std::cout << "OK heatmap render " << item.id << "\n";
  (*checks)++;

  HdrRhiViewport preview;
  preview.setSdrImage(sdr);
  preview.setGainMap(gain, w, h);
  preview.setTargetDisplayPeak(1000.0f);
  preview.setPreviewMode(PreviewMode::kLiveHdr);
  QImage composite = preview.renderSdrFallback();
  if (composite.isNull()) {
    return false;
  }
  const QRgb sdr_px = sdr.pixel(cx, cy);
  const QRgb hdr_px = composite.pixel(cx, cy);
  const int sdr_luma = qRed(sdr_px) + qGreen(sdr_px) + qBlue(sdr_px);
  const int hdr_luma = qRed(hdr_px) + qGreen(hdr_px) + qBlue(hdr_px);
  if (gain[cx + cy * w] > 1.5f && hdr_luma <= sdr_luma) {
    std::cerr << "HDR composite not brighter at high-gain pixel\n";
    return false;
  }
  std::cout << "OK hdr composite " << item.id << " luma " << sdr_luma << " -> " << hdr_luma << "\n";
  (*checks)++;

  return true;
}

bool wait_for_document_item(PreviewDocument* document, int index, QString* error) {
  QEventLoop loop;
  bool ready = false;
  QObject::connect(document, &PreviewDocument::itemReady, &loop, [&](int loaded) {
    if (loaded == index) {
      ready = true;
      loop.quit();
    }
  });
  QObject::connect(document, &PreviewDocument::itemFailed, &loop,
                   [&](int failed, const QString& message) {
                     if (failed == index) {
                       if (error) *error = message;
                       loop.quit();
                     }
                   });
  QTimer::singleShot(30000, &loop, &QEventLoop::quit);
  document->requestItem(index);
  loop.exec();
  if (!ready && error && error->isEmpty()) *error = QStringLiteral("document load timed out");
  return ready;
}

bool wait_for_document_gain(PreviewDocument* document, int index, QString* error) {
  if (!document->state(index).gain.empty()) return true;
  QEventLoop loop;
  bool ready = false;
  QObject::connect(document, &PreviewDocument::itemReady, &loop, [&](int loaded) {
    if (loaded == index && !document->state(index).gain.empty()) {
      ready = true;
      loop.quit();
    }
  });
  QObject::connect(document, &PreviewDocument::itemFailed, &loop,
                   [&](int failed, const QString& message) {
                     if (failed == index) {
                       if (error) *error = message;
                       loop.quit();
                     }
                   });
  QTimer::singleShot(30000, &loop, &QEventLoop::quit);
  document->requestGainMap(index);
  loop.exec();
  if (!ready && error && error->isEmpty()) *error = QStringLiteral("gain-map load timed out");
  return ready;
}

bool test_sdr_load_without_tiff(const PreviewSession& source, int* checks) {
  int source_index = -1;
  for (int i = 0; i < static_cast<int>(source.items.size()); ++i) {
    if (fs::exists(source.items[static_cast<size_t>(i)].sdr)) {
      source_index = i;
      break;
    }
  }
  if (source_index < 0) return false;

  PreviewSession session = source;
  session.items = {source.items[static_cast<size_t>(source_index)]};
  session.items[0].hdr_tiff.clear();
  session.items[0].gainmap_in.clear();
  session.work_dir = (fs::path(source.work_dir) / "sdr_only_test").string();
  fs::create_directories(session.work_dir);

  PreviewDocument document(session);
  QString error;
  if (!wait_for_document_item(&document, 0, &error)) {
    std::cerr << "sdr-only load: " << error.toStdString() << "\n";
    return false;
  }
  if (document.state(0).sdr.isNull() || !document.state(0).gain.empty()) {
    std::cerr << "sdr-only load should skip gain-map compute\n";
    return false;
  }
  std::cout << "OK SDR load without HDR TIFF\n";
  (*checks)++;
  return true;
}

bool test_hdr_bridge_protocol(int* checks) {
  const fs::path root = fs::temp_directory_path() / "uhdr_bridge_test";
  fs::remove_all(root);
  fs::create_directories(root);
  const std::string work_dir = root.u8string();
  std::string error;
  if (!write_hdr_bridge_enabled(work_dir, &error) || !hdr_bridge_enabled(work_dir)) {
    std::cerr << "bridge.json: " << error << "\n";
    return false;
  }
  if (!write_hdr_request(work_dir, "photo_1", &error)) {
    std::cerr << "write request: " << error << "\n";
    return false;
  }
  if (!fs::exists(fs::u8path(hdr_request_path(work_dir, "photo_1")))) {
    std::cerr << "request file missing\n";
    return false;
  }
  const fs::path tiff = root / "fake.tif";
  {
    std::ofstream out(tiff, std::ios::binary);
    out << "II";
  }
  HdrRequestResult done;
  done.ok = true;
  done.hdr_tiff = tiff.u8string();
  if (!write_hdr_done(work_dir, "photo_1", done, &error) ||
      !hdr_done_has_valid_tiff(work_dir, "photo_1")) {
    std::cerr << "done file: " << error << "\n";
    return false;
  }
  if (!write_hdr_priority(work_dir, "photo_1", &error) ||
      !fs::exists(fs::u8path(hdr_priority_path(work_dir)))) {
    std::cerr << "priority file: " << error << "\n";
    return false;
  }
  std::cout << "OK hdr bridge protocol\n";
  (*checks)++;
  fs::remove_all(root);
  return true;
}

bool test_hdr_tiff_client(const PreviewSession& source, int* checks) {
  int source_index = -1;
  for (int i = 0; i < static_cast<int>(source.items.size()); ++i) {
    if (fs::exists(source.items[static_cast<size_t>(i)].sdr)) {
      source_index = i;
      break;
    }
  }
  if (source_index < 0) return false;

  PreviewSession session = source;
  session.items = {source.items[static_cast<size_t>(source_index)]};
  const std::string original_tiff = session.items[0].hdr_tiff;
  session.items[0].hdr_tiff.clear();
  session.work_dir = (fs::path(source.work_dir) / "tiff_client_test").string();
  fs::create_directories(session.work_dir);
  fs::create_directories(fs::u8path(hdr_requests_dir(session.work_dir)));
  std::string error;
  if (!write_hdr_bridge_enabled(session.work_dir, &error)) return false;

  PreviewDocument document(session);
  HdrTiffClient client(&document);
  QEventLoop loop;
  bool ok = false;
  QString message;
  QTimer::singleShot(80, [&]() {
    HdrRequestResult done;
    done.ok = true;
    done.hdr_tiff = original_tiff.empty() ? session.items[0].sdr : original_tiff;
    write_hdr_done(session.work_dir, session.items[0].id, done, nullptr);
  });
  client.ensure({0}, [&](bool success, const QString& err) {
    ok = success;
    message = err;
    loop.quit();
  });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  if (!ok || document.item(0).hdr_tiff.empty()) {
    std::cerr << "hdr tiff client: " << message.toStdString() << "\n";
    return false;
  }
  std::cout << "OK hdr tiff client\n";
  (*checks)++;
  return true;
}

bool test_hdr_tiff_client_overlapping(const PreviewSession& source, int* checks) {
  int source_index = -1;
  for (int i = 0; i < static_cast<int>(source.items.size()); ++i) {
    if (fs::exists(source.items[static_cast<size_t>(i)].sdr)) {
      source_index = i;
      break;
    }
  }
  if (source_index < 0) return false;

  const SessionItem& src = source.items[static_cast<size_t>(source_index)];
  PreviewSession session = source;
  session.items = {src, src};
  session.items[0].id = src.id + "_a";
  session.items[1].id = src.id + "_b";
  session.items[0].hdr_tiff.clear();
  session.items[1].hdr_tiff.clear();
  session.work_dir = (fs::path(source.work_dir) / "tiff_client_overlap").string();
  fs::create_directories(session.work_dir);
  fs::create_directories(fs::u8path(hdr_requests_dir(session.work_dir)));
  std::string error;
  if (!write_hdr_bridge_enabled(session.work_dir, &error)) return false;

  PreviewDocument document(session);
  HdrTiffClient client(&document);
  client.requestMissing({0, 1});
  if (!fs::exists(fs::u8path(hdr_request_path(session.work_dir, session.items[0].id))) ||
      !fs::exists(fs::u8path(hdr_request_path(session.work_dir, session.items[1].id)))) {
    std::cerr << "overlap: missing preload requests\n";
    return false;
  }

  QEventLoop loop;
  bool ok = false;
  QString message;
  QTimer::singleShot(80, [&]() {
    HdrRequestResult done;
    done.ok = true;
    done.hdr_tiff = src.hdr_tiff.empty() ? src.sdr : src.hdr_tiff;
    write_hdr_done(session.work_dir, session.items[1].id, done, nullptr);
  });
  client.ensure({1}, [&](bool success, const QString& err) {
    ok = success;
    message = err;
    loop.quit();
  });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  if (!ok || document.item(1).hdr_tiff.empty()) {
    std::cerr << "overlap ensure item 1: " << message.toStdString() << "\n";
    return false;
  }
  if (!document.item(0).hdr_tiff.empty()) {
    std::cerr << "overlap: item 0 should still be pending\n";
    return false;
  }

  ok = false;
  message.clear();
  QEventLoop loop0;
  QTimer::singleShot(80, [&]() {
    HdrRequestResult done;
    done.ok = true;
    done.hdr_tiff = src.hdr_tiff.empty() ? src.sdr : src.hdr_tiff;
    write_hdr_done(session.work_dir, session.items[0].id, done, nullptr);
  });
  client.ensure({0}, [&](bool success, const QString& err) {
    ok = success;
    message = err;
    loop0.quit();
  });
  QTimer::singleShot(5000, &loop0, &QEventLoop::quit);
  loop0.exec();
  if (!ok || document.item(0).hdr_tiff.empty()) {
    std::cerr << "overlap ensure item 0: " << message.toStdString() << "\n";
    return false;
  }
  std::cout << "OK hdr tiff client overlapping\n";
  (*checks)++;
  return true;
}

bool test_document_persistence(const PreviewSession& source, int* checks) {
  int source_index = -1;
  for (int i = 0; i < static_cast<int>(source.items.size()); ++i) {
    if (fs::exists(source.items[static_cast<size_t>(i)].sdr) &&
        fs::exists(source.items[static_cast<size_t>(i)].hdr_tiff)) {
      source_index = i;
      break;
    }
  }
  if (source_index < 0) return false;

  PreviewSession session = source;
  session.items = {source.items[static_cast<size_t>(source_index)]};
  session.work_dir = (fs::path(source.work_dir) / "document_test").string();
  session.items[0].gainmap_in.clear();
  fs::create_directories(session.work_dir);

  PreviewDocument first(session);
  QString error;
  if (!wait_for_document_item(&first, 0, &error)) {
    std::cerr << "document load: " << error.toStdString() << "\n";
    return false;
  }
  if (!wait_for_document_gain(&first, 0, &error)) {
    std::cerr << "document gain: " << error.toStdString() << "\n";
    return false;
  }
  if (first.state(0).gainmap_override || !first.item(0).gainmap_in.empty()) {
    std::cerr << "auto-loaded gain map was treated as an encode override\n";
    return false;
  }
  std::cout << "OK native encode path (no auto gainmap bake)\n";
  (*checks)++;
  auto edited = first.state(0).gain;
  const size_t center = static_cast<size_t>(first.state(0).gain_height / 2) *
                            first.state(0).gain_width +
                        first.state(0).gain_width / 2;
  const float expected = edited[center] + 3.0f;
  edited[center] = expected;
  first.updateGainMap(0, edited, first.state(0).gain_width, first.state(0).gain_height, true);
  if (!first.saveGainMap(0, &error)) {
    std::cerr << "document save: " << error.toStdString() << "\n";
    return false;
  }

  PreviewSession reload_session = first.session();
  PreviewDocument second(reload_session);
  if (!wait_for_document_item(&second, 0, &error) || !wait_for_document_gain(&second, 0, &error) ||
      std::abs(second.state(0).gain[center] - expected) > 0.001f) {
    std::cerr << "document gain-map persistence failed\n";
    return false;
  }
  std::cout << "OK document async load + gain-map persistence\n";
  (*checks)++;
  return true;
}

bool test_gainmap_editor_core(int* checks) {
  GainMapEditor editor;
  std::vector<float> gain(100, 1.0f);
  editor.setAutoGainMap(gain, 10, 10);
  editor.setContentBoost(1.0f, 100.0f);
  editor.setTool(GainTool::kBrush);
  editor.applyNormalizedStroke({{0.5f, 0.5f, 1.0f}});
  if (editor.valueAt(5, 5) <= 1.01f) {
    std::cerr << "GainMapEditor brush stroke failed\n";
    return false;
  }
  editor.pushUndo();
  editor.applyNormalizedStroke({{0.5f, 0.5f, 1.0f}});
  if (!editor.undo()) {
    std::cerr << "GainMapEditor undo failed\n";
    return false;
  }
  std::cout << "OK GainMapEditor stroke/undo\n";
  (*checks)++;
  return true;
}

bool test_copy_feed_crop_to_others(int* checks) {
  PreviewSession session;
  session.items.resize(2);
  session.items[0].id = "a";
  session.items[1].id = "b";
  session.items[1].crop_offset = 0.2f;
  session.items[1].slice_count = 3;
  session.items[1].preview_slice_index = 2;
  PreviewDocument document(session);
  document.setItemInstagram(0, SliceAspect::k4x5, 0.5f, 1, 1, 2160, 2700);
  const int copied = document.copyFeedCropToOthers(0);
  if (copied != 1) {
    std::cerr << "copyFeedCropToOthers should copy 1 item, got " << copied << "\n";
    return false;
  }
  const SessionItem& dst = document.item(1);
  if (!dst.has_slice_override || dst.slice_aspect != SliceAspect::k4x5 || dst.output_width != 2160 ||
      dst.output_height != 2700) {
    std::cerr << "copyFeedCropToOthers did not copy aspect/size\n";
    return false;
  }
  if (dst.crop_offset < 0.19f || dst.crop_offset > 0.21f) {
    std::cerr << "copyFeedCropToOthers should keep crop offset, got " << dst.crop_offset << "\n";
    return false;
  }
  if (dst.slice_count != 1 || dst.preview_slice_index != 1) {
    std::cerr << "copyFeedCropToOthers should reset gallery slides to 1\n";
    return false;
  }
  std::cout << "OK copyFeedCropToOthers\n";
  (*checks)++;
  return true;
}

bool test_slice_plan_overlay(int* checks) {
  std::vector<CropRect> slices;
  std::string err;
  if (!compute_slices(4000, 3000, SliceAspect::k1x1, &slices, &err) || slices.size() != 1) {
    std::cerr << "slice plan 1:1 failed: " << err << "\n";
    return false;
  }
  if (slices[0].w != 3000 || slices[0].h != 3000 || slices[0].x != 500) {
    std::cerr << "1:1 crop expected 3000x3000 at x=500\n";
    return false;
  }
  (*checks)++;

  if (!compute_slices(6000, 4000, SliceAspect::k4x5, &slices, &err) || slices.size() != 1) {
    std::cerr << "4:5 landscape crop failed: " << err << "\n";
    return false;
  }
  if (slices[0].w != 3200 || slices[0].h != 4000) {
    std::cerr << "4:5 tile expected 3200x4000, got " << slices[0].w << "x" << slices[0].h << "\n";
    return false;
  }
  if (slices[0].x != 1400) {
    std::cerr << "4:5 centered x expected 1400, got " << slices[0].x << "\n";
    return false;
  }
  (*checks)++;

  if (!compute_slices(6000, 4000, SliceAspect::k4x5, &slices, &err, 0.0f) || slices[0].x != 0) {
    std::cerr << "4:5 crop_offset 0 should start at x=0\n";
    return false;
  }
  (*checks)++;

  if (!compute_slices(4000, 6000, SliceAspect::k4x5, &slices, &err) || slices.size() != 1) {
    std::cerr << "4:5 portrait vertical crop failed: " << err << "\n";
    return false;
  }
  if (slices[0].w != 4000 || slices[0].h != 5000 || slices[0].y != 500) {
    std::cerr << "vertical 4:5 expected 4000x5000 y=500, got " << slices[0].w << "x" << slices[0].h
              << " y=" << slices[0].y << "\n";
    return false;
  }
  (*checks)++;

  if (!compute_slices(6000, 4000, SliceAspect::k3x4, &slices, &err) || slices.size() != 1) {
    std::cerr << "3:4 pano should default to 1 tile: " << err << " count=" << slices.size() << "\n";
    return false;
  }
  if (slices[0].w != 3000 || slices[0].h != 4000 || slices[0].x != 1500) {
    std::cerr << "3:4 default 1-slide expected 3000x4000 at x=1500, got " << slices[0].w << "x"
              << slices[0].h << " x=" << slices[0].x << "\n";
    return false;
  }
  (*checks)++;

  if (max_slice_count(6000, 4000, SliceAspect::k3x4) != 2) {
    std::cerr << "3:4 pano max_slice_count expected 2\n";
    return false;
  }
  if (!compute_slices(6000, 4000, SliceAspect::k3x4, &slices, &err, 0.5f, 0) ||
      slices.size() != 2) {
    std::cerr << "3:4 pano requested_count=0 should pack 2 tiles: count=" << slices.size() << "\n";
    return false;
  }
  if (!compute_slices(6000, 4000, SliceAspect::k3x4, &slices, &err, 0.5f, 99) ||
      slices.size() != 2) {
    std::cerr << "3:4 pano should clamp requested_count to max: count=" << slices.size() << "\n";
    return false;
  }
  (*checks)++;

  if (max_slice_count(50000, 2000, SliceAspect::k1x1) != kInstagramGalleryMax) {
    std::cerr << "gallery cap expected " << kInstagramGalleryMax << ", got "
              << max_slice_count(50000, 2000, SliceAspect::k1x1) << "\n";
    return false;
  }
  if (!compute_slices(50000, 2000, SliceAspect::k1x1, &slices, &err, 0.5f, 0) ||
      slices.size() != kInstagramGalleryMax) {
    std::cerr << "gallery cap pack expected " << kInstagramGalleryMax << " tiles, got "
              << slices.size() << "\n";
    return false;
  }
  (*checks)++;

  unsigned ow = 0;
  unsigned oh = 0;
  if (!instagram_output_size(SliceAspect::k4x5, &ow, &oh) || ow != 1080 || oh != 1350) {
    std::cerr << "Instagram 4:5 size mismatch\n";
    return false;
  }
  if (!instagram_output_size(SliceAspect::k191x100, &ow, &oh) || ow != 1080 || oh != 566) {
    std::cerr << "Instagram landscape size mismatch\n";
    return false;
  }
  (*checks)++;

  unsigned x2w = 0;
  unsigned x2h = 0;
  if (!instagram_output_size_scaled(SliceAspect::k3x4, 2, 0, 0, &x2w, &x2h) || x2w != 2160 ||
      x2h != 2880) {
    std::cerr << "3:4 2x size should be 2160x2880\n";
    return false;
  }
  if (!instagram_output_size_scaled(SliceAspect::k4x5, 2, 0, 0, &x2w, &x2h) || x2w != 2160 ||
      x2h != 2700) {
    std::cerr << "4:5 2x size should be 2160x2700\n";
    return false;
  }
  (*checks)++;

  unsigned cw = 0;
  unsigned ch = 0;
  if (nearest_instagram_aspect(1152, 1440) != SliceAspect::k4x5) {
    std::cerr << "1152x1440 should match 4:5\n";
    return false;
  }
  if (nearest_instagram_aspect(2160, 2880) != SliceAspect::k3x4) {
    std::cerr << "2160x2880 should match 3:4\n";
    return false;
  }
  if (nearest_instagram_aspect(3000, 2000) != SliceAspect::kNone) {
    std::cerr << "3:2 should stay Original (no named preset)\n";
    return false;
  }
  if (!instagram_aspect_supported(3000, 2000)) {
    std::cerr << "3:2 should be inside Instagram's 1.91:1–3:4 range\n";
    return false;
  }
  if (instagram_aspect_supported(1080, 1920)) {
    std::cerr << "9:16 should be outside Instagram feed HDR range\n";
    return false;
  }
  (*checks)++;

  if (!clamp_encode_output_size(SliceAspect::k4x5, 3200, 4000, 0, 0, &cw, &ch) || cw != 2160 ||
      ch != 2700) {
    std::cerr << "4:5 crop above 2x should default to 2160x2700, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k3x4, 2448, 3264, 0, 0, &cw, &ch) || cw != 2160 ||
      ch != 2880) {
    std::cerr << "3:4 2448x3264 should default to 2160x2880, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k3x4, 1920, 2560, 0, 0, &cw, &ch) || cw != 1920 ||
      ch != 2560) {
    std::cerr << "3:4 between 1x and 2x should keep native 1920x2560, got " << cw << "x" << ch
              << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k4x5, 2160, 2700, 0, 0, &cw, &ch) || cw != 2160 ||
      ch != 2700) {
    std::cerr << "4:5 exact 2x should stay 2160x2700, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k4x5, 3200, 4000, 2160, 0, &cw, &ch) || cw != 2160 ||
      ch != 2700) {
    std::cerr << "4:5 --out-width 2160 should be 2160x2700, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k4x5, 3200, 4000, 1080, 0, &cw, &ch) || cw != 1080 ||
      ch != 1350) {
    std::cerr << "4:5 --out-width 1080 should be 1080x1350, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k4x5, 800, 1000, 1080, 0, &cw, &ch) || cw != 800 ||
      ch != 1000) {
    std::cerr << "4:5 crop smaller than 1080 must not upscale, got " << cw << "x" << ch << "\n";
    return false;
  }
  if (!clamp_encode_output_size(SliceAspect::k4x5, 800, 1000, 0, 0, &cw, &ch) || cw != 800 ||
      ch != 1000) {
    std::cerr << "4:5 crop below 1x should stay native, got " << cw << "x" << ch << "\n";
    return false;
  }
  (*checks)++;

  unsigned rw = 0;
  unsigned rh = 0;
  if (!resolve_item_export_size(6000, 4000, SliceAspect::k4x5, 0, 0, &rw, &rh) || rw != 2160 ||
      rh != 2700) {
    std::cerr << "resolve 4:5 6000x4000 smart pick expected 2160x2700, got " << rw << "x" << rh
              << "\n";
    return false;
  }
  if (!resolve_item_export_size(800, 1000, SliceAspect::k4x5, 2160, 2700, &rw, &rh) || rw != 800 ||
      rh != 1000) {
    std::cerr << "resolve 4:5 small source must not upscale, got " << rw << "x" << rh << "\n";
    return false;
  }
  if (!resolve_item_export_size(3200, 4000, SliceAspect::k4x5, 2160, 0, &rw, &rh) || rw != 2160 ||
      rh != 2700) {
    std::cerr << "resolve 4:5 requested 2160 expected 2160x2700, got " << rw << "x" << rh << "\n";
    return false;
  }
  (*checks)++;

  if (compute_slices(2, 2, SliceAspect::k4x5, &slices, &err)) {
    std::cerr << "2x2 4:5 should fail (tile too small)\n";
    return false;
  }
  err.clear();
  if (!compute_slices(4001, 3001, SliceAspect::k1x1, &slices, &err) || slices.size() != 1 ||
      slices[0].w != 3000 || slices[0].h != 3000) {
    std::cerr << "odd 4001x3001 1x1 should even-floor and crop, got " << slices.size() << " tiles "
              << (slices.empty() ? err : std::to_string(slices[0].w) + "x" +
                                             std::to_string(slices[0].h))
              << "\n";
    return false;
  }
  (*checks)++;

  std::cout << "OK slice plan Instagram crops\n";
  return true;
}

bool test_activity_log(int* checks) {
  const fs::path root = fs::temp_directory_path() / "uhdr_activity_log_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);
  const std::string dest = (root / "export.log").u8string();
  activity_log_set_hook({});
  activity_log_configure(root.u8string(), dest);
  activity_log_append("photo_1", "wait_tiff", "requested Lightroom HDR TIFF");
  activity_log_append("photo_1", "wait_tiff", "ready", 1240);
  const std::string work = activity_log_work_path();
  if (work.empty() || !fs::exists(fs::u8path(work)) || !fs::exists(fs::u8path(dest))) {
    std::cerr << "activity log files missing\n";
    return false;
  }
  std::ifstream in_work(work);
  std::ifstream in_dest(dest);
  std::string work_body((std::istreambuf_iterator<char>(in_work)), {});
  std::string dest_body((std::istreambuf_iterator<char>(in_dest)), {});
  if (work_body.find("photo_1 wait_tiff") == std::string::npos ||
      dest_body.find("+1240ms ready") == std::string::npos) {
    std::cerr << "activity log contents mismatch\n" << work_body << dest_body;
    return false;
  }
  (*checks)++;
  std::cout << "OK activity log file append\n";
  activity_log_configure("", "");
  return true;
}

}  // namespace

int gui_self_test_main(const std::string& session_path) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }

  int argc = 0;
  char** argv = nullptr;
  QApplication app(argc, argv);
  QImageReader::setAllocationLimit(0);

  PreviewSession session;
  std::string err;
  if (!load_session_file(session_path, &session, &err)) {
    return fail("session load: " + err);
  }
  if (session.items.empty()) {
    return fail("session has no items");
  }
  std::cout << "OK session load (" << session.items.size() << " items)\n";

  int checks = 1;
  if (!test_gainmap_editor_core(&checks)) {
    return fail("GainMapEditor core");
  }
  if (!test_slice_plan_overlay(&checks)) {
    return fail("slice plan");
  }
  if (!test_copy_feed_crop_to_others(&checks)) {
    return fail("copy feed crop to others");
  }
  if (!test_activity_log(&checks)) {
    return fail("activity log");
  }
  if (!test_hdr_bridge_protocol(&checks)) {
    return fail("hdr bridge protocol");
  }
  if (!test_sdr_load_without_tiff(session, &checks)) {
    return fail("sdr load without tiff");
  }
  if (!test_hdr_tiff_client(session, &checks)) {
    return fail("hdr tiff client");
  }
  if (!test_hdr_tiff_client_overlapping(session, &checks)) {
    return fail("hdr tiff client overlapping");
  }
  if (!test_document_persistence(session, &checks)) {
    return fail("preview document persistence");
  }
  for (const auto& item : session.items) {
    if (!test_session_item_gainmap(item, &checks)) {
      return fail("gainmap/gui checks failed for " + item.id);
    }
  }

  SessionItem encode_item;
  bool found_encode = false;
  for (const auto& item : session.items) {
    if (item.id == "legacy_1000" && fs::exists(item.sdr) && fs::exists(item.hdr_tiff)) {
      encode_item = item;
      found_encode = true;
      break;
    }
  }
  if (!found_encode) {
    for (const auto& item : session.items) {
      if (fs::exists(item.sdr) && fs::exists(item.hdr_tiff)) {
        encode_item = item;
        found_encode = true;
        break;
      }
    }
  }
  if (!found_encode) {
    return fail("no encodable session item");
  }

  fs::create_directories(fs::path(encode_item.out).parent_path());
  EncodeRequest req;
  req.hdr_tiff = encode_item.hdr_tiff;
  req.base_path = encode_item.sdr;
  req.out_path = encode_item.out;
  req.options = effective_encode_options(session, encode_item);
  const int enc = encode_from_paths(req, &err);
  if (enc != 0) {
    return fail("encode: " + err);
  }
  if (!fs::exists(encode_item.out)) {
    return fail("encode output missing");
  }
  std::cout << "OK headless encode " << encode_item.id << "\n";
  checks++;

  {
    std::ifstream jpeg(encode_item.out, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(jpeg)),
                                     std::istreambuf_iterator<char>());
    bool progressive = false;
    bool has_icc = false;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
      if (bytes[i] != 0xff) continue;
      const unsigned char m = bytes[i + 1];
      if (m == 0xda) break;
      if (m == 0xc2) progressive = true;
      if (m == 0xe2 && i + 16 < bytes.size() &&
          std::memcmp(bytes.data() + i + 4, "ICC_PROFILE", 11) == 0) {
        has_icc = true;
      }
    }
    if (!progressive) {
      return fail("default encode primary JPEG is not progressive (SOF2)");
    }
    if (!has_icc) {
      return fail("default encode is missing an ICC profile");
    }
    std::cout << "OK progressive JPEG with ICC profile\n";
    checks++;
  }

  {
    EncodeRequest preview = req;
    preview.out_path =
        (fs::path(encode_item.out).parent_path() / "legacy_uhdr_preview_cap.jpg").u8string();
    preview.preview_max_edge = 512;
    if (encode_from_paths(preview, &err) != 0) {
      return fail("preview_max_edge encode: " + err);
    }
    DecodedHdrFrame cap;
    if (!decode_ultrahdr_preview(preview.out_path, 4.0f, &cap, &err) || cap.width > 512 ||
        cap.height > 512) {
      return fail("preview_max_edge should cap long edge at 512, got " +
                  std::to_string(cap.width) + "x" + std::to_string(cap.height) + " " + err);
    }
    std::cout << "OK preview encode cap " << cap.width << "x" << cap.height << "\n";
    checks++;
  }

  DecodedHdrFrame decoded;
  if (!decode_ultrahdr_preview(encode_item.out, 4.0f, &decoded, &err) ||
      decoded.rgba_half.empty() || decoded.width <= 0 || decoded.height <= 0) {
    return fail("final Ultra HDR decode: " + err);
  }
  std::cout << "OK final Ultra HDR RGBA16F decode " << decoded.width << "x" << decoded.height
            << " cg=" << decoded.color_gamut << "\n";
  checks++;
  if (display_gamut_id(decoded.color_gamut) == 2) {
    return fail("native Ultra HDR decode used BT.2100 primaries; preview must not assume Rec.2020");
  }
  if (display_gamut_id(decoded.color_gamut) == 0) {
    const size_t mid =
        (static_cast<size_t>(decoded.height / 2) * static_cast<size_t>(decoded.width) +
         static_cast<size_t>(decoded.width / 2)) *
        4u;
    const LinearRgb px{half_to_float(decoded.rgba_half[mid]),
                       half_to_float(decoded.rgba_half[mid + 1]),
                       half_to_float(decoded.rgba_half[mid + 2])};
    const LinearRgb mapped = decoded_hdr_to_linear_srgb(px, decoded.color_gamut);
    if (std::abs(mapped.r - px.r) > 1e-4f || std::abs(mapped.g - px.g) > 1e-4f ||
        std::abs(mapped.b - px.b) > 1e-4f) {
      return fail("BT.709 decode was gamut-converted as if Rec.2020");
    }
  }
  if (display_gamut_id(decoded.color_gamut) != 1 && display_gamut_id(decoded.color_gamut) != 0) {
    return fail("default encode gamut is neither Display P3 nor BT.709");
  }
  std::cout << "OK decode primaries are display-referred (not Rec.2020)\n";
  checks++;
  if (decoded.gainmap_jpeg.empty()) {
    return fail("decoded Ultra HDR is missing the gain-map JPEG");
  }
  if (!decoded.gainmap_is_rgb) {
    return fail("default encode did not produce an RGB gain-map JPEG");
  }
  std::cout << "OK encoded RGB gain-map JPEG " << decoded.gainmap_jpeg.size() << " bytes\n";
  checks++;

  std::vector<float> overlay_gain;
  int overlay_w = 0;
  int overlay_h = 0;
  if (!compute_auto_gainmap(encode_item.sdr, encode_item.hdr_tiff, &overlay_gain, &overlay_w,
                            &overlay_h, &err)) {
    return fail("gain overlay heatmap compute: " + err);
  }
  GainMapEditor overlay_editor;
  overlay_editor.setAutoGainMap(overlay_gain, overlay_w, overlay_h);
  const QImage heat = overlay_editor.renderHeatmap();
  const QImage encoded_map =
      QImage::fromData(decoded.gainmap_jpeg.data(), static_cast<int>(decoded.gainmap_jpeg.size()),
                       "JPEG");
  if (heat.isNull() || encoded_map.isNull()) {
    return fail("gain overlay heatmap or encoded JPEG is empty");
  }
  const QImage encoded_scaled =
      encoded_map.scaled(heat.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
  int differ = 0;
  int samples = 0;
  const int y_step = std::max(1, heat.height() / 16);
  const int x_step = std::max(1, heat.width() / 16);
  for (int y = 0; y < heat.height(); y += y_step) {
    for (int x = 0; x < heat.width(); x += x_step) {
      const QRgb a = heat.pixel(x, y);
      const QRgb b = encoded_scaled.pixel(x, y);
      if (std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) +
              std::abs(qBlue(a) - qBlue(b)) >
          80) {
        ++differ;
      }
      ++samples;
    }
  }
  if (samples < 1 || differ * 2 < samples) {
    return fail("luma heatmap looks like the encoded gain-map JPEG");
  }
  std::cout << "OK gain overlay stays luma heatmap (not encoded JPEG)\n";
  checks++;

  EncodeRequest mono = req;
  mono.options.monochrome_gainmap = true;
  mono.out_path = (fs::path(encode_item.out).parent_path() / "legacy_uhdr_mono.jpg").u8string();
  if (encode_from_paths(mono, &err) != 0) {
    return fail("monochrome encode: " + err);
  }
  DecodedHdrFrame decoded_mono;
  if (!decode_ultrahdr_preview(mono.out_path, 4.0f, &decoded_mono, &err) ||
      decoded_mono.gainmap_jpeg.empty()) {
    return fail("monochrome gain-map decode: " + err);
  }
  if (decoded_mono.gainmap_is_rgb) {
    return fail("monochrome encode still stored an RGB gain-map JPEG");
  }
  std::cout << "OK monochrome gain-map JPEG is luma-only\n";
  checks++;
  fs::remove(mono.out_path);

  EncodeRequest mono_map = req;
  mono_map.options.monochrome_gainmap = true;
  mono_map.options.gainmap_scale = 2;
  mono_map.options.min_content_boost = 1.0f;
  mono_map.options.max_content_boost = 4.92f;
  mono_map.options.target_display_peak_nits = 1000.0f;
  mono_map.options.base_quality = 95;
  mono_map.options.gainmap_quality = 95;
  mono_map.out_path = (fs::path(encode_item.out).parent_path() / "mono_map.jpg").u8string();
  if (encode_from_paths(mono_map, &err) != 0) {
    return fail("mono map encode: " + err);
  }
  DecodedHdrFrame decoded_phone;
  if (!decode_ultrahdr_preview(mono_map.out_path, 4.0f, &decoded_phone, &err) ||
      decoded_phone.gainmap_jpeg.empty()) {
    return fail("mono map decode: " + err);
  }
  if (decoded_phone.gainmap_is_rgb) {
    return fail("mono map encode stored an RGB gain-map JPEG");
  }
  InspectReport phone_rep;
  if (!inspect_ultra_hdr_file(mono_map.out_path, &phone_rep, &err) || !phone_rep.is_ultra_hdr) {
    return fail("mono map inspect: " + err);
  }
  const int expect_gw = phone_rep.width / 2;
  const int expect_gh = phone_rep.height / 2;
  if (phone_rep.gainmap_width < expect_gw - 1 || phone_rep.gainmap_width > expect_gw + 1 ||
      phone_rep.gainmap_height < expect_gh - 1 || phone_rep.gainmap_height > expect_gh + 1) {
    return fail("mono map gainmap size " + std::to_string(phone_rep.gainmap_width) + "x" +
                std::to_string(phone_rep.gainmap_height) + " expected ~" +
                std::to_string(expect_gw) + "x" + std::to_string(expect_gh));
  }
  std::cout << "OK mono map luma gain map " << phone_rep.gainmap_width << "x"
            << phone_rep.gainmap_height << "\n";
  checks++;
  fs::remove(mono_map.out_path);

  EncodeRequest ig = req;
  ig.slice_aspect = SliceAspect::k4x5;
  ig.crop_offset = 0.5f;
  ig.output_width = 0;
  ig.output_height = 0;
  ig.out_path = (fs::path(encode_item.out).parent_path() / "ig_4x5.jpg").u8string();
  unsigned master_w = 0;
  unsigned master_h = 0;
  if (!probe_hdr_tiff_even_size(encode_item.hdr_tiff, &master_w, &master_h, &err)) {
    return fail("4:5 probe: " + err);
  }
  std::vector<CropRect> ig_tiles;
  if (!compute_slices(master_w, master_h, SliceAspect::k4x5, &ig_tiles, &err) || ig_tiles.empty()) {
    return fail("4:5 slice plan: " + err);
  }
  unsigned expect_w = 0;
  unsigned expect_h = 0;
  if (!clamp_encode_output_size(SliceAspect::k4x5, ig_tiles[0].w, ig_tiles[0].h, 0, 0, &expect_w,
                                &expect_h)) {
    return fail("4:5 clamp smart size");
  }
  if (encode_from_paths(ig, &err) != 0) {
    return fail("instagram 4:5 encode: " + err);
  }
  DecodedHdrFrame decoded_ig;
  if (!decode_ultrahdr_preview(ig.out_path, 4.0f, &decoded_ig, &err) ||
      decoded_ig.rgba_half.empty()) {
    return fail("instagram 4:5 decode: " + err);
  }
  if (static_cast<unsigned>(decoded_ig.width) != expect_w ||
      static_cast<unsigned>(decoded_ig.height) != expect_h) {
    return fail("instagram 4:5 smart output was " + std::to_string(decoded_ig.width) + "x" +
                std::to_string(decoded_ig.height) + " expected " + std::to_string(expect_w) + "x" +
                std::to_string(expect_h));
  }
  if (decoded_ig.gainmap_jpeg.empty()) {
    return fail("instagram 4:5 encode missing gain-map JPEG");
  }
  std::cout << "OK Instagram 4:5 smart size " << decoded_ig.width << "x" << decoded_ig.height
            << "\n";
  checks++;

  unsigned floor_w = 0;
  unsigned floor_h = 0;
  clamp_encode_output_size(SliceAspect::k4x5, ig_tiles[0].w, ig_tiles[0].h, 1080, 0, &floor_w,
                           &floor_h);
  ig.output_width = 1080;
  ig.output_height = 0;
  ig.out_path = (fs::path(encode_item.out).parent_path() / "ig_4x5_1080.jpg").u8string();
  if (encode_from_paths(ig, &err) != 0) {
    return fail("instagram 4:5 1080 encode: " + err);
  }
  DecodedHdrFrame decoded_ig_floor;
  if (!decode_ultrahdr_preview(ig.out_path, 4.0f, &decoded_ig_floor, &err) ||
      static_cast<unsigned>(decoded_ig_floor.width) != floor_w ||
      static_cast<unsigned>(decoded_ig_floor.height) != floor_h) {
    return fail("4:5 --out-width 1080 size mismatch");
  }
  std::cout << "OK Instagram 4:5 --out-width 1080 is " << decoded_ig_floor.width << "x"
            << decoded_ig_floor.height << "\n";
  checks++;
  fs::remove(ig.out_path);
  ig.out_path = (fs::path(encode_item.out).parent_path() / "ig_4x5.jpg").u8string();
  fs::remove(ig.out_path);

  SessionItem dsc;
  for (const auto& item : session.items) {
    if (item.id == "dsc02993" && fs::exists(item.sdr) && fs::exists(item.hdr_tiff)) {
      dsc = item;
      break;
    }
  }
  if (!dsc.id.empty()) {
    EncodeRequest feed = req;
    feed.hdr_tiff = dsc.hdr_tiff;
    feed.base_path = dsc.sdr;
    feed.slice_aspect = SliceAspect::k4x5;
    feed.output_width = 1080;
    feed.out_path = (fs::path(encode_item.out).parent_path() / "dsc_4x5_1080.jpg").u8string();
    if (encode_from_paths(feed, &err) != 0) {
      return fail("dsc 4:5 1080 encode: " + err);
    }
    DecodedHdrFrame dsc_ig;
    if (!decode_ultrahdr_preview(feed.out_path, 4.0f, &dsc_ig, &err) || dsc_ig.width != 1080 ||
        dsc_ig.height != 1350) {
      return fail("dsc 4:5 --out-width 1080 expected 1080x1350, got " +
                  std::to_string(dsc_ig.width) + "x" + std::to_string(dsc_ig.height));
    }
    std::cout << "OK 4:5 --out-width 1080 on 1152x1440 is 1080x1350\n";
    checks++;
    fs::remove(feed.out_path);

    feed.output_width = 0;
    feed.out_path = (fs::path(encode_item.out).parent_path() / "dsc_4x5_smart.jpg").u8string();
    if (encode_from_paths(feed, &err) != 0) {
      return fail("dsc 4:5 smart encode: " + err);
    }
    DecodedHdrFrame dsc_smart;
    if (!decode_ultrahdr_preview(feed.out_path, 4.0f, &dsc_smart, &err) || dsc_smart.width != 1152 ||
        dsc_smart.height != 1440) {
      return fail("dsc 4:5 smart pick expected 1152x1440, got " + std::to_string(dsc_smart.width) +
                  "x" + std::to_string(dsc_smart.height));
    }
    std::cout << "OK 4:5 smart pick on 1152x1440 is 1152x1440\n";
    checks++;
    fs::remove(feed.out_path);
  }

  PreviewSession batch = session;
  batch.items = {encode_item};
  batch.result_path = (fs::path(session.work_dir) / "self_test_result.json").string();
  fs::create_directories(session.work_dir);
  PreviewResult result;
  const int batch_code = apply_session_batch(&batch, &result, &err);
  if (batch_code != 0 || !result.approved) {
    return fail("apply_session_batch: " + err);
  }
  if (!fs::exists(batch.result_path)) {
    return fail("result.json not written");
  }
  std::cout << "OK apply_session_batch + result.json\n";
  checks++;

  PreviewSession dest_batch = session;
  dest_batch.items = {encode_item};
  dest_batch.dest_dir = (fs::path(session.work_dir) / "dest_override").u8string();
  dest_batch.result_path = (fs::path(session.work_dir) / "self_test_dest.json").string();
  PreviewResult dest_result;
  if (apply_session_batch(&dest_batch, &dest_result, &err) != 0 || !dest_result.approved) {
    return fail("dest_dir batch: " + err);
  }
  const fs::path dest_jpeg =
      fs::path(dest_batch.dest_dir) / fs::path(encode_item.out).filename();
  if (!fs::exists(dest_jpeg)) {
    return fail("dest_dir did not write " + dest_jpeg.u8string());
  }
  if (dest_result.dest_dir != dest_batch.dest_dir) {
    return fail("result.json dest_dir mismatch");
  }
  std::cout << "OK dest_dir rewrite " << dest_jpeg.u8string() << "\n";
  checks++;
  fs::remove(dest_jpeg);

  std::cout << "SELF_TEST_OK: " << checks << " checks passed\n";
  return 0;
}

}  // namespace uhdr_repack

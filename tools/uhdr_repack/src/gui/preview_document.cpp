#include "gui/preview_document.h"

#include "activity_log.h"
#include "encode_engine.h"
#include "gainmap_compute.h"
#include "gainmap_loader.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace uhdr_repack {

namespace {
std::atomic<uint64_t> g_final_preview_seq{0};
constexpr unsigned kPreviewEncodeMaxEdge = 2048;
}  // namespace

struct PreviewDocument::LoadResult {
  int index = -1;
  uint64_t generation = 0;
  PreviewItemState state;
};

struct PreviewDocument::FinalResult {
  int index = -1;
  uint64_t generation = 0;
  DecodedHdrFrame frame;
  QString error;
  std::string preview_path;
  int encode_ms = -1;
  int decode_ms = -1;
};

class PreviewDocument::Private {
 public:
  explicit Private(PreviewDocument* owner, PreviewSession value)
      : q(owner), session(std::move(value)), states(session.items.size()),
        generations(session.items.size(), 0), final_generations(session.items.size(), 0),
        final_in_flight(session.items.size(), false) {}

  std::string cachePath(int index) const {
    const SessionItem& item = session.items[static_cast<size_t>(index)];
    if (!session.work_dir.empty()) return session.work_dir + "/" + item.id + ".gainmap";
    return item.out + ".gainmap";
  }

  std::string loadPath(int index) const {
    const SessionItem& item = session.items[static_cast<size_t>(index)];
    return item.gainmap_in.empty() ? cachePath(index) : item.gainmap_in;
  }

  void invalidateFinal(int index) {
    ++final_generations[static_cast<size_t>(index)];
    final_in_flight[static_cast<size_t>(index)] = false;
    states[static_cast<size_t>(index)].final_dirty = true;
    states[static_cast<size_t>(index)].final_hdr = {};
  }

  PreviewDocument* q;
  PreviewSession session;
  std::vector<PreviewItemState> states;
  std::vector<uint64_t> generations;
  std::vector<uint64_t> final_generations;
  std::vector<bool> final_in_flight;
};

PreviewDocument::PreviewDocument(PreviewSession session, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>(this, std::move(session))) {}

PreviewDocument::~PreviewDocument() = default;

int PreviewDocument::itemCount() const {
  return static_cast<int>(d_->session.items.size());
}

const PreviewSession& PreviewDocument::session() const {
  return d_->session;
}

PreviewSession& PreviewDocument::mutableSession() {
  return d_->session;
}

const SessionItem& PreviewDocument::item(int index) const {
  return d_->session.items.at(static_cast<size_t>(index));
}

const PreviewItemState& PreviewDocument::state(int index) const {
  return d_->states.at(static_cast<size_t>(index));
}

void PreviewDocument::requestItem(int index) {
  if (index < 0 || index >= itemCount()) return;
  auto& state = d_->states[static_cast<size_t>(index)];
  if (state.loaded) {
    emit itemReady(index);
    return;
  }

  const uint64_t generation = ++d_->generations[static_cast<size_t>(index)];
  state.loading = true;
  state.error.clear();
  emit itemLoading(index);

  const SessionItem item_copy = d_->session.items[static_cast<size_t>(index)];
  const std::string load_path = d_->loadPath(index);
  auto future = QtConcurrent::run([index, generation, item_copy, load_path]() {
    LoadResult result;
    result.index = index;
    result.generation = generation;
    result.state.loading = false;
    result.state.sdr = QImage(QString::fromStdString(item_copy.sdr));
    if (result.state.sdr.isNull()) {
      result.state.error = QStringLiteral("Could not load SDR image: %1")
                               .arg(QString::fromStdString(item_copy.sdr));
      return result;
    }

    std::string error;
    bool loaded_cache = false;
    if (fs::exists(fs::u8path(load_path))) {
      loaded_cache = load_gainmap_raw_file(load_path, &result.state.gain,
                                           &result.state.gain_width,
                                           &result.state.gain_height, &error);
    }

    if (!result.state.gain.empty()) {
      const auto minmax =
          std::minmax_element(result.state.gain.begin(), result.state.gain.end());
      result.state.gain_min = *minmax.first;
      result.state.gain_max = *minmax.second;
    }
    result.state.loaded = true;
    result.state.gainmap_override = !item_copy.gainmap_in.empty() && loaded_cache;
    result.state.final_dirty = true;
    return result;
  });

  auto* watcher = new QFutureWatcher<LoadResult>(this);
  connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher] {
    const LoadResult result = watcher->result();
    watcher->deleteLater();
    if (result.index < 0 || result.index >= itemCount()) return;
    if (result.generation != d_->generations[static_cast<size_t>(result.index)]) return;
    d_->states[static_cast<size_t>(result.index)] = result.state;
    if (!result.state.error.isEmpty()) {
      emit itemFailed(result.index, result.state.error);
      return;
    }
    emit itemReady(result.index);
  });
  watcher->setFuture(future);
}

void PreviewDocument::requestGainMap(int index) {
  if (index < 0 || index >= itemCount()) return;
  auto& state = d_->states[static_cast<size_t>(index)];
  if (!state.loaded) return;
  if (!state.gain.empty()) {
    activity_log_append(item(index).id, "gainmap", "cache hit");
    emit itemReady(index);
    return;
  }

  const SessionItem item_copy = d_->session.items[static_cast<size_t>(index)];
  if (item_copy.hdr_tiff.empty()) {
    activity_log_append(item_copy.id, "gainmap", "fail HDR TIFF is not available");
    emit itemFailed(index, QStringLiteral("HDR TIFF is not available"));
    return;
  }

  activity_log_append(item_copy.id, "gainmap", "compute start");
  const uint64_t generation = ++d_->generations[static_cast<size_t>(index)];
  const std::string load_path = d_->loadPath(index);
  auto future = QtConcurrent::run([index, generation, item_copy, load_path]() {
    const auto t0 = std::chrono::steady_clock::now();
    LoadResult result;
    result.index = index;
    result.generation = generation;
    std::string error;
    bool loaded_cache = false;
    if (fs::exists(fs::u8path(load_path))) {
      loaded_cache = load_gainmap_raw_file(load_path, &result.state.gain,
                                           &result.state.gain_width,
                                           &result.state.gain_height, &error);
    }
    if (!loaded_cache &&
        !compute_auto_gainmap(item_copy.sdr, item_copy.hdr_tiff, &result.state.gain,
                              &result.state.gain_width, &result.state.gain_height, &error)) {
      result.state.error = QString::fromStdString(error);
      const int ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
              .count());
      activity_log_append(item_copy.id, "gainmap", "fail " + error, ms);
      return result;
    }
    if (!result.state.gain.empty()) {
      const auto minmax =
          std::minmax_element(result.state.gain.begin(), result.state.gain.end());
      result.state.gain_min = *minmax.first;
      result.state.gain_max = *minmax.second;
    }
    result.state.gainmap_override = !item_copy.gainmap_in.empty() && loaded_cache;
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    std::string detail = loaded_cache ? "from cache" : "computed";
    if (!result.state.gain.empty()) {
      detail += " " + std::to_string(result.state.gain_width) + "x" +
                std::to_string(result.state.gain_height);
    }
    activity_log_append(item_copy.id, "gainmap", detail, ms);
    return result;
  });

  auto* watcher = new QFutureWatcher<LoadResult>(this);
  connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher] {
    const LoadResult result = watcher->result();
    watcher->deleteLater();
    if (result.index < 0 || result.index >= itemCount()) return;
    if (result.generation != d_->generations[static_cast<size_t>(result.index)]) return;
    auto& state = d_->states[static_cast<size_t>(result.index)];
    if (!result.state.error.isEmpty()) {
      emit itemFailed(result.index, result.state.error);
      return;
    }
    state.gain = result.state.gain;
    state.gain_width = result.state.gain_width;
    state.gain_height = result.state.gain_height;
    state.gain_min = result.state.gain_min;
    state.gain_max = result.state.gain_max;
    if (result.state.gainmap_override) state.gainmap_override = true;
    emit itemReady(result.index);
  });
  watcher->setFuture(future);
}

void PreviewDocument::setHdrTiff(int index, const std::string& path) {
  if (index < 0 || index >= itemCount()) return;
  d_->session.items[static_cast<size_t>(index)].hdr_tiff = path;
  d_->invalidateFinal(index);
}

void PreviewDocument::requestFinalPreview(int index) {
  if (index < 0 || index >= itemCount()) return;
  auto& state = d_->states[static_cast<size_t>(index)];
  if (!state.loaded) {
    activity_log_append(item(index).id, "encode", "defer until SDR is loaded");
    return;
  }
  if (!state.final_dirty && !state.final_hdr.rgba_half.empty()) {
    activity_log_append(item(index).id, "encode", "preview cache hit");
    emit finalPreviewReady(index);
    return;
  }
  if (d_->final_in_flight[static_cast<size_t>(index)]) return;

  if (state.gainmap_override) {
    QString save_error;
    if (!saveGainMap(index, &save_error)) {
      emit finalPreviewFailed(index, save_error);
      return;
    }
  }

  const uint64_t generation = ++d_->final_generations[static_cast<size_t>(index)];
  d_->final_in_flight[static_cast<size_t>(index)] = true;
  const SessionItem item_copy = d_->session.items[static_cast<size_t>(index)];
  const bool bake_gainmap = state.gainmap_override && !item_copy.gainmap_in.empty();
  const EncodeOptions options = effective_encode_options(d_->session, item_copy);
  const SliceAspect aspect = effective_slice_aspect(d_->session, item_copy);
  const float crop_offset = effective_crop_offset(item_copy);
  const unsigned slice_count = effective_slice_count(item_copy);
  const unsigned preview_slice =
      aspect == SliceAspect::kNone ? 0u
                                   : (item_copy.preview_slice_index < 1 ? 1u : item_copy.preview_slice_index);
  const uint64_t seq = ++g_final_preview_seq;
  const std::string work_root =
      d_->session.work_dir.empty() ? fs::u8path(item_copy.out).parent_path().u8string()
                                   : d_->session.work_dir;
  const std::string preview_path =
      (fs::u8path(work_root) /
       fs::u8path(item_copy.id + ".final-preview." + std::to_string(seq) + ".jpg"))
          .u8string();

  activity_log_append(item_copy.id, "encode",
                      "preview start max_edge=" + std::to_string(kPreviewEncodeMaxEdge) + " q=" +
                          std::to_string(options.base_quality) + "/" +
                          std::to_string(options.gainmap_quality));

  auto future = QtConcurrent::run([index, generation, item_copy, options, preview_path,
                                   bake_gainmap, aspect, crop_offset, slice_count, preview_slice]() {
    FinalResult result;
    result.index = index;
    result.generation = generation;
    result.preview_path = preview_path;
    EncodeRequest request;
    request.hdr_tiff = item_copy.hdr_tiff;
    request.base_path = item_copy.sdr;
    request.out_path = preview_path;
    request.options = options;
    request.slice_aspect = aspect;
    request.crop_offset = crop_offset;
    request.slice_count = slice_count;
    request.preview_slice_index = preview_slice;
    request.preview_max_edge = kPreviewEncodeMaxEdge;
    request.output_width = item_copy.output_width;
    request.output_height = item_copy.output_height;
    if (bake_gainmap) request.gainmap_in = item_copy.gainmap_in;
    std::string error;
    fs::create_directories(fs::u8path(preview_path).parent_path());
    const auto t_enc = std::chrono::steady_clock::now();
    if (encode_from_paths(request, &error) != 0) {
      result.encode_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_enc)
              .count());
      result.error = QString::fromStdString(error);
      activity_log_append(item_copy.id, "encode", "fail " + error, result.encode_ms);
      return result;
    }
    result.encode_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_enc)
            .count());
    activity_log_append(item_copy.id, "encode", "preview jpeg", result.encode_ms);
    const auto t_dec = std::chrono::steady_clock::now();
    if (!decode_ultrahdr_preview(preview_path,
                                 std::max(1.0f, options.target_display_peak_nits / 203.0f),
                                 &result.frame, &error)) {
      result.decode_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_dec)
              .count());
      result.error = QString::fromStdString(error);
      activity_log_append(item_copy.id, "decode", "fail " + error, result.decode_ms);
      return result;
    }
    result.decode_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_dec)
            .count());
    activity_log_append(item_copy.id, "decode",
                        std::to_string(result.frame.width) + "x" + std::to_string(result.frame.height),
                        result.decode_ms);
    return result;
  });

  auto* watcher = new QFutureWatcher<FinalResult>(this);
  connect(watcher, &QFutureWatcher<FinalResult>::finished, this, [this, watcher] {
    const FinalResult result = watcher->result();
    watcher->deleteLater();
    if (!result.preview_path.empty()) {
      std::error_code ec;
      fs::remove(fs::u8path(result.preview_path), ec);
    }
    if (result.index < 0 || result.index >= itemCount()) return;
    const bool current =
        result.generation == d_->final_generations[static_cast<size_t>(result.index)];
    if (current) d_->final_in_flight[static_cast<size_t>(result.index)] = false;
    if (!current) return;
    if (!result.error.isEmpty()) {
      emit finalPreviewFailed(result.index, result.error);
      return;
    }
    auto& state = d_->states[static_cast<size_t>(result.index)];
    state.final_hdr = result.frame;
    state.final_dirty = false;
    emit finalPreviewReady(result.index);
  });
  watcher->setFuture(future);
}

void PreviewDocument::updateGainMap(int index, const std::vector<float>& gain, int width,
                                    int height, bool as_override) {
  if (index < 0 || index >= itemCount()) return;
  auto& state = d_->states[static_cast<size_t>(index)];
  state.gain = gain;
  state.gain_width = width;
  state.gain_height = height;
  state.modified = true;
  if (as_override) state.gainmap_override = true;
  d_->invalidateFinal(index);
  if (!gain.empty()) {
    const auto minmax = std::minmax_element(gain.begin(), gain.end());
    state.gain_min = *minmax.first;
    state.gain_max = *minmax.second;
  }
  emit itemChanged(index);
}

bool PreviewDocument::saveGainMap(int index, QString* error) {
  if (index < 0 || index >= itemCount()) return false;
  auto& state = d_->states[static_cast<size_t>(index)];
  if (state.gain.empty() || state.gain_width <= 0 || state.gain_height <= 0) {
    if (error) *error = QStringLiteral("No gain map is loaded");
    return false;
  }
  const std::string path = d_->cachePath(index);
  fs::create_directories(fs::u8path(path).parent_path());
  std::string native_error;
  if (!write_gainmap_raw_file(path, state.gain, state.gain_width, state.gain_height,
                              &native_error)) {
    if (error) *error = QString::fromStdString(native_error);
    return false;
  }
  if (state.gainmap_override) {
    d_->session.items[static_cast<size_t>(index)].gainmap_in = path;
  }
  state.modified = false;
  return true;
}

void PreviewDocument::setItemSkipped(int index, bool skipped) {
  if (index < 0 || index >= itemCount()) return;
  d_->session.items[static_cast<size_t>(index)].skipped = skipped;
  emit itemChanged(index);
}

void PreviewDocument::setEncodeOptions(int index, const EncodeOptions& options) {
  if (index < 0 || index >= itemCount()) return;
  auto& item = d_->session.items[static_cast<size_t>(index)];
  item.encode_options = options;
  item.has_encode_override = true;
  d_->invalidateFinal(index);
  emit itemChanged(index);
}

void PreviewDocument::setItemInstagram(int index, SliceAspect aspect, float crop_offset,
                                       unsigned slice_count, unsigned preview_slice_index,
                                       unsigned output_width, unsigned output_height) {
  if (index < 0 || index >= itemCount()) return;
  auto& item = d_->session.items[static_cast<size_t>(index)];
  const float clamped = std::clamp(crop_offset, 0.0f, 1.0f);
  const unsigned preview = preview_slice_index < 1 ? 1u : preview_slice_index;
  if (item.has_slice_override && item.slice_aspect == aspect && item.crop_offset == clamped &&
      item.slice_count == slice_count && item.preview_slice_index == preview &&
      item.output_width == output_width && item.output_height == output_height) {
    return;
  }
  item.slice_aspect = aspect;
  item.has_slice_override = true;
  item.crop_offset = clamped;
  item.slice_count = slice_count;
  item.preview_slice_index = preview;
  item.output_width = output_width;
  item.output_height = output_height;
  d_->invalidateFinal(index);
  emit itemChanged(index);
}

int PreviewDocument::copyFeedCropToOthers(int from_index) {
  if (from_index < 0 || from_index >= itemCount()) return 0;
  const SessionItem& src = item(from_index);
  const SliceAspect aspect = effective_slice_aspect(d_->session, src);
  const unsigned output_width = src.output_width;
  const unsigned output_height = src.output_height;
  int copied = 0;
  for (int i = 0; i < itemCount(); ++i) {
    if (i == from_index) continue;
    const SessionItem& dst = item(i);
    setItemInstagram(i, aspect, effective_crop_offset(dst), 1, 1, output_width, output_height);
    ++copied;
  }
  return copied;
}

void PreviewDocument::setSharedEncodeOptions(const EncodeOptions& options) {
  const EncodeOptions& cur = d_->session.default_encode_options;
  const bool same = cur.base_quality == options.base_quality &&
                    cur.gainmap_quality == options.gainmap_quality &&
                    cur.gainmap_scale == options.gainmap_scale &&
                    cur.min_content_boost == options.min_content_boost &&
                    cur.max_content_boost == options.max_content_boost &&
                    cur.target_display_peak_nits == options.target_display_peak_nits &&
                    cur.monochrome_gainmap == options.monochrome_gainmap;
  if (same) return;
  d_->session.default_encode_options = options;
  for (int i = 0; i < itemCount(); ++i) {
    d_->session.items[static_cast<size_t>(i)].has_encode_override = false;
    d_->invalidateFinal(i);
    emit itemChanged(i);
  }
}

void PreviewDocument::setDestDir(const std::string& path) {
  d_->session.dest_dir = path;
}

}  // namespace uhdr_repack

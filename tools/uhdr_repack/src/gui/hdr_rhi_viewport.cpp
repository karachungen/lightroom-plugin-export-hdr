#include "gui/hdr_rhi_viewport.h"

#include "color_primaries.h"
#include "half_float.h"

#include <QFile>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace uhdr_repack {

namespace {

QShader load_shader(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

QString format_name(QRhiSwapChain::Format format) {
  switch (format) {
    case QRhiSwapChain::HDRExtendedSrgbLinear:
      return QStringLiteral("Extended linear sRGB (16F)");
    case QRhiSwapChain::HDR10:
      return QStringLiteral("HDR10 PQ");
    case QRhiSwapChain::HDRExtendedDisplayP3Linear:
      return QStringLiteral("Extended linear Display P3 (16F)");
    case QRhiSwapChain::SDR:
      return QStringLiteral("SDR fallback");
  }
  return QStringLiteral("Unknown");
}

float srgb_to_linear(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f
                           : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
  value = std::max(value, 0.0f);
  return value <= 0.0031308f ? value * 12.92f
                             : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float tone_map(float value) {
  if (value <= 1.0f) return std::max(value, 0.0f);
  const float excess = value - 1.0f;
  return 1.0f + excess / (1.0f + excess);
}

struct alignas(16) PreviewUniforms {
  float mode_boost[4];
  float display[4];
  float gain_range[4];
  float view[4];
  float color[4];
};

}  // namespace

class HdrRhiViewport::Impl {
 public:
  explicit Impl(HdrRhiViewport* owner) : q(owner) {}

  ~Impl() { releaseAll(); }

  void init() {
    if (initialized) return;

#if defined(Q_OS_WIN)
    implementation = QRhi::D3D11;
    q->setSurfaceType(QSurface::Direct3DSurface);
    QRhiD3D11InitParams params;
    rhi.reset(QRhi::create(implementation, &params));
    viewport_status.backend = QStringLiteral("Direct3D 11");
#elif QT_CONFIG(metal)
    implementation = QRhi::Metal;
    q->setSurfaceType(QSurface::MetalSurface);
    QRhiMetalInitParams params;
    rhi.reset(QRhi::create(implementation, &params));
    viewport_status.backend = QStringLiteral("Metal");
#else
    implementation = QRhi::OpenGLES2;
    q->setSurfaceType(QSurface::OpenGLSurface);
    fallback_surface.reset(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = fallback_surface.get();
    params.window = q;
    rhi.reset(QRhi::create(implementation, &params));
    viewport_status.backend = QStringLiteral("OpenGL");
#endif

    if (!rhi) {
      viewport_status.error = QStringLiteral("Could not initialize the platform graphics backend");
      emit q->statusChanged(viewport_status);
      return;
    }

    swapchain.reset(rhi->newSwapChain());
    swapchain->setWindow(q);

    QRhiSwapChain::Format selected = QRhiSwapChain::SDR;
#if defined(Q_OS_MACOS)
    const std::array preferred = {QRhiSwapChain::HDRExtendedSrgbLinear,
                                  QRhiSwapChain::HDRExtendedDisplayP3Linear};
#else
    const std::array preferred = {QRhiSwapChain::HDRExtendedSrgbLinear,
                                  QRhiSwapChain::HDR10};
#endif
    for (const auto candidate : preferred) {
      if (swapchain->isFormatSupported(candidate)) {
        selected = candidate;
        break;
      }
    }

    hdr_active = selected != QRhiSwapChain::SDR;
    swapchain_format = selected;
    swapchain->setFormat(selected);
    if (!hdr_active) swapchain->setFlags(QRhiSwapChain::sRGB);

    render_pass.reset(swapchain->newCompatibleRenderPassDescriptor());
    swapchain->setRenderPassDescriptor(render_pass.get());

    viewport_status.initialized = true;
    viewport_status.hdr_active = hdr_active;
    viewport_status.swapchain_format = format_name(selected);
    if (!hdr_active) {
      viewport_status.error =
          QStringLiteral("HDR output is unavailable on this display; showing tone-mapped SDR");
    }

    initialized = true;
    createResources();
    resizeSwapChain();
    emit q->statusChanged(viewport_status);
  }

  void createResources() {
    if (!rhi || !render_pass) return;

    uniform_buffer.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(PreviewUniforms)));
    uniform_buffer->create();

    sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler->create();

    ensureTexture(sdr_texture, QRhiTexture::RGBA8, QSize(1, 1));
    ensureTexture(gain_texture, QRhiTexture::R16F, QSize(1, 1));
    ensureTexture(final_texture, QRhiTexture::RGBA16F, QSize(1, 1));

    bindings.reset(rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniform_buffer.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, sdr_texture.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, gain_texture.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, final_texture.get(), sampler.get()),
    });
    bindings->create();

    pipeline.reset(rhi->newGraphicsPipeline());
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex,
         load_shader(QStringLiteral(":/hdr/shaders/hdr_preview.vert.qsb"))},
        {QRhiShaderStage::Fragment,
         load_shader(QStringLiteral(":/hdr/shaders/hdr_preview.frag.qsb"))},
    });
    pipeline->setVertexInputLayout({});
    pipeline->setShaderResourceBindings(bindings.get());
    pipeline->setRenderPassDescriptor(render_pass.get());
    if (!pipeline->create()) {
      viewport_status.error = QStringLiteral("Could not create the HDR preview shader pipeline");
      emit q->statusChanged(viewport_status);
    }
    textures_dirty = true;
  }

  void ensureTexture(std::unique_ptr<QRhiTexture>& texture, QRhiTexture::Format format,
                     const QSize& size) {
    const QSize valid_size(std::max(1, size.width()), std::max(1, size.height()));
    if (!texture) {
      texture.reset(rhi->newTexture(format, valid_size));
    } else if (texture->pixelSize() == valid_size) {
      return;
    } else {
      texture->setPixelSize(valid_size);
    }
    texture->create();
  }

  void resizeSwapChain() {
    if (!swapchain) return;
    has_swapchain = swapchain->createOrResize();
    newly_exposed = false;
  }

  void releaseSwapChain() {
    if (has_swapchain && swapchain) {
      has_swapchain = false;
      swapchain->destroy();
    }
  }

  void releaseAll() {
    releaseSwapChain();
    pipeline.reset();
    bindings.reset();
    sampler.reset();
    final_texture.reset();
    gain_texture.reset();
    sdr_texture.reset();
    uniform_buffer.reset();
    render_pass.reset();
    swapchain.reset();
    rhi.reset();
#if QT_CONFIG(opengl)
    fallback_surface.reset();
#endif
    initialized = false;
  }

  void uploadTextures(QRhiResourceUpdateBatch* updates) {
    if (!textures_dirty || !updates) return;

    QImage upload = sdr_image.isNull()
                        ? QImage(1, 1, QImage::Format_RGBA8888)
                        : sdr_image.convertToFormat(QImage::Format_RGBA8888);
    if (sdr_image.isNull()) upload.fill(QColor(10, 12, 17));
    ensureTexture(sdr_texture, QRhiTexture::RGBA8, upload.size());
    updates->uploadTexture(sdr_texture.get(), upload);

    QSize gain_size(1, 1);
    QByteArray gain_bytes(sizeof(uint16_t), '\0');
    if (gain_w > 0 && gain_h > 0 &&
        gain.size() == static_cast<size_t>(gain_w) * static_cast<size_t>(gain_h)) {
      gain_size = QSize(gain_w, gain_h);
      gain_bytes.resize(static_cast<qsizetype>(gain.size() * sizeof(uint16_t)));
      auto* dst = reinterpret_cast<uint16_t*>(gain_bytes.data());
      for (size_t i = 0; i < gain.size(); ++i) dst[i] = float_to_half(gain[i]);
    } else {
      *reinterpret_cast<uint16_t*>(gain_bytes.data()) = float_to_half(1.0f);
    }
    ensureTexture(gain_texture, QRhiTexture::R16F, gain_size);
    updates->uploadTexture(
        gain_texture.get(),
        QRhiTextureUploadDescription(
            {{0, 0, QRhiTextureSubresourceUploadDescription(gain_bytes)}}));

    QSize final_size(1, 1);
    QByteArray final_bytes(4 * static_cast<int>(sizeof(uint16_t)), '\0');
    if (final_w > 0 && final_h > 0 &&
        final_half.size() ==
            static_cast<size_t>(final_w) * static_cast<size_t>(final_h) * 4u) {
      final_size = QSize(final_w, final_h);
      final_bytes = QByteArray(reinterpret_cast<const char*>(final_half.data()),
                               static_cast<qsizetype>(final_half.size() * sizeof(uint16_t)));
    }
    ensureTexture(final_texture, QRhiTexture::RGBA16F, final_size);
    updates->uploadTexture(
        final_texture.get(),
        QRhiTextureUploadDescription(
            {{0, 0, QRhiTextureSubresourceUploadDescription(final_bytes)}}));

    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniform_buffer.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, sdr_texture.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, gain_texture.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, final_texture.get(), sampler.get()),
    });
    bindings->create();
    textures_dirty = false;
  }

  void updateHdrInfo() {
    if (!swapchain || !hdr_active) return;
    const QRhiSwapChainHdrInfo info = swapchain->hdrInfo();
    viewport_status.sdr_white_level = info.sdrWhiteLevel;
    if (info.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred) {
      viewport_status.luminance_behavior = QStringLiteral("Scene-referred");
      sdr_white_scale = std::max(1.0f, info.sdrWhiteLevel / 80.0f);
    } else {
      viewport_status.luminance_behavior = QStringLiteral("Display-referred");
      sdr_white_scale = 1.0f;
    }
    if (info.limitsType == QRhiSwapChainHdrInfo::ColorComponentValue) {
      headroom = std::max(1.0f, info.limits.colorComponentValue.maxColorComponentValue);
      viewport_status.max_color_component = headroom;
    } else {
      headroom =
          std::max(1.0f, info.limits.luminanceInNits.maxLuminance /
                             std::max(info.sdrWhiteLevel, 80.0f));
      viewport_status.max_color_component = headroom;
    }
  }

  void render() {
    if (!has_swapchain || not_exposed || !pipeline) return;
    if (swapchain->currentPixelSize() != swapchain->surfacePixelSize() || newly_exposed) {
      resizeSwapChain();
      if (!has_swapchain) return;
    }

    QRhi::FrameOpResult result = rhi->beginFrame(swapchain.get());
    if (result == QRhi::FrameOpSwapChainOutOfDate) {
      resizeSwapChain();
      if (!has_swapchain) return;
      result = rhi->beginFrame(swapchain.get());
    }
    if (result != QRhi::FrameOpSuccess) {
      q->requestUpdate();
      return;
    }

    if (first_frame) {
      updateHdrInfo();
      first_frame = false;
      emit q->statusChanged(viewport_status);
    }

    auto* updates = rhi->nextResourceUpdateBatch();
    uploadTextures(updates);

    const QSize output = swapchain->currentPixelSize();
    const QSize image_size =
        mode == PreviewMode::kFinalHdr && final_w > 0 ? QSize(final_w, final_h) : sdr_image.size();
    PreviewUniforms uniforms{};
    uniforms.mode_boost[0] = static_cast<float>(mode);
    uniforms.mode_boost[1] = min_boost;
    uniforms.mode_boost[2] = max_boost;
    uniforms.mode_boost[3] = std::max(1.0f, target_peak_nits / 203.0f);
    uniforms.display[0] =
        image_size.height() > 0 ? static_cast<float>(image_size.width()) / image_size.height() : 1;
    uniforms.display[1] =
        output.height() > 0 ? static_cast<float>(output.width()) / output.height() : 1;
    uniforms.display[2] = headroom;
    uniforms.display[3] = sdr_white_scale;
    uniforms.gain_range[0] = visualization_min;
    uniforms.gain_range[1] = visualization_max;
    uniforms.gain_range[2] = hdr_active ? 0.0f : 1.0f;
    uniforms.gain_range[3] = final_half.empty() ? 0.0f : 1.0f;
    uniforms.view[0] = zoom;
    uniforms.view[1] = pan.x();
    uniforms.view[2] = -pan.y();
    uniforms.view[3] =
        swapchain_format == QRhiSwapChain::HDRExtendedDisplayP3Linear ? 1.0f : 0.0f;
    uniforms.color[0] = static_cast<float>(display_gamut_id(final_gamut));
    uniforms.color[1] = 0.0f;
    uniforms.color[2] = 0.0f;
    uniforms.color[3] = 0.0f;
    updates->updateDynamicBuffer(uniform_buffer.get(), 0, sizeof(uniforms), &uniforms);

    QRhiCommandBuffer* cb = swapchain->currentFrameCommandBuffer();
    cb->beginPass(swapchain->currentFrameRenderTarget(), QColor(7, 8, 11), {1.0f, 0}, updates);
    cb->setGraphicsPipeline(pipeline.get());
    cb->setViewport({0, 0, static_cast<float>(output.width()), static_cast<float>(output.height())});
    cb->setShaderResources(bindings.get());
    cb->draw(3);
    cb->endPass();

    rhi->endFrame(swapchain.get());
  }

  QImage fallbackImage() const {
    if (sdr_image.isNull()) return {};
    QImage out = sdr_image.convertToFormat(QImage::Format_RGB32);
    if (mode == PreviewMode::kSdr) return out;

    for (int y = 0; y < out.height(); ++y) {
      auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
      for (int x = 0; x < out.width(); ++x) {
        const int gx = gain_w > 0 ? std::clamp(x * gain_w / out.width(), 0, gain_w - 1) : 0;
        const int gy = gain_h > 0 ? std::clamp(y * gain_h / out.height(), 0, gain_h - 1) : 0;
        const size_t gain_index = static_cast<size_t>(gy) * static_cast<size_t>(gain_w) + gx;
        const float gain_value =
            gain_index < gain.size() ? std::clamp(gain[gain_index], min_boost, max_boost) : 1.0f;
        if (mode == PreviewMode::kGainMap) {
          const float lo = std::log(std::max(visualization_min, 0.0001f));
          const float hi = std::log(std::max(visualization_max, visualization_min + 0.0001f));
          const float t = std::clamp((std::log(std::max(gain_value, visualization_min)) - lo) /
                                         std::max(hi - lo, 0.0001f),
                                     0.0f, 1.0f);
          line[x] = qRgb(static_cast<int>(255 * t),
                         static_cast<int>(220 * (1.0f - std::abs(2.0f * t - 1.0f))),
                         static_cast<int>(255 * (1.0f - t)));
          continue;
        }

        std::array<float, 3> rgb{};
        if (mode == PreviewMode::kFinalHdr && !final_half.empty() && final_w > 0 && final_h > 0) {
          const int fx = std::clamp(x * final_w / out.width(), 0, final_w - 1);
          const int fy = std::clamp(y * final_h / out.height(), 0, final_h - 1);
          const size_t fi = (static_cast<size_t>(fy) * final_w + fx) * 4u;
          rgb = {half_to_float(final_half[fi]), half_to_float(final_half[fi + 1]),
                 half_to_float(final_half[fi + 2])};
          const LinearRgb mapped =
              decoded_hdr_to_linear_srgb({rgb[0], rgb[1], rgb[2]}, final_gamut);
          rgb = {mapped.r, mapped.g, mapped.b};
        } else {
          const float display_boost = std::max(1.0f, target_peak_nits / 203.0f);
          const float strength = std::clamp(
              std::log(std::max(max_boost, 1.01f)) / std::log(1000.0f), 0.0f, 1.25f);
          const float applied =
              1.0f + (std::min(gain_value, display_boost) - 1.0f) * strength;
          const QRgb pixel = line[x];
          rgb = {srgb_to_linear(qRed(pixel) / 255.0f) * applied,
                 srgb_to_linear(qGreen(pixel) / 255.0f) * applied,
                 srgb_to_linear(qBlue(pixel) / 255.0f) * applied};
        }
        line[x] = qRgb(static_cast<int>(255 * std::clamp(linear_to_srgb(tone_map(rgb[0])), 0.0f, 1.0f)),
                       static_cast<int>(255 * std::clamp(linear_to_srgb(tone_map(rgb[1])), 0.0f, 1.0f)),
                       static_cast<int>(255 * std::clamp(linear_to_srgb(tone_map(rgb[2])), 0.0f, 1.0f)));
      }
    }
    return out;
  }

  HdrRhiViewport* q;
  QRhi::Implementation implementation = QRhi::Null;
  QRhiSwapChain::Format swapchain_format = QRhiSwapChain::SDR;
#if QT_CONFIG(opengl)
  std::unique_ptr<QOffscreenSurface> fallback_surface;
#endif
  std::unique_ptr<QRhi> rhi;
  std::unique_ptr<QRhiSwapChain> swapchain;
  std::unique_ptr<QRhiRenderPassDescriptor> render_pass;
  std::unique_ptr<QRhiBuffer> uniform_buffer;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiTexture> sdr_texture;
  std::unique_ptr<QRhiTexture> gain_texture;
  std::unique_ptr<QRhiTexture> final_texture;
  std::unique_ptr<QRhiShaderResourceBindings> bindings;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline;

  QImage sdr_image;
  std::vector<float> gain;
  std::vector<uint16_t> final_half;
  int gain_w = 0;
  int gain_h = 0;
  int final_w = 0;
  int final_h = 0;
  int final_gamut = 0;
  PreviewMode mode = PreviewMode::kLiveHdr;
  float min_boost = 1.0f;
  float max_boost = 1000.0f;
  float target_peak_nits = 1000.0f;
  float visualization_min = 1.0f;
  float visualization_max = 32.0f;
  float headroom = 1.0f;
  float sdr_white_scale = 1.0f;
  float zoom = 1.0f;
  QPointF pan;
  QPoint last_pointer;
  bool panning = false;
  bool initialized = false;
  bool has_swapchain = false;
  bool not_exposed = false;
  bool newly_exposed = false;
  bool textures_dirty = true;
  bool first_frame = true;
  bool hdr_active = false;
  HdrViewportStatus viewport_status;
};

HdrRhiViewport::HdrRhiViewport(QWindow* parent) : QWindow(parent), d_(std::make_unique<Impl>(this)) {
  setTitle(tr("HDR image viewport"));
#if defined(Q_OS_WIN)
  setSurfaceType(QSurface::Direct3DSurface);
#elif QT_CONFIG(metal)
  setSurfaceType(QSurface::MetalSurface);
#else
  setSurfaceType(QSurface::OpenGLSurface);
#endif
  connect(this, &QWindow::screenChanged, this, [this] {
    if (!d_->initialized) return;
    d_->releaseSwapChain();
    d_->first_frame = true;
    d_->newly_exposed = true;
    d_->resizeSwapChain();
    requestUpdate();
  });
}

HdrRhiViewport::~HdrRhiViewport() = default;

void HdrRhiViewport::setSdrImage(const QImage& image) {
  d_->sdr_image = image;
  d_->textures_dirty = true;
  requestUpdate();
}

void HdrRhiViewport::setGainMap(const std::vector<float>& gain, int width, int height) {
  d_->gain = gain;
  d_->gain_w = width;
  d_->gain_h = height;
  d_->textures_dirty = true;
  requestUpdate();
}

void HdrRhiViewport::setFinalHdr(const std::vector<uint16_t>& rgba_half, int width, int height,
                                 int color_gamut) {
  d_->final_half = rgba_half;
  d_->final_w = width;
  d_->final_h = height;
  d_->final_gamut = color_gamut;
  d_->textures_dirty = true;
  requestUpdate();
}

void HdrRhiViewport::clearFinalHdr() {
  d_->final_half.clear();
  d_->final_w = 0;
  d_->final_h = 0;
  d_->final_gamut = 0;
  d_->textures_dirty = true;
  requestUpdate();
}

void HdrRhiViewport::setPreviewMode(PreviewMode mode) {
  d_->mode = mode;
  requestUpdate();
}

void HdrRhiViewport::setContentBoost(float min_boost, float max_boost) {
  d_->min_boost = std::max(0.0001f, min_boost);
  d_->max_boost = std::max(d_->min_boost, max_boost);
  requestUpdate();
}

void HdrRhiViewport::setTargetDisplayPeak(float nits) {
  d_->target_peak_nits = std::max(80.0f, nits);
  requestUpdate();
}

void HdrRhiViewport::setGainVisualizationRange(float min_gain, float max_gain) {
  d_->visualization_min = std::max(0.0001f, min_gain);
  d_->visualization_max = std::max(d_->visualization_min + 0.0001f, max_gain);
  requestUpdate();
}

void HdrRhiViewport::setZoom(float zoom) {
  d_->zoom = std::clamp(zoom, 0.25f, 16.0f);
  requestUpdate();
}

void HdrRhiViewport::fitToView() {
  d_->zoom = 1.0f;
  d_->pan = {};
  requestUpdate();
}

void HdrRhiViewport::showActualPixels() {
  const QSize image_size =
      d_->mode == PreviewMode::kFinalHdr && d_->final_w > 0 ? QSize(d_->final_w, d_->final_h)
                                                             : d_->sdr_image.size();
  if (image_size.isEmpty() || size().isEmpty()) return;
  const float fit_scale =
      std::min(static_cast<float>(width()) / image_size.width(),
               static_cast<float>(height()) / image_size.height());
  d_->zoom = std::clamp(1.0f / std::max(fit_scale, 0.0001f), 0.25f, 16.0f);
  d_->pan = {};
  requestUpdate();
}

PreviewMode HdrRhiViewport::previewMode() const {
  return d_->mode;
}

HdrViewportStatus HdrRhiViewport::status() const {
  return d_->viewport_status;
}

QImage HdrRhiViewport::renderSdrFallback() const {
  return d_->fallbackImage();
}

void HdrRhiViewport::releaseSwapChain() {
  d_->releaseSwapChain();
}

void HdrRhiViewport::exposeEvent(QExposeEvent*) {
  if (isExposed() && !d_->initialized) d_->init();
  if (isExposed() && d_->initialized && d_->swapchain && !d_->has_swapchain) {
    d_->not_exposed = false;
    d_->newly_exposed = true;
    d_->first_frame = true;
    d_->resizeSwapChain();
  }
  const QSize surface_size =
      (d_->has_swapchain && d_->swapchain) ? d_->swapchain->surfacePixelSize() : QSize();
  if ((!isExposed() || surface_size.isEmpty()) && d_->initialized) d_->not_exposed = true;
  if (isExposed() && d_->initialized && d_->not_exposed && !surface_size.isEmpty()) {
    d_->not_exposed = false;
    d_->newly_exposed = true;
  }
  if (isExposed() && d_->has_swapchain && !surface_size.isEmpty()) d_->render();
}

bool HdrRhiViewport::event(QEvent* event) {
  if (event->type() == QEvent::UpdateRequest) {
    d_->render();
    return true;
  }
  if (event->type() == QEvent::PlatformSurface) {
    auto* surface_event = static_cast<QPlatformSurfaceEvent*>(event);
    if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
      d_->releaseSwapChain();
      d_->newly_exposed = true;
    }
  }
  return QWindow::event(event);
}

void HdrRhiViewport::wheelEvent(QWheelEvent* event) {
  const float factor = event->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
  setZoom(d_->zoom * factor);
  event->accept();
}

void HdrRhiViewport::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) {
    d_->panning = true;
    d_->last_pointer = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
  }
}

void HdrRhiViewport::mouseMoveEvent(QMouseEvent* event) {
  if (!d_->panning || width() <= 0 || height() <= 0) return;
  const QPoint delta = event->pos() - d_->last_pointer;
  d_->last_pointer = event->pos();
  d_->pan += QPointF(static_cast<float>(delta.x()) / width(),
                     static_cast<float>(delta.y()) / height());
  requestUpdate();
  event->accept();
}

void HdrRhiViewport::mouseReleaseEvent(QMouseEvent* event) {
  if (d_->panning) {
    d_->panning = false;
    unsetCursor();
    event->accept();
  }
}

}  // namespace uhdr_repack

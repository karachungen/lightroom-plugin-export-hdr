(function () {
  "use strict";

  const SETTINGS_KEYS = [
    "baseQuality",
    "gainmapQuality",
    "gainmapScale",
    "minBoost",
    "maxBoost",
    "displayPeak",
    "monochromeGainmap",
    "sliceAspect",
    "sliceCount",
    "cropOffset",
    "previewSlice",
    "outputWidth",
    "outputHeight",
  ];

  const IG_OUTPUT = {
    none: null,
    "3x4": [1080, 1440],
    "4x5": [1080, 1350],
    "1x1": [1080, 1080],
    "191x100": [1080, 566],
  };

  const IG_RATIO = {
    "1x1": [1, 1],
    "4x5": [4, 5],
    "3x4": [3, 4],
    "191x100": [191, 100],
  };

  const IG_GALLERY_MAX = 20;
  const SETTINGS_DEBOUNCE_MS = 100;
  const IG_UPLOAD_MAX_BYTES = 8 * 1024 * 1024;
  const COLOR_MAP = {
    baseQuality: 95,
    gainmapQuality: 95,
    gainmapScale: 1,
    minBoost: 1,
    maxBoost: 16,
    displayPeak: 3250,
    monochromeGainmap: false,
  };
  const MONO_MAP = {
    baseQuality: 95,
    gainmapQuality: 95,
    gainmapScale: 2,
    minBoost: 1,
    maxBoost: 4.92,
    displayPeak: 1000,
    monochromeGainmap: true,
  };

  const state = {
    items: [],
    currentId: null,
    mode: "sdr",
    encodePreset: "color",
    settings: {
      baseQuality: COLOR_MAP.baseQuality,
      gainmapQuality: COLOR_MAP.gainmapQuality,
      gainmapScale: COLOR_MAP.gainmapScale,
      minBoost: COLOR_MAP.minBoost,
      maxBoost: COLOR_MAP.maxBoost,
      displayPeak: COLOR_MAP.displayPeak,
      monochromeGainmap: COLOR_MAP.monochromeGainmap,
      sliceAspect: "none",
      sliceCount: 1,
      cropOffset: 0.5,
      previewSlice: 1,
      outputWidth: 0,
      outputHeight: 0,
    },
    sdrImage: null,
    imageWidth: 0,
    imageHeight: 0,
    encodedWidth: 0,
    encodedHeight: 0,
    encodedBytes: 0,
    heatmapImage: null,
    gainmapEncoded: false,
    gainmapChannels: "luma",
    slices: [],
    sliceError: "",
    maxSliceCount: 0,
    cropMeta: { axis: "x", slack: 0 },
    drag: null,
    dragRaf: 0,
    settingsTimer: 0,
    lastPreviewRect: null,
    galleryCache: { max: -1, preset: "", chosen: -1 },
    overlayCache: { key: "", sdr: null, count: 0 },
    canvasScale: { x: 1, y: 1, offsetX: 0, offsetY: 0 },
    aspectPicked: {},
    sizeDrag: "",
    loaded: false,
    loadPhase: "",
    loadLabel: "",
    loadStarted: 0,
    loadTicker: 0,
  };

  const PHASE_LABEL = {
    render: "HDR TIFF · waiting on Lightroom",
    wait_tiff: "HDR TIFF · waiting on Lightroom",
    gainmap: "Gain map · computing",
    encode: "Preview encode",
    decode: "Preview decode",
  };

  const canvas = document.getElementById("preview-canvas");
  const ctx = canvas.getContext("2d");
  const filmstrip = document.getElementById("filmstrip");
  const sliceOverlay = document.getElementById("slice-overlay");
  const sliceThumbs = document.getElementById("slice-thumbs");
  const sliceHint = document.getElementById("slice-hint");
  const galleryPicker = document.getElementById("gallery-picker");
  const galleryNotice = document.getElementById("gallery-notice");
  const galleryCounts = document.getElementById("gallery-counts");

  function post(msg) {
    const payload = JSON.stringify(msg);
    if (window.uhdrNative && window.uhdrNative.post) {
      window.uhdrNative.post(payload);
    } else if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.uhdrNative) {
      window.webkit.messageHandlers.uhdrNative.postMessage(payload);
    } else {
      console.log("native>", payload);
    }
  }

  window.uhdrReceive = function (jsonText) {
    let msg = jsonText;
    if (typeof jsonText === "string") {
      try {
        msg = JSON.parse(jsonText);
      } catch (e) {
        return;
      }
    }
    if (!msg || typeof msg !== "object") return;
    handleNative(msg);
  };

  function includedCount() {
    return state.items.filter((item) => item.skipped !== true).length;
  }

  function updateCounts() {
    const n = state.items.length;
    const inc = includedCount();
    document.getElementById("queue-count").textContent = n === 1 ? "1 photo" : `${n} photos`;
    document.getElementById("apply-btn").textContent =
      inc === 1 ? "Encode 1 photo" : `Encode ${inc} photos`;
    const syncBtn = document.getElementById("sync-others");
    if (syncBtn) syncBtn.disabled = n < 2;
  }

  function aspectQueueLabel(preset) {
    if (!preset || preset === "none") return "Original";
    return String(preset).replace("x", ":");
  }

  function formatQueueMeta(item) {
    if (!item) return "";
    const label = aspectQueueLabel(item.sliceAspect);
    const w = Number(item.outputWidth) || 0;
    const h = Number(item.outputHeight) || 0;
    if (w >= 2 && h >= 2) return `${label} · ${w}×${h}`;
    return label;
  }

  function applyQueueMetaItems(items) {
    if (!Array.isArray(items)) return;
    items.forEach((patch) => {
      if (!patch || !patch.id) return;
      const item = state.items.find((x) => x.id === patch.id);
      if (!item) return;
      if (patch.sliceAspect !== undefined) item.sliceAspect = patch.sliceAspect;
      if (patch.outputWidth !== undefined) item.outputWidth = Number(patch.outputWidth) || 0;
      if (patch.outputHeight !== undefined) item.outputHeight = Number(patch.outputHeight) || 0;
    });
    renderFilmstrip();
  }

  function updateCurrentQueueMetaFromInspector() {
    const item = state.items.find((x) => x.id === state.currentId);
    if (!item) return;
    item.sliceAspect = state.settings.sliceAspect || "none";
    const resolved = resolveOutputSize();
    if (resolved) {
      item.outputWidth = resolved.w;
      item.outputHeight = resolved.h;
    }
    const selectorId =
      typeof CSS !== "undefined" && CSS.escape ? CSS.escape(item.id) : item.id.replace(/"/g, "");
    const meta = filmstrip.querySelector(`li[data-id="${selectorId}"] .meta`);
    if (meta) meta.textContent = formatQueueMeta(item);
  }

  function setDestField(path) {
    const el = document.getElementById("dest-dir");
    if (!el) return;
    if (document.activeElement === el) return;
    el.value = path || "";
  }

  function commitDestDir() {
    const el = document.getElementById("dest-dir");
    if (!el) return;
    post({ type: "setDestDir", path: el.value.trim() });
  }

  function handleNative(msg) {
    switch (msg.type) {
      case "session":
        state.items = msg.items || [];
        if (msg.destDir) setDestField(msg.destDir);
        renderFilmstrip();
        updateCounts();
        if (state.items.length) selectItem(state.items[0].id, false);
        reportPreviewRect();
        break;
      case "itemLoading":
        if (msg.id !== state.currentId) return;
        setSdrLoadOverlay(true, msg);
        break;
      case "itemReady":
        updateItemThumb(msg.id, msg.thumb || msg.sdrDataUrl);
        if (msg.id !== state.currentId) return;
        state.loaded = true;
        document.body.classList.remove("is-loading");
        setSdrLoadOverlay(false);
        if (msg.sliceAspect !== undefined) state.settings.sliceAspect = msg.sliceAspect;
        if (msg.cropOffset !== undefined) state.settings.cropOffset = Number(msg.cropOffset);
        if (msg.sliceCount !== undefined) state.settings.sliceCount = Number(msg.sliceCount);
        if (msg.previewSlice !== undefined) state.settings.previewSlice = Number(msg.previewSlice);
        if (msg.outputWidth !== undefined) state.settings.outputWidth = Number(msg.outputWidth) || 0;
        if (msg.outputHeight !== undefined) state.settings.outputHeight = Number(msg.outputHeight) || 0;
        if (msg.imageWidth !== undefined) state.imageWidth = Number(msg.imageWidth) || 0;
        if (msg.imageHeight !== undefined) state.imageHeight = Number(msg.imageHeight) || 0;
        const autoAspect = maybeAutoSelectAspect(!!msg.hasSliceOverride);
        syncIgPresets();
        loadImages(msg);
        syncSizeSliders();
        updateOutputSize();
        if (autoAspect) commitInstagramSettings();
        break;
      case "itemFailed":
        if (msg.id && msg.id !== state.currentId) return;
        document.body.classList.remove("is-loading");
        setSdrLoadOverlay(false);
        document.getElementById("status-text").textContent = msg.text || "Could not load image";
        break;
      case "heatmap":
        if (msg.id !== state.currentId) return;
        applyGainmapMessage(msg);
        break;
      case "slices":
        if (msg.id !== state.currentId) return;
        if (state.drag) break;
        state.slices = msg.rects || [];
        state.sliceError = msg.error || "";
        if (msg.cropOffset !== undefined) state.settings.cropOffset = Number(msg.cropOffset);
        if (msg.sliceCount !== undefined) state.settings.sliceCount = Number(msg.sliceCount);
        if (msg.maxSliceCount !== undefined) state.maxSliceCount = Number(msg.maxSliceCount);
        if (msg.previewSlice !== undefined) state.settings.previewSlice = Number(msg.previewSlice);
        if (msg.outputWidth !== undefined) state.settings.outputWidth = Number(msg.outputWidth) || 0;
        if (msg.outputHeight !== undefined) state.settings.outputHeight = Number(msg.outputHeight) || 0;
        renderSliceOverlay();
        syncSizeSliders();
        updateIgReadout();
        updateOutputSize();
        renderGalleryPicker();
        break;
      case "status":
        if (state.loadStarted) {
          state.loadLabel = msg.text || state.loadLabel;
          refreshLoadStatus();
        } else {
          document.getElementById("status-text").textContent = msg.text || "";
        }
        break;
      case "displayStatus":
        const pill = document.getElementById("display-status");
        pill.textContent = msg.text || "Display";
        pill.dataset.hdr = msg.hdrActive === true ? "true" : msg.hdrActive === false ? "false" : "unknown";
        break;
      case "busy":
        document.getElementById("apply-btn").disabled = !!msg.busy;
        break;
      case "destDir":
        setDestField(msg.path || "");
        break;
      case "queueMeta":
        applyQueueMetaItems(msg.items || []);
        if (msg.syncedCount !== undefined) {
          const n = Number(msg.syncedCount) || 0;
          document.getElementById("status-text").textContent =
            n === 1 ? "Synced to 1 other photo" : `Synced to ${n} other photos`;
        }
        break;
      case "hdrLoading":
        if (msg.loading) {
          state.encodedWidth = 0;
          state.encodedHeight = 0;
          state.encodedBytes = 0;
        } else if (msg.ready === true) {
          state.encodedWidth = Number(msg.encodedWidth) || 0;
          state.encodedHeight = Number(msg.encodedHeight) || 0;
          state.encodedBytes = Number(msg.encodedBytes) || 0;
        }
        setHdrLoading(!!msg.loading, msg.phase);
        if (!msg.loading && msg.ready === true && state.mode === "hdr") {
          document.body.classList.add("mode-hdr-ready");
        }
        updateOutputSize();
        break;
      case "settings":
        applySettings(msg);
        break;
      default:
        break;
    }
  }

  function applyGainmapMessage(msg) {
    state.gainmapEncoded = msg.encoded === true;
    state.gainmapChannels = msg.gainmapChannels === "rgb" ? "rgb" : "luma";
    if (msg.heatmapDataUrl) {
      state.heatmapImage = new Image();
      state.heatmapImage.onload = drawPreview;
      state.heatmapImage.src = msg.heatmapDataUrl;
    } else {
      state.heatmapImage = null;
    }
  }

  function updateItemThumb(id, src) {
    if (!src) return;
    const item = state.items.find((x) => x.id === id);
    if (item) item.thumb = src;
    const selectorId = typeof CSS !== "undefined" && CSS.escape ? CSS.escape(id) : id.replace(/"/g, "");
    const img = filmstrip.querySelector(`li[data-id="${selectorId}"] img`);
    if (img) img.src = src;
    const li = filmstrip.querySelector(`li[data-id="${selectorId}"]`);
    if (li) li.classList.remove("is-loading");
  }

  function renderFilmstrip() {
    const scrollTop = filmstrip.scrollTop;
    filmstrip.innerHTML = "";
    state.items.forEach((item) => {
      const li = document.createElement("li");
      li.dataset.id = item.id;
      if (item.id === state.currentId) li.classList.add("selected");
      if (item.skipped) li.classList.add("is-skipped");
      if (!item.thumb) li.classList.add("is-loading");

      const frame = document.createElement("div");
      frame.className = "frame";
      const img = document.createElement("img");
      img.alt = item.label || item.id;
      if (item.thumb) img.src = item.thumb;
      const skip = document.createElement("button");
      skip.type = "button";
      skip.className = "skip";
      skip.setAttribute("aria-pressed", item.skipped === true ? "true" : "false");
      skip.title = "Skip on encode";
      skip.textContent = item.skipped ? "×" : "✓";
      skip.addEventListener("click", (ev) => {
        ev.stopPropagation();
        item.skipped = !item.skipped;
        post({ type: "setSkipped", id: item.id, skipped: item.skipped });
        li.classList.toggle("is-skipped", item.skipped);
        skip.textContent = item.skipped ? "×" : "✓";
        skip.setAttribute("aria-pressed", item.skipped === true ? "true" : "false");
        updateCounts();
      });
      frame.appendChild(img);
      frame.appendChild(skip);

      const label = document.createElement("div");
      label.className = "label";
      label.textContent = item.label || item.id;
      const meta = document.createElement("div");
      meta.className = "meta";
      meta.textContent = formatQueueMeta(item);

      li.appendChild(frame);
      li.appendChild(label);
      li.appendChild(meta);
      li.addEventListener("click", () => selectItem(item.id, true));
      filmstrip.appendChild(li);
    });
    filmstrip.scrollTop = scrollTop;
  }

  function selectItem(id, notify) {
    state.currentId = id;
    state.loaded = false;
    document.body.classList.add("is-loading");
    renderFilmstrip();
    const item = state.items.find((x) => x.id === id);
    const title = item ? item.label : id;
    document.getElementById("stage-title").textContent = title;
    document.getElementById("photo-title").textContent = title;
    if (item) {
      state.imageWidth = Number(item.imageWidth) || 0;
      state.imageHeight = Number(item.imageHeight) || 0;
      setSdrLoadOverlay(true, {
        width: item.imageWidth,
        height: item.imageHeight,
        bytes: item.bytes,
      });
    } else {
      state.imageWidth = 0;
      state.imageHeight = 0;
      setSdrLoadOverlay(true, {});
    }
    state.encodedWidth = 0;
    state.encodedHeight = 0;
    state.encodedBytes = 0;
    updateOutputSize();
    if (notify) post({ type: "selectItem", id });
  }

  function boostFromSlider(raw) {
    return Number(raw) / 10;
  }

  function formatBoost(n) {
    const v = Number(n) || 0;
    if (Math.abs(v - Math.round(v)) < 0.05) return `${Math.round(v)}×`;
    return `${v.toFixed(1)}×`;
  }

  function mapScaleLabel(scale) {
    const n = Number(scale) || 1;
    if (n === 2) return "½";
    if (n === 1) return "1:1";
    return `1/${n}`;
  }

  function applyProfile(profile) {
    state.settings.baseQuality = profile.baseQuality;
    state.settings.gainmapQuality = profile.gainmapQuality;
    state.settings.gainmapScale = profile.gainmapScale;
    state.settings.minBoost = profile.minBoost;
    state.settings.maxBoost = profile.maxBoost;
    state.settings.displayPeak = profile.displayPeak;
    state.settings.monochromeGainmap = profile.monochromeGainmap;
  }

  function isMonoProfile(s) {
    return (
      !!s.monochromeGainmap &&
      Number(s.gainmapScale) === MONO_MAP.gainmapScale &&
      Math.abs(Number(s.maxBoost) - MONO_MAP.maxBoost) < 0.15
    );
  }

  function isColorProfile(s) {
    if (s.monochromeGainmap) return false;
    if (Number(s.gainmapScale) !== COLOR_MAP.gainmapScale) return false;
    const maxB = Number(s.maxBoost);
    const minB = Number(s.minBoost);
    if (Math.abs(minB - COLOR_MAP.minBoost) > 0.15) return false;
    if (Math.abs(maxB - COLOR_MAP.maxBoost) > 0.15) return false;
    const q = Number(s.baseQuality);
    const g = Number(s.gainmapQuality);
    return (q === 95 && g === 95) || (q === 85 && g === 85);
  }

  function normalizePresetName(name) {
    if (name === "instagram") return "color";
    if (name === "phone") return "mono";
    if (name === "color" || name === "mono" || name === "custom") return name;
    return "";
  }

  function inferEncodePreset(msg) {
    const named = normalizePresetName(msg.encodePreset);
    if (named) return named;
    if (isMonoProfile(state.settings)) return "mono";
    if (isColorProfile(state.settings)) return "color";
    return "custom";
  }

  function applyEncodeControlsToDom() {
    document.getElementById("base-quality").value = state.settings.baseQuality;
    document.getElementById("gainmap-quality").value = state.settings.gainmapQuality;
    document.getElementById("gainmap-scale").value = state.settings.gainmapScale;
    document.getElementById("min-boost").value = Math.round(state.settings.minBoost * 10);
    document.getElementById("max-boost").value = Math.round(state.settings.maxBoost * 10);
    document.getElementById("display-peak").value = state.settings.displayPeak;
    document.getElementById("monochrome-gainmap").checked = !!state.settings.monochromeGainmap;
  }

  function updateExportApplied() {
    const el = document.getElementById("export-applied");
    if (!el) return;
    const names = { color: "Color map", mono: "Mono map", custom: "Custom" };
    const name = names[state.encodePreset] || "Custom";
    const map = state.settings.monochromeGainmap ? "luma" : "RGB";
    el.textContent =
      `${name} · JPEG ${state.settings.baseQuality}/${state.settings.gainmapQuality}` +
      ` · ${map} map ${mapScaleLabel(state.settings.gainmapScale)}` +
      ` · boost ${formatBoost(state.settings.minBoost)}–${formatBoost(state.settings.maxBoost)}` +
      ` · ${Math.round(state.settings.displayPeak)} nits`;
  }

  function syncOutputs() {
    document.getElementById("base-quality-val").textContent = String(state.settings.baseQuality);
    document.getElementById("gainmap-quality-val").textContent = String(state.settings.gainmapQuality);
    document.getElementById("gainmap-scale-val").textContent = String(state.settings.gainmapScale);
    document.getElementById("min-boost-val").textContent = formatBoost(state.settings.minBoost);
    document.getElementById("max-boost-val").textContent = formatBoost(state.settings.maxBoost);
    document.getElementById("display-peak-val").textContent = String(Math.round(state.settings.displayPeak));
    updateExportApplied();
  }

  function syncDeliveryUi() {
    const custom = state.encodePreset === "custom";
    const color = state.encodePreset === "color";
    const mono = state.encodePreset === "mono";
    document.querySelectorAll("#delivery-presets .ig-card").forEach((card) => {
      card.classList.toggle("active", card.dataset.value === state.encodePreset);
    });
    document.getElementById("quality-sliders").hidden = !custom;
    document.getElementById("base-quality").disabled = !custom;
    document.getElementById("gainmap-quality").disabled = !custom;
    ["min-boost", "max-boost", "display-peak", "gainmap-scale"].forEach((id) => {
      document.getElementById(id).disabled = !custom;
    });
    document.getElementById("monochrome-gainmap").disabled = !custom;
    const colorNotice = document.getElementById("color-notice");
    const monoNotice = document.getElementById("mono-notice");
    if (colorNotice) colorNotice.hidden = !color;
    if (monoNotice) monoNotice.hidden = !mono;
    const hdrHint = document.getElementById("hdr-hint");
    if (color) {
      document.getElementById("delivery-hint").textContent =
        "JPEG 95, RGB full-res Display P3 map, 4-stop (16×) boost. Original pixels; Instagram accepts up to 8 MB.";
    } else if (mono) {
      document.getElementById("delivery-hint").textContent =
        "Luma gain map at half resolution, boost capped at 4.9×. Instagram may compress this less than color.";
    } else {
      document.getElementById("delivery-hint").textContent = "Shown in the encoded HDR preview.";
    }
    if (hdrHint) {
      hdrHint.textContent = custom
        ? "Applied on the next HDR encode"
        : `Locked to the ${color ? "Color map" : "Mono map"} delivery preset`;
    }
    updateExportApplied();
  }

  function setEncodePreset(preset, commit) {
    const named = normalizePresetName(preset);
    if (!named) return;
    if (named === state.encodePreset) return;
    state.encodePreset = named;
    if (named === "color") applyProfile(COLOR_MAP);
    else if (named === "mono") applyProfile(MONO_MAP);
    applyEncodeControlsToDom();
    syncOutputs();
    syncDeliveryUi();
    if (commit) commitEncodeSettings(true);
  }

  function applySettings(msg) {
    SETTINGS_KEYS.forEach((key) => {
      if (msg[key] !== undefined) state.settings[key] = msg[key];
    });
    if (msg.cropOffset !== undefined) state.settings.cropOffset = Number(msg.cropOffset);
    if (msg.sliceCount !== undefined) state.settings.sliceCount = Number(msg.sliceCount);
    if (msg.previewSlice !== undefined) state.settings.previewSlice = Number(msg.previewSlice);
    state.encodePreset = inferEncodePreset(msg);
    if (state.encodePreset === "color") applyProfile(COLOR_MAP);
    else if (state.encodePreset === "mono") applyProfile(MONO_MAP);
    applyEncodeControlsToDom();
    syncIgPresets();
    syncSizeSliders();
    syncOutputs();
    syncDeliveryUi();
    updateIgReadout();
    updateOutputSize();
    renderGalleryPicker();
  }

  function evenFloor(v) {
    if (v < 2) return 0;
    return v - (v % 2);
  }

  function nearestIgAspect(w, h) {
    if (w < 2 || h < 2) return "";
    const actual = w / h;
    const cands = [
      ["3x4", 3 / 4],
      ["4x5", 4 / 5],
      ["1x1", 1],
      ["191x100", 191 / 100],
    ];
    for (let i = 0; i < cands.length; i++) {
      const named = cands[i][1];
      if (Math.abs(actual - named) / named <= 0.01) return cands[i][0];
    }
    return "";
  }

  function igAspectSupported(w, h) {
    if (w < 2 || h < 2) return false;
    const r = w / h;
    return r + 0.01 >= 3 / 4 && r - 0.01 <= 191 / 100;
  }

  function cropPixels() {
    const w = evenFloor(Number(state.imageWidth) || 0);
    const h = evenFloor(Number(state.imageHeight) || 0);
    const preset = state.settings.sliceAspect || "none";
    if (preset === "none" || !state.slices.length) return { w, h };
    const r = state.slices[0];
    return {
      w: evenFloor(Math.round(r.w * state.imageWidth)),
      h: evenFloor(Math.round(r.h * state.imageHeight)),
    };
  }

  function heightForWidth(width, cropW, cropH, preset) {
    const spec = IG_RATIO[preset];
    if (spec) return evenFloor(Math.floor((width * spec[1]) / spec[0]));
    if (cropW < 2) return cropH;
    return evenFloor(Math.floor((width * cropH) / cropW));
  }

  function widthForHeight(height, cropW, cropH, preset) {
    const spec = IG_RATIO[preset];
    if (spec) return evenFloor(Math.floor((height * spec[0]) / spec[1]));
    if (cropH < 2) return cropW;
    return evenFloor(Math.floor((height * cropW) / cropH));
  }

  function igBox(preset, scale, crop) {
    const s = scale === 2 ? 2 : 1;
    const ig = IG_OUTPUT[preset];
    if (ig) return { w: evenFloor(ig[0] * s), h: evenFloor(ig[1] * s) };
    const c = crop || { w: 0, h: 0 };
    const w = evenFloor(1080 * s);
    return { w, h: heightForWidth(w, c.w, c.h, preset) };
  }

  function sizeRange() {
    const crop = cropPixels();
    if (crop.w < 2 || crop.h < 2) return null;
    const preset = state.settings.sliceAspect || "none";
    const x1 = igBox(preset, 1, crop);
    const x2 = igBox(preset, 2, crop);
    let minW = x1.w;
    let minH = x1.h;
    if (minW > crop.w || minH > crop.h) {
      minW = crop.w;
      minH = crop.h;
    }
    let maxW = x2.w;
    let maxH = x2.h;
    if (maxW > crop.w || maxH > crop.h) {
      maxW = crop.w;
      maxH = crop.h;
    }
    return { minW, minH, maxW, maxH, crop, x1W: x1.w, x1H: x1.h, x2W: x2.w, x2H: x2.h };
  }

  function smartPick(range) {
    const w = range.maxW;
    const h = range.maxH;
    if (w === range.x2W && h === range.x2H) {
      return { w, h, kind: "2×" };
    }
    return { w, h, kind: "native" };
  }

  function resolveOutputSize() {
    const range = sizeRange();
    if (!range) return null;
    let w = Number(state.settings.outputWidth) || 0;
    let h = Number(state.settings.outputHeight) || 0;
    const preset = state.settings.sliceAspect || "none";
    const pick = smartPick(range);
    if (w < 2 && h < 2) {
      w = pick.w;
      h = pick.h;
    } else if (w >= 2) {
      h = heightForWidth(w, range.crop.w, range.crop.h, preset);
    } else {
      w = widthForHeight(h, range.crop.w, range.crop.h, preset);
    }
    if (w < range.minW) {
      w = range.minW;
      h = heightForWidth(w, range.crop.w, range.crop.h, preset);
    }
    if (h < range.minH) {
      h = range.minH;
      w = widthForHeight(h, range.crop.w, range.crop.h, preset);
    }
    if (w > range.maxW) {
      w = range.maxW;
      h = heightForWidth(w, range.crop.w, range.crop.h, preset);
    }
    if (h > range.maxH) {
      h = range.maxH;
      w = widthForHeight(h, range.crop.w, range.crop.h, preset);
    }
    w = evenFloor(w);
    h = evenFloor(h);
    let kind = "scaled";
    if (w === pick.w && h === pick.h) kind = pick.kind;
    else if (w === range.x2W && h === range.x2H) kind = "2×";
    else if (w === range.x1W && h === range.x1H) kind = "1×";
    else if (w === range.crop.w && h === range.crop.h) kind = "native";
    return { w, h, range, kind, atSmart: w === pick.w && h === pick.h };
  }

  function maybeAutoSelectAspect(hasOverride) {
    const id = state.currentId;
    if (!id || hasOverride || state.aspectPicked[id]) return false;
    const match = nearestIgAspect(state.imageWidth, state.imageHeight);
    if (!match) return false;
    if ((state.settings.sliceAspect || "none") === match) return false;
    state.settings.sliceAspect = match;
    state.settings.outputWidth = 0;
    state.settings.outputHeight = 0;
    state.settings.cropOffset = 0.5;
    state.settings.sliceCount = 1;
    state.settings.previewSlice = 1;
    return true;
  }

  function syncSizeSliders() {
    const widthEl = document.getElementById("out-width");
    const heightEl = document.getElementById("out-height");
    const widthVal = document.getElementById("out-width-val");
    const heightVal = document.getElementById("out-height-val");
    const box = document.getElementById("size-sliders");
    const resolved = resolveOutputSize();
    if (!widthEl || !heightEl || !resolved) {
      if (box) box.hidden = true;
      return;
    }
    const { w, h, range } = resolved;
    const locked = range.maxW <= range.minW && range.maxH <= range.minH;
    if (box) box.hidden = false;
    widthEl.min = String(range.minW);
    widthEl.max = String(range.maxW);
    heightEl.min = String(range.minH);
    heightEl.max = String(range.maxH);
    if (state.sizeDrag !== "width") widthEl.value = String(w);
    if (state.sizeDrag !== "height") heightEl.value = String(h);
    widthEl.disabled = locked;
    heightEl.disabled = locked;
    if (widthVal) {
      widthVal.min = String(range.minW);
      widthVal.max = String(range.maxW);
      widthVal.disabled = locked;
      if (document.activeElement !== widthVal) widthVal.value = String(w);
    }
    if (heightVal) {
      heightVal.min = String(range.minH);
      heightVal.max = String(range.maxH);
      heightVal.disabled = locked;
      if (document.activeElement !== heightVal) heightVal.value = String(h);
    }
    document.querySelectorAll("#ig-presets .ig-card").forEach((card) => {
      const sizeEl = card.querySelector(".ig-size");
      if (!sizeEl) return;
      const key = card.dataset.value;
      const ig = IG_OUTPUT[key];
      if (key === "none") {
        const orig = cropPixels();
        sizeEl.textContent =
          (state.settings.sliceAspect || "none") === "none" && orig.w >= 2
            ? `${orig.w} × ${orig.h}`
            : "1080–2160 wide";
        return;
      }
      if (key === (state.settings.sliceAspect || "none")) {
        sizeEl.textContent = `${w} × ${h}`;
        return;
      }
      if (ig) sizeEl.textContent = `${ig[0]} × ${ig[1]}–${ig[0] * 2} × ${ig[1] * 2}`;
    });
  }

  function updateAspectWarn() {
    const el = document.getElementById("ig-aspect-warn");
    if (!el) return;
    const resolved = resolveOutputSize();
    if (!resolved) {
      el.hidden = true;
      return;
    }
    el.hidden = igAspectSupported(resolved.w, resolved.h);
  }

  function applyOutputSize(axis, rawValue, snap) {
    const range = sizeRange();
    if (!range) return false;
    const preset = state.settings.sliceAspect || "none";
    let value = evenFloor(Number(rawValue));
    if (!Number.isFinite(value) || value < 2) return false;
    let w;
    let h;
    if (axis === "width") {
      w = value;
      [range.x1W, range.x2W].forEach((target) => {
        if (snap && Math.abs(w - target) <= 16 && target >= range.minW && target <= range.maxW) {
          w = target;
        }
      });
      h = heightForWidth(w, range.crop.w, range.crop.h, preset);
    } else {
      h = value;
      [range.x1H, range.x2H].forEach((target) => {
        if (snap && Math.abs(h - target) <= 16 && target >= range.minH && target <= range.maxH) {
          h = target;
        }
      });
      w = widthForHeight(h, range.crop.w, range.crop.h, preset);
    }
    state.settings.outputWidth = w;
    state.settings.outputHeight = h;
    const resolved = resolveOutputSize();
    if (!resolved) return false;
    if (resolved.atSmart) {
      state.settings.outputWidth = 0;
      state.settings.outputHeight = 0;
    } else {
      state.settings.outputWidth = resolved.w;
      state.settings.outputHeight = resolved.h;
    }
    syncSizeSliders();
    updateIgReadout();
    updateOutputSize();
    return true;
  }

  function sizeFieldInRange(axis, value, range) {
    if (axis === "width") return value >= range.minW && value <= range.maxW;
    return value >= range.minH && value <= range.maxH;
  }

  function syncIgPresets() {
    document.querySelectorAll("#ig-presets .ig-card").forEach((card) => {
      card.classList.toggle("active", card.dataset.value === (state.settings.sliceAspect || "none"));
    });
  }

  function updateIgReadout() {
    const el = document.getElementById("ig-readout");
    if (!el) return;
    const resolved = resolveOutputSize();
    const preset = state.settings.sliceAspect || "none";
    const n = state.slices.length;
    const slides = n > 1 ? ` · ${n} slides` : n === 1 && preset !== "none" ? " · 1 slide" : "";
    if (!resolved) {
      el.textContent = "Waiting for photo…";
      updateAspectWarn();
      updateCurrentQueueMetaFromInspector();
      return;
    }
    const label = preset === "none" ? "Original" : preset.replace("x", ":");
    el.textContent = `${label} · ${resolved.w} × ${resolved.h} · ${resolved.kind}${slides}`;
    updateAspectWarn();
    updateCurrentQueueMetaFromInspector();
  }

  function formatFileSize(bytes) {
    const n = Number(bytes) || 0;
    if (n <= 0) return "";
    if (n < 1024) return `${n} B`;
    const kb = n / 1024;
    if (kb < 1024) {
      return kb >= 10 ? `${Math.round(kb)} KB` : `${kb.toFixed(1)} KB`;
    }
    const mb = kb / 1024;
    return mb >= 10 ? `${mb.toFixed(1)} MB` : `${mb.toFixed(2)} MB`;
  }

  function updateOutputSize() {
    const el = document.getElementById("output-size");
    const warn = document.getElementById("output-size-warn");
    if (!el) return;
    const showWarn = (Number(state.encodedBytes) || 0) > IG_UPLOAD_MAX_BYTES;
    if (warn) warn.hidden = !showWarn;
    let text = "";
    const encodedW = Number(state.encodedWidth) || 0;
    const encodedH = Number(state.encodedHeight) || 0;
    if (state.mode === "hdr" && encodedW >= 2 && encodedH >= 2) {
      text = `${encodedW} × ${encodedH}`;
    } else {
      const resolved = resolveOutputSize();
      if (!resolved) {
        el.textContent = "Waiting for photo…";
        if (warn) warn.hidden = true;
        return;
      }
      text = `${resolved.w} × ${resolved.h}`;
    }
    const file = formatFileSize(state.encodedBytes);
    el.textContent = file ? `${text} · ${file}` : text;
  }

  function aspectCss(preset) {
    const spec = IG_RATIO[preset];
    if (!spec) return "1 / 1";
    return `${spec[0]} / ${spec[1]}`;
  }

  function renderGalleryPicker() {
    if (!galleryPicker || !galleryCounts || !galleryNotice) return;
    const preset = state.settings.sliceAspect || "none";
    const max = state.maxSliceCount || 0;
    const show = preset !== "none" && max > 1 && !state.sliceError;
    galleryPicker.hidden = !show;
    if (!show) {
      if (state.galleryCache.max !== 0) {
        galleryCounts.innerHTML = "";
        galleryNotice.textContent = "";
        state.galleryCache = { max: 0, preset: "", chosen: -1 };
      }
      return;
    }
    const label = preset.replace("x", ":");
    const chosen = Math.max(1, Math.min(Number(state.settings.sliceCount) || 1, max));
    galleryNotice.textContent =
      `Fits ${max} matching ${label} slides. Upload as an Instagram gallery.`;
    if (state.galleryCache.max === max && state.galleryCache.preset === preset) {
      galleryCounts.querySelectorAll(".gallery-count").forEach((btn) => {
        const n = Number(btn.dataset.count);
        const active = n === chosen;
        btn.classList.toggle("active", active);
        btn.setAttribute("aria-checked", active ? "true" : "false");
      });
      state.galleryCache.chosen = chosen;
      return;
    }
    galleryCounts.innerHTML = "";
    for (let n = 1; n <= max; n += 1) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "gallery-count" + (n === chosen ? " active" : "");
      btn.setAttribute("role", "radio");
      btn.setAttribute("aria-checked", n === chosen ? "true" : "false");
      btn.dataset.count = String(n);
      const frames = document.createElement("span");
      frames.className = "gallery-count-frames";
      const shown = Math.min(n, 5);
      for (let i = 0; i < shown; i += 1) {
        const frame = document.createElement("span");
        frame.className = "gallery-count-frame";
        frame.style.aspectRatio = aspectCss(preset);
        frames.appendChild(frame);
      }
      const num = document.createElement("span");
      num.className = "gallery-count-n";
      num.textContent = n === 1 ? "1" : String(n);
      btn.appendChild(frames);
      btn.appendChild(num);
      btn.addEventListener("click", () => {
        state.settings.sliceCount = n;
        state.settings.previewSlice = 1;
        commitInstagramSettings();
      });
      galleryCounts.appendChild(btn);
    }
    state.galleryCache = { max, preset, chosen };
  }

  function computeLocalSlices() {
    const spec = IG_RATIO[state.settings.sliceAspect];
    if (!state.sdrImage || !spec) {
      state.cropMeta = { axis: "x", slack: 0 };
      state.maxSliceCount = 0;
      return { rects: [], error: "" };
    }
    const w = evenFloor(state.sdrImage.width);
    const h = evenFloor(state.sdrImage.height);
    const rw = spec[0];
    const rh = spec[1];
    const packH = w * rh >= h * rw;
    let tileW;
    let tileH;
    let maxCount;
    let axis;
    if (packH) {
      tileH = h;
      tileW = evenFloor(Math.floor((tileH * rw) / rh));
      maxCount = tileW < 2 ? 0 : Math.floor(w / tileW);
      axis = "x";
    } else {
      tileW = w;
      tileH = evenFloor(Math.floor((tileW * rh) / rw));
      maxCount = tileH < 2 ? 0 : Math.floor(h / tileH);
      axis = "y";
    }
    if (maxCount > IG_GALLERY_MAX) maxCount = IG_GALLERY_MAX;
    state.maxSliceCount = maxCount;
    if (tileW < 2 || tileH < 2 || maxCount < 1) {
      state.cropMeta = { axis, slack: 0 };
      const label = state.settings.sliceAspect;
      return { rects: [], error: `Image too small for ${label} crop` };
    }
    let count = Number(state.settings.sliceCount);
    if (!Number.isFinite(count)) count = 1;
    if (count === 0) count = maxCount;
    if (count < 1) count = 1;
    if (count > maxCount) count = maxCount;
    state.settings.sliceCount = count;
    const slack = axis === "x" ? w - count * tileW : h - count * tileH;
    state.cropMeta = { axis, slack };
    const offset = Math.min(1, Math.max(0, Number(state.settings.cropOffset) || 0));
    let start = evenFloor(Math.round(slack * offset));
    if (axis === "x") {
      if (start + count * tileW > w) start = evenFloor(w - count * tileW);
    } else if (start + count * tileH > h) {
      start = evenFloor(h - count * tileH);
    }
    const rects = [];
    for (let i = 0; i < count; i += 1) {
      if (axis === "x") {
        rects.push({
          x: (start + i * tileW) / w,
          y: 0,
          w: tileW / w,
          h: tileH / h,
        });
      } else {
        rects.push({
          x: 0,
          y: (start + i * tileH) / h,
          w: tileW / w,
          h: tileH / h,
        });
      }
    }
    return { rects, error: "" };
  }

  function readInspectorSettings() {
    let minBoost = boostFromSlider(document.getElementById("min-boost").value);
    let maxBoost = boostFromSlider(document.getElementById("max-boost").value);
    if (minBoost > maxBoost) {
      const tmp = minBoost;
      minBoost = maxBoost;
      maxBoost = tmp;
    }
    if (state.encodePreset === "color") {
      applyProfile(COLOR_MAP);
    } else if (state.encodePreset === "mono") {
      applyProfile(MONO_MAP);
    } else {
      state.settings.baseQuality = Number(document.getElementById("base-quality").value);
      state.settings.gainmapQuality = Number(document.getElementById("gainmap-quality").value);
      state.settings.gainmapScale = Number(document.getElementById("gainmap-scale").value);
      state.settings.minBoost = minBoost;
      state.settings.maxBoost = maxBoost;
      state.settings.displayPeak = Number(document.getElementById("display-peak").value);
      state.settings.monochromeGainmap = document.getElementById("monochrome-gainmap").checked;
    }
    syncOutputs();
  }

  function postSettings() {
    const payload = { type: "setSettings" };
    SETTINGS_KEYS.forEach((key) => {
      payload[key] = state.settings[key];
    });
    state.encodedWidth = 0;
    state.encodedHeight = 0;
    state.encodedBytes = 0;
    updateOutputSize();
    post(payload);
  }

  function cancelSettingsTimer() {
    if (state.settingsTimer) {
      clearTimeout(state.settingsTimer);
      state.settingsTimer = 0;
    }
  }

  function scheduleSettingsPost() {
    cancelSettingsTimer();
    state.settingsTimer = setTimeout(() => {
      state.settingsTimer = 0;
      readInspectorSettings();
      postSettings();
    }, SETTINGS_DEBOUNCE_MS);
  }

  function commitEncodeSettings(immediate) {
    readInspectorSettings();
    if (immediate) {
      cancelSettingsTimer();
      postSettings();
      return;
    }
    scheduleSettingsPost();
  }

  function commitInstagramSettings() {
    cancelSettingsTimer();
    readInspectorSettings();
    applyPlannedSlices();
    postSettings();
  }

  function applyPlannedSlices() {
    const planned = computeLocalSlices();
    state.slices = planned.rects;
    state.sliceError = planned.error;
    if (state.settings.previewSlice > state.slices.length) {
      state.settings.previewSlice = Math.max(1, state.slices.length);
    }
    renderSliceOverlay();
    syncSizeSliders();
    updateIgReadout();
    updateOutputSize();
    renderGalleryPicker();
  }

  function loadImages(msg) {
    state.overlayCache = { key: "", sdr: null, count: 0 };
    state.lastPreviewRect = null;
    state.sdrImage = new Image();
    state.sdrImage.onload = () => {
      applyPlannedSlices();
      resizeCanvas();
      drawPreview();
      reportPreviewRect();
    };
    state.sdrImage.src = msg.sdrDataUrl;
    applyGainmapMessage(msg);
  }

  function resizeCanvas() {
    const wrap = document.getElementById("preview-frame") || document.getElementById("canvas-wrap");
    const w = wrap.clientWidth;
    const h = Math.max(240, wrap.clientHeight);
    canvas.width = w;
    canvas.height = h;
    if (!state.sdrImage) return;
    const scale = Math.min(w / state.sdrImage.width, h / state.sdrImage.height);
    const dw = state.sdrImage.width * scale;
    const dh = state.sdrImage.height * scale;
    state.canvasScale = {
      x: scale,
      y: scale,
      offsetX: (w - dw) / 2,
      offsetY: (h - dh) / 2,
      dw,
      dh,
    };
  }

  function drawPreview() {
    ctx.fillStyle = "#070605";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    if (!state.sdrImage) return;
    const s = state.canvasScale;
    if (state.mode === "gain" && state.heatmapImage) {
      ctx.globalAlpha = 1;
      ctx.drawImage(state.sdrImage, s.offsetX, s.offsetY, s.dw, s.dh);
      ctx.globalAlpha = 0.76;
      ctx.drawImage(state.heatmapImage, s.offsetX, s.offsetY, s.dw, s.dh);
      ctx.globalAlpha = 1;
    } else if (state.mode !== "hdr" || !document.body.classList.contains("mode-hdr-ready")) {
      ctx.drawImage(state.sdrImage, s.offsetX, s.offsetY, s.dw, s.dh);
    }
    if (state.slices.length && state.settings.sliceAspect !== "none" &&
        (state.mode !== "hdr" || !document.body.classList.contains("mode-hdr-ready"))) {
      const ux = Math.min.apply(null, state.slices.map((r) => r.x));
      const uy = Math.min.apply(null, state.slices.map((r) => r.y));
      const u2 = Math.max.apply(null, state.slices.map((r) => r.x + r.w));
      const v2 = Math.max.apply(null, state.slices.map((r) => r.y + r.h));
      ctx.fillStyle = "rgba(7, 6, 5, 0.55)";
      ctx.fillRect(s.offsetX, s.offsetY, s.dw, uy * s.dh);
      ctx.fillRect(s.offsetX, s.offsetY + v2 * s.dh, s.dw, s.dh - v2 * s.dh);
      ctx.fillRect(s.offsetX, s.offsetY + uy * s.dh, ux * s.dw, (v2 - uy) * s.dh);
      ctx.fillRect(s.offsetX + u2 * s.dw, s.offsetY + uy * s.dh, s.dw - u2 * s.dw, (v2 - uy) * s.dh);
    }
    renderSliceOverlay({
      thumbs: !state.drag,
      reportRect: !state.drag,
    });
  }

  function updateSliceHint() {
    if (!sliceHint) return;
    if (state.sliceError && !/Master dimensions must be even/i.test(state.sliceError)) {
      sliceHint.hidden = false;
      sliceHint.textContent = state.sliceError;
      return;
    }
    sliceHint.hidden = true;
    sliceHint.textContent = "";
  }

  function sliceKey(rects) {
    return rects.map((r) => `${r.x},${r.y},${r.w},${r.h}`).join("|");
  }

  function layoutSliceRects(htmlGuides, selected) {
    const s = state.canvasScale;
    if (!htmlGuides) {
      if (sliceOverlay.childElementCount) sliceOverlay.innerHTML = "";
      return;
    }
    const n = state.slices.length;
    if (sliceOverlay.childElementCount !== n) {
      sliceOverlay.innerHTML = "";
      for (let i = 0; i < n; i += 1) {
        const div = document.createElement("div");
        div.className = "slice-rect";
        div.dataset.index = String(i + 1);
        sliceOverlay.appendChild(div);
      }
    }
    for (let idx = 0; idx < n; idx += 1) {
      const rect = state.slices[idx];
      const div = sliceOverlay.children[idx];
      div.classList.toggle("is-selected", idx + 1 === selected);
      div.style.left = `${s.offsetX + rect.x * s.dw}px`;
      div.style.top = `${s.offsetY + rect.y * s.dh}px`;
      div.style.width = `${rect.w * s.dw}px`;
      div.style.height = `${rect.h * s.dh}px`;
    }
  }

  function paintSliceThumbs(key, selected) {
    const n = state.slices.length;
    const rebuild = sliceThumbs.childElementCount !== n || state.overlayCache.sdr !== state.sdrImage;
    if (rebuild) {
      sliceThumbs.innerHTML = "";
      state.slices.forEach((rect, idx) => {
        const thumb = document.createElement("canvas");
        thumb.addEventListener("click", () => {
          state.settings.previewSlice = idx + 1;
          commitInstagramSettings();
        });
        sliceThumbs.appendChild(thumb);
      });
      state.overlayCache.sdr = state.sdrImage;
      state.overlayCache.key = "";
    }
    if (rebuild || state.overlayCache.key !== key) {
      state.slices.forEach((rect, idx) => {
        const thumb = sliceThumbs.children[idx];
        const tw = Math.max(40, rect.w * state.sdrImage.width * 0.08);
        const th = Math.max(40, rect.h * state.sdrImage.height * 0.08);
        thumb.width = tw;
        thumb.height = th;
        thumb.getContext("2d").drawImage(
          state.sdrImage,
          rect.x * state.sdrImage.width,
          rect.y * state.sdrImage.height,
          rect.w * state.sdrImage.width,
          rect.h * state.sdrImage.height,
          0,
          0,
          tw,
          th
        );
      });
      state.overlayCache.key = key;
      state.overlayCache.count = n;
    }
    for (let idx = 0; idx < n; idx += 1) {
      sliceThumbs.children[idx].classList.toggle("is-selected", idx + 1 === selected);
    }
  }

  function renderSliceOverlay(opts) {
    const thumbs = !opts || opts.thumbs !== false;
    const reportRect = !opts || opts.reportRect !== false;
    updateSliceHint();
    const htmlGuides = state.mode !== "hdr" || !document.body.classList.contains("mode-hdr-ready");
    const showThumbs = state.slices.length && state.sdrImage && state.settings.sliceAspect !== "none";
    sliceThumbs.hidden = !showThumbs;
    sliceOverlay.classList.toggle("is-active", showThumbs && htmlGuides && state.cropMeta.slack > 0);
    if (!showThumbs) {
      if (sliceOverlay.childElementCount) sliceOverlay.innerHTML = "";
      if (sliceThumbs.childElementCount) sliceThumbs.innerHTML = "";
      sliceOverlay.classList.remove("is-active", "is-dragging");
      state.overlayCache = { key: "", sdr: null, count: 0 };
      if (reportRect) reportPreviewRect();
      return;
    }
    const selected = Math.max(1, Math.min(state.settings.previewSlice || 1, state.slices.length));
    layoutSliceRects(htmlGuides, selected);
    if (thumbs) paintSliceThumbs(sliceKey(state.slices), selected);
    if (reportRect) reportPreviewRect();
  }

  function reportPreviewRect() {
    const r = canvas.getBoundingClientRect();
    const prev = state.lastPreviewRect;
    if (prev && prev.x === r.left && prev.y === r.top && prev.w === r.width &&
        prev.h === r.height && prev.mode === state.mode) {
      return;
    }
    state.lastPreviewRect = { x: r.left, y: r.top, w: r.width, h: r.height, mode: state.mode };
    post({
      type: "previewRect",
      x: r.left,
      y: r.top,
      w: r.width,
      h: r.height,
      mode: state.mode,
    });
  }

  function refreshLoadStatus() {
    const el = document.getElementById("status-text");
    if (!el || !state.loadStarted) return;
    const sec = (Date.now() - state.loadStarted) / 1000;
    el.textContent = `${state.loadLabel} · ${sec.toFixed(1)}s`;
    const loader = document.getElementById("hdr-loader");
    if (loader) {
      const copy = loader.querySelector("p");
      if (copy) copy.textContent = el.textContent;
    }
  }

  function stopLoadTicker() {
    if (state.loadTicker) {
      clearInterval(state.loadTicker);
      state.loadTicker = 0;
    }
    state.loadStarted = 0;
    state.loadPhase = "";
    state._lastLoadPhase = "";
  }

  function formatFileMb(bytes) {
    return formatFileSize(bytes);
  }

  function sdrLoadCopy(msg) {
    const w = Number(msg && msg.width) || 0;
    const h = Number(msg && msg.height) || 0;
    const size = formatFileMb(msg && msg.bytes);
    if (w > 0 && h > 0 && size) return `Loading ${w}×${h} · ${size}…`;
    if (size) return `Loading ${size}…`;
    if (w > 0 && h > 0) return `Loading ${w}×${h}…`;
    return "Loading image…";
  }

  function setSdrLoadOverlay(show, msg) {
    const loader = document.getElementById("hdr-loader");
    if (!loader) return;
    if (!show) {
      if (!document.body.classList.contains("is-hdr-loading")) {
        loader.hidden = true;
      }
      return;
    }
    document.body.classList.add("is-loading");
    loader.hidden = false;
    const copy = loader.querySelector("p");
    if (copy) copy.textContent = sdrLoadCopy(msg || {});
    const status = document.getElementById("status-text");
    if (status) status.textContent = sdrLoadCopy(msg || {});
  }

  function setHdrLoading(loading, phase) {
    const loader = document.getElementById("hdr-loader");
    document.body.classList.toggle("is-hdr-loading", loading);
    if (loader) loader.hidden = !loading;
    if (loading) {
      document.body.classList.remove("mode-hdr-ready");
      state.loadPhase = phase || state.loadPhase || "encode";
      state.loadLabel = PHASE_LABEL[state.loadPhase] || PHASE_LABEL.encode;
      if (phase && state._lastLoadPhase !== phase) {
        state.loadStarted = Date.now();
        state._lastLoadPhase = phase;
      }
      if (!state.loadStarted) state.loadStarted = Date.now();
      if (!state.loadTicker) {
        state.loadTicker = setInterval(refreshLoadStatus, 250);
      }
      refreshLoadStatus();
    } else {
      stopLoadTicker();
      if (loader) loader.hidden = true;
    }
  }

  function setMode(mode) {
    if (mode === "live" || mode === "final") mode = "hdr";
    state.mode = mode;
    document.body.className = `mode-${mode}${state.loaded ? "" : " is-loading"}`;
    document.body.classList.remove("is-hdr-loading", "mode-hdr-ready");
    const loader = document.getElementById("hdr-loader");
    if (loader) loader.hidden = true;
    stopLoadTicker();
    document.querySelectorAll(".mode-btn").forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.mode === mode);
    });
    drawPreview();
    post({ type: "setMode", mode });
    reportPreviewRect();
    updateOutputSize();
  }

  document.querySelectorAll(".mode-btn").forEach((btn) => {
    btn.addEventListener("click", () => setMode(btn.dataset.mode));
  });

  [
    "base-quality",
    "gainmap-quality",
    "gainmap-scale",
    "min-boost",
    "max-boost",
    "display-peak",
  ].forEach((id) => {
    const el = document.getElementById(id);
    el.addEventListener("input", () => commitEncodeSettings(false));
    el.addEventListener("change", () => commitEncodeSettings(true));
  });
  document.getElementById("monochrome-gainmap").addEventListener("change", () => {
    commitEncodeSettings(true);
  });

  ["out-width", "out-height"].forEach((id) => {
    const el = document.getElementById(id);
    if (!el) return;
    const axis = id === "out-width" ? "width" : "height";
    el.addEventListener("pointerdown", () => {
      state.sizeDrag = axis;
    });
    el.addEventListener("input", () => {
      state.sizeDrag = axis;
      applyOutputSize(axis, el.value, true);
      scheduleSettingsPost();
    });
    el.addEventListener("change", () => {
      applyOutputSize(axis, el.value, true);
      state.sizeDrag = "";
      cancelSettingsTimer();
      postSettings();
    });
  });
  [
    ["out-width-val", "width"],
    ["out-height-val", "height"],
  ].forEach(([id, axis]) => {
    const el = document.getElementById(id);
    if (!el) return;
    const commitTyped = (live) => {
      const range = sizeRange();
      if (!range || el.value === "") return false;
      const n = Number(el.value);
      if (!Number.isFinite(n)) return false;
      if (live && !sizeFieldInRange(axis, n, range)) return false;
      return applyOutputSize(axis, n, false);
    };
    el.addEventListener("input", () => {
      if (commitTyped(true)) scheduleSettingsPost();
    });
    el.addEventListener("keydown", (event) => {
      if (event.key !== "Enter") return;
      event.preventDefault();
      el.blur();
    });
    el.addEventListener("blur", () => {
      commitTyped(false);
      const resolved = resolveOutputSize();
      if (resolved) el.value = String(axis === "width" ? resolved.w : resolved.h);
      syncSizeSliders();
      cancelSettingsTimer();
      postSettings();
    });
    el.addEventListener(
      "wheel",
      (event) => {
        event.preventDefault();
      },
      { passive: false }
    );
  });
  window.addEventListener("pointerup", () => {
    state.sizeDrag = "";
  });

  document.querySelectorAll("#delivery-presets .ig-card").forEach((card) => {
    card.addEventListener("click", () => {
      setEncodePreset(card.dataset.value, true);
    });
  });

  document.querySelectorAll("#ig-presets .ig-card").forEach((card) => {
    card.addEventListener("click", () => {
      state.settings.sliceAspect = card.dataset.value;
      state.settings.cropOffset = 0.5;
      state.settings.sliceCount = 1;
      state.settings.previewSlice = 1;
      state.settings.outputWidth = 0;
      state.settings.outputHeight = 0;
      if (state.currentId) state.aspectPicked[state.currentId] = true;
      syncIgPresets();
      syncSizeSliders();
      commitInstagramSettings();
    });
  });

  const syncOthers = document.getElementById("sync-others");
  if (syncOthers) {
    syncOthers.addEventListener("click", () => {
      if (state.items.length < 2) return;
      commitEncodeSettings(true);
      commitInstagramSettings();
      post({ type: "syncToOthers" });
    });
  }

  document.getElementById("apply-btn").addEventListener("click", () => {
    commitDestDir();
    post({ type: "applyAll" });
  });
  document.getElementById("cancel-btn").addEventListener("click", () => post({ type: "cancel" }));
  document.getElementById("dest-browse").addEventListener("click", () => post({ type: "chooseDest" }));
  document.getElementById("dest-dir").addEventListener("change", commitDestDir);
  document.getElementById("dest-dir").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      commitDestDir();
    }
  });

  function slackDisplayPx() {
    const s = state.canvasScale;
    if (!s || !state.sdrImage || state.cropMeta.slack < 2) return 0;
    if (state.cropMeta.axis === "x") {
      return (state.cropMeta.slack / state.sdrImage.width) * s.dw;
    }
    return (state.cropMeta.slack / state.sdrImage.height) * s.dh;
  }

  sliceOverlay.addEventListener("pointerdown", (ev) => {
    if (!sliceOverlay.classList.contains("is-active")) return;
    const slackPx = slackDisplayPx();
    if (slackPx < 2) return;
    sliceOverlay.classList.add("is-dragging");
    sliceOverlay.setPointerCapture(ev.pointerId);
    state.drag = {
      x: ev.clientX,
      y: ev.clientY,
      offset: Number(state.settings.cropOffset) || 0,
      slackPx,
    };
    ev.preventDefault();
  });
  sliceOverlay.addEventListener("pointermove", (ev) => {
    if (!state.drag) return;
    const delta = state.cropMeta.axis === "x" ? ev.clientX - state.drag.x : ev.clientY - state.drag.y;
    state.settings.cropOffset = Math.min(1, Math.max(0, state.drag.offset + delta / state.drag.slackPx));
    const planned = computeLocalSlices();
    state.slices = planned.rects;
    state.sliceError = planned.error;
    if (state.dragRaf) return;
    state.dragRaf = requestAnimationFrame(() => {
      state.dragRaf = 0;
      if (!state.drag) return;
      drawPreview();
    });
  });
  function endDrag(ev) {
    if (!state.drag) return;
    state.drag = null;
    if (state.dragRaf) {
      cancelAnimationFrame(state.dragRaf);
      state.dragRaf = 0;
    }
    sliceOverlay.classList.remove("is-dragging");
    if (ev && sliceOverlay.hasPointerCapture(ev.pointerId)) {
      sliceOverlay.releasePointerCapture(ev.pointerId);
    }
    commitInstagramSettings();
  }
  sliceOverlay.addEventListener("pointerup", endDrag);
  sliceOverlay.addEventListener("pointercancel", endDrag);

  window.addEventListener("keydown", (ev) => {
    if (ev.target && (ev.target.tagName === "INPUT" || ev.target.tagName === "TEXTAREA")) return;
    const idx = state.items.findIndex((item) => item.id === state.currentId);
    if (ev.key === "ArrowDown" || ev.key === "ArrowRight") {
      if (state.settings.sliceAspect !== "none" && state.cropMeta.slack > 0 &&
          (ev.shiftKey || ev.altKey)) {
        const dir = ev.key === "ArrowRight" || ev.key === "ArrowDown" ? 1 : -1;
        state.settings.cropOffset = Math.min(1, Math.max(0, (Number(state.settings.cropOffset) || 0) + dir * 0.04));
        commitInstagramSettings();
        drawPreview();
        ev.preventDefault();
        return;
      }
      if (idx >= 0 && idx + 1 < state.items.length) selectItem(state.items[idx + 1].id, true);
      ev.preventDefault();
    } else if (ev.key === "ArrowUp" || ev.key === "ArrowLeft") {
      if (state.settings.sliceAspect !== "none" && state.cropMeta.slack > 0 &&
          (ev.shiftKey || ev.altKey)) {
        const dir = ev.key === "ArrowLeft" || ev.key === "ArrowUp" ? -1 : 1;
        state.settings.cropOffset = Math.min(1, Math.max(0, (Number(state.settings.cropOffset) || 0) + dir * 0.04));
        commitInstagramSettings();
        drawPreview();
        ev.preventDefault();
        return;
      }
      if (idx > 0) selectItem(state.items[idx - 1].id, true);
      ev.preventDefault();
    } else if (ev.key === "1") setMode("sdr");
    else if (ev.key === "2") setMode("gain");
    else if (ev.key === "3" || ev.key === "4") setMode("hdr");
  });

  window.addEventListener("resize", () => {
    resizeCanvas();
    drawPreview();
    reportPreviewRect();
  });

  if (typeof ResizeObserver !== "undefined") {
    new ResizeObserver(() => {
      resizeCanvas();
      drawPreview();
      reportPreviewRect();
    }).observe(document.getElementById("preview-frame") || document.getElementById("canvas-wrap"));
  }

  syncDeliveryUi();
  post({ type: "ready" });
  reportPreviewRect();
})();

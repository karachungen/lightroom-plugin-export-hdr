#version 440

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform PreviewParams {
    vec4 mode_boost;   // mode, min boost, max boost, display boost
    vec4 display;      // image aspect, viewport aspect, HDR headroom, SDR white scale
    vec4 gain_range;   // stable visualization min/max, SDR fallback, final available
    vec4 view;         // zoom, pan x/y in normalized viewport coordinates, output is Display P3
    vec4 color;        // x = decoded HDR gamut: 0 BT.709, 1 Display P3, 2 BT.2100
};

layout(binding = 1) uniform sampler2D sdrTexture;
layout(binding = 2) uniform sampler2D gainTexture;
layout(binding = 3) uniform sampler2D finalTexture;

vec3 srgbToLinear(vec3 c)
{
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

vec3 linearToSrgb(vec3 c)
{
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 rec2020ToSrgb(vec3 c)
{
    return mat3(
         1.6605, -0.1246, -0.0182,
        -0.5876,  1.1329, -0.1006,
        -0.0728, -0.0083,  1.1187
    ) * c;
}

vec3 srgbToDisplayP3(vec3 c)
{
    return mat3(
        0.8226, 0.0332, 0.0171,
        0.1775, 0.9668, 0.0724,
        0.0000, 0.0000, 0.9103
    ) * c;
}

vec3 displayP3ToSrgb(vec3 c)
{
    return mat3(
         1.2249, -0.0421, -0.0196,
        -0.2249,  1.0421, -0.0786,
         0.0000,  0.0000,  1.0983
    ) * c;
}

vec3 toOutputPrimaries(vec3 linearSrgb)
{
    return view.w > 0.5 ? srgbToDisplayP3(linearSrgb) : linearSrgb;
}

vec3 decodedHdrToLinearSrgb(vec3 c)
{
    int gamut = int(color.x + 0.5);
    if (gamut == 2)
        return rec2020ToSrgb(c);
    if (gamut == 1)
        return displayP3ToSrgb(c);
    return c;
}

vec3 toneMapSdr(vec3 c)
{
    // Deterministic extended Reinhard shoulder; paper white remains unchanged.
    vec3 excess = max(c - vec3(1.0), vec3(0.0));
    return min(c, vec3(1.0)) + excess / (vec3(1.0) + excess);
}

vec3 gainHeat(float gainValue)
{
    float lo = log(max(gain_range.x, 0.0001));
    float hi = log(max(gain_range.y, gain_range.x + 0.0001));
    float t = clamp((log(max(gainValue, gain_range.x)) - lo) / max(hi - lo, 0.0001), 0.0, 1.0);
    vec3 blue = vec3(0.08, 0.32, 0.95);
    vec3 cyan = vec3(0.00, 0.86, 0.86);
    vec3 yellow = vec3(1.00, 0.78, 0.08);
    vec3 red = vec3(1.00, 0.18, 0.08);
    if (t < 0.35)
        return mix(blue, cyan, t / 0.35);
    if (t < 0.72)
        return mix(cyan, yellow, (t - 0.35) / 0.37);
    return mix(yellow, red, (t - 0.72) / 0.28);
}

void main()
{
    float imageAspect = display.x;
    float viewportAspect = display.y;
    vec2 imageUv = uv;
    if (viewportAspect > imageAspect) {
        float widthScale = imageAspect / viewportAspect;
        imageUv.x = (uv.x - 0.5) / widthScale + 0.5;
    } else {
        float heightScale = viewportAspect / imageAspect;
        imageUv.y = (uv.y - 0.5) / heightScale + 0.5;
    }
    imageUv = (imageUv - vec2(0.5) - view.yz) / max(view.x, 0.01) + vec2(0.5);

    if (any(lessThan(imageUv, vec2(0.0))) || any(greaterThan(imageUv, vec2(1.0)))) {
        fragColor = vec4(0.027, 0.031, 0.043, 1.0);
        return;
    }

    int mode = int(mode_boost.x + 0.5);
    vec3 sdr = texture(sdrTexture, imageUv).rgb;
    float gainValue = clamp(texture(gainTexture, imageUv).r, mode_boost.y, mode_boost.z);

    if (mode == 0) {
        vec3 linearSdr = srgbToLinear(sdr) * display.w;
        fragColor = vec4(toOutputPrimaries(linearSdr), 1.0);
        return;
    }
    if (mode == 1) {
        vec3 heat = gainHeat(gainValue);
        // SDR swapchains use the sRGB flag and expect linear inputs.
        if (gain_range.z > 0.5)
            heat = srgbToLinear(heat);
        fragColor = vec4(toOutputPrimaries(heat) * display.w, 1.0);
        return;
    }

    vec3 hdrLinear;
    if (mode == 3 && gain_range.w > 0.5)
        hdrLinear = decodedHdrToLinearSrgb(texture(finalTexture, imageUv).rgb);
    else {
        float displayBoost = max(mode_boost.w, 1.0);
        float strength = clamp(log(max(mode_boost.z, 1.01)) / log(1000.0), 0.0, 1.25);
        float applied = mix(1.0, min(gainValue, displayBoost), strength);
        hdrLinear = srgbToLinear(sdr) * applied;
    }

    hdrLinear = max(hdrLinear, vec3(0.0));
    hdrLinear = toOutputPrimaries(hdrLinear);
    if (gain_range.z > 0.5)
        // Tone-map in linear space; QRhiSwapChain::sRGB handles encoding.
        hdrLinear = toneMapSdr(hdrLinear);
    else
        hdrLinear = min(hdrLinear, vec3(max(display.z, 1.0)));

    fragColor = vec4(hdrLinear, 1.0);
}

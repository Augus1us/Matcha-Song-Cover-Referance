#include "media.h"
#include "music_player_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace native_music_player::detail {

struct MusicRgb {
    float r = 0.f, g = 0.f, b = 0.f;
};

struct MusicPalette {
    ImVec4 top      = ImVec4(0.075f, 0.170f, 0.220f, 1.f);
    ImVec4 bottom   = ImVec4(0.040f, 0.105f, 0.155f, 1.f);
    ImVec4 accentA  = ImVec4(0.20f, 0.53f, 0.71f, 1.f);
    ImVec4 accentB  = ImVec4(0.20f, 0.32f, 0.57f, 1.f);
    ImVec4 accentC  = ImVec4(0.48f, 0.20f, 0.50f, 1.f);
    ImVec4 progress = ImVec4(0.78f, 0.88f, 0.93f, 1.f);
};

namespace {

MusicPalette s_paletteCurrent;
MusicPalette s_paletteTarget;
bool  s_paletteReady = false;
float s_motion = 0.f;

float Luma(const MusicRgb& c) {
    return c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f;
}

float Saturation(const MusicRgb& c) {
    float hi = std::max(c.r, std::max(c.g, c.b));
    float lo = std::min(c.r, std::min(c.g, c.b));
    return hi > 0.0001f ? (hi - lo) / hi : 0.f;
}

float Hue(const MusicRgb& c) {
    const float hi = std::max(c.r, std::max(c.g, c.b));
    const float lo = std::min(c.r, std::min(c.g, c.b));
    const float delta = hi - lo;
    if (delta <= 0.0001f) return 0.f;
    float hue = 0.f;
    if (hi == c.r) hue = std::fmod((c.g - c.b) / delta, 6.f);
    else if (hi == c.g) hue = (c.b - c.r) / delta + 2.f;
    else hue = (c.r - c.g) / delta + 4.f;
    hue /= 6.f;
    return hue < 0.f ? hue + 1.f : hue;
}

MusicRgb Mix(const MusicRgb& a, const MusicRgb& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t
    };
}

MusicRgb Tone(MusicRgb c, float value, float saturationScale,
              float saturationFloor) {
    float hi = std::max(c.r, std::max(c.g, c.b));
    float lo = std::min(c.r, std::min(c.g, c.b));
    float delta = hi - lo;
    float hue = 0.f;
    if (delta > 0.0001f) {
        if (hi == c.r) hue = std::fmod((c.g - c.b) / delta, 6.f);
        else if (hi == c.g) hue = (c.b - c.r) / delta + 2.f;
        else hue = (c.r - c.g) / delta + 4.f;
        hue /= 6.f;
        if (hue < 0.f) hue += 1.f;
    }
    float saturation = hi > 0.0001f ? delta / hi : 0.f;
    saturation = std::clamp(saturation * saturationScale + 0.04f,
                            saturationFloor, 0.92f);
    value = std::clamp(value, 0.f, 1.f);

    float h6 = hue * 6.f;
    int sector = (int)std::floor(h6);
    float fraction = h6 - (float)sector;
    float p = value * (1.f - saturation);
    float q = value * (1.f - saturation * fraction);
    float t = value * (1.f - saturation * (1.f - fraction));
    switch ((sector % 6 + 6) % 6) {
    case 0: return { value, t, p };
    case 1: return { q, value, p };
    case 2: return { p, value, t };
    case 3: return { p, q, value };
    case 4: return { t, p, value };
    default:return { value, p, q };
    }
}

ImVec4 ToVec4(const MusicRgb& c) {
    return ImVec4(std::clamp(c.r, 0.f, 1.f),
                  std::clamp(c.g, 0.f, 1.f),
                  std::clamp(c.b, 0.f, 1.f), 1.f);
}

ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImU32 Color(ImVec4 color, float alpha = 1.f) {
    color.w = std::clamp(color.w * alpha, 0.f, 1.f);
    return ImGui::ColorConvertFloat4ToU32(color);
}

MusicPalette ExtractPalette(const uint8_t* bgra, int width, int height) {
    MusicPalette fallback;
    if (!bgra || width <= 0 || height <= 0) return fallback;

    struct Candidate {
        MusicRgb color;
        float score = 0.f;
        float weight = 0.f;
    };
    Candidate blocks[16] = {};
    struct HueBucket {
        MusicRgb sum;
        float weight = 0.f;
    };
    constexpr int kHueBuckets = 24;
    HueBucket hueBuckets[kHueBuckets] = {};
    MusicRgb overall = {};
    float overallWeight = 0.f;

    for (int by = 0; by < 4; ++by) {
        for (int bx = 0; bx < 4; ++bx) {
            int x0 = width * bx / 4, x1 = width * (bx + 1) / 4;
            int y0 = height * by / 4, y1 = height * (by + 1) / 4;
            int stepX = std::max(1, (x1 - x0) / 12);
            int stepY = std::max(1, (y1 - y0) / 12);
            MusicRgb sum = {};
            float weight = 0.f;
            for (int y = y0; y < y1; y += stepY) {
                for (int x = x0; x < x1; x += stepX) {
                    const uint8_t* px = bgra + ((size_t)y * width + x) * 4;
                    float alpha = px[3] / 255.f;
                    if (alpha < 0.25f) continue;
                    MusicRgb c = { px[2] / 255.f, px[1] / 255.f, px[0] / 255.f };
                    const float saturation = Saturation(c);
                    const float value = std::max(c.r, std::max(c.g, c.b));
                    float sampleWeight = alpha * (0.35f + saturation * 1.3f);
                    sum.r += c.r * sampleWeight;
                    sum.g += c.g * sampleWeight;
                    sum.b += c.b * sampleWeight;
                    weight += sampleWeight;
                    if (saturation >= 0.10f && value >= 0.06f) {
                        const int bucket = std::clamp(
                            (int)std::floor(Hue(c) * kHueBuckets),
                            0, kHueBuckets - 1);
                        const float hueWeight = alpha * std::pow(saturation, 1.35f) *
                            (0.20f + std::sqrt(value));
                        hueBuckets[bucket].sum.r += c.r * hueWeight;
                        hueBuckets[bucket].sum.g += c.g * hueWeight;
                        hueBuckets[bucket].sum.b += c.b * hueWeight;
                        hueBuckets[bucket].weight += hueWeight;
                    }
                }
            }
            Candidate& block = blocks[by * 4 + bx];
            if (weight > 0.f) {
                block.color = { sum.r / weight, sum.g / weight, sum.b / weight };
                float lum = Luma(block.color);
                block.score = Saturation(block.color) * 1.15f +
                    (1.f - std::abs(lum - 0.5f) * 2.f) * 0.16f;
                block.weight = weight;
                overall.r += block.color.r * weight;
                overall.g += block.color.g * weight;
                overall.b += block.color.b * weight;
                overallWeight += weight;
            }
        }
    }
    if (overallWeight <= 0.f) return fallback;
    overall = { overall.r / overallWeight, overall.g / overallWeight,
                overall.b / overallWeight };

    int selectedBuckets[3] = { -1, -1, -1 };
    for (int pick = 0; pick < 3; ++pick) {
        float bestScore = -1.f;
        for (int i = 0; i < kHueBuckets; ++i) {
            if (hueBuckets[i].weight <= 0.f) continue;
            bool alreadySelected = false;
            float nearestHue = 0.5f;
            for (int previous = 0; previous < pick; ++previous) {
                if (selectedBuckets[previous] == i) alreadySelected = true;
                const float raw = std::abs((float)i - selectedBuckets[previous]) /
                    (float)kHueBuckets;
                nearestHue = std::min(nearestHue, std::min(raw, 1.f - raw));
            }
            if (alreadySelected) continue;
            const float separation = pick == 0 ? 1.f :
                (0.24f + 0.76f * std::clamp(nearestHue / 0.16f, 0.f, 1.f));
            const float score = hueBuckets[i].weight * separation;
            if (score > bestScore) {
                selectedBuckets[pick] = i;
                bestScore = score;
            }
        }
    }
    auto bucketColor = [&](int index, const MusicRgb& fallbackColor) {
        if (index < 0 || hueBuckets[index].weight <= 0.f) return fallbackColor;
        const float inverse = 1.f / hueBuckets[index].weight;
        return MusicRgb{
            hueBuckets[index].sum.r * inverse,
            hueBuckets[index].sum.g * inverse,
            hueBuckets[index].sum.b * inverse
        };
    };
    MusicRgb dominant[3] = {
        bucketColor(selectedBuckets[0], overall),
        bucketColor(selectedBuckets[1], overall),
        bucketColor(selectedBuckets[2], overall)
    };
    std::sort(std::begin(dominant), std::end(dominant),
        [](const MusicRgb& left, const MusicRgb& right) {
            return Luma(left) < Luma(right);
        });
    const MusicRgb a = dominant[0];
    const MusicRgb b = dominant[2];
    const MusicRgb c = dominant[1];
    MusicPalette result;
    // Surface brightness follows the cover instead of being pinned dark.
    //
    // These were fixed HSV values of 0.23 and 0.15, so the card came out the
    // same near-black whatever the artwork was: a bright pink cover rendered as
    // dark maroon at about a third of the art's own luminance, where the
    // reference sits close to the cover's level and reads light. Saturation is
    // also pulled in a little so the result is a soft wash rather than a block
    // of colour.
    result.top = ToVec4(Tone(Mix(overall, a, 0.78f), 0.52f, 0.88f, 0.15f));
    result.bottom = ToVec4(Tone(
        Mix(Mix(overall, b, 0.62f), c, 0.18f), 0.42f, 0.88f, 0.13f));
    result.accentA = ToVec4(Tone(a, 0.66f, 1.18f, 0.30f));
    result.accentB = ToVec4(Tone(b, 0.58f, 1.16f, 0.26f));
    result.accentC = ToVec4(Tone(c, 0.61f, 1.16f, 0.26f));
    result.progress = ToVec4(Tone(
        Mix(Mix(a, b, 0.45f), c, 0.22f), 0.90f, 1.12f, 0.18f));
    return result;
}

void UpdatePalette() {
    if (!s_paletteReady) return;
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    float blend = 1.f - std::exp(-3.8f * dt);
    s_paletteCurrent.top = Lerp(s_paletteCurrent.top, s_paletteTarget.top, blend);
    s_paletteCurrent.bottom = Lerp(s_paletteCurrent.bottom, s_paletteTarget.bottom, blend);
    s_paletteCurrent.accentA = Lerp(s_paletteCurrent.accentA, s_paletteTarget.accentA, blend);
    s_paletteCurrent.accentB = Lerp(s_paletteCurrent.accentB, s_paletteTarget.accentB, blend);
    s_paletteCurrent.accentC = Lerp(s_paletteCurrent.accentC, s_paletteTarget.accentC, blend);
    s_paletteCurrent.progress = Lerp(s_paletteCurrent.progress, s_paletteTarget.progress, blend);
}

void DrawRoundedGradient(ImDrawList* dl, const ImVec2& min, const ImVec2& max,
                         ImU32 topLeft, ImU32 topRight, ImU32 bottomRight,
                         ImU32 bottomLeft, float rounding) {
    const int firstVertex = dl->VtxBuffer.Size;
    dl->PathRect(min, max, rounding);
    dl->PathFillConvex(IM_COL32_WHITE);
    const float width = std::max(max.x - min.x, 0.001f);
    const float height = std::max(max.y - min.y, 0.001f);
    auto channel = [](ImU32 color, int shift) {
        return (float)((color >> shift) & 0xFF);
    };
    auto bilerp = [&](int shift, float x, float y) {
        float top = channel(topLeft, shift) +
            (channel(topRight, shift) - channel(topLeft, shift)) * x;
        float bottom = channel(bottomLeft, shift) +
            (channel(bottomRight, shift) - channel(bottomLeft, shift)) * x;
        return top + (bottom - top) * y;
    };
    for (int i = firstVertex; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& vertex = dl->VtxBuffer[i];
        float x = std::clamp((vertex.pos.x - min.x) / width, 0.f, 1.f);
        float y = std::clamp((vertex.pos.y - min.y) / height, 0.f, 1.f);
        int coverageAlpha = (int)((vertex.col >> IM_COL32_A_SHIFT) & 0xFF);
        int gradientAlpha = (int)bilerp(IM_COL32_A_SHIFT, x, y);
        vertex.col = IM_COL32(
            (int)bilerp(IM_COL32_R_SHIFT, x, y),
            (int)bilerp(IM_COL32_G_SHIFT, x, y),
            (int)bilerp(IM_COL32_B_SHIFT, x, y),
            gradientAlpha * coverageAlpha / 255);
    }
}

void DrawBlob(ImDrawList* dl, ImVec2 center, float radius,
              ImVec4 color, float alpha) {
    constexpr int layers = 14;
    for (int i = layers; i >= 1; --i) {
        float scale = (float)i / (float)layers;
        float layerAlpha = alpha * (1.15f - scale) / (float)layers;
        dl->AddCircleFilled(center, radius * scale,
                            Color(color, layerAlpha), 48);
    }
}

}  // namespace

void EnsureVisualPalette() {
    if (s_paletteReady) return;
    s_paletteCurrent = MusicPalette{};
    s_paletteTarget = s_paletteCurrent;
    s_paletteReady = true;
}

void SetPaletteFromArt(const uint8_t* bgra, int width, int height) {
    s_paletteTarget = ExtractPalette(bgra, width, height);
    if (!s_paletteReady) {
        s_paletteCurrent = s_paletteTarget;
        s_paletteReady = true;
    }
}

void SamplePaletteRegionsBGRA(const uint8_t* bgra, int width, int height,
                              int gridSide, float* outRgb) {
    if (!bgra || width <= 0 || height <= 0 || gridSide <= 0) return;
    const MusicRgb dominantColors[3] = {
        { s_paletteTarget.accentA.x, s_paletteTarget.accentA.y,
          s_paletteTarget.accentA.z },
        { s_paletteTarget.accentB.x, s_paletteTarget.accentB.y,
          s_paletteTarget.accentB.z },
        { s_paletteTarget.accentC.x, s_paletteTarget.accentC.y,
          s_paletteTarget.accentC.z }
    };
    for (int by = 0; by < gridSide; ++by) {
        for (int bx = 0; bx < gridSide; ++bx) {
            const int x0 = bx * width / gridSide;
            const int x1 = std::max(x0 + 1, (bx + 1) * width / gridSide);
            const int y0 = by * height / gridSide;
            const int y1 = std::max(y0 + 1, (by + 1) * height / gridSide);
            const int stepX = std::max(1, (x1 - x0) / 16);
            const int stepY = std::max(1, (y1 - y0) / 16);
            MusicRgb sum = {};
            float weight = 0.f;
            for (int y = y0; y < y1; y += stepY) {
                for (int x = x0; x < x1; x += stepX) {
                    const uint8_t* px = bgra + ((size_t)y * width + x) * 4u;
                    const float alpha = px[3] / 255.f;
                    if (alpha < 0.25f) continue;
                    const MusicRgb c = {
                        px[2] / 255.f, px[1] / 255.f, px[0] / 255.f
                    };
                    const float sampleWeight = alpha *
                        (0.32f + Saturation(c) * 1.70f);
                    sum.r += c.r * sampleWeight;
                    sum.g += c.g * sampleWeight;
                    sum.b += c.b * sampleWeight;
                    weight += sampleWeight;
                }
            }
            MusicRgb region = weight > 0.f
                ? MusicRgb{ sum.r / weight, sum.g / weight, sum.b / weight }
                : MusicRgb{ s_paletteTarget.top.x,
                            s_paletteTarget.top.y,
                            s_paletteTarget.top.z };
            if (weight > 0.f) {
                int nearest = 0;
                float nearestDistance = FLT_MAX;
                for (int i = 0; i < 3; ++i) {
                    const float dr = region.r - dominantColors[i].r;
                    const float dg = region.g - dominantColors[i].g;
                    const float db = region.b - dominantColors[i].b;
                    const float distance = dr * dr + dg * dg + db * db;
                    if (distance < nearestDistance) {
                        nearest = i;
                        nearestDistance = distance;
                    }
                }
                const float pull = 0.42f +
                    std::min(0.20f, Saturation(region) * 0.20f);
                region = Mix(region, dominantColors[nearest], pull);
            }
            const float sourceValue = std::max(region.r,
                std::max(region.g, region.b));
            const float sourceSaturation = Saturation(region);
            const float value = std::clamp(
                sourceValue * 0.84f + 0.025f, 0.025f, 0.68f);
            region = Tone(region, value, 1.04f,
                sourceSaturation > 0.12f
                    ? std::min(sourceSaturation, 0.36f) : 0.02f);
            float* out = outRgb + (by * gridSide + bx) * 3;
            out[0] = region.r;
            out[1] = region.g;
            out[2] = region.b;
        }
    }
}

ImU32 LyricHighlightColor() {
    return Color(Lerp(s_paletteCurrent.progress,
                      ImVec4(1.f, 1.f, 1.f, 1.f), 0.72f), 1.f);
}

void ResetPaletteState() {
    s_paletteReady = false;
    s_paletteCurrent = MusicPalette{};
    s_paletteTarget = MusicPalette{};
    s_motion = 0.f;
}

void DrawPlayerBackground(ImDrawList* dl, const ImVec2& min, const ImVec2& size,
                          bool playing, bool showLyrics, bool artworkView,
                          bool fullScreen, bool haveArt, float hover,
                          float uiScale) {
    (void)uiScale;   // geometry below is already derived from `size`
    UpdatePalette();
    const MusicPalette& p = s_paletteCurrent;
    ImVec2 max(min.x + size.x, min.y + size.y);
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    if (playing) s_motion += dt * 0.32f;

    const float rounding = Px(fullScreen ? 16.f : 10.f);
    const bool compactSurface = !fullScreen && !showLyrics && !artworkView;
    dl->AddRectFilled(min, max, IM_COL32(4, 5, 8, 255), rounding);

    ImTextureID atmosphere = AlbumAtmosphereTexture();
    if (haveArt && atmosphere) {
        float& fade = AlbumAtmosphereFadeRef();
        fade += (1.f - fade) * (1.f - std::exp(-3.4f * dt));
        const float fieldAlpha = (fullScreen ? 204.f :
            (artworkView ? 184.f : (showLyrics ? 232.f : 136.f))) *
            std::clamp(fade, 0.f, 1.f);
        dl->AddImageRounded(atmosphere, min, max,
                            ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                            IM_COL32(255, 255, 255, (int)fieldAlpha), rounding);
        dl->AddRectFilled(min, max,
            IM_COL32(3, 4, 7,
                fullScreen ? 90 : (artworkView ? 64 : (showLyrics ? 58 : 66))),
            rounding);
    }
    ImVec4 surfaceTop = Lerp(p.top, p.accentA, 0.035f);
    ImVec4 surfaceBottom = Lerp(p.bottom, p.top, 0.09f);
    if (showLyrics && !fullScreen) {
        surfaceTop = Lerp(p.top, p.accentA, 0.10f);
        surfaceBottom = Lerp(p.bottom, p.accentC, 0.09f);
    }
    const float paletteOpacity = fullScreen ? 0.28f
        : (artworkView ? 0.42f : (showLyrics ? 0.46f : 0.80f));
    DrawRoundedGradient(dl,
        ImVec2(min.x + 1.f, min.y + 1.f), ImVec2(max.x - 1.f, max.y - 1.f),
        Color(surfaceTop, paletteOpacity),
        Color(Lerp(surfaceTop, p.accentA, 0.10f),
              fullScreen ? 0.25f : paletteOpacity),
        Color(Lerp(surfaceBottom, p.accentB, 0.08f),
              fullScreen ? 0.30f : paletteOpacity),
        Color(surfaceBottom, fullScreen ? 0.34f : paletteOpacity),
        rounding - 1.f);

    if (!fullScreen)
        dl->AddRectFilled(min, max,
            IM_COL32(3, 4, 7,
                compactSurface ? 30 : (showLyrics ? 20 : 66)), rounding);
    if (compactSurface)
        dl->AddRectFilled(min, max, IM_COL32(38, 39, 43, 24), rounding);

    float waveA = std::sin(s_motion * 1.07f);
    float waveB = std::cos(s_motion * 0.83f);
    float waveC = std::sin(s_motion * 0.61f + 1.8f);
    float waveD = std::cos(s_motion * 0.49f + 0.7f);
    float radius = std::min(size.x, size.y) * 0.58f;
    const float atmosphereScale = fullScreen ? 0.52f : 1.f;
    DrawBlob(dl,
        ImVec2(min.x + size.x * (0.16f + waveA * 0.060f),
               min.y + size.y * (0.16f + waveB * 0.050f)),
        radius, p.accentA, (showLyrics ? 0.165f : 0.085f) * atmosphereScale);
    DrawBlob(dl,
        ImVec2(min.x + size.x * (0.86f - waveB * 0.055f),
               min.y + size.y * (0.82f + waveA * 0.050f)),
        radius * 0.90f, p.accentB,
        (showLyrics ? 0.135f : 0.070f) * atmosphereScale);
    if (showLyrics && !fullScreen) {
        DrawBlob(dl,
            ImVec2(min.x + size.x * (0.50f + waveC * 0.085f),
                   min.y + size.y * (0.48f + waveD * 0.065f)),
            radius * 0.82f, p.accentC, 0.105f);
        DrawBlob(dl,
            ImVec2(min.x + size.x * (0.22f - waveD * 0.04f),
                   min.y + size.y * (0.78f + waveC * 0.04f)),
            radius * 0.68f, p.accentA, 0.075f);
    }

    if (artworkView && haveArt) {
        ImTextureID art = AlbumArtTexture();
        if (art) {
            // Fill the whole card. The cover used to be drawn SQUARE, sized by
            // the card's width, so on a card taller than it is wide the art
            // stopped short and left a band of plain background along the
            // bottom edge -- visible as a dead strip under the controls.
            const ImVec2 artMin(min.x + 1.f, min.y + 1.f);
            const ImVec2 artMax(max.x - 1.f, max.y - 1.f);
            const float artExtent = artMax.y - artMin.y;
            // Cover-fit rather than stretch: crop the long axis in UV space so
            // a square cover fills a non-square card without distorting faces.
            const float rectW = artMax.x - artMin.x;
            const float rectH = artExtent;
            float uvX = 0.f, uvY = 0.f;
            if (rectW > rectH) uvY = (1.f - rectH / rectW) * 0.5f;
            else if (rectH > rectW) uvX = (1.f - rectW / rectH) * 0.5f;
            dl->AddImageRounded(art, artMin, artMax,
                                ImVec2(uvX, uvY), ImVec2(1.f - uvX, 1.f - uvY),
                                IM_COL32_WHITE, rounding - 1.f);
            const float fadeTop = artMax.y - std::min(112.f, artExtent * 0.37f);
            // The alpha-ramped atmosphere copy carries the dissolve now, so the
            // flat darkening below it is only a whisper -- at the old strength
            // the two stacked and the bottom of every cover went muddy.
            if (ImTextureID fade = AlbumArtworkFadeTexture())
                dl->AddImage(fade, artMin, artMax,
                             ImVec2(0.f, 0.f), ImVec2(1.f, 1.f));
            dl->AddRectFilledMultiColor(
                ImVec2(artMin.x, fadeTop), artMax,
                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                IM_COL32(2, 3, 5, 12), IM_COL32(2, 3, 5, 12));
        }
    }

    DrawRoundedGradient(dl,
        ImVec2(min.x, min.y), ImVec2(max.x, max.y),
        // Much lighter black wash than before. The bottom corners carried
        // alpha 28-36 (112/90 in fullscreen), which put a heavy vignette across
        // the lower half of the card -- the reference has no such shading, its
        // surface is close to even top to bottom.
        IM_COL32(0, 0, 0, fullScreen ? 34 : (artworkView ? 0 : 4)),
        IM_COL32(0, 0, 0, fullScreen ? 28 : (artworkView ? 0 : 6)),
        IM_COL32(0, 0, 0, fullScreen ? 16 :
            (artworkView ? 0 : (compactSurface ? 8 : 12))),
        IM_COL32(0, 0, 0, fullScreen ? 22 :
            (artworkView ? 0 : (compactSurface ? 10 : 15))),
        rounding);

    if (compactSurface && hover > 0.001f)
        dl->AddRectFilled(min, max,
            IM_COL32(255, 255, 255, (int)(11.f * hover)), rounding);

    // One hairline, one radius. Previously an outer dark ring at `rounding` and
    // an inner light ring at `rounding - 1.5f` were drawn 1.5px apart: two
    // different curves at the corners, which read as a doubled, ragged edge
    // exactly where the eye checks a rounded rectangle. Insetting a stroke on a
    // rounded rect needs the radius reduced by the SAME inset to stay
    // concentric, and even then the two strokes fight each other at this size.
    //
    // A single 1px light border sitting just inside the fill matches the real
    // player; the card already separates from the desktop by its own contrast.
    dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f),
                ImVec2(max.x - 0.5f, max.y - 0.5f),
                IM_COL32(236, 242, 248,
                    (int)((fullScreen ? 30.f : 26.f) + hover * 18.f)),
                rounding - 0.5f, 0, 1.f);
}

}  // namespace native_music_player::detail

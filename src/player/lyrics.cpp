#include "media.h"
#include "music_player_host.h"
#include "music_player_internal.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace native_music_player::detail {

namespace {

std::vector<media::LyricLine> g_cache;
std::vector<float> g_lineHeights;
uint64_t g_cacheRevision = UINT64_MAX;
uint64_t g_layoutRevision = UINT64_MAX;
float    g_scroll = 0.f;
float    g_scrollTarget = 0.f;
float    g_layoutWidth = -1.f;
float    g_contentHeight = 0.f;

// ImGui advances by exactly one font size between wrapped lines and adds no
// leading, so a lyric that wraps is packed tight against itself while the gap
// to the NEXT lyric stays wide. That mismatch is what makes wrapped lyrics look
// cramped. Wrapping is done by hand here so every line -- wrapped or not --
// uses the same rhythm.
constexpr float kLyricLeading = 1.34f;

int LyricWrapLineCount(ImFont* font, float size, const char* text, float wrapWidth) {
    if (!font || !text || !*text || wrapWidth <= 1.f) return 1;
    const char* s = text;
    const char* end = text + std::strlen(text);
    int lines = 0;
    while (s < end && lines < 64) {
        const char* wrap = font->CalcWordWrapPosition(size, s, end, wrapWidth);
        if (wrap <= s) wrap = s + 1;          // never stall on a too-narrow box
        ++lines;
        s = wrap;
        while (s < end && *s == ' ') ++s;
    }
    return std::max(1, lines);
}

// Returns the height consumed, so the caller's layout and this stay in step.
float DrawWrappedLyric(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                       ImU32 col, const char* text, float wrapWidth) {
    if (!font || !text) return 0.f;
    const float step = size * kLyricLeading;
    const char* s = text;
    const char* end = text + std::strlen(text);
    float y = pos.y;
    int guard = 0;
    while (s < end && guard++ < 64) {
        const char* wrap = font->CalcWordWrapPosition(size, s, end, wrapWidth);
        if (wrap <= s) wrap = s + 1;
        dl->AddText(font, size, ImVec2(pos.x, y), col, s, wrap);
        y += step;
        s = wrap;
        while (s < end && *s == ' ') ++s;
    }
    return y - pos.y;
}
bool     g_manualScroll = false;
uint64_t g_manualUntilMs = 0;
bool     g_positionSyncRequested = false;
uint64_t g_syncPulseUntilMs = 0;
double   g_previewSeekSec = -1.0;
uint64_t g_previewSeekTickMs = 0;

int FindActiveLyric(double pos) {
    int active = -1;
    for (int i = 0; i < (int)g_cache.size(); ++i) {
        if (g_cache[i].timeSec <= pos + 0.08) active = i;
        else break;
    }
    return active;
}

std::string Ellipsize(const std::string& s, ImFont* font, float size, float width) {
    if (music_host::Measure(font, size, s.c_str()).x <= width) return s;
    std::string out = s;
    while (out.size() > 1 &&
           music_host::Measure(font, size, (out + "...").c_str()).x > width)
        out.pop_back();
    return out + "...";
}

}  // namespace

void UpdateLyrics(const media::NowPlaying& np) {
    if (g_cacheRevision != np.lyricsRevision) {
        g_cache = np.lyrics;
        g_cacheRevision = np.lyricsRevision;
    }
}

PlaybackView ResolvePlayback(double position, double duration,
                             double playbackRate, uint64_t snapshotTick,
                             bool playing) {
    double pos = position;
    if (playing && duration > 0.0) {
        uint64_t now = GetTickCount64();
        if (now > snapshotTick)
            pos += ((double)(now - snapshotTick) / 1000.0) * playbackRate;
        if (pos > duration) pos = duration;
    }
    if (g_previewSeekSec >= 0.0) {
        const uint64_t now = GetTickCount64();
        const uint64_t ageMs = now >= g_previewSeekTickMs
            ? now - g_previewSeekTickMs : 0;
        if (ageMs < 900) {
            pos = g_previewSeekSec;
            if (playing) pos += ((double)ageMs / 1000.0) * playbackRate;
            if (duration > 0.0) pos = std::clamp(pos, 0.0, duration);
        } else {
            g_previewSeekSec = -1.0;
        }
    }
    PlaybackView view;
    view.position = pos;
    view.progress = duration > 0.0
        ? (float)std::clamp(pos / duration, 0.0, 1.0) : 0.f;
    view.activeLyric = FindActiveLyric(pos);
    return view;
}

void DrawArtworkLyricOverlay(ImDrawList* dl, ImFont* regular, ImFont* bold,
                             ImVec2 wp, ImVec2 ws, const char* title,
                             const char* artist, const char* album,
                             int activeLyric) {
    const float artExtent = std::min(ws.x - 2.f, std::max(120.f, ws.y - 2.f));
    const float artBottom = wp.y + 1.f + artExtent;
    const float textX = wp.x + 8.f;
    const float textWidth = ws.x - 14.f;
    dl->PushClipRect(ImVec2(wp.x + 1.f, wp.y + 1.f),
                     ImVec2(wp.x + ws.x - 1.f, artBottom), true);

    const std::string artTitle = Ellipsize(title ? title : "", bold, 19.f, textWidth);
    std::string artByline = artist ? artist : "";
    if (album && album[0]) {
        if (!artByline.empty()) artByline += " \xE2\x80\x94 ";
        artByline += album;
    }
    artByline = Ellipsize(artByline, bold, 14.6f, textWidth);

    const float railY = wp.y + ws.y - 94.f;
    const bool hasLyric = activeLyric >= 0 && activeLyric < (int)g_cache.size();
    const float lyricY = railY - 28.f;
    const float titleY = (hasLyric ? lyricY - 50.f : railY - 49.f) - 3.f;
    dl->AddText(bold, 19.f, ImVec2(textX, titleY + 1.f),
                IM_COL32(0, 0, 0, 92), artTitle.c_str());
    dl->AddText(bold, 19.f, ImVec2(textX, titleY),
                IM_COL32(255, 255, 255, 248), artTitle.c_str());
    if (!artByline.empty()) {
        dl->AddText(bold, 14.6f, ImVec2(textX + 1.f, titleY + 25.f),
                    IM_COL32(0, 0, 0, 82), artByline.c_str());
        dl->AddText(bold, 14.6f, ImVec2(textX, titleY + 24.f),
                    IM_COL32(255, 255, 255, 190), artByline.c_str());
    }
    if (hasLyric) {
        dl->AddText(bold, 18.f, ImVec2(textX + 1.f, lyricY + 2.f),
                    IM_COL32(0, 0, 0, 155),
                    g_cache[activeLyric].text.c_str(), nullptr, textWidth);
        dl->AddText(bold, 18.f, ImVec2(textX, lyricY),
                    IM_COL32(255, 255, 255, 245),
                    g_cache[activeLyric].text.c_str(), nullptr, textWidth);
    }
    dl->PopClipRect();
}

static void DrawSyncButton(const LyricsPanelContext& ctx, float bottomY) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;
    const bool canPositionSync = ctx.lyricsSynced && ctx.activeLyric >= 0 &&
        !g_cache.empty() && !ctx.lyricsLoading;
    const char* syncText = "Sync";
    ImVec2 syncTextSize = music_host::Measure(ctx.bold, 11.5f, syncText);
    ImVec2 syncSize(syncTextSize.x + 22.f, 25.f);
    ImVec2 syncPos(wp.x + (ws.x - syncSize.x) * 0.5f, bottomY);
    ImGui::SetCursorScreenPos(syncPos);
    ImGui::InvisibleButton("##lyrics_sync", syncSize);
    bool syncHovered = ImGui::IsItemHovered();
    bool syncClicked = ImGui::IsItemClicked() && canPositionSync;
    const uint64_t syncNowMs = GetTickCount64();
    const bool syncAnimating = syncNowMs < g_syncPulseUntilMs;
    const float syncPhase = syncAnimating
        ? 1.f - (float)(g_syncPulseUntilMs - syncNowMs) / 650.f : 1.f;
    const float syncGlow = syncAnimating
        ? std::sin(std::clamp(syncPhase, 0.f, 1.f) * 3.14159265f) : 0.f;
    float syncPress = music_host::animation::ClickBounce(
        ImGui::GetID("##lyrics_sync_press"), syncClicked);
    float syncInset = (1.f - std::clamp(syncPress, 0.88f, 1.05f)) * 4.f
        - syncGlow * 1.2f;
    ImVec2 syncDrawMin(syncPos.x + syncInset, syncPos.y + syncInset * 0.5f);
    ImVec2 syncDrawMax(syncPos.x + syncSize.x - syncInset,
                       syncPos.y + syncSize.y - syncInset * 0.5f);
    dl->AddRectFilled(syncDrawMin, syncDrawMax,
                      IM_COL32(255, 255, 255,
                          (int)((syncHovered ? 38.f : 24.f) + syncGlow * 18.f)),
                      13.f);
    dl->AddRect(syncDrawMin, syncDrawMax,
                IM_COL32(255, 255, 255,
                    (int)((syncHovered ? 70.f : 40.f) + syncGlow * 55.f)),
                13.f, 0, 1.f);
    music_host::DrawText(dl, ctx.bold, 11.5f,
        ImVec2(syncPos.x + 11.f,
               syncPos.y + (syncSize.y - syncTextSize.y) * 0.5f +
               (1.f - syncPress) * 1.2f),
        IM_COL32(255, 255, 255, syncHovered ? 235 : 190), syncText);
    if (syncClicked) {
        g_positionSyncRequested = true;
        g_syncPulseUntilMs = GetTickCount64() + 650;
    }
    for (int i = 0; i < 3; ++i) {
        const float dotWave = syncAnimating
            ? 0.5f + 0.5f * std::sin(syncPhase * 10.f - i * 1.25f) : 0.f;
        dl->AddCircleFilled(ImVec2(wp.x + 17.f + i * 9.f, syncPos.y + 12.5f),
                            2.35f + dotWave * 0.45f,
                            IM_COL32(255, 255, 255,
                                (int)(105.f + dotWave * 105.f)), 10);
    }
}

void DrawLyricsPanel(const LyricsPanelContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;

    const bool lyricsChanged = g_layoutRevision != g_cacheRevision;
    const float S = ctx.uiScale;
    float lyricsTop = ctx.fullScreen ? wp.y + ws.y * 0.055f : wp.y + Px(82.f);
    // 112 used to reserve a strip for the Sync pill and its dots. That control
    // is gone, but the reservation stayed, leaving a tall empty band between
    // the last lyric and the progress bar. The panel now runs down to just
    // above the bar.
    float lyricsBottom = ctx.fullScreen ? wp.y + ws.y - Px(52.f)
                                        : wp.y + ws.y - Px(76.f);
    float lyricsHeight = std::max(Px(80.f), lyricsBottom - lyricsTop);
    float lyricX = ctx.fullScreen ? ctx.fullLyricsX : wp.x + Px(17.f);
    float lyricWidth = ctx.fullScreen ? ctx.fullLyricsWidth : ws.x - Px(39.f);
    // Text tracks the card: the compact sizes were derived from ws.x already,
    // but were clamped to a fixed pixel band so they stopped growing well before
    // the card did.
    float inactiveSize = ctx.fullScreen
        ? 21.f * S : std::clamp(ws.x * 0.046f, 13.5f * S, 15.2f * S);
    float activeSize = ctx.fullScreen
        ? 26.f * S : std::clamp(ws.x * 0.056f, 16.5f * S, 18.5f * S);
    const ImVec2 viewMin(lyricX, lyricsTop);
    const ImVec2 viewMax(lyricX + lyricWidth, lyricsTop + lyricsHeight);

    auto centeredMessage = [&](const char* text, ImFont* font, float size) {
        ImVec2 measured = music_host::Measure(font, size, text);
        music_host::DrawText(dl, font, size,
            ImVec2(wp.x + (ws.x - measured.x) * 0.5f,
                   lyricsTop + (lyricsHeight - measured.y) * 0.5f),
            IM_COL32(255, 255, 255, 150), text);
    };

    if (ctx.lyricsLoading) {
        centeredMessage("Finding synced lyrics...", ctx.regular, 12.f);
    } else if (ctx.instrumental) {
        centeredMessage("Instrumental track", ctx.bold, 13.5f);
    } else if (g_cache.empty()) {
        centeredMessage("No lyrics found for this track", ctx.regular, 12.f);
    } else {
        const int active = ctx.activeLyric;

        bool layoutChanged = lyricsChanged ||
            std::abs(g_layoutWidth - lyricWidth) > 1.f ||
            g_lineHeights.size() != g_cache.size();
        if (layoutChanged) {
            g_lineHeights.resize(g_cache.size());
            g_contentHeight = 0.f;
            g_layoutWidth = lyricWidth;
            float layoutSize = inactiveSize + 0.7f;
            for (int i = 0; i < (int)g_cache.size(); ++i) {
                const int wrapped = LyricWrapLineCount(
                    ctx.regular, layoutSize, g_cache[i].text.c_str(), lyricWidth);
                // Gap between separate lyrics; the leading inside a wrapped
                // lyric comes from kLyricLeading above.
                g_lineHeights[i] = wrapped * layoutSize * kLyricLeading +
                    Px(ctx.fullScreen ? 23.f : 18.f);
                g_contentHeight += g_lineHeights[i];
            }
        }

        float activeOffset = 0.f;
        for (int i = 0; i < active && i < (int)g_lineHeights.size(); ++i)
            activeOffset += g_lineHeights[i];

        const float manualScrollMax = 0.f;
        const float manualScrollMin = std::min(0.f, lyricsHeight - g_contentHeight);
        float followTarget = 0.f;
        if (ctx.lyricsSynced && active >= 0) {
            followTarget = lyricsHeight * (ctx.fullScreen ? 0.46f : 0.48f)
                - activeOffset - g_lineHeights[active] * 0.5f;
        }

        const bool lyricsHovered = ImGui::IsMouseHoveringRect(viewMin, viewMax, true);
        const float wheel = lyricsHovered ? ImGui::GetIO().MouseWheel : 0.f;
        followTarget = std::clamp(followTarget, manualScrollMin, manualScrollMax);

        ImGuiID scrollSpringId = ImGui::GetID("##lyric_scroll_spring");
        if (layoutChanged) {
            g_manualScroll = false;
            g_manualUntilMs = 0;
            g_scrollTarget = ctx.lyricsSynced ? followTarget : 0.f;
            g_scroll = g_scrollTarget;
            music_host::animation::SetSpring(scrollSpringId, g_scrollTarget);
        }

        if (g_positionSyncRequested) {
            g_manualScroll = false;
            g_manualUntilMs = 0;
            g_scrollTarget = followTarget;
            g_positionSyncRequested = false;
        }

        const uint64_t nowMs = GetTickCount64();
        const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
        const bool panelActive = lyricsHovered &&
            (std::abs(mouseDelta.x) > 0.05f ||
             std::abs(mouseDelta.y) > 0.05f ||
             ImGui::IsMouseDown(ImGuiMouseButton_Left));
        if (ctx.lyricsSynced && g_manualScroll && panelActive)
            g_manualUntilMs = nowMs + 5000;
        if (ctx.lyricsSynced && g_manualScroll && nowMs >= g_manualUntilMs) {
            g_manualScroll = false;
        }
        if (ctx.lyricsSynced && wheel != 0.f) {
            if (!g_manualScroll)
                g_scrollTarget = std::clamp(g_scroll,
                                             manualScrollMin, manualScrollMax);
            g_manualScroll = true;
            g_manualUntilMs = nowMs + 5000;
            g_scrollTarget = std::clamp(
                g_scrollTarget + wheel * 62.f, manualScrollMin, manualScrollMax);
        } else if (ctx.lyricsSynced && !g_manualScroll) {
            g_scrollTarget = followTarget;
        }
        if (!ctx.lyricsSynced) {
            if (wheel != 0.f) {
                g_scrollTarget = std::clamp(
                    g_scrollTarget + wheel * 62.f, manualScrollMin, manualScrollMax);
            } else if (layoutChanged) {
                g_scrollTarget = 0.f;
            }
        }
        const bool syncingNow = GetTickCount64() < g_syncPulseUntilMs;
        g_scroll = music_host::animation::SpringF(scrollSpringId, g_scrollTarget,
                                                  syncingNow ? 12.f : 19.f, 1.f);

        dl->PushClipRect(ImVec2(lyricX - 5.f, lyricsTop),
                         ImVec2(lyricX + lyricWidth, lyricsTop + lyricsHeight), true);
        float y = lyricsTop + g_scroll;
        for (int i = 0; i < (int)g_cache.size(); ++i) {
            if (y + g_lineHeights[i] >= lyricsTop &&
                y <= lyricsTop + lyricsHeight) {
                int distance = active >= 0 ? std::abs(i - active) : 3;
                bool isActive = ctx.lyricsSynced && i == active;
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(ImVec2(lyricX, y));
                ImGui::InvisibleButton("##lyric_seek",
                    ImVec2(std::max(1.f, lyricWidth - (ctx.fullScreen ? 2.f : 17.f)),
                           g_lineHeights[i]));
                const bool lineHovered = ImGui::IsItemHovered();
                const bool lineClicked = ImGui::IsItemClicked() &&
                    ctx.lyricsSynced && g_cache[i].timeSec >= 0.0;
                if (lineHovered && ctx.lyricsSynced)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGuiID focusId = ImGui::GetID("##lyric_focus");
                ImGuiID hoverId = ImGui::GetID("##lyric_hover");
                if (layoutChanged)
                    music_host::animation::SetSpring(focusId, isActive ? 1.f : 0.f);
                float focus = std::clamp(music_host::animation::SpringF(
                    focusId, isActive ? 1.f : 0.f, 17.f, 0.92f), 0.f, 1.f);
                float hover = music_host::animation::Anim(hoverId, lineHovered, 15.f);
                ImGui::PopID();

                if (lineClicked) {
                    const double seekSec = g_cache[i].timeSec;
                    media::RequestSeek(seekSec);
                    g_previewSeekSec = seekSec;
                    g_previewSeekTickMs = GetTickCount64();
                    g_manualScroll = false;
                    g_manualUntilMs = 0;
                    g_positionSyncRequested = true;
                    g_syncPulseUntilMs = GetTickCount64() + 650;
                }

                float distanceAlpha = ctx.fullScreen
                    ? (distance == 0 ? 0.46f :
                       distance == 1 ? 0.28f :
                       distance == 2 ? 0.12f : 0.035f)
                    : std::max(0.15f, 0.54f - distance * 0.10f);
                float alpha = distanceAlpha + (1.f - distanceAlpha) * focus;
                alpha = std::min(1.f, alpha + hover * 0.18f);
                float size = inactiveSize + (activeSize - inactiveSize) * focus;
                // Cross-fade the weight across the middle of the transition.
                // Switching fonts outright at focus > 0.48 made the line pop
                // from regular to bold in a single frame while its size was
                // still easing -- the size animated, the weight jumped, and the
                // change read as broken rather than smooth.
                ImFont* font = focus > 0.5f ? ctx.bold : ctx.regular;
                const float blendLo = 0.30f, blendHi = 0.70f;
                float weightBlend = std::clamp(
                    (focus - blendLo) / (blendHi - blendLo), 0.f, 1.f);
                const bool crossFading = focus > blendLo && focus < blendHi;
                float depthOffset = ctx.fullScreen ? 0.f :
                    std::min(5.f, distance * 1.1f) * (1.f - focus) * S;
                ImVec2 textPos(lyricX + depthOffset + hover * 3.f * S,
                               y - focus * 1.5f * S);

                if (hover > 0.01f) {
                    dl->AddRectFilled(
                        ImVec2(lyricX - 6.f, y - 4.f),
                        ImVec2(lyricX + lyricWidth - 16.f,
                               y + g_lineHeights[i] - 5.f),
                        IM_COL32(255, 255, 255, (int)(14.f * hover)), 7.f);
                }
                if (ctx.fullScreen && !isActive && distance <= 3) {
                    const int blurAlpha = (int)(255.f * alpha * 0.07f);
                    const ImVec2 blurOffsets[] = {
                        ImVec2(-3.f, 0.f), ImVec2(3.f, 0.f),
                        ImVec2(0.f, -3.f), ImVec2(0.f, 3.f),
                        ImVec2(-2.1f, -2.1f), ImVec2(2.1f, -2.1f),
                        ImVec2(-2.1f, 2.1f), ImVec2(2.1f, 2.1f),
                        ImVec2(-1.25f, 0.f), ImVec2(1.25f, 0.f),
                        ImVec2(0.f, -1.25f), ImVec2(0.f, 1.25f)
                    };
                    // Must wrap identically to the sharp pass below, or the
                    // glow separates from the glyphs on any lyric that wraps.
                    for (const ImVec2& off : blurOffsets) {
                        DrawWrappedLyric(dl, font, size,
                            ImVec2(textPos.x + off.x, textPos.y + off.y),
                            IM_COL32(255, 255, 255, blurAlpha),
                            g_cache[i].text.c_str(), lyricWidth - depthOffset);
                    }
                }
                const float textAlpha = ctx.fullScreen && !isActive && !lineHovered
                    ? alpha * 0.48f : alpha;
                if (crossFading) {
                    DrawWrappedLyric(dl, ctx.regular, size, textPos,
                        IM_COL32(255, 255, 255,
                            (int)(255.f * textAlpha * (1.f - weightBlend))),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset);
                    DrawWrappedLyric(dl, ctx.bold, size, textPos,
                        IM_COL32(255, 255, 255,
                            (int)(255.f * textAlpha * weightBlend)),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset);
                } else {
                    DrawWrappedLyric(dl, font, size, textPos,
                        IM_COL32(255, 255, 255, (int)(255.f * textAlpha)),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset);
                }

                if (isActive && !ctx.fullScreen) {
                    const double lineStart = g_cache[i].timeSec;
                    double lineEnd = ctx.duration;
                    if (i + 1 < (int)g_cache.size() &&
                        g_cache[i + 1].timeSec >= 0.0) {
                        lineEnd = g_cache[i + 1].timeSec;
                    }
                    const float lineProgress = lineEnd > lineStart
                        ? (float)std::clamp(
                            (ctx.position - lineStart) / (lineEnd - lineStart), 0.0, 1.0)
                        : 1.f;
                    const float wrapWidth = lyricWidth - depthOffset;
                    const float renderedWidth = std::min(wrapWidth,
                        font->CalcTextSizeA(size, FLT_MAX, wrapWidth,
                            g_cache[i].text.c_str()).x);
                    const float sweepRight = textPos.x +
                        renderedWidth * lineProgress;
                    if (sweepRight > textPos.x + 0.5f) {
                        dl->PushClipRect(
                            ImVec2(textPos.x, lyricsTop),
                            ImVec2(sweepRight, lyricsTop + lyricsHeight), true);
                        // Same wrapping as the base pass, otherwise the
                        // karaoke sweep drifts off the glyphs it is filling.
                        DrawWrappedLyric(dl, font, size, textPos,
                            LyricHighlightColor(),
                            g_cache[i].text.c_str(), wrapWidth);
                        dl->PopClipRect();
                    }
                }
            }
            y += g_lineHeights[i];
        }
        dl->PopClipRect();

        if (!ctx.fullScreen && g_cache.size() > 1 &&
            g_contentHeight > lyricsHeight) {
            float trackH = lyricsHeight - 12.f;
            ImGui::SetCursorScreenPos(
                ImVec2(lyricX + lyricWidth - 13.f, lyricsTop));
            ImGui::InvisibleButton("##lyric_scrollbar",
                ImVec2(12.f, lyricsHeight));
            bool scrollbarHovered = ImGui::IsItemHovered();
            bool scrollbarActive = ImGui::IsItemActive();
            if (scrollbarActive) {
                float pointer = std::clamp(
                    (ImGui::GetIO().MousePos.y - lyricsTop - 6.f) /
                    std::max(trackH, 1.f), 0.f, 1.f);
                g_manualScroll = ctx.lyricsSynced;
                g_manualUntilMs = GetTickCount64() + 5000;
                g_scrollTarget = manualScrollMax -
                    pointer * (manualScrollMax - manualScrollMin);
            }
            float progress = 0.f;
            if (g_manualScroll || !ctx.lyricsSynced) {
                float span = std::max(0.001f, manualScrollMax - manualScrollMin);
                progress = std::clamp((manualScrollMax - g_scroll) / span, 0.f, 1.f);
            } else {
                progress = active <= 0 ? 0.f :
                    (float)active / (float)(g_cache.size() - 1);
            }
            float thumbH = std::max(22.f,
                trackH * std::min(1.f, lyricsHeight / g_contentHeight));
            float thumbY = lyricsTop + 6.f + (trackH - thumbH) * progress;
            float scrollFocus = music_host::animation::Anim(
                ImGui::GetID("##lyric_scroll_focus"),
                lyricsHovered || scrollbarHovered || scrollbarActive ||
                g_manualScroll, 12.f);
            // Near-invisible at rest, fades in while the lyrics are hovered or
            // being scrolled. The old resting alpha of 58 left a permanent bar
            // down the side that Matcha never shows.
            dl->AddRectFilled(ImVec2(lyricX + lyricWidth - 7.f, thumbY),
                              ImVec2(lyricX + lyricWidth - 4.f, thumbY + thumbH),
                              IM_COL32(255, 255, 255,
                                  (int)(10 + 96 * scrollFocus)), 2.f);
        }
    }

    // No Sync pill / dots: Matcha has neither, and manual scrolling already
    // re-centres on its own after g_manualUntilMs, so the button was redundant.
    // DrawSyncButton() is kept for now but no longer drawn.
    (void)&DrawSyncButton;

    g_layoutRevision = g_cacheRevision;
}

}  // namespace native_music_player::detail

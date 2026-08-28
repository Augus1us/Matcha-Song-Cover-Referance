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
float    g_layoutTypeScale = -1.f;
float    g_contentHeight = 0.f;

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

void AddTextBlurred(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                    ImU32 col, const char* begin, const char* end, float blur) {
    if (blur < 0.35f) {
        dl->AddText(font, size, pos, col, begin, end);
        return;
    }
    static const float kTap[13][2] = {
        { 0.00f,  0.00f},
        { 1.00f,  0.00f}, { 0.50f,  0.87f}, {-0.50f,  0.87f},
        {-1.00f,  0.00f}, {-0.50f, -0.87f}, { 0.50f, -0.87f},
        { 0.52f,  0.30f}, { 0.00f,  0.60f}, {-0.52f,  0.30f},
        {-0.52f, -0.30f}, { 0.00f, -0.60f}, { 0.52f, -0.30f},
    };
    constexpr int kTaps = 13;
    const float a = (float)((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    if (a <= 0.003f) return;
    const float per = 1.f - std::pow(1.f - a, 1.f / (float)kTaps);
    const ImU32 tapCol = (col & ~IM_COL32_A_MASK) |
        ((ImU32)std::clamp((int)(per * 255.f + 0.5f), 1, 255) << IM_COL32_A_SHIFT);
    for (int i = 0; i < kTaps; ++i) {
        dl->AddText(font, size,
                    ImVec2(pos.x + kTap[i][0] * blur, pos.y + kTap[i][1] * blur),
                    tapCol, begin, end);
    }
}

// Returns the height consumed, so the caller's layout and this stay in step.
float DrawWrappedLyric(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                       ImU32 col, const char* text, float wrapWidth,
                       float blur = 0.f) {
    if (!font || !text) return 0.f;
    const float step = size * kLyricLeading;
    const char* s = text;
    const char* end = text + std::strlen(text);
    float y = pos.y;
    int guard = 0;
    while (s < end && guard++ < 64) {
        const char* wrap = font->CalcWordWrapPosition(size, s, end, wrapWidth);
        if (wrap <= s) wrap = s + 1;
        AddTextBlurred(dl, font, size, ImVec2(pos.x, y), col, s, wrap, blur);
        y += step;
        s = wrap;
        while (s < end && *s == ' ') ++s;
    }
    return y - pos.y;
}
bool     g_lyricsScaledUp = false;
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
                             int activeLyric, bool lyricsLoading) {
    const float artBottom = wp.y + ws.y - 1.f;
    const float textX = wp.x + Px(8.f);
    const float textWidth = ws.x - Px(14.f);
    dl->PushClipRect(ImVec2(wp.x + 1.f, wp.y + 1.f),
                     ImVec2(wp.x + ws.x - 1.f, artBottom), true);

    const float titleSize = Px(19.f);
    const float bylineSize = Px(14.6f);
    const float lyricSize = Px(18.f);

    const std::string artTitle = Ellipsize(title ? title : "", bold, titleSize, textWidth);
    std::string artByline = artist ? artist : "";
    if (album && album[0]) {
        {
            static const char kDash[] = { 32, 32, (char)0xE2, (char)0x80,
                                          (char)0x94, 32, 32, 0 };
            if (!artByline.empty()) artByline += kDash;
        }
        artByline += album;
    }
    artByline = Ellipsize(artByline, bold, bylineSize, textWidth);

    const float railY = wp.y + ws.y - Px(94.f);
    const bool hasLyric = activeLyric >= 0 && activeLyric < (int)g_cache.size();

    const float lyricH = hasLyric
        ? bold->CalcTextSizeA(lyricSize, FLT_MAX, textWidth,
                              g_cache[activeLyric].text.c_str()).y
        : 0.f;
    const float lyricY = railY - lyricH - Px(10.f);
    const float titleY = (hasLyric ? lyricY - Px(22.f) : railY - Px(49.f)) - Px(3.f)
                       - (artByline.empty() ? 0.f : Px(28.f));
    dl->AddText(bold, titleSize, ImVec2(textX, titleY + 1.f),
                IM_COL32(0, 0, 0, 92), artTitle.c_str());
    dl->AddText(bold, titleSize, ImVec2(textX, titleY),
                IM_COL32(255, 255, 255, 248), artTitle.c_str());
    if (!artByline.empty()) {
        dl->AddText(bold, bylineSize, ImVec2(textX + 1.f, titleY + Px(25.f)),
                    IM_COL32(0, 0, 0, 82), artByline.c_str());
        dl->AddText(bold, bylineSize, ImVec2(textX, titleY + Px(24.f)),
                    IM_COL32(255, 255, 255, 190), artByline.c_str());
    }
    {
        const float dotR = std::max(1.6f, Px(2.4f)), step = dotR * 4.2f;
        const float dotY = titleY + (artByline.empty() ? Px(26.f) : Px(44.f));
        const int lit = lyricsLoading
            ? (int)(ImGui::GetTime() * 2.6) % 3 : 0;
        for (int i = 0; i < 3; ++i)
            dl->AddCircleFilled(ImVec2(textX + dotR + i * step, dotY), dotR,
                                IM_COL32(255, 255, 255, i == lit ? 200 : 96), 12);
    }
    if (hasLyric) {
        dl->AddText(bold, lyricSize, ImVec2(textX + 1.f, lyricY + Px(2.f)),
                    IM_COL32(0, 0, 0, 155),
                    g_cache[activeLyric].text.c_str(), nullptr, textWidth);
        dl->AddText(bold, lyricSize, ImVec2(textX, lyricY),
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
    const float syncFont = Px(11.5f);
    ImVec2 syncTextSize = music_host::Measure(ctx.bold, syncFont, syncText);
    ImVec2 syncSize(syncTextSize.x + Px(22.f), Px(25.f));
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
    const float syncRound = (syncDrawMax.y - syncDrawMin.y) * 0.5f;
    music_host::DrawShadow(dl, syncDrawMin, syncDrawMax, syncRound, 8, 9.f, 0.34f);
    dl->AddRectFilled(syncDrawMin, syncDrawMax,
                      IM_COL32(16, 14, 18,
                          (int)((syncHovered ? 238.f : 214.f) + syncGlow * 17.f)),
                      syncRound);
    dl->AddRect(syncDrawMin, syncDrawMax,
                IM_COL32(255, 255, 255,
                    (int)((syncHovered ? 46.f : 26.f) + syncGlow * 55.f)),
                syncRound, 0, 1.f);
    music_host::DrawText(dl, ctx.bold, syncFont,
        ImVec2(syncPos.x + (syncSize.x - syncTextSize.x) * 0.5f,
               syncPos.y + (syncSize.y - syncTextSize.y) * 0.5f +
               (1.f - syncPress) * 1.2f),
        IM_COL32(255, 255, 255, syncHovered ? 255 : 232), syncText);
    if (syncClicked) {
        g_positionSyncRequested = true;
        g_syncPulseUntilMs = GetTickCount64() + 650;
    }
}

void DrawLyricsPanel(const LyricsPanelContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;

    const bool lyricsChanged = g_layoutRevision != g_cacheRevision;
    const float S = ctx.uiScale;
    float lyricsTop = ctx.fullScreen
        ? (ctx.fullArtSize > 1.f ? ctx.fullArtY + ctx.fullArtSize * 0.41f
                                 : wp.y + ws.y * 0.135f)
        : wp.y + Px(82.f);
    float lyricsBottom = ctx.fullScreen ? wp.y + ws.y - Px(52.f)
                                        : wp.y + ws.y - Px(76.f);
    float lyricsHeight = std::max(Px(80.f), lyricsBottom - lyricsTop);
    float lyricX = ctx.fullScreen ? ctx.fullLyricsX : wp.x + Px(17.f);
    float lyricWidth = ctx.fullScreen ? ctx.fullLyricsWidth : ws.x - Px(39.f);
    const float typeScale = g_lyricsScaledUp ? 1.22f : 1.f;
    float inactiveSize = typeScale * (ctx.fullScreen
        ? std::clamp(ws.x * 0.019f, 25.f, 46.f)
        : std::clamp(ws.x * 0.055f, 14.5f * S, 18.5f * S));
    float activeSize = typeScale * (ctx.fullScreen
        ? std::clamp(ws.x * 0.024f, 31.f, 58.f)
        : std::clamp(ws.x * 0.063f, 16.5f * S, 21.f * S));
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
            std::abs(g_layoutTypeScale - typeScale) > 0.001f ||
            g_lineHeights.size() != g_cache.size();
        g_layoutTypeScale = typeScale;
        if (layoutChanged) {
            g_lineHeights.resize(g_cache.size());
            g_contentHeight = 0.f;
            g_layoutWidth = lyricWidth;
            for (int i = 0; i < (int)g_cache.size(); ++i) {
                const int wrapped = LyricWrapLineCount(
                    ctx.bold, activeSize, g_cache[i].text.c_str(), lyricWidth);
                g_lineHeights[i] = wrapped * inactiveSize * kLyricLeading +
                    Px(ctx.fullScreen ? 17.f : 11.f);
                g_contentHeight += g_lineHeights[i];
            }
        }

        float activeOffset = 0.f;
        for (int i = 0; i < active && i < (int)g_lineHeights.size(); ++i)
            activeOffset += g_lineHeights[i];

        const float manualScrollMax = 0.f;
        const float tailLine = g_lineHeights.empty() ? 0.f : g_lineHeights.back();
        const float manualScrollMin =
            std::min(0.f, -std::max(0.f, g_contentHeight - tailLine));
        float followTarget = 0.f;
        if (ctx.lyricsSynced && active >= 0) {
            followTarget = -activeOffset;
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
                    ? (distance == 0 ? 0.86f :
                       distance == 1 ? 0.72f :
                       distance == 2 ? 0.54f : 0.38f)
                    : std::max(0.46f, 0.86f - distance * 0.08f);
                float alpha = distanceAlpha + (1.f - distanceAlpha) * focus;
                alpha = std::min(1.f, alpha + hover * 0.18f);
                float size = inactiveSize + (activeSize - inactiveSize) * focus;
                ImFont* font = ctx.bold;
                const bool crossFading = false;
                const float weightBlend = 1.f;
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
                float blurPx = 0.f;
                if (!isActive && distance >= 2) {
                    const float steps = std::min(3.f, (float)distance - 1.f);
                    blurPx = size * (ctx.fullScreen ? 0.055f : 0.030f) * steps;
                }
                if (lineHovered) blurPx = 0.f;   // pull a line into focus to click it
                float textAlpha = ctx.fullScreen && !isActive && !lineHovered
                    ? alpha * 0.88f : alpha;
                {
                    const float lineMid = y + g_lineHeights[i] * 0.5f;
                    const float topEdge = Px(10.f);
                    const float botEdge = Px(ctx.fullScreen ? 58.f : 34.f);
                    const float topF =
                        std::clamp((lineMid - lyricsTop) / topEdge, 0.f, 1.f);
                    const float botF = std::clamp(
                        (lyricsTop + lyricsHeight - lineMid) / botEdge, 0.f, 1.f);
                    const float ef = topF * topF * (3.f - 2.f * topF) *
                                     botF * botF * (3.f - 2.f * botF);
                    textAlpha *= ef;
                }
                if (crossFading) {
                    DrawWrappedLyric(dl, ctx.regular, size, textPos,
                        IM_COL32(255, 255, 255,
                            (int)(255.f * textAlpha * (1.f - weightBlend))),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset, blurPx);
                    DrawWrappedLyric(dl, ctx.bold, size, textPos,
                        IM_COL32(255, 255, 255,
                            (int)(255.f * textAlpha * weightBlend)),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset, blurPx);
                } else {
                    DrawWrappedLyric(dl, font, size, textPos,
                        IM_COL32(255, 255, 255, (int)(255.f * textAlpha)),
                        g_cache[i].text.c_str(), lyricWidth - depthOffset, blurPx);
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

        if (ctx.lyricsSynced && g_manualScroll && !g_cache.empty()) {
            DrawSyncButton(ctx, lyricsBottom - Px(27.f));
        }

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
            dl->AddRectFilled(ImVec2(lyricX + lyricWidth - 7.f, thumbY),
                              ImVec2(lyricX + lyricWidth - 4.f, thumbY + thumbH),
                              IM_COL32(255, 255, 255,
                                  (int)(10 + 96 * scrollFocus)), 2.f);
        }
    }

    {
        const float dotR = std::max(1.6f, Px(2.4f));
        const float step = dotR * 4.2f;
        const float dotY = std::max(wp.y + Px(22.f),
                                    lyricsTop - Px(ctx.fullScreen ? 20.f : 16.f));
        const int lit = ctx.lyricsLoading
            ? (int)(ImGui::GetTime() * 2.6) % 3 : 0;
        for (int i = 0; i < 3; ++i) {
            dl->AddCircleFilled(ImVec2(lyricX + dotR + i * step, dotY), dotR,
                                IM_COL32(255, 255, 255, i == lit ? 168 : 74), 12);
        }
    }

    g_layoutRevision = g_cacheRevision;
}

bool LyricsScaledUp() { return g_lyricsScaledUp; }

void ToggleLyricsScale() { g_lyricsScaledUp = !g_lyricsScaledUp; }

}  // namespace native_music_player::detail

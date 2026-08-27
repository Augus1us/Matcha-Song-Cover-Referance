#include "media.h"
#include "music_player_host.h"
#include "music_player_internal.h"
#include "music_player_icons.h"

#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdio>

namespace native_music_player::detail {

// Transport glyphs, matched to Apple Music / Matcha.
//
// Previous and next are DOUBLE TRIANGLES (backward.fill / forward.fill), not
// the bar-plus-triangle "skip to start/end" glyph -- that is a different symbol
// and reads as the wrong control at a glance. The two triangles meet at the
// centre with no gap and no bar.
//
// kind: 0 = previous, 1 = play, 2 = pause, 3 = next.
// Transport marks come straight from the SVGs in assets/icons (Bootstrap Icons,
// MIT). Their corners are rounded by small arcs in the path data; the previous
// hand-placed triangles had sharp points, which is what made the row look spiky
// beside the real player.
//
// kind: 0 = previous, 1 = play, 2 = pause, 3 = next.
void DrawMediaGlyph(ImDrawList* dl, ImVec2 c, float r, int kind, ImU32 col) {
    // r is a half-extent; the SVG viewBox spans the whole icon box.
    const float box = r * 2.35f;
    switch (kind) {
    case 0: DrawSvgIcon(dl, icons::kPrevPath, c, box, icons::kPrevViewBox, col); break;
    case 1: DrawSvgIcon(dl, icons::kPlayPath, c, box, icons::kPlayViewBox, col); break;
    case 2: DrawSvgIcon(dl, icons::kPausePath, c, box, icons::kPauseViewBox, col); break;
    case 3: DrawSvgIcon(dl, icons::kNextPath, c, box, icons::kNextViewBox, col); break;
    }
}

// Utility marks: fullscreen and the lyrics bubble come from the same SVG set.
// Shuffle and repeat have no filled equivalent there, so they stay as Lucide
// geometry (https://lucide.dev, ISC) -- stroked, at a matching weight.
//
// kind: 0 = fullscreen, 1 = shuffle, 2 = repeat, 3 = lyrics.
void DrawUtilityGlyph(ImDrawList* dl, ImVec2 c, float r, int kind, ImU32 col) {
    const float box = r * 2.35f;
    if (kind == 0) {
        DrawSvgIcon(dl, icons::kFullscreenPath, c, box, icons::kFullscreenViewBox, col);
        return;
    }
    if (kind == 3) {
        // The soft bubble is a stroked outline plus filled quote marks, so its
        // two subpaths take different operations rather than both being filled.
        StrokeSvgPath(dl, icons::kLyricsPath0, c, box, icons::kLyricsViewBox, col,
                      std::max(1.15f, r * 0.17f));
        DrawSvgIcon(dl, icons::kLyricsPath1, c, box, icons::kLyricsViewBox, col);
        return;
    }
    const float k = std::max(2.4f, r * 0.92f) / 12.f;   // 24x24 grid
    const float stroke = std::max(1.15f, r * 0.19f);
    auto P = [&](float x, float y) {
        return ImVec2(c.x + (x - 12.f) * k, c.y + (y - 12.f) * k);
    };
    auto poly = [&](std::initializer_list<ImVec2> pts) {
        for (const ImVec2& p : pts) dl->PathLineTo(p);
        dl->PathStroke(col, 0, stroke);
        dl->AddCircleFilled(*pts.begin(), stroke * 0.5f, col, 8);
        dl->AddCircleFilled(*(pts.end() - 1), stroke * 0.5f, col, 8);
    };
    if (kind == 1) {          // lucide "shuffle"
        poly({P(18, 2), P(22, 6), P(18, 10)});
        poly({P(18, 14), P(22, 18), P(18, 22)});
        poly({P(2, 18), P(3.97f, 18), P(5.7f, 17.6f), P(7.27f, 16.3f),
              P(12.73f, 7.7f), P(14.3f, 6.4f), P(16.03f, 6), P(22, 6)});
        poly({P(2, 6), P(3.97f, 6), P(5.9f, 6.5f), P(7.57f, 8.2f)});
        poly({P(22, 18), P(15.96f, 18), P(14.1f, 17.6f), P(12.66f, 16.2f),
              P(12.3f, 15.75f)});
    } else {                  // lucide "repeat"
        poly({P(17, 2), P(21, 6), P(17, 10)});
        poly({P(3, 11), P(3, 10), P(3.6f, 8), P(5, 6.6f), P(7, 6), P(21, 6)});
        poly({P(7, 22), P(3, 18), P(7, 14)});
        poly({P(21, 13), P(21, 14), P(20.4f, 16), P(19, 17.4f), P(17, 18), P(3, 18)});
    }
}

void DrawTransportControls(const TransportContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;
    const bool fullScreen = *ctx.fullScreen;
    const bool compactMusic = ctx.compact;
    const bool artworkView = *ctx.artworkView;
    const bool showLyrics = *ctx.showLyrics;
    const float S = ctx.uiScale;

    const float barY = fullScreen ? wp.y + ws.y - Px(103.f)
        : (compactMusic ? wp.y + ws.y - Px(63.f)
                        : (artworkView ? wp.y + ws.y - Px(78.f)
                                       : wp.y + ws.y - Px(59.f)));
    const float barThickness = Px(fullScreen ? 2.5f : 4.f);
    const float leftInset = Px(compactMusic ? 16.f : (artworkView ? 7.f : 13.f));
    const float rightInset = Px(compactMusic ? 16.f : (artworkView ? 13.f : 8.f));
    ImVec2 barMin(fullScreen ? ctx.fullColumnX : wp.x + leftInset, barY),
           barMax(fullScreen ? ctx.fullColumnX + ctx.fullColumnWidth : wp.x + ws.x - rightInset,
                  barY + barThickness);
    static double timelineDragSeekSec = -1.0;
    double displayPosition = ctx.position;
    float displayProgress = ctx.progress;
    ImGui::PushID("music_timeline");
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barY - Px(6.f)));
    ImGui::InvisibleButton("##seek", ImVec2(barMax.x - barMin.x, Px(14.f)));
    if (ImGui::IsItemActive() && ctx.duration > 0.0) {
        const float pointer = std::clamp(
            (ImGui::GetIO().MousePos.x - barMin.x) /
                std::max(1.f, barMax.x - barMin.x),
            0.f, 1.f);
        timelineDragSeekSec = pointer * ctx.duration;
        displayPosition = timelineDragSeekSec;
        displayProgress = pointer;
    }
    if (ImGui::IsItemDeactivated() && timelineDragSeekSec >= 0.0) {
        media::RequestSeek(timelineDragSeekSec);
        displayPosition = timelineDragSeekSec;
        displayProgress = ctx.duration > 0.0
            ? (float)(timelineDragSeekSec / ctx.duration) : 0.f;
        timelineDragSeekSec = -1.0;
    }
    ImGui::PopID();
    dl->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, 76), 2.f);
    dl->AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * displayProgress, barMax.y),
                      IM_COL32(244, 248, 252, 228), 2.f);
    if (ctx.duration > 0.0) {
        const float thumbX = barMin.x + (barMax.x - barMin.x) * displayProgress;
        dl->AddCircleFilled(ImVec2(thumbX, barY + barThickness * 0.5f),
                            (fullScreen ? 1.9f : (compactMusic ? 2.15f : 2.8f)) * S,
                            IM_COL32(255, 255, 255, 240), 18);
    }

    auto fmt = [](double s, char* b, size_t n) {
        int t = (int)s; std::snprintf(b, n, "%d:%02d", t / 60, t % 60);
    };
    char pb[16], rb[20], remaining[16];
    fmt(displayPosition, pb, sizeof(pb));
    fmt(std::max(0.0, ctx.duration - displayPosition), remaining, sizeof(remaining));
    std::snprintf(rb, sizeof(rb), "-%s", remaining);
    const float timeSize = (compactMusic ? 9.5f : (fullScreen ? 10.5f : 10.f)) * S;
    const float timeY = barY + Px(6.f);
    music_host::DrawText(dl, ctx.regular, timeSize, ImVec2(barMin.x, timeY),
        IM_COL32(255, 255, 255, 136), pb);
    music_host::DrawText(dl, ctx.regular, timeSize,
        ImVec2(barMax.x - music_host::Measure(ctx.regular, timeSize, rb).x, timeY),
        IM_COL32(255, 255, 255, 136), rb);
    const char* status = "Lossless";
    const float statusTextSize = (compactMusic ? 10.f : (fullScreen ? 10.5f : 10.5f)) * S;
    ImVec2 statusSize = music_host::Measure(ctx.bold, statusTextSize, status);
    music_host::DrawText(dl, ctx.bold, statusTextSize,
        ImVec2((barMin.x + barMax.x - statusSize.x) * 0.5f, timeY),
        IM_COL32(255, 255, 255, 172), status);

    const float cy = fullScreen ? barY + Px(34.f)
        : (compactMusic ? wp.y + ws.y - Px(23.f)
                        : (artworkView ? wp.y + ws.y - Px(31.f)
                                       : wp.y + ws.y - Px(20.f)));
    float cx = fullScreen ? (barMin.x + barMax.x) * 0.5f
        : wp.x + ws.x * (compactMusic ? 0.48f : 0.5f);
    // Spacing is capped by the width actually available, not just scaled: the
    // row also carries the utility glyphs at each end, and at narrow widths the
    // fixed 35-69px spacing pushed next/repeat into each other. Reserving room
    // for the outer glyphs and dividing what's left keeps them apart at every
    // size instead of overlapping once the card is small.
    const float outerReserve = Px(compactMusic ? 30.f : 56.f);
    const float usable = std::max(60.f, ws.x - outerReserve * 2.f);
    const float wantSpacing = (compactMusic ? 37.f
        : (fullScreen ? 56.f : (artworkView ? 69.f : 35.f))) * S;
    const float controlSpacing = std::min(wantSpacing, usable * 0.5f * 0.62f);
    const float primaryRadius = (compactMusic ? 12.f : (fullScreen ? 19.f : 15.f)) * S;
    const float secondaryRadius = (compactMusic ? 9.5f : (fullScreen ? 15.5f : 13.f)) * S;
    struct Ctl { float dx; int kind; } ctls[3] = {
        {-controlSpacing, 0}, {0, -1}, {controlSpacing, 3}
    };
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        float bx = cx + ctls[i].dx;
        float hitRadius = i == 1 ? primaryRadius : secondaryRadius;
        ImGui::SetCursorScreenPos(ImVec2(bx - hitRadius, cy - hitRadius));
        ImGui::InvisibleButton("##mc", ImVec2(hitRadius * 2.f, hitRadius * 2.f));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        float hv = music_host::animation::Anim(ImGui::GetID("##h"), hov);
        float bnc = music_host::animation::ClickBounce(ImGui::GetID("##b"), clk);
        int kind = ctls[i].kind;
        if (kind == -1) kind = ctx.playing ? 2 : 1;
        if (i == 1) {
            // Plain white glyph in EVERY mode. The lyrics/expanded layout used
            // to put play/pause inside a filled white disc -- that is Spotify's
            // treatment, not Apple's, and it made the transport row read as a
            // different app the moment the card was expanded.
            if (hv > 0.01f)
                dl->AddCircleFilled(ImVec2(bx, cy), primaryRadius,
                                    IM_COL32(255, 255, 255, (int)(18 * hv)), 20);
            DrawMediaGlyph(dl, ImVec2(bx, cy), 7.3f * S * bnc, kind,
                           IM_COL32(255, 255, 255, (int)(225 + 30 * hv)));
        } else {
            if (hv > 0.01f)
                dl->AddCircleFilled(ImVec2(bx, cy), secondaryRadius,
                                    IM_COL32(255, 255, 255, (int)(22 * hv)), 20);
            DrawMediaGlyph(dl, ImVec2(bx, cy),
                           (compactMusic ? 4.2f : 5.5f) * S * bnc, kind,
                           IM_COL32(255, 255, 255, (int)(185 + 55 * hv)));
        }
        if (clk) {
            if (i == 0)      media::RequestSkipPrevious();
            else if (i == 1) media::RequestTogglePlayPause();
            else             media::RequestSkipNext();
        }
        ImGui::PopID();
    }

    auto utilityButton = [&](const char* id, const ImVec2& center, int icon,
                             bool selected) {
        ImGui::PushID(id);
        const float hit = Px(10.f);
        ImGui::SetCursorScreenPos(ImVec2(center.x - hit, center.y - hit));
        ImGui::InvisibleButton("##utility", ImVec2(hit * 2.f, hit * 2.f));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const float press = music_host::animation::ClickBounce(
            ImGui::GetID("##utility_press"), clicked);
        if ((hovered || selected) && !(fullScreen && selected)) {
            dl->AddCircleFilled(center, Px(10.f) * press,
                                IM_COL32(255, 255, 255, selected ? 30 : 16), 16);
        }
        DrawUtilityGlyph(dl, center,
                         (compactMusic ? 5.5f : (fullScreen ? 5.7f : 6.4f)) * S * press,
                         icon,
                         IM_COL32(255, 255, 255,
                                  selected ? 226 : (hovered ? 196 : 132)));
        ImGui::PopID();
        return clicked;
    };

    bool fullScreenClicked = false;
    bool shuffleClicked = false;
    bool repeatClicked = false;
    bool lyricsClicked = false;
    // Shuffle and repeat are anchored to the TRANSPORT ROW, not to the card
    // edges. They used to sit at fixed insets (wp.x + 75, ws.x - 70) while the
    // row stayed centred, so on a narrow card the row grew out to meet them and
    // next/repeat drew on top of each other. Placing them one gap outside the
    // outermost transport button makes an overlap geometrically impossible, and
    // the clamp keeps them inside the card when it is very small.
    // One even step between all five marks: shuffle | prev | play | next |
    // repeat sit at cx + k*controlSpacing. Using a smaller side gap put shuffle
    // and repeat much closer to prev/next than prev/next were to play, so the
    // row read as three groups instead of one evenly spaced set.
    const float sideGap = controlSpacing;
    const float edgeInset = Px(compactMusic ? 18.f : 20.f);
    const float shuffleX = std::max(wp.x + edgeInset, cx - controlSpacing - sideGap);
    const float repeatX = std::min(wp.x + ws.x - edgeInset,
                                   cx + controlSpacing + sideGap);
    if (artworkView) {
        shuffleClicked = utilityButton("shuffle_toggle", ImVec2(shuffleX, cy),
                                       1, ctx.shuffleActive);
        repeatClicked = utilityButton("repeat_toggle", ImVec2(repeatX, cy),
                                      2, ctx.repeatActive);
    } else {
        fullScreenClicked = !fullScreen && utilityButton(
            "fullscreen_toggle", ImVec2(wp.x + edgeInset, cy), 0, false);
        shuffleClicked = utilityButton(
            "shuffle_toggle",
            ImVec2(fullScreen ? barMin.x + Px(3.f)
                              : std::max(wp.x + edgeInset + Px(24.f), shuffleX), cy),
            1, ctx.shuffleActive);
        repeatClicked = utilityButton(
            "repeat_toggle",
            ImVec2(fullScreen ? barMax.x - Px(7.f)
                              : std::min(wp.x + ws.x - edgeInset - Px(24.f), repeatX), cy),
            2, ctx.repeatActive);
        lyricsClicked = utilityButton(
            "lyrics_toggle",
            ImVec2(fullScreen ? wp.x + Px(14.f) : wp.x + ws.x - edgeInset,
                   fullScreen ? wp.y + ws.y - Px(20.f) : cy), 3, showLyrics);
    }

    if (fullScreenClicked) {
        *ctx.fullScreen = true;
        *ctx.showLyrics = true;
        *ctx.artworkView = false;
    }
    if (shuffleClicked) media::RequestToggleShuffle();
    if (repeatClicked) media::RequestToggleRepeat();
    if (lyricsClicked) {
        *ctx.showLyrics = !showLyrics;
    }
}

}  // namespace native_music_player::detail

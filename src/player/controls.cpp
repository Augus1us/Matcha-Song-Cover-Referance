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
// kind: 0 = expand, 1 = shuffle, 2 = repeat, 3 = lyrics bubble, 4 = "Aa" bubble.
void DrawUtilityGlyph(ImDrawList* dl, ImVec2 c, float r, int kind, ImU32 col,
                      ImFont* labelFont) {
    const float box = r * 2.35f;
    if (kind == 0) {
        // Two opposed diagonal arrows, not the square bracket the fullscreen SVG
        // draws. The reference's expand mark is a resize arrow pair: a head at
        // the upper-left and one at the lower-right, each with a short tail, and
        // no connecting shaft between them.
        const float a = r * 0.74f, head = r * 0.50f;
        const float w = std::max(1.25f, r * 0.23f);
        const ImVec2 tl(c.x - a, c.y - a), br(c.x + a, c.y + a);
        // Upper-left arrow: tail runs inward, head opens back out.
        dl->AddLine(tl, ImVec2(tl.x + head * 1.15f, tl.y + head * 1.15f), col, w);
        dl->AddLine(tl, ImVec2(tl.x + head, tl.y), col, w);
        dl->AddLine(tl, ImVec2(tl.x, tl.y + head), col, w);
        // Lower-right arrow, mirrored.
        dl->AddLine(br, ImVec2(br.x - head * 1.15f, br.y - head * 1.15f), col, w);
        dl->AddLine(br, ImVec2(br.x - head, br.y), col, w);
        dl->AddLine(br, ImVec2(br.x, br.y - head), col, w);
        return;
    }
    if (kind == 3 || kind == 4) {
        // SOLID speech bubble with the label knocked out of it. The reference
        // draws a filled white bubble with dark glyphs inside; we used to stroke
        // the outline and fill the quote marks in the same colour, which reads
        // as a hollow outline mark and is much lighter than their solid chip.
        //
        // The bubble is the icon's own outline path, filled instead of stroked,
        // so the corner radii and the tail come from the artwork rather than
        // from a rounded rect guessed to match it.
        DrawSvgIcon(dl, icons::kLyricsPath0, c, box, icons::kLyricsViewBox, col);
        // Glyphs are punched in the CARD's ink, not in white -- they are holes
        // in the filled shape. Alpha tracks the bubble's so it dims as one.
        const int alpha = (int)((col >> IM_COL32_A_SHIFT) & 0xFF);
        const ImU32 ink = IM_COL32(52, 43, 48, alpha);
        if (kind == 3) {
            // The quote marks are a subpath of the same SVG, already positioned
            // inside the bubble, so they land correctly at any size.
            DrawSvgIcon(dl, icons::kLyricsPath1, c, box, icons::kLyricsViewBox, ink);
        } else if (labelFont) {
            // Body of the bubble spans y 4.75..16.25 of a 24 viewBox, so its
            // centre sits slightly above the icon's, and the tail hangs below.
            const float unit = box / icons::kLyricsViewBox;
            const float bodyH = 11.5f * unit;
            const float bodyCy = c.y + (10.5f - 12.f) * unit;
            const float ls = bodyH * 0.66f;
            const ImVec2 m = labelFont->CalcTextSizeA(ls, FLT_MAX, 0.f, "Aa");
            dl->AddText(labelFont, ls,
                        ImVec2(c.x - m.x * 0.5f, bodyCy - m.y * 0.5f), ink, "Aa");
        }
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

    // In fullscreen the progress bar sits directly under the art + title block
    // (which is vertically centred), instead of being pinned to the very bottom
    // of the window -- that pinning is what left the huge empty middle on a
    // truly fullscreen window. The gap below the art tracks the title size so it
    // scales with the art.
    const float fsTitle = std::clamp(ctx.fullArtSize * 0.072f, 17.f, 30.f);
    const float barY = fullScreen
        ? ctx.fullArtY + ctx.fullArtSize + fsTitle * 3.4f + 20.f
        : (compactMusic ? wp.y + ws.y - Px(63.f)
                        : (artworkView ? wp.y + ws.y - Px(78.f)
                                       : wp.y + ws.y - Px(59.f)));
    const float barThickness = fullScreen ? std::max(2.5f, ctx.fullArtSize * 0.012f)
                                          : Px(4.f);
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
    // Fully pill-rounded track and fill (radius = half thickness), matching the
    // reference's rounder bar.
    const float barR = (barMax.y - barMin.y) * 0.5f;
    dl->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, 76), barR);
    dl->AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * displayProgress, barMax.y),
                      IM_COL32(244, 248, 252, 228), barR);
    if (ctx.duration > 0.0) {
        const float thumbX = barMin.x + (barMax.x - barMin.x) * displayProgress;
        dl->AddCircleFilled(ImVec2(thumbX, barY + barThickness * 0.5f),
                            fullScreen ? std::clamp(ctx.fullArtSize * 0.012f, 1.9f, 5.f)
                                       : (compactMusic ? 2.15f : 2.8f) * S,
                            IM_COL32(255, 255, 255, 240), 18);
    }

    auto fmt = [](double s, char* b, size_t n) {
        int t = (int)s; std::snprintf(b, n, "%d:%02d", t / 60, t % 60);
    };
    char pb[16], rb[20], remaining[16];
    fmt(displayPosition, pb, sizeof(pb));
    fmt(std::max(0.0, ctx.duration - displayPosition), remaining, sizeof(remaining));
    std::snprintf(rb, sizeof(rb), "-%s", remaining);
    const float timeSize = fullScreen ? std::clamp(ctx.fullArtSize * 0.035f, 10.5f, 17.f)
                                      : (compactMusic ? 9.5f : 10.f) * S;
    const float timeY = barY + Px(6.f);
    music_host::DrawText(dl, ctx.regular, timeSize, ImVec2(barMin.x, timeY),
        IM_COL32(255, 255, 255, 136), pb);
    music_host::DrawText(dl, ctx.regular, timeSize,
        ImVec2(barMax.x - music_host::Measure(ctx.regular, timeSize, rb).x, timeY),
        IM_COL32(255, 255, 255, 136), rb);
    const char* status = "Lossless";
    const float statusTextSize = fullScreen ? std::clamp(ctx.fullArtSize * 0.035f, 10.5f, 17.f)
                                            : (compactMusic ? 10.f : 10.5f) * S;
    ImVec2 statusSize = music_host::Measure(ctx.bold, statusTextSize, status);
    music_host::DrawText(dl, ctx.bold, statusTextSize,
        ImVec2((barMin.x + barMax.x - statusSize.x) * 0.5f, timeY),
        IM_COL32(255, 255, 255, 172), status);

    const float cy = fullScreen ? barY + std::max(34.f, ctx.fullArtSize * 0.11f)
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
    const float primaryRadius = fullScreen
        ? std::clamp(ctx.fullArtSize * 0.072f, 19.f, 34.f)
        : (compactMusic ? 13.5f : 17.f) * S;
    const float secondaryRadius = fullScreen
        ? std::clamp(ctx.fullArtSize * 0.058f, 15.5f, 27.f)
        : (compactMusic ? 11.f : 14.5f) * S;
    // Glyph half-extents. In fullscreen these scale with the button (which
    // scales with the art); elsewhere they keep their tuned pixel sizes times
    // the UI scale. Without this the fullscreen glyphs stayed ~7px inside a
    // 26px button, because their size was tied to uiScale, which is 1 when the
    // window itself is fullscreen.
    const float playGlyph = fullScreen ? primaryRadius * 0.46f : 8.7f * S;
    const float skipGlyph = fullScreen ? secondaryRadius * 0.58f
                                       : (compactMusic ? 5.2f : 6.7f) * S;
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
        float hv = music_host::animation::Anim(ImGui::GetID("##h"), hov, 20.f);
        float bnc = music_host::animation::ClickBounce(ImGui::GetID("##b"), clk);
        float glow = music_host::animation::ClickGlow(ImGui::GetID("##g"), clk);
        float press = music_host::animation::PressPulse(ImGui::GetID("##p"), clk);
        int kind = ctls[i].kind;
        if (kind == -1) kind = ctx.playing ? 2 : 1;
        const float radius = (i == 1) ? primaryRadius : secondaryRadius;
        // Hover: a soft disc fades and grows in behind the glyph.
        if (hv > 0.01f)
            dl->AddCircleFilled(ImVec2(bx, cy), radius * (0.88f + 0.22f * hv),
                                IM_COL32(255, 255, 255, (int)(12 + 40 * hv)), 24);
        // Click: an expanding ring ripples out and fades.
        if (glow >= 0.f) {
            const float rr = radius * (0.85f + glow * 1.55f);
            const float a = 120.f * (1.f - glow);
            dl->AddCircle(ImVec2(bx, cy), rr, IM_COL32(255, 255, 255, (int)a),
                          0, std::max(1.f, 2.4f * (1.f - glow)));
        }
        // Glyph grows a touch on hover, springs on click, dips on the press.
        const float baseGlyph = (i == 1) ? playGlyph : skipGlyph;
        const float glyphSz = baseGlyph * bnc * (1.f + 0.08f * hv) * (1.f - 0.12f * press);
        const int baseAlpha = (i == 1) ? 225 : 185;
        const int hoverAdd  = (i == 1) ? 30 : 55;
        DrawMediaGlyph(dl, ImVec2(bx, cy), glyphSz, kind,
                       IM_COL32(255, 255, 255, (int)(baseAlpha + hoverAdd * hv)));
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
        // Hitbox matches the drawn size (bigger in fullscreen), so the clickable
        // area is not a tiny 10px square under a large glyph.
        const float utilHit = fullScreen
            ? std::clamp(ctx.fullArtSize * 0.055f, 10.f, 20.f) : Px(11.f);
        ImGui::SetCursorScreenPos(ImVec2(center.x - utilHit, center.y - utilHit));
        ImGui::InvisibleButton("##utility", ImVec2(utilHit * 2.f, utilHit * 2.f));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const float hv = music_host::animation::Anim(
            ImGui::GetID("##uh"), hovered, 20.f);
        const float bnc = music_host::animation::ClickBounce(
            ImGui::GetID("##ub"), clicked);
        const float glow = music_host::animation::ClickGlow(
            ImGui::GetID("##ug"), clicked);
        const float press = music_host::animation::PressPulse(
            ImGui::GetID("##up"), clicked);
        // The two bubble marks are solid chips in the reference; a disc behind
        // them just muddies their edge, so only the stroked marks get one.
        const bool bubble = (icon == 3 || icon == 4);
        const float fill = std::max(hv, selected ? 1.f : 0.f);
        if (fill > 0.01f && !bubble && !(fullScreen && selected))
            dl->AddCircleFilled(center, utilHit * (0.82f + 0.2f * fill),
                IM_COL32(255, 255, 255, (int)((selected ? 26 : 12) + 26 * hv)), 24);
        // Click: expanding ring ripple.
        if (glow >= 0.f)
            dl->AddCircle(center, utilHit * (0.8f + glow * 1.5f),
                IM_COL32(255, 255, 255, (int)(100.f * (1.f - glow))),
                0, std::max(1.f, 2.f * (1.f - glow)));
        const float utilGlyph = fullScreen
            ? std::clamp(ctx.fullArtSize * 0.035f, 5.7f, 13.f)
            : (compactMusic ? 6.4f : 7.4f) * S;
        // A filled chip at 132 alpha reads as washed-out grey, where the
        // reference's bubbles are near-solid white whether or not lyrics are on.
        const int glyphAlpha = bubble
            ? (selected ? 244 : (int)(206 + 40 * hv))
            : (selected ? 232 : (int)(132 + 74 * hv));
        DrawUtilityGlyph(dl, center,
            utilGlyph * bnc * (1.f + 0.08f * hv) * (1.f - 0.12f * press), icon,
            IM_COL32(255, 255, 255, glyphAlpha), ctx.bold);
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
        // In fullscreen these used to be pinned to the ends of the progress bar
        // while prev/play/next stayed on their even step, so the row came out as
        // 38/56/56/35 instead of one rhythm. The reference's five marks are
        // evenly spaced, so shuffle and repeat take the same step as the rest.
        shuffleClicked = utilityButton(
            "shuffle_toggle",
            ImVec2(fullScreen ? shuffleX
                              : std::max(wp.x + edgeInset + Px(24.f), shuffleX), cy),
            1, ctx.shuffleActive);
        // Repeat is pulled further in than before: the right end of the row now
        // carries TWO bubbles rather than one, and at the old clamp repeat sat
        // exactly where the "Aa" chip goes.
        repeatClicked = utilityButton(
            "repeat_toggle",
            ImVec2(fullScreen ? repeatX
                              : std::min(wp.x + ws.x - edgeInset - Px(46.f), repeatX), cy),
            2, ctx.repeatActive);
        // The reference pairs the lyrics bubble with an "Aa" bubble: bottom
        // right of the panel row (Aa then quote), bottom left in fullscreen
        // (quote then Aa). Extra clearance from the right edge keeps the quote
        // off the resize grip in the corner.
        const float bubbleGap = fullScreen
            ? std::clamp(ctx.fullArtSize * 0.09f, 22.f, 46.f) : Px(22.f);
        const float quoteX = fullScreen ? wp.x + Px(16.f)
                                        : wp.x + ws.x - edgeInset - Px(3.f);
        const float bubbleY = fullScreen ? wp.y + ws.y - Px(20.f) : cy;
        const float aaX = fullScreen ? quoteX + bubbleGap : quoteX - bubbleGap;
        if (utilityButton("lyric_scale_toggle", ImVec2(aaX, bubbleY), 4,
                          LyricsScaledUp()))
            ToggleLyricsScale();
        lyricsClicked = utilityButton(
            "lyrics_toggle", ImVec2(quoteX, bubbleY), 3, showLyrics);
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

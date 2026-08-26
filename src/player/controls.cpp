#include "media.h"
#include "music_player_host.h"
#include "music_player_internal.h"

#include <algorithm>
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
void DrawMediaGlyph(ImDrawList* dl, ImVec2 c, float r, int kind, ImU32 col) {
    // Apple's transport marks are wider than tall; a full-height triangle looks
    // stubby next to the real thing.
    const float h = r * 0.86f;
    // Apple leaves a hairline between the two marks. Sharing a vertex fuses
    // them into one arrow with a notch, which is a different symbol.
    const float g = r * 0.16f;
    switch (kind) {
    case 0:
        dl->AddTriangleFilled(ImVec2(c.x - g,     c.y - h), ImVec2(c.x - g,     c.y + h),
                              ImVec2(c.x - r,     c.y), col);
        dl->AddTriangleFilled(ImVec2(c.x + r,     c.y - h), ImVec2(c.x + r,     c.y + h),
                              ImVec2(c.x + g,     c.y), col);
        break;
    case 1: {
        // Nudged right of centre: a play triangle centred on its bounding box
        // reads as sitting too far left, because its visual mass is at the base.
        const float px = c.x - r * 0.18f;
        dl->AddTriangleFilled(ImVec2(px,         c.y - h * 1.06f),
                              ImVec2(px,         c.y + h * 1.06f),
                              ImVec2(px + r * 1.7f, c.y), col);
        break;
    }
    case 2: {
        const float bw = std::max(2.f, r * 0.36f);      // bar width
        const float gap = r * 0.30f;
        dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - h * 1.06f),
                          ImVec2(c.x - gap,      c.y + h * 1.06f), col, bw * 0.42f);
        dl->AddRectFilled(ImVec2(c.x + gap,      c.y - h * 1.06f),
                          ImVec2(c.x + gap + bw, c.y + h * 1.06f), col, bw * 0.42f);
        break;
    }
    case 3:
        dl->AddTriangleFilled(ImVec2(c.x + g,     c.y - h), ImVec2(c.x + g,     c.y + h),
                              ImVec2(c.x + r,     c.y), col);
        dl->AddTriangleFilled(ImVec2(c.x - r,     c.y - h), ImVec2(c.x - r,     c.y + h),
                              ImVec2(c.x - g,     c.y), col);
        break;
    }
}

void DrawUtilityGlyph(ImDrawList* dl, ImVec2 c, float r, int kind, ImU32 col) {
    if (ImFont* icons = music_host::overlay::GetFont(2)) {
        static const char* glyphs[] = {
            "\xEE\x9D\x80",
            "\xEE\xA2\xB1",
            "\xEE\xA3\xAE",
            "\xEE\xA3\xB2"
        };
        const char* glyph = glyphs[std::clamp(kind, 0, 3)];
        const float px = r * 2.25f;
        const ImVec2 measured = icons->CalcTextSizeA(px, FLT_MAX, 0.f, glyph);
        dl->AddText(icons, px,
                    ImVec2(c.x - measured.x * 0.5f,
                           c.y - measured.y * 0.5f - 0.5f),
                    col, glyph);
        return;
    }
    const float k = std::max(2.8f, r * 0.72f);
    const float stroke = std::max(1.05f, r * 0.17f);
    switch (kind) {
    case 0:
        dl->AddLine(ImVec2(c.x - k, c.y - k * 0.25f), ImVec2(c.x - k, c.y - k), col, stroke);
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x - k * 0.25f, c.y - k), col, stroke);
        dl->AddLine(ImVec2(c.x + k, c.y - k * 0.25f), ImVec2(c.x + k, c.y - k), col, stroke);
        dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x + k * 0.25f, c.y - k), col, stroke);
        dl->AddLine(ImVec2(c.x - k, c.y + k * 0.25f), ImVec2(c.x - k, c.y + k), col, stroke);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x - k * 0.25f, c.y + k), col, stroke);
        dl->AddLine(ImVec2(c.x + k, c.y + k * 0.25f), ImVec2(c.x + k, c.y + k), col, stroke);
        dl->AddLine(ImVec2(c.x + k, c.y + k), ImVec2(c.x + k * 0.25f, c.y + k), col, stroke);
        break;
    case 1: {
        // Two paths crossing, arrowheads on the right. The previous version was
        // the right shape but drawn inside k (~4px here): the horizontal runs
        // collapsed to nothing and what survived read as a Bluetooth mark.
        // Widening past k and shortening the verticals makes it legible at the
        // size it is actually rendered.
        const float w = k * 1.18f;
        const float hh = k * 0.60f;
        const float tip = w * 0.98f;
        const float headBack = w * 0.55f;
        const float headHalf = hh * 0.62f;
        // lower-left -> upper-right
        dl->AddLine(ImVec2(c.x - w, c.y + hh), ImVec2(c.x - w * 0.42f, c.y + hh), col, stroke);
        dl->AddLine(ImVec2(c.x - w * 0.42f, c.y + hh), ImVec2(c.x + w * 0.42f, c.y - hh), col, stroke);
        dl->AddLine(ImVec2(c.x + w * 0.42f, c.y - hh), ImVec2(c.x + headBack, c.y - hh), col, stroke);
        dl->AddTriangleFilled(ImVec2(c.x + tip, c.y - hh),
                              ImVec2(c.x + headBack, c.y - hh - headHalf),
                              ImVec2(c.x + headBack, c.y - hh + headHalf), col);
        // upper-left -> lower-right
        dl->AddLine(ImVec2(c.x - w, c.y - hh), ImVec2(c.x - w * 0.42f, c.y - hh), col, stroke);
        dl->AddLine(ImVec2(c.x - w * 0.42f, c.y - hh), ImVec2(c.x + w * 0.42f, c.y + hh), col, stroke);
        dl->AddLine(ImVec2(c.x + w * 0.42f, c.y + hh), ImVec2(c.x + headBack, c.y + hh), col, stroke);
        dl->AddTriangleFilled(ImVec2(c.x + tip, c.y + hh),
                              ImVec2(c.x + headBack, c.y + hh - headHalf),
                              ImVec2(c.x + headBack, c.y + hh + headHalf), col);
        break;
    }
    case 2:
        dl->PathLineTo(ImVec2(c.x - k * 0.94f, c.y + k * 0.02f));
        dl->PathBezierCubicCurveTo(
            ImVec2(c.x - k * 0.94f, c.y - k * 0.43f),
            ImVec2(c.x - k * 0.58f, c.y - k * 0.62f),
            ImVec2(c.x - k * 0.20f, c.y - k * 0.62f));
        dl->PathLineTo(ImVec2(c.x + k * 0.58f, c.y - k * 0.62f));
        dl->PathStroke(col, 0, stroke);
        dl->AddTriangleFilled(ImVec2(c.x + k, c.y - k * 0.46f),
                              ImVec2(c.x + k * 0.42f, c.y - k * 0.88f),
                              ImVec2(c.x + k * 0.42f, c.y - k * 0.04f), col);
        dl->PathLineTo(ImVec2(c.x + k * 0.94f, c.y - k * 0.02f));
        dl->PathBezierCubicCurveTo(
            ImVec2(c.x + k * 0.94f, c.y + k * 0.43f),
            ImVec2(c.x + k * 0.58f, c.y + k * 0.62f),
            ImVec2(c.x + k * 0.20f, c.y + k * 0.62f));
        dl->PathLineTo(ImVec2(c.x - k * 0.58f, c.y + k * 0.62f));
        dl->PathStroke(col, 0, stroke);
        dl->AddTriangleFilled(ImVec2(c.x - k, c.y + k * 0.46f),
                              ImVec2(c.x - k * 0.42f, c.y + k * 0.04f),
                              ImVec2(c.x - k * 0.42f, c.y + k * 0.88f), col);
        break;
    case 3:
        dl->AddRect(ImVec2(c.x - k, c.y - k * 0.70f),
                    ImVec2(c.x + k, c.y + k * 0.60f), col, 2.3f, 0, stroke);
        dl->AddTriangleFilled(ImVec2(c.x - k * 0.18f, c.y + k * 0.60f),
                              ImVec2(c.x + k * 0.24f, c.y + k * 0.60f),
                              ImVec2(c.x - k * 0.03f, c.y + k), col);
        for (int i = -1; i <= 1; ++i)
            dl->AddCircleFilled(ImVec2(c.x + i * k * 0.45f, c.y - k * 0.05f),
                                std::max(0.8f, r * 0.11f), col, 8);
        break;
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

    const float barY = fullScreen ? wp.y + ws.y - 103.f
        : (compactMusic ? wp.y + ws.y - 63.f
                        : (artworkView ? wp.y + ws.y - 94.f
                                       : wp.y + ws.y - 59.f));
    const float barThickness = fullScreen ? 2.5f : 4.f;
    const float leftInset = compactMusic ? 16.f : (artworkView ? 7.f : 13.f);
    const float rightInset = compactMusic ? 16.f : (artworkView ? 13.f : 8.f);
    ImVec2 barMin(fullScreen ? ctx.fullColumnX : wp.x + leftInset, barY),
           barMax(fullScreen ? ctx.fullColumnX + ctx.fullColumnWidth : wp.x + ws.x - rightInset,
                  barY + barThickness);
    static double timelineDragSeekSec = -1.0;
    double displayPosition = ctx.position;
    float displayProgress = ctx.progress;
    ImGui::PushID("music_timeline");
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barY - 6.f));
    ImGui::InvisibleButton("##seek", ImVec2(barMax.x - barMin.x, 14.f));
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
                            fullScreen ? 1.9f : (compactMusic ? 2.15f : 2.8f),
                            IM_COL32(255, 255, 255, 240), 18);
    }

    auto fmt = [](double s, char* b, size_t n) {
        int t = (int)s; std::snprintf(b, n, "%d:%02d", t / 60, t % 60);
    };
    char pb[16], rb[20], remaining[16];
    fmt(displayPosition, pb, sizeof(pb));
    fmt(std::max(0.0, ctx.duration - displayPosition), remaining, sizeof(remaining));
    std::snprintf(rb, sizeof(rb), "-%s", remaining);
    const float timeSize = compactMusic ? 9.5f : (fullScreen ? 9.f : 10.f);
    music_host::DrawText(dl, ctx.regular, timeSize, ImVec2(barMin.x, barY + 6),
        IM_COL32(255, 255, 255, 136), pb);
    music_host::DrawText(dl, ctx.regular, timeSize,
        ImVec2(barMax.x - music_host::Measure(ctx.regular, timeSize, rb).x, barY + 6),
        IM_COL32(255, 255, 255, 136), rb);
    const char* status = "Lossless";
    const float statusTextSize = compactMusic ? 10.f : (fullScreen ? 9.f : 10.5f);
    ImVec2 statusSize = music_host::Measure(ctx.bold, statusTextSize, status);
    music_host::DrawText(dl, ctx.bold, statusTextSize,
        ImVec2((barMin.x + barMax.x - statusSize.x) * 0.5f, barY + 6.f),
        IM_COL32(255, 255, 255, 172), status);

    const float cy = fullScreen ? barY + 34.f
        : (compactMusic ? wp.y + ws.y - 23.f
                        : (artworkView ? wp.y + ws.y - 35.f
                                       : wp.y + ws.y - 20.f));
    float cx = fullScreen ? (barMin.x + barMax.x) * 0.5f
        : wp.x + ws.x * (compactMusic ? 0.48f : 0.5f);
    const float controlSpacing = compactMusic ? 37.f
        : (fullScreen ? 46.f : (artworkView ? 69.f : 35.f));
    const float primaryRadius = compactMusic ? 12.f : (fullScreen ? 16.f : 15.f);
    const float secondaryRadius = compactMusic ? 9.5f : (fullScreen ? 13.f : 13.f);
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
            DrawMediaGlyph(dl, ImVec2(bx, cy), 7.3f * bnc, kind,
                           IM_COL32(255, 255, 255, (int)(225 + 30 * hv)));
        } else {
            if (hv > 0.01f)
                dl->AddCircleFilled(ImVec2(bx, cy), secondaryRadius,
                                    IM_COL32(255, 255, 255, (int)(22 * hv)), 20);
            DrawMediaGlyph(dl, ImVec2(bx, cy), (compactMusic ? 4.2f : 5.5f) * bnc, kind,
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
        ImGui::SetCursorScreenPos(ImVec2(center.x - 10.f, center.y - 10.f));
        ImGui::InvisibleButton("##utility", ImVec2(20.f, 20.f));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const float press = music_host::animation::ClickBounce(
            ImGui::GetID("##utility_press"), clicked);
        if ((hovered || selected) && !(fullScreen && selected)) {
            dl->AddCircleFilled(center, 10.f * press,
                                IM_COL32(255, 255, 255, selected ? 30 : 16), 16);
        }
        DrawUtilityGlyph(dl, center,
                         (compactMusic ? 5.5f : (fullScreen ? 5.7f : 6.4f)) * press,
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
    if (artworkView) {
        shuffleClicked = utilityButton(
            "shuffle_toggle", ImVec2(wp.x + 20.f, cy), 1, ctx.shuffleActive);
        repeatClicked = utilityButton(
            "repeat_toggle", ImVec2(wp.x + ws.x - 26.f, cy),
            2, ctx.repeatActive);
    } else {
        fullScreenClicked = !fullScreen && utilityButton(
            "fullscreen_toggle", ImVec2(wp.x + 19.f, cy), 0, false);
        shuffleClicked = utilityButton(
            "shuffle_toggle", ImVec2(fullScreen ? barMin.x + 3.f
                                                 : wp.x + (compactMusic ? 80.f : 75.f), cy),
            1, ctx.shuffleActive);
        repeatClicked = utilityButton(
            "repeat_toggle", ImVec2(fullScreen ? barMax.x - 7.f
                                                : wp.x + ws.x - (compactMusic ? 80.f : 70.f), cy),
            2, ctx.repeatActive);
        lyricsClicked = utilityButton(
            "lyrics_toggle", ImVec2(fullScreen ? wp.x + 14.f
                                                 : wp.x + ws.x - (compactMusic ? 20.f : 18.f),
                                      fullScreen ? wp.y + ws.y - 20.f : cy), 3,
            showLyrics);
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

#include "music_player_host.h"
#include "music_player_internal.h"

#include <algorithm>
#include <string>

namespace native_music_player::detail {

namespace {

std::string Ellipsize(const std::string& s, ImFont* font, float size, float width) {
    if (music_host::Measure(font, size, s.c_str()).x <= width) return s;
    std::string out = s;
    while (out.size() > 1 &&
           music_host::Measure(font, size, (out + "...").c_str()).x > width)
        out.pop_back();
    return out + "...";
}

void DrawNotPlayingIcon(ImDrawList* dl, ImVec2 c, ImU32 color, float scale) {
    dl->AddCircleFilled(ImVec2(c.x - 5 * scale, c.y + 6 * scale),
                        3.f * scale, color, 16);
    dl->AddCircleFilled(ImVec2(c.x + 5 * scale, c.y + 4 * scale),
                        3.f * scale, color, 16);
    dl->AddLine(ImVec2(c.x - 2.f * scale, c.y + 5.4f * scale),
                ImVec2(c.x - 2.f * scale, c.y - 7 * scale), color, 1.6f);
    dl->AddLine(ImVec2(c.x + 8.f * scale, c.y + 3.4f * scale),
                ImVec2(c.x + 8.f * scale, c.y - 9 * scale), color, 1.6f);
    dl->AddLine(ImVec2(c.x - 2.6f * scale, c.y - 7 * scale),
                ImVec2(c.x + 8.6f * scale, c.y - 9.5f * scale), color, 2.4f);
}

void DrawFullscreenHeader(const HeaderContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;
    const ImVec2 artMin(ctx.fullArtX, wp.y + ws.y * 0.19f);
    const ImVec2 artMax(artMin.x + ctx.fullArtSize, artMin.y + ctx.fullArtSize);
    if (ctx.haveArt) {
        dl->AddRectFilled(ImVec2(artMin.x + 3.f, artMin.y + 6.f),
                          ImVec2(artMax.x + 3.f, artMax.y + 6.f),
                          IM_COL32(0, 0, 0, 118), 5.f);
        dl->AddImageRounded(AlbumArtTexture(), artMin, artMax,
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.f);
        dl->AddRect(artMin, artMax, IM_COL32(0, 0, 0, 190), 5.f, 0, 1.4f);
    } else {
        dl->AddRectFilled(artMin, artMax, IM_COL32(255, 255, 255, 16), 8.f);
    }

    const std::string title = Ellipsize(ctx.title ? ctx.title : "",
                                         ctx.bold, 14.f, ctx.fullColumnWidth);
    const std::string artist = Ellipsize(ctx.artist ? ctx.artist : "",
                                          ctx.regular, 10.5f, ctx.fullColumnWidth);
    const float infoY = artMax.y + 37.f;
    music_host::DrawText(dl, ctx.bold, 14.f, ImVec2(ctx.fullColumnX, infoY),
        IM_COL32(255, 255, 255, 248), title.c_str());
    if (!artist.empty())
        music_host::DrawText(dl, ctx.regular, 10.5f,
            ImVec2(ctx.fullColumnX, infoY + 18.f),
            IM_COL32(255, 255, 255, 184), artist.c_str());

    const ImVec2 closeMin(wp.x + ws.x - 32.f, wp.y + 14.f);
    const ImVec2 restoreMin(closeMin.x - 28.f, closeMin.y);
    ImGui::SetCursorScreenPos(restoreMin);
    ImGui::InvisibleButton("##music_full_restore", ImVec2(24.f, 24.f));
    const bool restoreHover = ImGui::IsItemHovered();
    const bool restoreClicked = ImGui::IsItemClicked();
    if (restoreHover)
        dl->AddCircleFilled(ImVec2(restoreMin.x + 12.f, restoreMin.y + 12.f),
                            12.f, IM_COL32(255, 255, 255, 22), 16);
    const ImU32 restoreColor = IM_COL32(255, 255, 255, restoreHover ? 230 : 172);
    dl->AddRect(ImVec2(restoreMin.x + 8.f, restoreMin.y + 8.f),
                ImVec2(restoreMin.x + 16.f, restoreMin.y + 16.f),
                restoreColor, 1.4f, 0, 1.35f);

    ImGui::SetCursorScreenPos(closeMin);
    ImGui::InvisibleButton("##music_full_close", ImVec2(24.f, 24.f));
    const bool closeHover = ImGui::IsItemHovered();
    const bool closeClicked = ImGui::IsItemClicked();
    if (closeHover)
        dl->AddCircleFilled(ImVec2(closeMin.x + 12.f, closeMin.y + 12.f), 12.f,
                            IM_COL32(255, 255, 255, 22), 16);
    const ImU32 closeColor = IM_COL32(255, 255, 255, closeHover ? 240 : 174);
    dl->AddLine(ImVec2(closeMin.x + 8.f, closeMin.y + 8.f),
                ImVec2(closeMin.x + 16.f, closeMin.y + 16.f), closeColor, 1.5f);
    dl->AddLine(ImVec2(closeMin.x + 16.f, closeMin.y + 8.f),
                ImVec2(closeMin.x + 8.f, closeMin.y + 16.f), closeColor, 1.5f);
    if (closeClicked || restoreClicked) {
        if (ctx.fullScreenOut) *ctx.fullScreenOut = false;
        if (ctx.showLyrics) *ctx.showLyrics = true;
        if (ctx.artworkView) *ctx.artworkView = false;
        if (ctx.restoreNormalPositionOut)
            *ctx.restoreNormalPositionOut = true;
    }
}

void DrawArtworkBackButton(const HeaderContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;
    const float S = ctx.uiScale;
    const float bw = Px(30.f), bh = Px(28.f);
    const ImVec2 backMin(wp.x + Px(5.f), wp.y + Px(13.f));
    ImGui::SetCursorScreenPos(backMin);
    ImGui::InvisibleButton("##art_back", ImVec2(bw, bh));
    const bool backHovered = ImGui::IsItemHovered();
    const bool backClicked = ImGui::IsItemClicked();
    if (backHovered) {
        dl->AddRectFilled(backMin, ImVec2(backMin.x + bw, backMin.y + bh),
                          IM_COL32(8, 10, 14, 158), Px(9.f));
        dl->AddRect(backMin, ImVec2(backMin.x + bw, backMin.y + bh),
                    IM_COL32(255, 255, 255, 86), Px(9.f), 0, 1.f);
    }
    const ImU32 arrow = IM_COL32(255, 255, 255, backHovered ? 245 : 92);
    dl->AddLine(ImVec2(backMin.x + Px(18.f), backMin.y + Px(8.f)),
                ImVec2(backMin.x + Px(12.f), backMin.y + Px(14.f)), arrow, 1.7f * S);
    dl->AddLine(ImVec2(backMin.x + Px(12.f), backMin.y + Px(14.f)),
                ImVec2(backMin.x + Px(18.f), backMin.y + Px(20.f)), arrow, 1.7f * S);

    const ImVec2 aaCenter(wp.x + ws.x - Px(27.f), wp.y + Px(31.f));
    ImGui::SetCursorScreenPos(ImVec2(aaCenter.x - bw * 0.5f, aaCenter.y - bh * 0.5f));
    ImGui::InvisibleButton("##art_lyrics", ImVec2(bw, bh));
    const bool aaHovered = ImGui::IsItemHovered();
    const bool aaClicked = ImGui::IsItemClicked();
    const bool lyricsVisible = ctx.showLyrics && *ctx.showLyrics;
    const ImVec2 aaSize = music_host::Measure(ctx.bold, 15.f * S, "Aa");
    music_host::DrawText(dl, ctx.bold, 15.f * S,
        ImVec2(aaCenter.x - aaSize.x * 0.5f,
               aaCenter.y - aaSize.y * 0.5f),
        IM_COL32(255, 255, 255,
            aaHovered ? 238 : (lyricsVisible ? 186 : 112)), "Aa");
    if (backClicked && ctx.artworkView) *ctx.artworkView = false;
    if (aaClicked && ctx.showLyrics) *ctx.showLyrics = !*ctx.showLyrics;
}

void DrawCompactHeader(const HeaderContext& ctx) {
    ImDrawList* dl = ctx.drawList;
    const ImVec2 wp = ctx.windowPosition;
    const ImVec2 ws = ctx.windowSize;
    const bool compact = ctx.compact;
    const bool showLyrics = ctx.showLyrics && *ctx.showLyrics;
    const float S = ctx.uiScale;
    const float artSz = Px(compact ? 48.f : (showLyrics ? 44.f : 48.f));
    const ImVec2 artMin(wp.x + Px(10.f), wp.y + Px(12.f));
    const ImVec2 artMax(artMin.x + artSz, artMin.y + artSz);
    bool artHovered = false;
    bool artClicked = false;
    float artPress = 1.f;
    if (ctx.haveArt) {
        ImGui::PushID("album_art_view");
        ImGui::SetCursorScreenPos(artMin);
        ImGui::InvisibleButton("##open", ImVec2(artSz, artSz));
        artHovered = ImGui::IsItemHovered();
        artClicked = ImGui::IsItemClicked();
        artPress = music_host::animation::ClickBounce(
            ImGui::GetID("##press"), artClicked);
        ImGui::PopID();
    }
    const float inset = (1.f - std::clamp(artPress, 0.90f, 1.04f)) * 8.f * S;
    const ImVec2 artDrawMin(artMin.x + inset, artMin.y + inset);
    const ImVec2 artDrawMax(artMax.x - inset, artMax.y - inset);
    if (ctx.haveArt) {
        dl->AddRectFilled(ImVec2(artDrawMin.x + Px(2.f), artDrawMin.y + Px(4.f)),
                          ImVec2(artDrawMax.x + Px(2.f), artDrawMax.y + Px(4.f)),
                          IM_COL32(0, 0, 0, 88), Px(8.f));
        dl->AddImageRounded(AlbumArtTexture(),
                            artDrawMin, artDrawMax, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, Px(8.f));
        dl->AddRect(artDrawMin, artDrawMax,
                    IM_COL32(255, 255, 255, artHovered ? 78 : 34),
                    Px(8.f), 0, 1.f);
        if (artHovered) {
            dl->AddRectFilled(artDrawMin, artDrawMax,
                              IM_COL32(0, 0, 0, 58), Px(9.f));
            const ImVec2 c((artDrawMin.x + artDrawMax.x) * 0.5f,
                           (artDrawMin.y + artDrawMax.y) * 0.5f);
            const ImU32 icon = IM_COL32(255, 255, 255, 225);
            const float e = Px(7.f), i3 = Px(3.f);
            dl->AddLine(ImVec2(c.x - e, c.y - i3), ImVec2(c.x - e, c.y - e), icon, 1.4f * S);
            dl->AddLine(ImVec2(c.x - e, c.y - e), ImVec2(c.x - i3, c.y - e), icon, 1.4f * S);
            dl->AddLine(ImVec2(c.x + e, c.y + i3), ImVec2(c.x + e, c.y + e), icon, 1.4f * S);
            dl->AddLine(ImVec2(c.x + e, c.y + e), ImVec2(c.x + i3, c.y + e), icon, 1.4f * S);
        }
        if (artClicked && ctx.artworkView) *ctx.artworkView = true;
    } else {
        dl->AddRectFilled(artMin, artMax, IM_COL32(255, 255, 255, 18), Px(9.f));
        ImVec2 c(artMin.x + artSz * 0.5f, artMin.y + artSz * 0.5f);
        DrawNotPlayingIcon(dl, c, IM_COL32(255, 255, 255, 115), 1.f);
    }

    const float tx = artMax.x + Px(compact ? 9.f : 6.f);
    const float availW = wp.x + ws.x - tx - Px(9.f);
    const std::string titleShown = Ellipsize(ctx.title ? ctx.title : "",
                                              ctx.bold, 12.f * S, availW);
    music_host::DrawText(dl, ctx.bold, 12.f * S,
        ImVec2(tx, wp.y + Px(compact ? 14.f : 15.f)),
        IM_COL32(255, 255, 255, 245), titleShown.c_str());
    if (ctx.artist && ctx.artist[0]) {
        const std::string artistShown = Ellipsize(ctx.artist,
                                                    ctx.regular, 10.f * S, availW);
        music_host::DrawText(dl, ctx.regular, 10.f * S,
            ImVec2(tx, wp.y + Px(compact ? 29.f : 32.f)),
            IM_COL32(255, 255, 255, 174), artistShown.c_str());
    }
}

}  // namespace

void DrawHeader(const HeaderContext& ctx) {
    if (ctx.fullScreen) {
        DrawFullscreenHeader(ctx);
        return;
    }
    if (ctx.artworkView && *ctx.artworkView) {
        DrawArtworkBackButton(ctx);
        return;
    }
    DrawCompactHeader(ctx);
}

void DrawNotPlayingMessage(ImDrawList* dl, ImFont* regular, ImFont* bold,
                          ImVec2 wp, ImVec2 ws) {
    ImVec2 c(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.42f);
    DrawNotPlayingIcon(dl, c, IM_COL32(255, 255, 255, 120), 1.2f);
    const char* line1 = "Not playing";
    const char* line2 = "Start Spotify or another media app";
    ImVec2 s1 = music_host::Measure(bold, 13.f, line1);
    ImVec2 s2 = music_host::Measure(regular, 11.5f, line2);
    music_host::DrawText(dl, bold, 13.f,
        ImVec2(wp.x + (ws.x - s1.x) * 0.5f, c.y + 17.f),
        IM_COL32(255, 255, 255, 210), line1);
    music_host::DrawText(dl, regular, 11.5f,
        ImVec2(wp.x + (ws.x - s2.x) * 0.5f, c.y + 35.f),
        IM_COL32(255, 255, 255, 105), line2);
}

}  // namespace native_music_player::detail

#include "media.h"
#include "music_player_host.h"
#include "music_player_ui.h"
#include "music_player_internal.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace native_music_player {

PlayerOptions g_playerOptions;

namespace {

ImVec2 g_cardMin{0, 0};
ImVec2 g_cardMax{0, 0};

struct WindowState {
    bool initialized = false;
    bool wasFullScreen = false;
    bool wasWindowFull = false;   // OS window was borderless-fullscreen last frame
    bool restoreNormalPosition = false;
    int  previousMode = -1;
    ImVec2 lastSize{304.f, 141.f};
    ImVec2 animatedSize{304.f, 141.f};
    bool modeTransition = false;
    ImVec2 fullScreenPos{-1.f, -1.f};
    bool fullScreenPosValid = false;
    // How big the user dragged the card, as a multiple of the mode-s base size.
    // Without this the card had no memory of its own size: DesiredSize() always
    // returns the BASE size for the mode, so every frame that re-applied a size
    // -- a mode change, the tail of a transition -- threw the drag away and the
    // card snapped back. One scalar is enough because resizing is uniform, and
    // keeping it here also carries the size across mode switches.
    float userScale = 1.f;
};

constexpr float kMinUserScale = 0.78f;
constexpr float kMaxUserScaleCompact = 1.70f;
constexpr float kMaxUserScaleExpanded = 1.55f;

float g_uiScale = 1.0f;

}  // namespace

namespace detail {
float UiScale() { return g_uiScale; }
void SetUiScale(float scale) { g_uiScale = std::clamp(scale, 0.6f, 2.4f); }
float Px(float v) {
    const float scaled = v * g_uiScale;
    // Keep hairlines visible: rounding a 1px rule at 0.8 scale to 0 erases it.
    return scaled < 1.f && v > 0.f ? 1.f : std::round(scaled);
}
}  // namespace detail

namespace {

WindowState& State() {
    static WindowState s;
    return s;
}

// The window is not freely resizable: every mode has one natural shape, and a
// drag only chooses HOW BIG that shape is -- one scalar, like a size slider.
// Free resize let the card be stretched into proportions the layout was never
// designed for (squashed art, lyrics in a tall thin column), which is the main
// reason it did not read as the real player.
//
// A drag is projected onto the mode's aspect line rather than following one
// axis, so dragging any handle -- corner or edge -- scales the whole card and
// it tracks the cursor instead of fighting it. For base (w0,h0) and a dragged
// (w,h), the closest point on { s*(w0,h0) } is s = (w0*w + h0*h)/(w0^2 + h0^2).
struct AspectLock { float baseW, baseH, minScale, maxScale; };
AspectLock g_aspectLock{};

void ApplyUniformScale(ImGuiSizeCallbackData* data) {
    const AspectLock* a = (const AspectLock*)data->UserData;
    if (!a || a->baseW <= 0.f || a->baseH <= 0.f) return;
    const float denom = a->baseW * a->baseW + a->baseH * a->baseH;
    float s = (a->baseW * data->DesiredSize.x + a->baseH * data->DesiredSize.y) / denom;
    s = std::clamp(s, a->minScale, a->maxScale);
    // ROUND, do not let the result stay fractional.
    //
    // The projection returns a fractional size (e.g. 416.93 x 684.05) and ImGui
    // then FLOORS the window size to 416 x 684. Next frame that floored size
    // projects to a slightly smaller s, which floors again -- a feedback loop
    // that walked the card ~1px smaller every frame until it crept all the way
    // back to its base size. That is the resize "snapping back".
    //
    // Rounding to whole pixels here makes the mapping a fixed point: the same
    // size in gives the same size out, so once the drag ends the card holds.
    data->DesiredSize.x = std::floor(a->baseW * s + 0.5f);
    data->DesiredSize.y = std::floor(a->baseH * s + 0.5f);
}

ImVec2 DesiredSize(const ImGuiViewport* vp, bool fullScreen, bool artworkView,
                   bool expanded, ImVec2& outFull) {
    constexpr float kCompactWidth = 304.f;
    constexpr float kCompactHeight = 141.f;
    constexpr float kExpandedWidth = 295.f;
    constexpr float kExpandedHeight = 484.f;
    constexpr float kArtworkWidth = 293.f;
    constexpr float kArtworkHeight = 303.f;
    // When the host window itself is fullscreen, the card fills it edge to edge;
    // otherwise the fullscreen LAYOUT is a bounded, centred card.
    if (music_host::overlay::IsFullscreenWindow()) {
        outFull = vp->WorkSize;
    } else {
        const float fullWidth = std::min(806.f,
            std::max(360.f, vp->WorkSize.x - 36.f));
        const float fullHeight = std::min(520.f,
            std::max(400.f, fullWidth * 0.509f));
        outFull = ImVec2(fullWidth, fullHeight);
    }
    if (fullScreen) return outFull;
    if (artworkView) return ImVec2(kArtworkWidth, kArtworkHeight);
    if (expanded) return ImVec2(kExpandedWidth, kExpandedHeight);
    return ImVec2(kCompactWidth, kCompactHeight);
}

}  // namespace

bool CursorOverMusicCard() {
    if (g_cardMax.x <= g_cardMin.x) return false;
    POINT cur;
    if (!GetCursorPos(&cur)) return false;
    HWND self = music_host::overlay::GetOverlayWindow();
    if (self) ScreenToClient(self, &cur);
    return cur.x >= (LONG)g_cardMin.x && cur.x <= (LONG)g_cardMax.x
        && cur.y >= (LONG)g_cardMin.y && cur.y <= (LONG)g_cardMax.y;
}

void ShutdownMusicPlayer() {
    detail::ReleaseVisualAssets();
}

void DrawMusicPlayer() {
    if (!g_playerOptions.visible) {
        g_cardMin = g_cardMax = ImVec2(0, 0);
        return;
    }
    media::Tick();

    bool& showLyrics = g_playerOptions.showLyrics;
    bool& artworkView = g_playerOptions.showArtwork;
    bool& fullScreen = g_playerOptions.fullScreen;
    const bool artworkMode = artworkView && !fullScreen;
    const bool expandedMode = showLyrics || artworkView || fullScreen;
    const bool compactMode = !expandedMode;

    WindowState& st = State();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 fullSize;
    const ImVec2 desiredSize = DesiredSize(vp, fullScreen, artworkMode,
                                            expandedMode, fullSize);
    const int mode = fullScreen ? 2 : (artworkMode ? 3 : (expandedMode ? 1 : 0));

    float defX = std::clamp(g_playerOptions.x, 8.f,
        std::max(8.f, vp->WorkSize.x - 304.f - 8.f));
    float defY = g_playerOptions.y < 0.f
        ? std::max(14.f, vp->WorkSize.y - desiredSize.y - 14.f)
        : std::clamp(g_playerOptions.y, 8.f,
            std::max(8.f, vp->WorkSize.y - desiredSize.y - 8.f));

    const bool firstFrame = !st.initialized;
    const bool modeChanged = st.previousMode >= 0 && st.previousMode != mode;
    // A window-fullscreen card fills the monitor exactly; the user-scale slider
    // does not apply to it.
    const bool windowFull = music_host::overlay::IsFullscreenWindow();
    const float maxUserScale = compactMode ? kMaxUserScaleCompact
                                           : kMaxUserScaleExpanded;
    st.userScale = windowFull ? 1.f
                              : std::clamp(st.userScale, kMinUserScale, maxUserScale);
    // Everything below sizes against the user-s scale, not the raw base.
    const ImVec2 scaledDesired(desiredSize.x * st.userScale,
                               desiredSize.y * st.userScale);
    if (firstFrame) {
        st.animatedSize = scaledDesired;
        st.lastSize = scaledDesired;
    } else if (modeChanged) {
        st.animatedSize = st.lastSize;
        if (mode == 2) {
            st.animatedSize.x = std::max(st.animatedSize.x, 720.f);
            st.animatedSize.y = std::max(st.animatedSize.y, 400.f);
        }
        st.modeTransition = true;
    }
    if (st.modeTransition) {
        const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
        const float blend = 1.f - std::exp(-13.f * dt);
        st.animatedSize.x += (scaledDesired.x - st.animatedSize.x) * blend;
        st.animatedSize.y += (scaledDesired.y - st.animatedSize.y) * blend;
        if (std::abs(st.animatedSize.x - scaledDesired.x) < 0.35f &&
            std::abs(st.animatedSize.y - scaledDesired.y) < 0.35f) {
            st.animatedSize = scaledDesired;
            st.modeTransition = false;
        }
    }
    const ImVec2 requestedSize = st.modeTransition ? st.animatedSize : scaledDesired;
    if (firstFrame || modeChanged || st.modeTransition || fullScreen)
        ImGui::SetNextWindowSize(requestedSize, ImGuiCond_Always);
    // No resize handle while the window fills the monitor: the card is locked to
    // the window, so the aspect-lock constraint would just fight the OS size.
    if (!fullScreen && !st.modeTransition && !windowFull) {
        // Same rule in every mode. Artwork and lyrics modes used to be free-form
        // between a min and a max, so they could be dragged into shapes the
        // layout does not support; now they scale exactly like the compact card.
        g_aspectLock.baseW = desiredSize.x;
        g_aspectLock.baseH = desiredSize.y;
        g_aspectLock.minScale = kMinUserScale;
        g_aspectLock.maxScale = maxUserScale;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(desiredSize.x * g_aspectLock.minScale,
                   desiredSize.y * g_aspectLock.minScale),
            ImVec2(desiredSize.x * g_aspectLock.maxScale,
                   desiredSize.y * g_aspectLock.maxScale),
            ApplyUniformScale, &g_aspectLock);
    }
    if (fullScreen) {
        if (windowFull) {
            // Card pinned to the window's top-left, filling it every frame; no
            // remembered position, no drag.
            ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        } else if (firstFrame || modeChanged || st.wasWindowFull) {
            const ImVec2 centered(
                vp->WorkPos.x + (vp->WorkSize.x - requestedSize.x) * 0.5f,
                vp->WorkPos.y + (vp->WorkSize.y - requestedSize.y) * 0.5f);
            ImGui::SetNextWindowPos(
                st.fullScreenPosValid ? st.fullScreenPos : centered,
                ImGuiCond_Always);
        }
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + defX, vp->WorkPos.y + defY),
            (firstFrame || st.wasFullScreen || st.restoreNormalPosition)
                ? ImGuiCond_Always : ImGuiCond_Once);
    }
    st.restoreNormalPosition = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove;
    if (fullScreen || st.modeTransition || windowFull)
        flags |= ImGuiWindowFlags_NoResize;
    // Resize ONLY from the bottom-right grip -- not the edges or the other
    // corners. ImGui resizes from edges when ConfigWindowsResizeFromEdges is on
    // (the default), which let the card be dragged from any side and briefly
    // stretched off its aspect line before the constraint re-projected it -- the
    // distortion that also shoved the lyrics into each other mid-drag. The flag
    // is read inside Begin (that is where the manual resize runs), so it is
    // scoped to this one window and restored immediately, leaving any other
    // window in the host untouched.
    ImGuiIO& io = ImGui::GetIO();
    const bool prevResizeEdges = io.ConfigWindowsResizeFromEdges;
    io.ConfigWindowsResizeFromEdges = false;
    // Hide ImGui's own resize grip. It renders a filled blue wedge in the
    // bottom-right corner (the dark theme's 0.26/0.59/0.98 accent), which is not
    // a Matcha element and sat on top of the two thin corner strokes we draw
    // ourselves further down. Zeroing the colour keeps the grip's hit box and
    // drag behaviour -- only the wedge stops being painted. Pushed around Begin
    // because that is where the grip is both colour-resolved and rendered.
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::Begin("##spotify_player", nullptr, flags);
    ImGui::PopStyleColor(3);
    io.ConfigWindowsResizeFromEdges = prevResizeEdges;
    st.initialized = true;

    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    // Resize is uniform, so width alone defines the scale for the whole card.
    const float liveScale = desiredSize.x > 1.f ? ws.x / desiredSize.x : 1.f;
    detail::SetUiScale(liveScale);
    // Adopt whatever the user dragged to, so it survives the next frame and the
    // next mode switch. Skipped mid-transition, where the size is ours and not
    // theirs.
    if (!st.modeTransition && !firstFrame && !modeChanged)
        st.userScale = std::clamp(liveScale, kMinUserScale, maxUserScale);
    const bool resizing = std::abs(ws.x - st.lastSize.x) > 0.5f ||
                          std::abs(ws.y - st.lastSize.y) > 0.5f;
    st.lastSize = ws;
    st.previousMode = mode;
    st.wasWindowFull = windowFull;

    // Only keep the card on screen when it is NOT being resized. This clamp ran
    // every frame, so growing the card near a screen edge pushed its position
    // back to keep the far edge inside the viewport -- the card appeared to walk
    // away from the cursor while dragging the grip, and shrinking never undid
    // it. While resizing, the top-left stays exactly where the user put it.
    if (!resizing) {
        ImVec2 clampedPos(
            std::clamp(wp.x, vp->WorkPos.x + 8.f,
                std::max(vp->WorkPos.x + 8.f,
                         vp->WorkPos.x + vp->WorkSize.x - ws.x - 8.f)),
            std::clamp(wp.y, vp->WorkPos.y + 8.f,
                std::max(vp->WorkPos.y + 8.f,
                         vp->WorkPos.y + vp->WorkSize.y - ws.y - 8.f)));
        if (std::abs(clampedPos.x - wp.x) > 0.5f ||
            std::abs(clampedPos.y - wp.y) > 0.5f) {
            ImGui::SetWindowPos(clampedPos);
            wp = clampedPos;
        }
    }

    const float dragLeft = detail::Px(fullScreen ? 12.f
        : (artworkView ? 48.f : (compactMode ? 76.f : 74.f)));
    const float dragRightReserve = detail::Px(fullScreen ? 76.f : 0.f);
    const float dragHeight = detail::Px(fullScreen ? 46.f
        : (artworkView ? 38.f : (compactMode ? 68.f : 76.f)));
    ImGui::SetCursorScreenPos(ImVec2(wp.x + dragLeft, wp.y));
    ImGui::InvisibleButton("##musicdrag",
        ImVec2(std::max(20.f, ws.x - dragLeft - dragRightReserve), dragHeight));
    if (ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImVec2 target(wp.x + delta.x, wp.y + delta.y);
        target.x = std::clamp(target.x, vp->WorkPos.x + 8.f,
            std::max(vp->WorkPos.x + 8.f,
                     vp->WorkPos.x + vp->WorkSize.x - ws.x - 8.f));
        target.y = std::clamp(target.y, vp->WorkPos.y + 8.f,
            std::max(vp->WorkPos.y + 8.f,
                     vp->WorkPos.y + vp->WorkSize.y - ws.y - 8.f));
        ImGui::SetWindowPos(target);
        wp = target;
    }
    if (fullScreen) {
        st.fullScreenPos = wp;
        st.fullScreenPosValid = true;
    } else {
        g_playerOptions.x = wp.x - vp->WorkPos.x;
        g_playerOptions.y = wp.y - vp->WorkPos.y;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* regular = music_host::overlay::GetMusicRegularFont();
    ImFont* bold = music_host::overlay::GetMusicBoldFont();

    char title[160] = {}, artist[160] = {}, album[160] = {};
    bool playing = false;
    bool shuffleActive = false, repeatActive = false;
    bool lyricsLoading = false, lyricsSynced = false, instrumental = false;
    double posSec = 0.0, durSec = 0.0, playbackRate = 1.0;
    uint64_t snapTick = 0;
    if (media::TryAcquireSnapshot()) {
        const media::NowPlaying& np = media::Current();
        std::strncpy(title, np.title, sizeof(title) - 1);
        std::strncpy(artist, np.artist, sizeof(artist) - 1);
        std::strncpy(album, np.album, sizeof(album) - 1);
        playing = np.playing;
        shuffleActive = np.shuffleActive;
        repeatActive = np.repeatActive;
        posSec = np.positionSec;
        durSec = np.durationSec;
        playbackRate = np.playbackRate;
        snapTick = np.snapshotTickMs;
        lyricsLoading = np.lyricsLoading;
        lyricsSynced = np.lyricsSynced;
        instrumental = np.instrumental;
        detail::UpdateLyrics(np);
        detail::UpdateAlbumArt(np);
        media::ReleaseSnapshot();
    }
    detail::EnsureVisualPalette();
    const bool haveArt = detail::HasAlbumArt();
    const bool hasTrack = (title[0] != 0);

    const bool hovered = ImGui::IsMouseHoveringRect(
        wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
    const float hover = music_host::animation::Anim(
        ImGui::GetID("##music_card_hover"), hovered, 11.f);
    music_host::DrawShadow(ImGui::GetBackgroundDrawList(), wp,
               ImVec2(wp.x + ws.x, wp.y + ws.y),
               16.f + hover * 8.f, 10, 11.f, 0.40f + hover * 0.14f);
    detail::DrawPlayerBackground(dl, wp, ws, playing, showLyrics, artworkView,
                                  fullScreen, haveArt, hover, g_uiScale);

    if (!hasTrack) {
        detail::DrawNotPlayingMessage(dl, regular, bold, wp, ws);
        g_cardMin = wp;
        g_cardMax = ImVec2(wp.x + ws.x, wp.y + ws.y);
        st.wasFullScreen = fullScreen;
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Fullscreen layout scales with the window instead of sitting tiny in the
    // corners. The old caps (art 158, column 195, lyrics 300) were sized for the
    // bounded ~806-wide card, so on a truly fullscreen window everything hugged
    // the edges and left a huge empty middle. The reference instead makes the
    // art large, the left column a vertically-centred block (art, title,
    // progress, controls), and the lyrics fill the right half in big type.
    // The reference's fullscreen cover is a good deal larger than a third of the
    // height -- it runs to roughly 40% and dominates the left column, where ours
    // read as a thumbnail floating in empty space.
    const float fullArtSize = std::clamp(ws.y * 0.405f, 150.f, 560.f);
    const float fullColumnWidth = std::max(195.f, fullArtSize + 24.f);
    // The reference insets its whole left column about a tenth of the width;
    // at 5.5% ours hugged the card edge with the title nearly touching it.
    const float fullColumnX = wp.x + std::max(ws.x * 0.10f, 40.f);
    const float fullArtX = fullColumnX + (fullColumnWidth - fullArtSize) * 0.5f;
    // The left block is art + info (~66) + progress (~44) + controls (~62); centre
    // it vertically, but never let it ride above a small top margin.
    const float fullBlockHeight = fullArtSize + 172.f;
    const float fullArtY = wp.y + std::max(ws.y * 0.05f,
                                           (ws.y - fullBlockHeight) * 0.5f);
    const float fullLyricsX = wp.x + ws.x * 0.52f;
    const float fullLyricsWidth = std::max(240.f,
        wp.x + ws.x - std::max(ws.x * 0.05f, 40.f) - fullLyricsX);

    detail::HeaderContext hctx{};
    hctx.uiScale = g_uiScale;
    hctx.album = album;
    hctx.drawList = dl;
    hctx.regular = regular;
    hctx.bold = bold;
    hctx.windowPosition = wp;
    hctx.windowSize = ws;
    hctx.title = title;
    hctx.artist = artist;
    hctx.haveArt = haveArt;
    hctx.compact = compactMode;
    hctx.fullScreen = fullScreen;
    hctx.fullColumnX = fullColumnX;
    hctx.fullColumnWidth = fullColumnWidth;
    hctx.fullArtX = fullArtX;
    hctx.fullArtSize = fullArtSize;
    hctx.fullArtY = fullArtY;
    hctx.artworkView = &artworkView;
    hctx.showLyrics = &showLyrics;
    hctx.fullScreenOut = &fullScreen;
    hctx.restoreNormalPositionOut = &st.restoreNormalPosition;
    detail::DrawHeader(hctx);

    const detail::PlaybackView view = detail::ResolvePlayback(
        posSec, durSec, playbackRate, snapTick, playing);

    if (artworkView && haveArt) {
        detail::DrawArtworkLyricOverlay(dl, regular, bold, wp, ws,
                                        title, artist, album,
                                        showLyrics ? view.activeLyric : -1,
                                        showLyrics && lyricsLoading);
    }

    if (showLyrics && !artworkView && ws.y > 245.f) {
        detail::LyricsPanelContext lctx{};
        lctx.uiScale = g_uiScale;
        lctx.drawList = dl;
        lctx.regular = regular;
        lctx.bold = bold;
        lctx.windowPosition = wp;
        lctx.windowSize = ws;
        lctx.fullLyricsX = fullLyricsX;
        lctx.fullLyricsWidth = fullLyricsWidth;
        lctx.fullArtY = fullArtY;
        lctx.fullArtSize = fullArtSize;
        lctx.position = view.position;
        lctx.duration = durSec;
        lctx.activeLyric = view.activeLyric;
        lctx.fullScreen = fullScreen;
        lctx.lyricsLoading = lyricsLoading;
        lctx.lyricsSynced = lyricsSynced;
        lctx.instrumental = instrumental;
        detail::DrawLyricsPanel(lctx);
    }

    detail::TransportContext tctx{};
    tctx.uiScale = g_uiScale;
    tctx.drawList = dl;
    tctx.regular = regular;
    tctx.bold = bold;
    tctx.windowPosition = wp;
    tctx.windowSize = ws;
    tctx.fullColumnX = fullColumnX;
    tctx.fullColumnWidth = fullColumnWidth;
    tctx.fullArtY = fullArtY;
    tctx.fullArtSize = fullArtSize;
    tctx.position = view.position;
    tctx.duration = durSec;
    tctx.progress = view.progress;
    tctx.playing = playing;
    tctx.compact = compactMode;
    tctx.shuffleActive = shuffleActive;
    tctx.repeatActive = repeatActive;
    tctx.fullScreen = &fullScreen;
    tctx.showLyrics = &showLyrics;
    tctx.artworkView = &artworkView;
    detail::DrawTransportControls(tctx);

    if (!fullScreen && !st.modeTransition) {
        const ImVec2 corner(wp.x + ws.x - 6.f, wp.y + ws.y - 6.f);
        const bool gripHovered = ImGui::IsMouseHoveringRect(
            ImVec2(corner.x - 18.f, corner.y - 18.f),
            ImVec2(corner.x + 4.f, corner.y + 4.f), true);
        const float gripFocus = music_host::animation::Anim(
            ImGui::GetID("##music_resize_focus"), gripHovered, 14.f);
        const ImU32 gripColor = IM_COL32(
            255, 255, 255, (int)(26.f + gripFocus * 96.f));
        // Kept tight to the corner. The longer of these two strokes reached far
        // enough inward to cross the lyrics button sitting at the same height,
        // so the grip and the icon overlapped; the reference corner is clean.
        dl->AddLine(ImVec2(corner.x - 4.f, corner.y),
                    ImVec2(corner.x, corner.y - 4.f), gripColor, 1.1f);
        dl->AddLine(ImVec2(corner.x - 8.f, corner.y),
                    ImVec2(corner.x, corner.y - 8.f), gripColor, 1.1f);
    }

    g_cardMin = wp;
    g_cardMax = ImVec2(wp.x + ws.x, wp.y + ws.y);
    st.wasFullScreen = fullScreen;
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace native_music_player

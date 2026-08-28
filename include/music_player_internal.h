#pragma once

#include "media.h"
#include "imgui.h"

#include <vector>

namespace native_music_player::detail {

// Every layout constant in the player is authored against the mode's BASE size
// (the DesiredSize for that mode). Because resizing is now a single uniform
// scale, the ratio of the live window to that base is one number -- so the whole
// card can be scaled by multiplying through it. Without this, dragging the card
// bigger grew the panel but left the icons, text, and padding at their original
// pixel sizes, which is the single most obvious way it stopped looking like the
// real player.
float UiScale();
void SetUiScale(float scale);
// Rounds to whole pixels so 1px rules and hairlines do not land on half-pixels
// and blur once scaled.
float Px(float v);

void EnsureVisualPalette();
void SetPaletteFromArt(const uint8_t* bgra, int width, int height);
void SamplePaletteRegionsBGRA(const uint8_t* bgra, int width, int height,
                              int gridSide, float* outRgb);
void ResetPaletteState();
void UpdateAlbumArt(const media::NowPlaying& nowPlaying);
bool HasAlbumArt();
ImTextureID AlbumArtTexture();
ImTextureID AlbumAtmosphereTexture();
// Alpha-ramped copy of the atmosphere, laid over the cover's lower third
// in artwork mode so the art dissolves into the card instead of ending
// on a hard edge. Null until album art has been uploaded.
ImTextureID AlbumArtworkFadeTexture();
float& AlbumAtmosphereFadeRef();
ImU32 LyricHighlightColor();
void DrawPlayerBackground(ImDrawList* drawList, const ImVec2& position,
                          const ImVec2& size, bool playing, bool showLyrics,
                          bool artworkView, bool fullScreen, bool haveArt,
                          float hoverAmount, float uiScale);
void ReleaseVisualAssets();

// Fill an SVG path (from music_player_icons.h) centred on `center`, scaled so
// the viewBox spans `size` pixels.
void FlattenSvgPath(const char* d, std::vector<std::vector<ImVec2>>& outSubpaths);
void DrawSvgIcon(ImDrawList* drawList, const char* pathData, ImVec2 center,
                 float size, float viewBox, ImU32 color);
void StrokeSvgPath(ImDrawList* drawList, const char* pathData, ImVec2 center,
                   float size, float viewBox, ImU32 color, float thickness);

void DrawMediaGlyph(ImDrawList* drawList, ImVec2 center, float radius,
                    int kind, ImU32 color);
// kind: 0 = expand, 1 = shuffle, 2 = repeat, 3 = lyrics bubble, 4 = "Aa" bubble.
// The two bubble marks letter themselves, so they need a font; every other kind
// ignores it and may be passed nullptr.
void DrawUtilityGlyph(ImDrawList* drawList, ImVec2 center, float radius,
                      int kind, ImU32 color, ImFont* labelFont = nullptr);

void UpdateLyrics(const media::NowPlaying& nowPlaying);

// The "Aa" bubble beside the lyrics bubble steps the lyric type up a size, the
// way the same mark does in Apple Music. State lives with the lyrics panel.
bool LyricsScaledUp();
void ToggleLyricsScale();

struct PlaybackView {
    double position = 0.0;
    float progress = 0.0f;
    int activeLyric = -1;
};

PlaybackView ResolvePlayback(double position, double duration,
                             double playbackRate, uint64_t snapshotTick,
                             bool playing);

void DrawArtworkLyricOverlay(ImDrawList* drawList, ImFont* regular,
                             ImFont* bold, ImVec2 windowPosition,
                             ImVec2 windowSize, const char* title,
                             const char* artist, const char* album,
                             int activeLyric, bool lyricsLoading);

struct LyricsPanelContext {
    float uiScale = 1.0f;
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImVec2 windowPosition{};
    ImVec2 windowSize{};
    float fullLyricsX = 0.0f;
    float fullLyricsWidth = 0.0f;
    // Fullscreen artwork block, so the lyric column can be aligned to the cover
    // instead of to the card's top edge.
    float fullArtY = 0.0f;
    float fullArtSize = 0.0f;
    double position = 0.0;
    double duration = 0.0;
    int activeLyric = -1;
    bool fullScreen = false;
    bool lyricsLoading = false;
    bool lyricsSynced = false;
    bool instrumental = false;
};

void DrawLyricsPanel(const LyricsPanelContext& context);

struct TransportContext {
    float uiScale = 1.0f;
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImVec2 windowPosition{};
    ImVec2 windowSize{};
    float fullColumnX = 0.0f;
    float fullColumnWidth = 0.0f;
    float fullArtY = 0.0f;
    float fullArtSize = 0.0f;
    double position = 0.0;
    double duration = 0.0;
    float progress = 0.0f;
    bool playing = false;
    bool compact = false;
    bool shuffleActive = false;
    bool repeatActive = false;
    bool* fullScreen = nullptr;
    bool* showLyrics = nullptr;
    bool* artworkView = nullptr;
};

void DrawTransportControls(const TransportContext& context);

struct HeaderContext {
    float uiScale = 1.0f;
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    // The reference writes the sub-line as "Artist - Album", not the artist
    // alone; album may be empty, in which case only the artist is shown.
    const char* album = "";
    ImVec2 windowPosition{};
    ImVec2 windowSize{};
    const char* title = "";
    const char* artist = "";
    bool haveArt = false;
    bool compact = false;
    bool fullScreen = false;
    float fullColumnX = 0.0f;
    float fullColumnWidth = 0.0f;
    float fullArtX = 0.0f;
    float fullArtY = 0.0f;
    float fullArtSize = 0.0f;
    bool* artworkView = nullptr;
    bool* showLyrics = nullptr;
    bool* fullScreenOut = nullptr;
    bool* restoreNormalPositionOut = nullptr;
};

void DrawHeader(const HeaderContext& context);
void DrawNotPlayingMessage(ImDrawList* drawList, ImFont* regular, ImFont* bold,
                          ImVec2 windowPosition, ImVec2 windowSize);

} // namespace native_music_player::detail

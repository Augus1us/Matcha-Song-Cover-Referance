#pragma once

#include "media.h"
#include "imgui.h"

namespace native_music_player::detail {

void EnsureVisualPalette();
void SetPaletteFromArt(const uint8_t* bgra, int width, int height);
void SamplePaletteRegionsBGRA(const uint8_t* bgra, int width, int height,
                              int gridSide, float* outRgb);
void ResetPaletteState();
void UpdateAlbumArt(const media::NowPlaying& nowPlaying);
bool HasAlbumArt();
ImTextureID AlbumArtTexture();
ImTextureID AlbumAtmosphereTexture();
float& AlbumAtmosphereFadeRef();
ImU32 LyricHighlightColor();
void DrawPlayerBackground(ImDrawList* drawList, const ImVec2& position,
                          const ImVec2& size, bool playing, bool showLyrics,
                          bool artworkView, bool fullScreen, bool haveArt,
                          float hoverAmount);
void ReleaseVisualAssets();

void DrawMediaGlyph(ImDrawList* drawList, ImVec2 center, float radius,
                    int kind, ImU32 color);
void DrawUtilityGlyph(ImDrawList* drawList, ImVec2 center, float radius,
                      int kind, ImU32 color);

void UpdateLyrics(const media::NowPlaying& nowPlaying);

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
                             int activeLyric);

struct LyricsPanelContext {
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImVec2 windowPosition{};
    ImVec2 windowSize{};
    float fullLyricsX = 0.0f;
    float fullLyricsWidth = 0.0f;
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
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImVec2 windowPosition{};
    ImVec2 windowSize{};
    float fullColumnX = 0.0f;
    float fullColumnWidth = 0.0f;
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
    ImDrawList* drawList = nullptr;
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
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

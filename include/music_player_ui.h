#pragma once

namespace native_music_player {

struct PlayerOptions {
    bool visible = true;
    bool showLyrics = true;
    bool showArtwork = false;
    bool fullScreen = false;
    float x = 16.0f;
    float y = -1.0f;
};

extern PlayerOptions g_playerOptions;

bool CursorOverMusicCard();
void DrawMusicPlayer();
void ShutdownMusicPlayer();

} // namespace native_music_player

#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Windows media-session data used by the player UI. The worker reads GSMTC
// through C++/WinRT and decodes artwork to BGRA8 for the D3D render thread.

namespace media {

struct LyricLine {
    double      timeSec = -1.0; // negative for plain, unsynchronized lyrics
    std::string text;
};

struct NowPlaying {
    bool         playing = false;
    bool         shuffleActive = false;
    bool         repeatActive = false;
    // The source that produced this snapshot. The UI uses this to report a
    // real Spotify connection instead of guessing from the current title.
    bool         sourceConnected = false;
    bool         sourceIsSpotify = false;
    char         sourceApp[96] = {};
    char         title[160]  = {};
    char         artist[160] = {};
    char         album[160]  = {};

    // Album art — owned, BGRA8, dirty=true when new bytes arrived since last
    // texture upload. The HUD layer is expected to clear `dirty` after upload.
    int          artW = 0;
    int          artH = 0;
    uint8_t*     artBgra = nullptr;   // owned by the media thread; size = artW*artH*4
    bool         artDirty = false;

    // Playback progress in seconds. 0/0 = unknown.
    double       positionSec = 0.0;
    double       durationSec = 0.0;
    double       playbackRate = 1.0;
    uint64_t     snapshotTickMs = 0;

    // Lyrics are fetched separately from online providers. `lyricsRevision` changes
    // whenever the list is cleared or replaced so the render thread can keep
    // a local cache without copying the vector every frame.
    std::vector<LyricLine> lyrics;
    bool         lyricsLoading = false;
    bool         lyricsSynced = false;
    bool         instrumental = false;
    uint64_t     lyricsRevision = 0;
};

void Init();
void Shutdown();
void Tick();                      // Cheap; ensures bg thread is alive.
const NowPlaying& Current();      // Mutex-locked snapshot. Safe to read fields,
                                  // not to keep pointers across frames (artBgra
                                  // may be reallocated).

// Lock the media state for the duration of texture upload. Returns false if
// nothing is playing or no album art is available. While locked, artBgra is
// stable. ALWAYS call ReleaseSnapshot after.
bool TryAcquireSnapshot();
void ReleaseSnapshot();

// Playback controls. These post a request to the background thread which
// invokes the corresponding WinRT method on the current session next tick.
// Safe to call from the UI thread; cheap (just sets a flag).
void RequestTogglePlayPause();
void RequestSkipNext();
void RequestSkipPrevious();
void RequestToggleShuffle();
void RequestToggleRepeat();
void RequestSeek(double positionSec);
void RequestLyricsRefresh();

// Spotify is preferred by default. Turning this off permits any Windows media
// session to become the active source.
void SetSpotifyOnly(bool enabled);
bool IsSpotifyOnly();

} // namespace media

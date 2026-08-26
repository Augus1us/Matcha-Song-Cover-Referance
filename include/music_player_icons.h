#pragma once

// Icon path data, generated from assets/icons/*.svg.
//
// Bootstrap Icons (MIT) for the transport and fullscreen marks, plus a custom
// chat-quote bubble. Kept as source strings rather than a baked font so the
// shapes stay exactly what the SVG says -- including the small corner arcs that
// make a filled play triangle read as rounded instead of spiky.

namespace native_music_player::icons {

// book-fill.svg  (viewBox 16)
constexpr float kBookViewBox = 16.0f;
constexpr const char* kBookPath = "M8 1.783C7.015.936 5.587.81 4.287.94c-1.514.153-3.042.672-3.994 1.105A.5.5 0 0 0 0 2.5v11a.5.5 0 0 0 .707.455c.882-.4 2.303-.881 3.68-1.02 1.409-.142 2.59.087 3.223.877a.5.5 0 0 0 .78 0c.633-.79 1.814-1.019 3.222-.877 1.378.139 2.8.62 3.681 1.02A.5.5 0 0 0 16 13.5v-11a.5.5 0 0 0-.293-.455c-.952-.433-2.48-.952-3.994-1.105C10.413.809 8.985.936 8 1.783";

// chat-left-quote.svg  (viewBox 16)
constexpr float kLyricsAltViewBox = 16.0f;
constexpr const char* kLyricsAltPath0 = "M14 1a1 1 0 0 1 1 1v8a1 1 0 0 1-1 1H4.414A2 2 0 0 0 3 11.586l-2 2V2a1 1 0 0 1 1-1zM2 0a2 2 0 0 0-2 2v12.793a.5.5 0 0 0 .854.353l2.853-2.853A1 1 0 0 1 4.414 12H14a2 2 0 0 0 2-2V2a2 2 0 0 0-2-2z";
constexpr const char* kLyricsAltPath1 = "M7.066 4.76A1.665 1.665 0 0 0 4 5.668a1.667 1.667 0 0 0 2.561 1.406c-.131.389-.375.804-.777 1.22a.417.417 0 1 0 .6.58c1.486-1.54 1.293-3.214.682-4.112zm4 0A1.665 1.665 0 0 0 8 5.668a1.667 1.667 0 0 0 2.561 1.406c-.131.389-.375.804-.777 1.22a.417.417 0 1 0 .6.58c1.486-1.54 1.293-3.214.682-4.112z";

// chat-quote-soft.svg  (viewBox 24)
constexpr float kLyricsViewBox = 24.0f;
constexpr const char* kLyricsPath0 = "M6.4 4.75h11.2a2.65 2.65 0 0 1 2.65 2.65v6.2a2.65 2.65 0 0 1-2.65 2.65h-6.35L7.1 19.1v-2.85h-.7a2.65 2.65 0 0 1-2.65-2.65V7.4A2.65 2.65 0 0 1 6.4 4.75Z";
constexpr const char* kLyricsPath1 = "M8.05 8.2h2.25v2.08c0 1.4-.74 2.38-2.02 2.98l-.57-.88c.66-.39 1.02-.82 1.1-1.31h-.76V8.2Zm4.15 0h2.25v2.08c0 1.4-.74 2.38-2.02 2.98l-.57-.88c.66-.39 1.02-.82 1.1-1.31h-.76V8.2Z";

// fullscreen.svg  (viewBox 16)
constexpr float kFullscreenViewBox = 16.0f;
constexpr const char* kFullscreenPath = "M1.5 1a.5.5 0 0 0-.5.5v4a.5.5 0 0 1-1 0v-4A1.5 1.5 0 0 1 1.5 0h4a.5.5 0 0 1 0 1zM10 .5a.5.5 0 0 1 .5-.5h4A1.5 1.5 0 0 1 16 1.5v4a.5.5 0 0 1-1 0v-4a.5.5 0 0 0-.5-.5h-4a.5.5 0 0 1-.5-.5M.5 10a.5.5 0 0 1 .5.5v4a.5.5 0 0 0 .5.5h4a.5.5 0 0 1 0 1h-4A1.5 1.5 0 0 1 0 14.5v-4a.5.5 0 0 1 .5-.5m15 0a.5.5 0 0 1 .5.5v4a1.5 1.5 0 0 1-1.5 1.5h-4a.5.5 0 0 1 0-1h4a.5.5 0 0 0 .5-.5v-4a.5.5 0 0 1 .5-.5";

// pause-fill.svg  (viewBox 16)
constexpr float kPauseViewBox = 16.0f;
constexpr const char* kPausePath = "M5.5 3.5A1.5 1.5 0 0 1 7 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5m5 0A1.5 1.5 0 0 1 12 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5";

// play-fill.svg  (viewBox 16)
constexpr float kPlayViewBox = 16.0f;
constexpr const char* kPlayPath = "m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393";

// skip-backward-fill.svg  (viewBox 16)
constexpr float kPrevViewBox = 16.0f;
constexpr const char* kPrevPath = "M.5 3.5A.5.5 0 0 0 0 4v8a.5.5 0 0 0 1 0V8.753l6.267 3.636c.54.313 1.233-.066 1.233-.697v-2.94l6.267 3.636c.54.314 1.233-.065 1.233-.696V4.308c0-.63-.693-1.01-1.233-.696L8.5 7.248v-2.94c0-.63-.692-1.01-1.233-.696L1 7.248V4a.5.5 0 0 0-.5-.5";

// skip-forward-fill.svg  (viewBox 16)
constexpr float kNextViewBox = 16.0f;
constexpr const char* kNextPath = "M15.5 3.5a.5.5 0 0 1 .5.5v8a.5.5 0 0 1-1 0V8.753l-6.267 3.636c-.54.313-1.233-.066-1.233-.697v-2.94l-6.267 3.636C.693 12.703 0 12.324 0 11.693V4.308c0-.63.693-1.01 1.233-.696L7.5 7.248v-2.94c0-.63.693-1.01 1.233-.696L15 7.248V4a.5.5 0 0 1 .5-.5";

}  // namespace native_music_player::icons

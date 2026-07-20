#include "media.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>
#include <wincodec.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

// Don't pull `using namespace winrt::Windows::Foundation` — it aliases
// IUnknown/IInspectable which collide with the COM versions WIC uses.
namespace wmc = winrt::Windows::Media::Control;
namespace wstr = winrt::Windows::Storage::Streams;
namespace wjson = winrt::Windows::Data::Json;

// Tiny RAII wrapper for raw COM pointers (avoids WRL/ComPtr to dodge the
// IUnknown ambiguity vs winrt's IUnknown).
template<typename T>
struct ComHandle {
    T* p = nullptr;
    ~ComHandle() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    T* Get() const { return p; }
};

// Polls the Windows GSMTC session manager
// for title/artist/thumbnail/progress. Runs on a dedicated thread so WinRT's
// .get() calls don't block the render loop.

namespace media {

static NowPlaying        s_state;
static std::mutex        s_mtx;
static std::thread       s_thread;
static std::atomic<bool> s_running{false};

// Pending playback-control flags. The HUD sets these; the background thread
// consumes them on its next tick by invoking the corresponding WinRT method
// on the current GSMTC session. Atomics so we don't need a separate lock.
static std::atomic<bool> s_reqTogglePlay{false};
static std::atomic<bool> s_reqSkipNext{false};
static std::atomic<bool> s_reqSkipPrev{false};
static std::atomic<bool> s_reqToggleShuffle{false};
static std::atomic<bool> s_reqToggleRepeat{false};
static std::atomic<double> s_reqSeekSec{-1.0};
static std::atomic<bool> s_reqLyricsRefresh{false};
static std::atomic<uint64_t> s_lyricsGeneration{0};
static std::atomic<uint64_t> s_artGeneration{0};
static std::atomic<int> s_lyricsRetryCount{0};
static std::atomic<uint64_t> s_lyricsRetryAtMs{0};
static std::string s_lyricsTrackKey;
static std::atomic<bool> s_spotifyOnly{true};

// EVENT-DRIVEN update signal (was polling before). GSMTC fires
// MediaPropertiesChanged / PlaybackInfoChanged / TimelinePropertiesChanged
// the moment Spotify/YouTube/etc. update — set from those handlers so the
// worker refetches on the very next tick instead of waiting for the next
// poll. Seeded true so the first fetch after start happens immediately.
static std::atomic<bool> s_dirty{true};

void RequestTogglePlayPause() { s_reqTogglePlay.store(true); s_dirty.store(true); }
void RequestSkipNext()        { s_reqSkipNext.store(true);   s_dirty.store(true); }
void RequestSkipPrevious()    { s_reqSkipPrev.store(true);   s_dirty.store(true); }
void RequestToggleShuffle()   { s_reqToggleShuffle.store(true); s_dirty.store(true); }
void RequestToggleRepeat()    { s_reqToggleRepeat.store(true);  s_dirty.store(true); }
void RequestSeek(double positionSec) {
    if (!std::isfinite(positionSec) || positionSec < 0.0) return;
    s_reqSeekSec.store(positionSec);
    s_dirty.store(true);
}
void RequestLyricsRefresh() {
    s_lyricsRetryCount.store(0);
    s_lyricsRetryAtMs.store(0);
    s_reqLyricsRefresh.store(true);
    s_dirty.store(true);
}
void SetSpotifyOnly(bool enabled) { s_spotifyOnly.store(enabled); s_dirty.store(true); }
bool IsSpotifyOnly() { return s_spotifyOnly.load(); }

struct LyricsResult {
    std::vector<LyricLine> lines;
    bool synced = false;
    bool instrumental = false;
};

struct HttpResult {
    DWORD status = 0;
    std::string body;
};

static std::wstring UrlEncode(const std::string& input) {
    static const wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            out.push_back((wchar_t)ch);
        } else {
            out.push_back(L'%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

static HttpResult FetchHttps(const wchar_t* host, const std::wstring& path,
                             DWORD receiveTimeoutMs = 8000) {
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"NativeMusicPlayer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    WinHttpSetTimeouts(session, 3000, 3000, 3000, receiveTimeoutMs);
    HINTERNET connect = WinHttpConnect(session, host,
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    WinHttpAddRequestHeaders(request, L"Accept: */*\r\n",
                             (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD statusSize = sizeof(result.status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);

        constexpr size_t kMaxBody = 2 * 1024 * 1024;
        DWORD available = 0;
        while (result.body.size() < kMaxBody &&
               WinHttpQueryDataAvailable(request, &available) && available > 0) {
            size_t remaining = kMaxBody - result.body.size();
            DWORD amount = (DWORD)std::min<size_t>(available, remaining);
            std::vector<char> chunk(amount);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), amount, &read)) break;
            result.body.append(chunk.data(), read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

static HttpResult FetchLrclib(const std::wstring& path) {
    return FetchHttps(L"lrclib.net", path);
}

static HttpResult FetchHttpsUrl(std::string url, DWORD receiveTimeoutMs = 10000) {
    constexpr const char* kHttps = "https://";
    constexpr const char* kHttp = "http://";
    if (url.rfind(kHttp, 0) == 0) url.replace(0, std::strlen(kHttp), kHttps);
    if (url.rfind(kHttps, 0) != 0) return {};

    size_t hostStart = std::strlen(kHttps);
    size_t pathStart = url.find('/', hostStart);
    std::string host = pathStart == std::string::npos
        ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    std::string path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    if (host.empty()) return {};
    return FetchHttps(winrt::to_hstring(host).c_str(), winrt::to_hstring(path).c_str(),
                      receiveTimeoutMs);
}

static bool DecodeArtworkBgra(IWICImagingFactory* wic,
                              const std::vector<uint8_t>& encoded,
                              std::vector<uint8_t>& outBgra,
                              int& outW, int& outH) {
    if (!wic || encoded.empty()) return false;
    ComHandle<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<uint8_t*>(encoded.data()),
                                            (DWORD)encoded.size()))) return false;
    ComHandle<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                            WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    ComHandle<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    constexpr UINT kMaxArtworkSide = 512;
    UINT targetW = width, targetH = height;
    if (width > kMaxArtworkSide || height > kMaxArtworkSide) {
        if (width >= height) {
            targetW = kMaxArtworkSide;
            targetH = (UINT)((float)height * kMaxArtworkSide / width);
        } else {
            targetH = kMaxArtworkSide;
            targetW = (UINT)((float)width * kMaxArtworkSide / height);
        }
        targetW = std::max<UINT>(1, targetW);
        targetH = std::max<UINT>(1, targetH);
    }

    ComHandle<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler))) return false;
    if (FAILED(scaler->Initialize(frame.Get(), targetW, targetH,
                                  WICBitmapInterpolationModeFant))) return false;
    ComHandle<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) return false;

    outBgra.resize((size_t)targetW * targetH * 4);
    if (FAILED(converter->CopyPixels(nullptr, targetW * 4,
                                     (UINT)outBgra.size(), outBgra.data()))) return false;
    outW = (int)targetW;
    outH = (int)targetH;
    return true;
}

static std::vector<LyricLine> ParseSyncedLyrics(const std::string& lrc) {
    std::vector<LyricLine> result;
    std::istringstream input(lrc);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<double> timestamps;
        size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[') {
            size_t close = line.find(']', cursor + 1);
            if (close == std::string::npos) break;
            std::string tag = line.substr(cursor + 1, close - cursor - 1);
            size_t colon = tag.find(':');
            if (colon == std::string::npos) break;
            char* endMin = nullptr;
            char* endSec = nullptr;
            long minutes = std::strtol(tag.c_str(), &endMin, 10);
            double seconds = std::strtod(tag.c_str() + colon + 1, &endSec);
            if (endMin != tag.c_str() && endSec != tag.c_str() + colon + 1 &&
                minutes >= 0 && seconds >= 0.0) {
                timestamps.push_back((double)minutes * 60.0 + seconds);
                cursor = close + 1;
                continue;
            }
            break;
        }
        if (timestamps.empty()) continue;
        while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
        std::string text = line.substr(cursor);
        if (text.empty()) continue;
        for (double timestamp : timestamps)
            result.push_back({ timestamp, text });
    }
    std::sort(result.begin(), result.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.timeSec < b.timeSec; });
    return result;
}

static std::vector<LyricLine> ParsePlainLyrics(const std::string& plain) {
    std::vector<LyricLine> result;
    std::istringstream input(plain);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.push_back({ -1.0, line });
    }
    return result;
}

static LyricsResult ParseLyricsObject(const wjson::JsonObject& object) {
    LyricsResult result;
    result.instrumental = object.GetNamedBoolean(L"instrumental", false);
    std::string synced = winrt::to_string(
        object.GetNamedString(L"syncedLyrics", winrt::hstring{}));
    std::string plain = winrt::to_string(
        object.GetNamedString(L"plainLyrics", winrt::hstring{}));
    if (!synced.empty()) {
        result.lines = ParseSyncedLyrics(synced);
        result.synced = !result.lines.empty();
    }
    if (result.lines.empty() && !plain.empty())
        result.lines = ParsePlainLyrics(plain);
    return result;
}

static std::string NormalizeMatchText(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool pendingSpace = false;
    for (unsigned char ch : value) {
        if (ch < 0x80 && std::isalnum(ch)) {
            if (pendingSpace && !out.empty()) out.push_back(' ');
            out.push_back((char)std::tolower(ch));
            pendingSpace = false;
        } else if (ch >= 0x80) {
            if (pendingSpace && !out.empty()) out.push_back(' ');
            out.push_back((char)ch);
            pendingSpace = false;
        } else {
            pendingSpace = true;
        }
    }
    return out;
}

static bool RelatedMatchText(const std::string& expected, const std::string& candidate) {
    if (expected == candidate) return true;
    if (expected.size() < 4 || candidate.size() < 4) return false;
    if (candidate.size() > expected.size() &&
        candidate.compare(0, expected.size(), expected) == 0 &&
        candidate[expected.size()] == ' ') return true;
    return expected.size() > candidate.size() &&
           expected.compare(0, candidate.size(), candidate) == 0 &&
           expected[candidate.size()] == ' ';
}

static std::string LookupAppleArtworkUrl(const std::string& title,
                                         const std::string& artist,
                                         const std::string& album,
                                         double durationSec) {
    if (title.empty() || artist.empty()) return {};
    std::wstring path = L"/search?term=" + UrlEncode(title + " " + artist) +
                        L"&entity=song&limit=15";
    HttpResult search = FetchHttps(L"itunes.apple.com", path, 10000);
    if (search.status != 200 || search.body.empty()) return {};

    const std::string wantedTitle = NormalizeMatchText(title);
    const std::string wantedArtist = NormalizeMatchText(artist);
    const std::string wantedAlbum = NormalizeMatchText(album);
    std::string bestUrl;
    double bestScore = std::numeric_limits<double>::max();
    try {
        wjson::JsonObject root = wjson::JsonObject::Parse(winrt::to_hstring(search.body));
        wjson::JsonArray records = root.GetNamedArray(L"results");
        for (uint32_t i = 0; i < records.Size(); ++i) {
            wjson::JsonObject candidate = records.GetObjectAt(i);
            std::string candidateTitle = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"trackName", winrt::hstring{})));
            std::string candidateArtist = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"artistName", winrt::hstring{})));
            if (!RelatedMatchText(wantedTitle, candidateTitle) ||
                !RelatedMatchText(wantedArtist, candidateArtist)) continue;

            double candidateDuration = candidate.GetNamedNumber(L"trackTimeMillis", 0.0) / 1000.0;
            double durationDelta = durationSec > 1.0 && candidateDuration > 1.0
                ? std::fabs(candidateDuration - durationSec) : 0.0;
            double durationTolerance = std::max(12.0, durationSec * 0.08);
            if (durationSec > 30.0 && candidateDuration > 1.0 &&
                durationDelta > durationTolerance) continue;

            std::string candidateAlbum = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"collectionName", winrt::hstring{})));
            double score = durationDelta;
            if (candidateTitle != wantedTitle) score += 80.0;
            if (candidateArtist != wantedArtist) score += 40.0;
            if (!wantedAlbum.empty() && candidateAlbum == wantedAlbum) score -= 5.0;
            std::string url = winrt::to_string(
                candidate.GetNamedString(L"artworkUrl100", winrt::hstring{}));
            if (!url.empty() && score < bestScore) {
                bestScore = score;
                bestUrl = std::move(url);
            }
        }
    } catch (...) {
        return {};
    }

    size_t sizeTag = bestUrl.find("100x100");
    if (sizeTag != std::string::npos) bestUrl.replace(sizeTag, 7, "600x600");
    return bestUrl;
}

static void BeginArtworkFetch(const std::string& trackKey,
                              const std::string& title,
                              const std::string& artist,
                              const std::string& album,
                              double durationSec) {
    uint64_t generation = s_artGeneration.fetch_add(1) + 1;
    if (title.empty() || artist.empty()) return;

    std::thread([generation, trackKey, title, artist, album, durationSec] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct ApartmentGuard {
                ~ApartmentGuard() { winrt::uninit_apartment(); }
            } apartmentGuard;
            std::string url = LookupAppleArtworkUrl(title, artist, album, durationSec);
            HttpResult response = url.empty() ? HttpResult{} : FetchHttpsUrl(url, 12000);
            if (response.status != 200 || response.body.empty() ||
                response.body.size() > 4 * 1024 * 1024) {
                return;
            }

            ComHandle<IWICImagingFactory> wic;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&wic)))) {
                return;
            }
            std::vector<uint8_t> encoded(response.body.begin(), response.body.end());
            std::vector<uint8_t> decoded;
            int width = 0, height = 0;
            if (!DecodeArtworkBgra(wic.Get(), encoded, decoded, width, height) ||
                width < 256 || height < 256 || !s_running.load() ||
                generation != s_artGeneration.load()) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(s_mtx);
                const std::string currentKey = std::string(s_state.title) + "\n" +
                    s_state.artist + "\n" + s_state.album;
                if (currentKey == trackKey && generation == s_artGeneration.load()) {
                    delete[] s_state.artBgra;
                    s_state.artBgra = new uint8_t[decoded.size()];
                    std::memcpy(s_state.artBgra, decoded.data(), decoded.size());
                    s_state.artW = width;
                    s_state.artH = height;
                    s_state.artDirty = true;
                }
            }
        } catch (...) {}
    }).detach();
}

static LyricsResult LookupAppleLyrics(const std::string& title, const std::string& artist,
                                      const std::string& album, double durationSec) {
    LyricsResult result;
    if (title.empty() || artist.empty()) return result;

    std::wstring searchPath = L"/search?term=" + UrlEncode(title + " " + artist) +
                              L"&entity=song&limit=15";
    HttpResult search = FetchHttps(L"itunes.apple.com", searchPath, 10000);
    if (search.status != 200 || search.body.empty()) return result;

    uint64_t bestId = 0;
    double bestScore = std::numeric_limits<double>::max();
    const std::string wantedTitle = NormalizeMatchText(title);
    const std::string wantedArtist = NormalizeMatchText(artist);
    const std::string wantedAlbum = NormalizeMatchText(album);
    try {
        wjson::JsonObject root = wjson::JsonObject::Parse(winrt::to_hstring(search.body));
        wjson::JsonArray records = root.GetNamedArray(L"results");
        for (uint32_t i = 0; i < records.Size(); ++i) {
            wjson::JsonObject candidate = records.GetObjectAt(i);
            std::string candidateTitle = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"trackName", winrt::hstring{})));
            std::string candidateArtist = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"artistName", winrt::hstring{})));
            if (!RelatedMatchText(wantedTitle, candidateTitle) ||
                !RelatedMatchText(wantedArtist, candidateArtist)) continue;

            double candidateDuration = candidate.GetNamedNumber(L"trackTimeMillis", 0.0) / 1000.0;
            double durationDelta = durationSec > 1.0 && candidateDuration > 1.0
                ? std::fabs(candidateDuration - durationSec) : 0.0;
            double durationTolerance = std::max(12.0, durationSec * 0.08);
            if (durationSec > 30.0 && candidateDuration > 1.0 &&
                durationDelta > durationTolerance) continue;

            double score = durationDelta;
            if (candidateTitle != wantedTitle) score += 80.0;
            if (candidateArtist != wantedArtist) score += 40.0;
            std::string candidateAlbum = NormalizeMatchText(winrt::to_string(
                candidate.GetNamedString(L"collectionName", winrt::hstring{})));
            if (!wantedAlbum.empty() && candidateAlbum == wantedAlbum) score -= 5.0;
            if (score < bestScore) {
                double id = candidate.GetNamedNumber(L"trackId", 0.0);
                if (id > 0.0) {
                    bestId = (uint64_t)std::llround(id);
                    bestScore = score;
                }
            }
        }
    } catch (...) {
        return result;
    }
    if (bestId == 0) return result;

    std::wstring lyricsPath = L"/apple-music/lyrics?id=" + std::to_wstring(bestId) + L"&v=2";
    HttpResult lyrics = FetchHttps(L"lyrics.paxsenix.org", lyricsPath, 15000);
    if (lyrics.status != 200 || lyrics.body.empty()) return result;
    try {
        wjson::JsonObject object = wjson::JsonObject::Parse(winrt::to_hstring(lyrics.body));
        std::string lrc = winrt::to_string(
            object.GetNamedString(L"lrc", winrt::hstring{}));
        if (!lrc.empty()) {
            result.lines = ParseSyncedLyrics(lrc);
            result.synced = !result.lines.empty();
        }
        if (result.lines.empty()) {
            std::string plain = winrt::to_string(
                object.GetNamedString(L"plain", winrt::hstring{}));
            if (!plain.empty()) result.lines = ParsePlainLyrics(plain);
        }
    } catch (...) {}
    return result;
}

static LyricsResult LookupLrclibLyrics(const std::string& title, const std::string& artist,
                                       const std::string& album, double durationSec,
                                       bool deepLookup) {
    LyricsResult result;
    LyricsResult plainFallback;
    int duration = std::max(1, (int)std::lround(durationSec));
    HttpResult response;
    std::wstring path;
    if (deepLookup) {
        path = L"/api/get?track_name=" + UrlEncode(title) +
               L"&artist_name=" + UrlEncode(artist) +
               L"&album_name=" + UrlEncode(album) +
               L"&duration=" + std::to_wstring(duration);
        response = FetchLrclib(path);
        if (response.status == 200 && !response.body.empty()) {
            try {
                result = ParseLyricsObject(wjson::JsonObject::Parse(winrt::to_hstring(response.body)));
                if (result.synced || result.instrumental) return result;
                if (!result.lines.empty()) plainFallback = result;
            } catch (...) {}
        }
    }

    // GSMTC album metadata is often incomplete. Search first on normal track
    // changes and select the closest-duration synchronized result; the slower
    // exact endpoint above is reserved for an explicit Sync request.
    path = L"/api/search?track_name=" + UrlEncode(title) +
           L"&artist_name=" + UrlEncode(artist);
    response = FetchLrclib(path);
    if (response.status != 200 || response.body.empty()) return plainFallback;
    try {
        wjson::JsonArray records = wjson::JsonArray::Parse(winrt::to_hstring(response.body));
        double bestScore = std::numeric_limits<double>::max();
        wjson::JsonObject best{ nullptr };
        for (uint32_t i = 0; i < records.Size(); ++i) {
            wjson::JsonObject candidate = records.GetObjectAt(i);
            double candidateDuration = candidate.GetNamedNumber(L"duration", 0.0);
            bool hasSynced = !candidate.GetNamedString(
                L"syncedLyrics", winrt::hstring{}).empty();
            double score = std::fabs(candidateDuration - durationSec) + (hasSynced ? 0.0 : 1000.0);
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
        if (best) result = ParseLyricsObject(best);
    } catch (...) {}
    if (result.synced || result.instrumental || !result.lines.empty()) return result;
    return plainFallback;
}

static LyricsResult LookupLyrics(const std::string& title, const std::string& artist,
                                 const std::string& album, double durationSec,
                                 bool deepLookup) {
    LyricsResult lrclib = LookupLrclibLyrics(title, artist, album, durationSec, deepLookup);
    if (lrclib.synced || lrclib.instrumental) return lrclib;

    LyricsResult apple = LookupAppleLyrics(title, artist, album, durationSec);
    if (apple.synced || apple.instrumental || !apple.lines.empty()) return apple;
    return lrclib;
}

static void BeginLyricsFetch(const std::string& trackKey, const std::string& title,
                             const std::string& artist, const std::string& album,
                             double durationSec, bool deepLookup) {
    uint64_t generation = s_lyricsGeneration.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        s_lyricsTrackKey = trackKey;
        s_state.lyrics.clear();
        s_state.lyricsSynced = false;
        s_state.instrumental = false;
        s_state.lyricsLoading = !title.empty();
        ++s_state.lyricsRevision;
    }
    if (title.empty()) return;

    std::thread([generation, trackKey, title, artist, album, durationSec, deepLookup] {
        LyricsResult result = LookupLyrics(title, artist, album, durationSec, deepLookup);
        if (!s_running.load() || generation != s_lyricsGeneration.load()) return;
        const bool found = result.synced || result.instrumental || !result.lines.empty();
        if (found) {
            s_lyricsRetryCount.store(0);
            s_lyricsRetryAtMs.store(0);
        } else {
            const int failures = s_lyricsRetryCount.fetch_add(1) + 1;
            if (failures < 3) {
                const uint64_t delayMs = failures == 1 ? 3000 : 12000;
                s_lyricsRetryAtMs.store(GetTickCount64() + delayMs);
            } else {
                s_lyricsRetryAtMs.store(0);
            }
        }
        std::lock_guard<std::mutex> lock(s_mtx);
        if (trackKey != s_lyricsTrackKey) return;
        s_state.lyrics = std::move(result.lines);
        s_state.lyricsSynced = result.synced;
        s_state.instrumental = result.instrumental;
        s_state.lyricsLoading = false;
        ++s_state.lyricsRevision;
    }).detach();
}

void Init() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread([] {
        // Multi-threaded apartment — blocking .get() on async ops behaves
        // better in MTA than STA (STA needs message pumping, can hang on
        // shutdown). MTA is the right choice for a worker thread that
        // doesn't own any UI elements.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        IWICImagingFactory* wic = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&wic)))) {
            wic = nullptr; // album art will be skipped if WIC unavailable
        }

        // One-shot logging gate: log first session-found and first not-found.
        bool loggedNoSession = false;
        bool loggedSession   = false;
        std::string artTrackKey;
        bool haveArtForTrack = false;
        uint64_t lastArtAttemptTick = 0;

        // Flicker dampener — when the WinRT session momentarily drops or
        // throws (which happens on Spotify track changes / Cortana hiccups),
        // we don't blank title+artist immediately. We mark when the "no
        // session / error" state started; only after `kStickyMs` of
        // sustained empty state do we actually clear the displayed track.
        // Successful fetches reset the timer.
        constexpr uint64_t kStickyMs = 4000;
        uint64_t emptyStateStartTick = 0;

        // Cache the session manager across polls. RequestAsync().get() is a
        // heavyweight WinRT round-trip; calling it every 90ms was the real
        // "music takes ~5s to update" bug — it stalled the whole poll loop.
        // Acquire once, reuse, and only re-acquire if it drops or errors.
        wmc::GlobalSystemMediaTransportControlsSessionManager s_mgr{ nullptr };

        // Event-driven update pipeline. Whenever GSMTC fires a change event,
        // s_dirty flips and the sleep loop wakes; that way title/state/pos
        // updates land within one 10ms slice of the source app broadcasting
        // them, instead of waiting up to a full poll interval.
        wmc::GlobalSystemMediaTransportControlsSession s_boundSession{ nullptr };
        winrt::event_token tokProps{}, tokPlay{}, tokTime{}, tokCurSess{}, tokSessions{};

        auto bindSession = [&](wmc::GlobalSystemMediaTransportControlsSession newSess) {
            if (s_boundSession) {
                try {
                    if (tokProps.value) s_boundSession.MediaPropertiesChanged(tokProps);
                    if (tokPlay.value)  s_boundSession.PlaybackInfoChanged(tokPlay);
                    if (tokTime.value)  s_boundSession.TimelinePropertiesChanged(tokTime);
                } catch (...) {}
                tokProps = tokPlay = tokTime = {};
            }
            s_boundSession = newSess;
            if (!newSess) return;
            try {
                tokProps = newSess.MediaPropertiesChanged(
                    [](auto&&, auto&&){ s_dirty.store(true); });
                tokPlay = newSess.PlaybackInfoChanged(
                    [](auto&&, auto&&){ s_dirty.store(true); });
                tokTime = newSess.TimelinePropertiesChanged(
                    [](auto&&, auto&&){ s_dirty.store(true); });
                s_dirty.store(true);
            } catch (...) {}
        };

        auto fetchOnce = [&]() {
            try {
                if (!s_mgr) {
                    s_mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                    if (s_mgr) {
                        try {
                            tokCurSess = s_mgr.CurrentSessionChanged(
                                [](auto&&, auto&&){ s_dirty.store(true); });
                            tokSessions = s_mgr.SessionsChanged(
                                [](auto&&, auto&&){ s_dirty.store(true); });
                        } catch (...) {}
                    }
                }
                auto& mgr = s_mgr;
                if (!mgr) {
                    if (!loggedNoSession) {
                        loggedNoSession = true;
                    }
                    return;
                }
                auto session = mgr.GetCurrentSession();
                auto sessions = mgr.GetSessions();
                wmc::GlobalSystemMediaTransportControlsSession spotifySession{ nullptr };
                for (auto const& candidate : sessions) {
                    std::string source = winrt::to_string(candidate.SourceAppUserModelId());
                    std::transform(source.begin(), source.end(), source.begin(),
                        [](unsigned char ch) { return (char)std::tolower(ch); });
                    if (source.find("spotify") != std::string::npos) {
                        spotifySession = candidate;
                        break;
                    }
                }
                if (spotifySession) {
                    session = spotifySession;
                } else if (s_spotifyOnly.load()) {
                    session = nullptr;
                } else if (!session && sessions.Size() > 0) {
                    session = sessions.GetAt(0);
                }

                std::string sourceApp;
                bool sourceIsSpotify = false;
                if (session) {
                    sourceApp = winrt::to_string(session.SourceAppUserModelId());
                    std::string sourceLower = sourceApp;
                    std::transform(sourceLower.begin(), sourceLower.end(), sourceLower.begin(),
                        [](unsigned char ch) { return (char)std::tolower(ch); });
                    sourceIsSpotify = sourceLower.find("spotify") != std::string::npos;
                }

                // Re-bind handlers whenever the selected source changes.
                // Keep Spotify selected while other media sessions are active.
                if (session != s_boundSession) {
                    bindSession(session);
                }
                if (!session) {
                    if (!loggedNoSession) {
                        loggedNoSession = true;
                    }
                    uint64_t nowT = GetTickCount64();
                    if (emptyStateStartTick == 0) emptyStateStartTick = nowT;
                    std::lock_guard<std::mutex> lk(s_mtx);
                    s_state.playing = false;
                    s_state.sourceConnected = false;
                    s_state.sourceIsSpotify = false;
                    s_state.sourceApp[0] = 0;
                    s_state.snapshotTickMs = nowT;
                    // Only fully clear after sustained no-session — avoids
                    // the "widget vanishes for a tick during track changes"
                    // flicker. Position/duration still tick down to 0 so the
                    // displayed scrub bar doesn't lie.
                    if (nowT - emptyStateStartTick > kStickyMs) {
                        bool hadTrack = s_state.title[0] != 0;
                        s_state.title[0] = 0;
                        s_state.artist[0] = 0;
                        s_state.album[0] = 0;
                        s_state.positionSec = 0.0;
                        s_state.durationSec = 0.0;
                        if (hadTrack || s_state.lyricsLoading || !s_state.lyrics.empty()) {
                            s_lyricsGeneration.fetch_add(1);
                            s_lyricsTrackKey.clear();
                            s_state.lyrics.clear();
                            s_state.lyricsLoading = false;
                            s_state.lyricsSynced = false;
                            s_state.instrumental = false;
                            ++s_state.lyricsRevision;
                        }
                        artTrackKey.clear();
                        haveArtForTrack = false;
                    }
                    return;
                }
                // Session found — reset the empty-state grace timer.
                emptyStateStartTick = 0;
                if (!loggedSession) {
                    loggedSession = true;
                    loggedNoSession = false;
                }

                // Consume any pending playback-control requests from the UI.
                // We do these BEFORE reading properties so the new state
                // (e.g., "now paused" after a toggle) is reflected on the
                // very next snapshot — no perceived input lag.
                try {
                    if (s_reqTogglePlay.exchange(false)) {
                        session.TryTogglePlayPauseAsync().get();
                    }
                    if (s_reqSkipNext.exchange(false)) {
                        session.TrySkipNextAsync().get();
                    }
                    if (s_reqSkipPrev.exchange(false)) {
                        session.TrySkipPreviousAsync().get();
                    }
                    if (s_reqToggleShuffle.exchange(false)) {
                        bool shuffleActive = false;
                        auto playbackInfo = session.GetPlaybackInfo();
                        if (playbackInfo) {
                            auto reportedShuffle = playbackInfo.IsShuffleActive();
                            shuffleActive = reportedShuffle && reportedShuffle.Value();
                        }
                        session.TryChangeShuffleActiveAsync(!shuffleActive).get();
                    }
                    if (s_reqToggleRepeat.exchange(false)) {
                        using RepeatMode = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
                        RepeatMode nextMode = RepeatMode::List;
                        auto playbackInfo = session.GetPlaybackInfo();
                        if (playbackInfo) {
                            auto reportedRepeat = playbackInfo.AutoRepeatMode();
                            if (reportedRepeat && reportedRepeat.Value() != RepeatMode::None)
                                nextMode = RepeatMode::None;
                        }
                        session.TryChangeAutoRepeatModeAsync(nextMode).get();
                    }
                    const double seekSec = s_reqSeekSec.exchange(-1.0);
                    if (seekSec >= 0.0 && std::isfinite(seekSec)) {
                        constexpr double kTicksPerSecond = 10000000.0;
                        const int64_t seekTicks = (int64_t)std::llround(
                            std::max(0.0, seekSec) * kTicksPerSecond);
                        session.TryChangePlaybackPositionAsync(seekTicks).get();
                    }
                } catch (...) {
                    // Some apps don't support all controls; swallow + continue.
                }

                auto propsOp = session.TryGetMediaPropertiesAsync();
                auto props = propsOp.get();
                std::string title, artist, album;
                if (props) {
                    title  = winrt::to_string(props.Title());
                    artist = winrt::to_string(props.Artist());
                    album  = winrt::to_string(props.AlbumTitle());
                }

                // Playback position
                auto pi = session.GetPlaybackInfo();
                bool playing = false;
                bool shuffleActive = false;
                bool repeatActive = false;
                double playbackRate = 1.0;
                if (pi) {
                    auto status = pi.PlaybackStatus();
                    playing = (status == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    auto reportedShuffle = pi.IsShuffleActive();
                    shuffleActive = reportedShuffle && reportedShuffle.Value();
                    auto reportedRepeat = pi.AutoRepeatMode();
                    repeatActive = reportedRepeat &&
                        reportedRepeat.Value() != winrt::Windows::Media::MediaPlaybackAutoRepeatMode::None;
                    auto reportedRate = pi.PlaybackRate();
                    if (reportedRate && std::isfinite(reportedRate.Value()) &&
                        reportedRate.Value() > 0.0) {
                        playbackRate = reportedRate.Value();
                    }
                }
                auto ti = session.GetTimelineProperties();
                double posSec = 0.0, durSec = 0.0;
                uint64_t positionSampleTick = GetTickCount64();
                if (ti) {
                    auto pos = ti.Position();
                    auto end = ti.EndTime();
                    posSec = std::chrono::duration<double>(pos).count();
                    durSec = std::chrono::duration<double>(end).count();

                    // Position() belongs to LastUpdatedTime(), not to the time
                    // this getter runs. Bring it forward to one well-defined
                    // sample instant so the render thread can extrapolate from
                    // that point without every GSMTC refresh pulling it back.
                    auto sampleTime = winrt::clock::now();
                    positionSampleTick = GetTickCount64();
                    double sampleAge = std::chrono::duration<double>(
                        sampleTime - ti.LastUpdatedTime()).count();
                    if (playing && std::isfinite(sampleAge) &&
                        sampleAge > 0.0 && sampleAge < 86400.0) {
                        posSec += sampleAge * playbackRate;
                    }
                    if (durSec > 0.0)
                        posSec = std::clamp(posSec, 0.0, durSec);
                }

                // Thumbnail (album art). GSMTC properties can be polled often,
                // but decoding and uploading identical art every poll is costly.
                const std::string trackKey = title + "\n" + artist + "\n" + album;
                const bool trackChanged = trackKey != artTrackKey;
                const bool refreshLyrics = s_reqLyricsRefresh.exchange(false);
                const uint64_t lyricsNow = GetTickCount64();
                const uint64_t retryAt = s_lyricsRetryAtMs.load();
                const bool retryLyrics = retryAt != 0 && lyricsNow >= retryAt;
                if (trackChanged) {
                    artTrackKey = trackKey;
                    haveArtForTrack = false;
                    s_lyricsRetryCount.store(0);
                    s_lyricsRetryAtMs.store(0);
                }
                if (retryLyrics) s_lyricsRetryAtMs.store(0);

                std::vector<uint8_t> jpgBytes;
                uint64_t nowTick = GetTickCount64();
                bool shouldFetchArt = props && (trackChanged ||
                    (!haveArtForTrack && nowTick - lastArtAttemptTick >= 2000));
                if (shouldFetchArt) {
                    lastArtAttemptTick = nowTick;
                    try {
                        auto thumbRef = props.Thumbnail();
                        if (thumbRef) {
                            auto streamOp = thumbRef.OpenReadAsync();
                            auto stream = streamOp.get();
                            if (stream) {
                                uint64_t sz = stream.Size();
                                if (sz > 0 && sz < 4 * 1024 * 1024) {
                                    wstr::DataReader reader(stream.GetInputStreamAt(0));
                                    auto loadOp = reader.LoadAsync((uint32_t)sz);
                                    loadOp.get();
                                    jpgBytes.resize((size_t)sz);
                                    auto buf = reader.ReadBuffer((uint32_t)sz);
                                    auto bytes = buf.data();
                                    std::memcpy(jpgBytes.data(), bytes, (size_t)sz);
                                }
                            }
                        }
                    } catch (...) {}
                }

                std::vector<uint8_t> decoded;
                int w = 0, h = 0;
                bool gotArt = shouldFetchArt && DecodeArtworkBgra(wic, jpgBytes, decoded, w, h);
                if (gotArt) haveArtForTrack = true;

                {
                    std::lock_guard<std::mutex> lk(s_mtx);
                    s_state.playing = playing;
                    s_state.shuffleActive = shuffleActive;
                    s_state.repeatActive = repeatActive;
                    s_state.sourceConnected = true;
                    s_state.sourceIsSpotify = sourceIsSpotify;
                    std::strncpy(s_state.sourceApp, sourceApp.c_str(), sizeof(s_state.sourceApp) - 1);
                    s_state.sourceApp[sizeof(s_state.sourceApp) - 1] = 0;
                    std::strncpy(s_state.title, title.c_str(), sizeof(s_state.title) - 1);
                    s_state.title[sizeof(s_state.title) - 1] = 0;
                    std::strncpy(s_state.artist, artist.c_str(), sizeof(s_state.artist) - 1);
                    s_state.artist[sizeof(s_state.artist) - 1] = 0;
                    std::strncpy(s_state.album, album.c_str(), sizeof(s_state.album) - 1);
                    s_state.album[sizeof(s_state.album) - 1] = 0;
                    s_state.positionSec = posSec;
                    s_state.durationSec = durSec;
                    s_state.playbackRate = playbackRate;
                    s_state.snapshotTickMs = positionSampleTick;
                    if (gotArt) {
                        if (s_state.artBgra) { delete[] s_state.artBgra; s_state.artBgra = nullptr; }
                        s_state.artBgra = new uint8_t[decoded.size()];
                        std::memcpy(s_state.artBgra, decoded.data(), decoded.size());
                        s_state.artW = w;
                        s_state.artH = h;
                        s_state.artDirty = true;
                    } else if (trackChanged) {
                        if (s_state.artBgra) { delete[] s_state.artBgra; s_state.artBgra = nullptr; }
                        s_state.artW = 0;
                        s_state.artH = 0;
                        s_state.artDirty = true;
                    }
                }
                if (trackChanged)
                    BeginArtworkFetch(trackKey, title, artist, album, durSec);
                if (trackChanged || refreshLyrics || retryLyrics)
                    BeginLyricsFetch(trackKey, title, artist, album, durSec,
                                     refreshLyrics || retryLyrics);
            } catch (winrt::hresult_error const&) {
                s_mgr = nullptr; // force re-acquire next poll
                if (!loggedNoSession) {
                    loggedNoSession = true;
                }
                // Transient WinRT errors are common around track changes —
                // do NOT blank title/artist, just mark not-playing. The
                // sticky timer above will eventually clear if the error
                // persists past kStickyMs.
                uint64_t nowT = GetTickCount64();
                if (emptyStateStartTick == 0) emptyStateStartTick = nowT;
                std::lock_guard<std::mutex> lk(s_mtx);
                s_state.playing = false;
                s_state.sourceConnected = false;
                s_state.sourceIsSpotify = false;
                s_state.sourceApp[0] = 0;
                s_state.snapshotTickMs = nowT;
                if (nowT - emptyStateStartTick > kStickyMs) {
                    s_state.title[0] = 0;
                    s_state.artist[0] = 0;
                }
            } catch (...) {
                s_mgr = nullptr; // force re-acquire next poll
                if (!loggedNoSession) {
                    loggedNoSession = true;
                }
                uint64_t nowT = GetTickCount64();
                if (emptyStateStartTick == 0) emptyStateStartTick = nowT;
                std::lock_guard<std::mutex> lk(s_mtx);
                s_state.playing = false;
                s_state.sourceConnected = false;
                s_state.sourceIsSpotify = false;
                s_state.sourceApp[0] = 0;
                s_state.snapshotTickMs = nowT;
                if (nowT - emptyStateStartTick > kStickyMs) {
                    s_state.title[0] = 0;
                    s_state.artist[0] = 0;
                }
            }
        };

        while (s_running.load()) {
            // Consume the dirty flag so events fired during fetchOnce trigger
            // ONE more pass (not silently lost).
            s_dirty.store(false);
            fetchOnce();

            bool hasVisibleMedia = false;
            {
                std::lock_guard<std::mutex> lk(s_mtx);
                hasVisibleMedia = s_state.playing || s_state.title[0] != 0;
            }

            // Event-driven wake. GSMTC handlers set s_dirty within ms of any
            // real change (title, play/pause, position). We sleep long between
            // fetches — 1s when media's playing (so the position bar keeps
            // healing forward even in the rare case an event is dropped), 3s
            // when idle (discovery only). Control-request atomics keep the
            // sub-15ms feel on user actions.
            const int totalMs = hasVisibleMedia ? 1000 : 3000;
            for (int slept = 0; slept < totalMs && s_running.load(); slept += 10) {
                if (s_dirty.load() ||
                    s_reqTogglePlay.load() || s_reqSkipNext.load() || s_reqSkipPrev.load())
                    break;
                Sleep(10);
            }
        }

        // Clean up event subscriptions on shutdown so WinRT doesn't hold the
        // session ref forever.
        try {
            if (s_boundSession) {
                if (tokProps.value) s_boundSession.MediaPropertiesChanged(tokProps);
                if (tokPlay.value)  s_boundSession.PlaybackInfoChanged(tokPlay);
                if (tokTime.value)  s_boundSession.TimelinePropertiesChanged(tokTime);
            }
            if (s_mgr) {
                if (tokCurSess.value) s_mgr.CurrentSessionChanged(tokCurSess);
                if (tokSessions.value) s_mgr.SessionsChanged(tokSessions);
            }
        } catch (...) {}

        if (wic) wic->Release();
        winrt::uninit_apartment();
    });
}

void Shutdown() {
    if (!s_running.exchange(false)) return;
    s_lyricsGeneration.fetch_add(1); // discard any in-flight lyrics result
    s_artGeneration.fetch_add(1);
    // Don't .join() — the WinRT thread can be inside a blocking .get() on a
    // RequestAsync call. Joining can hang for seconds (Windows shows "Not
    // Responding"). Detach instead; the process is about to exit anyway, the
    // OS will reap the thread cleanly.
    if (s_thread.joinable()) s_thread.detach();
    // Don't free s_state.artBgra here — the still-running detached thread may
    // race against the free. Process exit will reclaim the memory.
}

void Tick() {
    // No-op: the background thread does the work. Kept for API compatibility
    // with the old window-title-scraping implementation.
}

const NowPlaying& Current() { return s_state; }

bool TryAcquireSnapshot() {
    s_mtx.lock();
    if (!s_state.playing && s_state.title[0] == 0) {
        s_mtx.unlock();
        return false;
    }
    return true;
}

void ReleaseSnapshot() {
    s_mtx.unlock();
}

} // namespace media

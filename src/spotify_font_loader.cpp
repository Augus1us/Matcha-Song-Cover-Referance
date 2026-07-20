#include "imgui.h"

#include <Windows.h>
#include <cstdlib>
#include <string>

struct MusicFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

MusicFonts LoadMusicFonts(ImFontAtlas* atlas) {
    MusicFonts fonts;
    if (!atlas) return fonts;

    auto load = [atlas](const char* path) -> ImFont* {
        const DWORD attributes = GetFileAttributesA(path);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return nullptr;
        }
        return atlas->AddFontFromFileTTF(path, 18.0f);
    };

    char* appData = nullptr;
    size_t appDataLength = 0;
    if (_dupenv_s(&appData, &appDataLength, "APPDATA") == 0 && appData) {
        const std::string directory =
            std::string(appData) + "\\Spotify\\Apps\\xpui\\fonts\\";
        fonts.regular = load((directory + "SpotifyMixUI-Regular.woff2").c_str());
        fonts.bold = load((directory + "SpotifyMixUI-Bold.woff2").c_str());
        free(appData);
    }

    if (!fonts.regular)
        fonts.regular = load("C:\\Windows\\Fonts\\arial.ttf");
    if (!fonts.bold)
        fonts.bold = load("C:\\Windows\\Fonts\\ARLRDBD.TTF");
    return fonts;
}

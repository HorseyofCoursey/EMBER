/*
    network_stub.cpp

    Trivial no-op implementation of the net:: interface (network.h), built
    instead of the real network.cpp for the default (non-Gonic) release --
    see platformio.ini. Linking the real network.cpp pulls in WiFi/
    HTTPClient/WiFiClientSecure, which costs ~16KB of heap just by being
    present (not by being used), and that's the room the full-screen
    visualizer needs to survive a real session's heap fragmentation. Most
    users don't use Gonic at all, so the default build drops it entirely to
    get that room back; see the Gonic-enabled build (platformio.ini env
    `cardputer-adv-gonic`) for Subsonic/Gonic streaming instead.

    main.cpp's KEY_NETWORK handler and KEY_FULLVIS's #ifndef ENABLE_GONIC
    guard mean none of these ever actually get called in this build --
    they only need to exist to satisfy the linker.
*/

#include "network.h"

namespace net {
    void enter() {}
    void exit() {}
    void tick() {}
    void drawScreen() {}
    void moveCursor(int) {}
    void onEnter() {}
    bool goBack() { return true; }
    void playNext(int) {}
    void showMessage(const char*, uint32_t) {}
    void clearMessage() {}
    int  songCount() { return 0; }
    const char* songTitle(int) { return ""; }
    void songMeta(int, char* artist, size_t artistSz, char* album, size_t albumSz, char* title, size_t titleSz) {
        if (artistSz) artist[0] = '\0';
        if (albumSz)  album[0]  = '\0';
        if (titleSz)  title[0]  = '\0';
    }
    String streamURL(int) { return String(); }
    String albumArtURL() { return String(); }
    int  currentIndex() { return -1; }
    void setCurrent(int) {}
    void setCurrentInvalid() {}
    const char* currentAlbumId() { return ""; }
}

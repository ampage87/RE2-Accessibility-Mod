// What the mod core needs from its host. On Windows the host is the ASI plugin inside
// bio2.exe; in development it is the simulator harness (tests/harness.cpp).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Reads game memory. Returns false if the address is not readable.
struct MemoryReader {
    virtual ~MemoryReader() {}
    virtual bool read(uint32_t address, void* out, size_t size) = 0;
    // Writes game memory (assist options). Hosts that cannot write return false.
    virtual bool write(uint32_t, const void*, size_t) { return false; }

    template <class T> T get(uint32_t a, T fallback = T()) {
        T v;
        return read(a, &v, sizeof(T)) ? v : fallback;
    }
    uint8_t  u8(uint32_t a)  { return get<uint8_t>(a); }
    uint16_t u16(uint32_t a) { return get<uint16_t>(a); }
    int16_t  s16(uint32_t a) { return get<int16_t>(a); }
    uint32_t u32(uint32_t a) { return get<uint32_t>(a); }
    int32_t  s32(uint32_t a) { return get<int32_t>(a); }
    std::vector<uint8_t> bytes(uint32_t a, size_t n) {
        std::vector<uint8_t> v(n);
        if (!read(a, v.data(), n)) v.clear();
        return v;
    }
};

// Logical game buttons the mod can hold down (auto-walk, turning to targets).
enum GameButton : uint32_t {
    BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8,
    BTN_ACTION = 16, BTN_RUN = 32, BTN_AIM = 64, BTN_CANCEL = 128,
};

// A string the game's PC menus drew this frame (save/load screens and their dialogs).
struct ScreenText {
    int x = 0, y = 0, colour = 0;   // x 0 = centred; colour 3 = highlighted
    std::string text;
};

struct Host {
    virtual ~Host() {}
    virtual MemoryReader& mem() = 0;
    // speech: interrupt = cut off whatever is being said
    virtual void speak(const std::string& text, bool interrupt) = 0;
    virtual void stop_speech() = 0;
    // short sound cues (wall bump, hit...). Hosts without sound may log them.
    virtual void cue(const std::string& name) = 0;
    // buttons the mod wants held this frame (0 = release everything the mod holds)
    virtual void set_buttons(uint32_t buttons) = 0;
    // buttons the player is physically holding (for wall-bump detection)
    virtual uint32_t player_buttons() = 0;
    virtual std::string game_data_dir() = 0;   // folder holding Pl0, Pl1, Common
    virtual std::string mod_data_dir() = 0;    // folder holding the mod's own data files
    virtual void log(const std::string& line) = 0;
    virtual uint64_t ms() = 0;                 // monotonic milliseconds
    // strings drawn by the game's PC menus in the latest frame (empty when none are up)
    virtual std::vector<ScreenText> screen_text() { return {}; }
    // persistent settings (RE2Access.ini on Windows)
    virtual int get_setting(const std::string& key, int def) { (void)key; return def; }
    virtual void put_setting(const std::string& key, int value) { (void)key; (void)value; }
    // a recorded sound file (audio description lines). Plays at once, cutting off the previous
    // file; hosts without sound log it.
    virtual void play_file(const std::string& path) { log("[AUDIO] play " + path); }
    virtual void stop_file() {}
    // on while the narrated introduction is on screen: its description lines play quieter (so
    // the narrator can be heard) and the game's sound is not lowered while they play
    virtual void set_intro_audio(bool on) { log(std::string("[AUDIO] introduction ") + (on ? "on" : "off")); }
    // a picture of the game's screen for diagnosis (F11 does the same). Hosts without one log it.
    virtual void save_picture() { log("[PICTURE] (not available here)"); }
};

// Mod hotkeys (bound to physical keys by the host; see docs/KEYS.md).
enum Hotkey {
    HK_NONE = 0,
    HK_NEXT, HK_PREV,                 // catalog: next / previous entry
    HK_CAT_NEXT, HK_CAT_PREV,         // catalog: next / previous category
    HK_WHERE,                         // directions to the selected entry
    HK_GO,                            // auto-walk to the selected entry
    HK_FACE,                          // turn to face the selected entry
    HK_TARGET, HK_TARGET_NEXT,        // turn to the nearest / next enemy
    HK_ROOM,                          // where am I
    HK_HEALTH, HK_INVENTORY, HK_ENEMIES, HK_SCAN, HK_COMPASS,
    HK_EXPLORE,
    HK_REPEAT, HK_HELP, HK_VERSION, HK_DIAG,
    HK_UNLIMITED_HEALTH, HK_UNLIMITED_AMMO,   // assist options (on/off)
    HK_MAP, HK_MAP_MODE,              // the map: from here / open or close
    HK_GET_AWAY,                      // run from the nearest enemy to open space
    HK_TIMER,                         // time left on the escape countdown
    HK_COUNT
};

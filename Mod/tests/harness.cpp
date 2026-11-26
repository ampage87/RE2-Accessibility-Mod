// Simulator host for the real mod code. Built for Linux/macOS/Windows with any C++17
// compiler; the simulator (re2sim/cosim.py) starts it and talks over stdin/stdout.
//
// Protocol, one exchange per game frame:
//   simulator -> harness:  "FRAME <ms> <player_buttons>\n"
//                           "MEM <hex addr> <len>\n" + <len raw bytes>   (any number)
//                           "TEXT <x> <y> <colour> <hex text>\n"         (PC menu strings drawn)
//                           "KEY <hotkey number>\n"                      (any number)
//                           "END\n"
//   harness -> simulator:  "SAY <0|1> <text>\n" | "CUE <name>\n" | "LOG <text>\n" (any number)
//                           "WRITE <hex addr> <hex bytes>\n" (memory the mod changed)
//                           "BUTTONS <bits>\n"
//                           "DONE\n"
// Memory persists between frames; the simulator resends regions that change.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "../src/mod.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

struct SimMemory : MemoryReader {
    std::map<uint32_t, std::vector<uint8_t>> regions;   // base -> bytes
    bool read(uint32_t a, void* out, size_t n) override {
        auto it = regions.upper_bound(a);
        if (it == regions.begin()) return false;
        --it;
        uint32_t base = it->first;
        const auto& v = it->second;
        if (a < base || a + n > base + v.size()) return false;
        std::memcpy(out, v.data() + (a - base), n);
        return true;
    }
    void write(uint32_t a, std::vector<uint8_t> bytes) { regions[a] = std::move(bytes); }
    std::string* out = nullptr;
    // mod writes (assists): applied here and reported so the simulator can apply them too
    bool write(uint32_t a, const void* in, size_t n) override {
        auto it = regions.upper_bound(a);
        if (it == regions.begin()) return false;
        --it;
        if (a < it->first || a + n > it->first + it->second.size()) return false;
        std::memcpy(it->second.data() + (a - it->first), in, n);
        if (out) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "WRITE %x ", a);
            *out += buf;
            for (size_t i = 0; i < n; i++) { std::snprintf(buf, sizeof buf, "%02x", ((const uint8_t*)in)[i]); *out += buf; }
            *out += "\n";
        }
        return true;
    }
};

struct SimHost : Host {
    SimMemory memory;
    std::string data_dir, mod_dir;
    uint32_t buttons = 0, player = 0;
    uint64_t now = 0;
    std::string out;
    MemoryReader& mem() override { return memory; }
    void speak(const std::string& t, bool interrupt) override { out += "SAY " + std::string(interrupt ? "1 " : "0 ") + t + "\n"; }
    void stop_speech() override {}
    void cue(const std::string& n) override { out += "CUE " + n + "\n"; }
    void set_buttons(uint32_t b) override { buttons = b; }
    uint32_t player_buttons() override { return player; }
    std::string game_data_dir() override { return data_dir; }
    std::string mod_data_dir() override { return mod_dir; }
    void log(const std::string& l) override {
        std::string s = l;
        for (auto& c : s) if (c == '\n') c = ' ';
        out += "LOG " + s + "\n";
    }
    uint64_t ms() override { return now; }
    std::map<std::string, int> settings;
    std::vector<ScreenText> text, text_next;
    std::vector<ScreenText> screen_text() override { return text; }
    int get_setting(const std::string& k, int def) override { auto it = settings.find(k); return it == settings.end() ? def : it->second; }
    void put_setting(const std::string& k, int v) override { settings[k] = v; }
};

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: harness <game data dir> <mod data dir>\n"); return 2; }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    SimHost host;
    host.data_dir = argv[1];
    host.mod_dir = argv[2];
    host.memory.out = &host.out;
    re2::Mod mod(host);
    std::cout << host.out << "READY\n" << std::flush;
    host.out.clear();
    std::string line;
    std::vector<int> keys;
    while (std::getline(std::cin, line)) {
        if (line.rfind("FRAME ", 0) == 0) {
            unsigned long long ms = 0; unsigned pb = 0;
            std::sscanf(line.c_str() + 6, "%llu %u", &ms, &pb);
            host.now = ms; host.player = pb;
            keys.clear();
            host.text_next.clear();
        } else if (line.rfind("MEM ", 0) == 0) {
            unsigned a = 0; size_t n = 0;
            std::sscanf(line.c_str() + 4, "%x %zu", &a, &n);
            std::vector<uint8_t> b(n);
            std::cin.read(reinterpret_cast<char*>(b.data()), (std::streamsize)n);
            host.memory.write(a, std::move(b));
        } else if (line.rfind("TEXT ", 0) == 0) {
            ScreenText t;
            char hex[1024] = {0};
            std::sscanf(line.c_str() + 5, "%d %d %d %1023s", &t.x, &t.y, &t.colour, hex);
            for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
                unsigned v = 0;
                std::sscanf(hex + i, "%2x", &v);
                t.text += (char)v;
            }
            host.text_next.push_back(t);
        } else if (line.rfind("KEY ", 0) == 0) {
            keys.push_back(std::atoi(line.c_str() + 4));
        } else if (line == "END") {
            host.text.swap(host.text_next);
            mod.frame();
            for (int k : keys) mod.hotkey((Hotkey)k);
            std::cout << host.out << "BUTTONS " << host.buttons << "\nBUSY " << (mod.busy() ? 1 : 0)
                      << "\nDONE\n" << std::flush;
            host.out.clear();
        } else if (line == "QUIT") {
            break;
        }
    }
    return 0;
}

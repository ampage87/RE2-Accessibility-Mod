// Checks the examine-spot naming (Mod::examine_noun) against messages met in the game.
// Build: g++ -std=c++17 -Isrc tests/nouns.cpp src/mod.cpp src/state.cpp src/room.cpp -o tests/nouns
#include "mod.h"
#include <cstdio>
int main() {
    struct { const char* msg; const char* want; } cases[] = {
        {"The number 12 has been etched. Will you turn the faucet on?", "number 12"},
        {"A police B1 map. Will you take it?", "police B1 map"},
        {"You can light a fire here.", "place to light a fire"},
        {"Nothing but an oil painting wrapped in cloth.", "oil painting"},
        {"Nothing useful on the shelf.", "shelf"},
        {"It looks like there was a recent accident here.", "recent accident"},
        {"There's a button below the picture. Will you push it?", "button below the picture"},
        {"None of these books appear to be useful.", "books"},
        {"An open/close switch for the shutter.", "open-close switch"},
        {"It's a fire place. An oil painting hangs above it.", "fire place"},
        {"It's locked.", ""},
    };
    int bad = 0;
    for (auto& c : cases) {
        std::string got = re2::Mod::examine_noun(c.msg);
        if (got != c.want) { std::printf("FAIL %s -> [%s], want [%s]\n", c.msg, got.c_str(), c.want); bad++; }
    }
    std::printf("%d/%zu noun checks passed\n", (int)(sizeof cases / sizeof cases[0]) - bad, sizeof cases / sizeof cases[0]);
    return bad != 0;
}

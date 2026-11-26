// Room data: RDT parsing (collision, cameras, camera zones, messages, script AOT
// declarations), geometry helpers and the navigation grid. Ported from the simulator
// (rdt.py, world.py), which was validated by walking every exit of every room.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace re2 {

constexpr int DIR_FULL = 4096;
// The player's collision half-width: bio2.exe sets player +0x90/+0x92 to 0x1C2 (Leon and
// Claire); the mod reads the live value and uses this when it cannot.
constexpr double PLAYER_RADIUS = 450;
// Collision flag bits (bio2.exe 0x4C0486 -> 0x4CC730 -> 0x4E03B0): an entry pushes the player
// out when its flag word has the player's bit 0x8000 and not the "ignore" bit 0x0100. (Zombies
// use 0x0400 and other enemies 0x1000, which is why some entries stop enemies but not the player.)
constexpr uint16_t PLAYER_BLOCK_BIT = 0x8000;
constexpr uint16_t IGNORE_BIT = 0x0100;
constexpr uint16_t CLIMB_BIT = 0x8000;     // climb/drop boxes the player can use: the player's check (bio2.exe 0x4CDE9A)
                                           // takes the player's bit; 0x1000 is the partner's (0x4CE179). 603's step up from the crate has only 0x8000

struct Pt { double x = 0, z = 0; };
using Quad = std::vector<Pt>;

struct Collision {
    int index = 0;
    int x = 0, z = 0, w = 0, d = 0;
    int shape = 0;          // 0 box, 1-3 triangles, 6 circle, 7/8 rounded, 9 climb-up, 10 drop-down, 11 slope, 12 stairs
    uint32_t floor_mask = 0;
    uint16_t raw_type = 0, raw_flag = 0;
    bool on_floor(int f) const { return f >= 0 && f <= 31 && (floor_mask & (1u << f)); }
    bool contains(double px, double pz) const;
    bool walkable() const { return shape >= 11 && shape <= 15; }
    bool blocks_player() const { return (raw_flag & PLAYER_BLOCK_BIT) != 0 && !(raw_flag & IGNORE_BIT); }
    bool climbable() const { return (raw_flag & CLIMB_BIT) != 0; }
    // does a body of half-width r centred at (px, pz) overlap this shape?
    bool overlaps(double px, double pz, double r) const;
};

// floors a stairway or slope rises: its floor mask runs from the bottom floor to the top one
// (116's long flight: floors 1 to 4); the type's multiplier says the same for ordinary stairs
// but not for those
inline int stair_rise(const Collision& c) {
    if (!c.floor_mask) return 1;
    int low = 0, top = 31;
    while (low < 31 && !(c.floor_mask & (1u << low))) low++;
    while (top > 0 && !(c.floor_mask & (1u << top))) top--;
    if (top > low) return top - low;
    return std::max(1, (c.raw_type >> 6) & 0x1F);
}

struct Camera { int flag = 0, dist = 0; int fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0; };

struct ScriptDoor { Quad quad; int stage = 0, room = 0; int aot = -1, lock_id = 0, lock_type = 0, anim = 0, tex = 0; bool parked = false; };   // stage 0-based

struct RoomFile {
    bool ok = false;
    int stage = 0, room = 0, player = 0;   // stage 0-based
    std::vector<Collision> collisions;
    std::vector<Camera> cameras;
    std::vector<std::string> messages;     // English, decoded
    std::vector<ScriptDoor> script_doors;  // every door declared anywhere in the room's scripts
    std::vector<int> proc_message;         // main-script procedure -> first message it shows (-1 none)
    std::vector<std::pair<size_t, size_t>> main_procs;   // main-script procedures: [start, end) in raw
    std::vector<std::vector<uint8_t>> message_bytes;     // English messages as stored (for patching checks)
    std::vector<uint8_t> raw;
    size_t init_start = 0, init_end = 0;                  // the init script's first procedure
};

// Loads <data>/Pl<p>/Rdt/ROOM<s+1><rr><p>.RDT (or Rdu for English data).
RoomFile load_room(const std::string& data_dir, int stage0, int room, int player);
void parse_sca(const uint8_t* p, size_t n, std::vector<Collision>& out);
std::string decode_text(const uint8_t* p, size_t n, int inserted_item, bool* yesno = nullptr);
std::vector<uint8_t> encode_text(const std::string& text);   // characters only (no control codes)
std::string room_code(int stage0, int room);   // "100"

// ---- geometry ------------------------------------------------------------------------
void facing_vector(int dir, double& fx, double& fz);
int dir_towards(double fx, double fz, double tx, double tz);
int angle_diff(int a, int b);                  // b - a in -2048..2047, + = clockwise (right)
std::string compass(int dir);
std::string relative_direction(int player_dir, double px, double pz, double tx, double tz);
std::string metres(double units);
bool point_in_quad(double px, double pz, const Quad& q);
Pt quad_centre(const Quad& q);
Quad rect_quad(double x, double z, double w, double d);
const Collision* blocking_at(const std::vector<Collision>& cols, const std::vector<int>& floors,
                             double px, double pz, double radius = PLAYER_RADIUS);
bool stair_underside(const Collision& c, const std::vector<Collision>& cols);
// Is there floor under (px, pz) at this floor? Always true on floor 0.
bool supported_at(const std::vector<Collision>& cols, int floor, double px, double pz);

class NavGrid {
public:
    // need_support: above floor 0, only places with something solid on the floor below (or
    // stairs) count as floor; otherwise the open air above a lower level looks walkable.
    NavGrid(const std::vector<Collision>& cols, int floor, int cell = 100, double radius = PLAYER_RADIUS + 20,
            bool need_support = true);
    bool find_path(double sx, double sz, double tx, double tz, int goal_radius_cells, std::vector<Pt>& out) const;
    std::vector<std::pair<int, int>> reachable(double sx, double sz) const;
    bool line_free(Pt a, Pt b) const;
    Pt to_world(int i, int j) const { return {minx + i * cell + cell / 2.0, minz + j * cell + cell / 2.0}; }
    std::pair<int, int> to_cell(double x, double z) const { return {int((x - minx) / cell), int((z - minz) / cell)}; }
    bool is_free(int i, int j) const { return i >= 0 && j >= 0 && i < w && j < h && free_[j * w + i]; }
    bool nearest_free(int i, int j, int& oi, int& oj, int maxr = 25) const;
    int floor() const { return floor_; }
    double radius() const { return radius_; }
    // places to keep away from (enemies): paths go around them when they can
    void set_avoid(const std::vector<std::pair<Pt, double>>& circles) { avoid_ = circles; }
    double clearance(const std::vector<Pt>& path) const;   // closest approach to an avoided point
private:
    std::vector<std::pair<Pt, double>> avoid_;
    bool in_avoid(double x, double z) const {
        for (auto& c : avoid_) if (std::hypot(x - c.first.x, z - c.first.z) < c.second) return true;
        return false;
    }
    std::vector<Collision> cols_;
    int floor_, cell, w = 0, h = 0;
    double minx = 0, minz = 0, radius_;
    std::vector<uint8_t> free_;
    void smooth(std::vector<Pt>& path) const;
};

}  // namespace re2

#include "room.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <queue>
#include <set>
#include <map>
#include "tables.inc"

namespace re2 {

static const double PI = 3.14159265358979323846;

template <class T> static T rd(const std::vector<uint8_t>& b, size_t o) {
    T v{};
    if (o + sizeof(T) <= b.size()) std::memcpy(&v, &b[o], sizeof(T));
    return v;
}

std::string room_code(int stage0, int room) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%X%02X", stage0 + 1, room & 0xFF);
    return buf;
}

// ---------------------------------------------------------------- text
std::string decode_text(const uint8_t* p, size_t n, int inserted_item, bool* yesno) {
    std::string out;
    if (yesno) *yesno = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b == 0xFE) break;
        if (b == 0xF9 || b == 0xFA) { i++; continue; }          // colour / start text
        if (b == 0xFB) { if (yesno) *yesno = true; break; }   // yes/no question: the message ends there
        if (b == 0xFC) { out += ' '; continue; }               // line break
        if (b == 0xFD) { out += ' '; i++; continue; }          // page break
        if (b == 0xF8) {                                        // item name insertion
            if (inserted_item > 0 && inserted_item < kItemCount && kItemNames[inserted_item][0])
                out += kItemNames[inserted_item];
            else out += "item";
            i++;
            continue;
        }
        if (b == 0xF3) { out += '?'; continue; }
        if (b == 0xEF) { i++; continue; }                       // text colour
        if (b == 0xF0) {                                        // extra glyph: 09 0A 0B = the S.T.A.R.S. logo
            if (i + 1 < n && p[i + 1] == 0x09) out += "S.T.A.R.S.";
            i++;
            continue;
        }
        if (b < kMsgCharCount) out += kMsgChars[b];
    }
    // tidy spaces
    std::string t;
    for (char c : out) {
        if (c == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += c;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    // curly quotes are UTF-8 in the table; keep as is
    return t;
}

std::vector<uint8_t> encode_text(const std::string& text) {
    std::vector<uint8_t> out;
    for (char ch : text) {
        for (int b = 0; b < kMsgCharCount; b++)
            if (kMsgChars[b][0] == ch && kMsgChars[b][1] == 0) { out.push_back((uint8_t)b); break; }
    }
    return out;
}

// ---------------------------------------------------------------- RDT
void parse_sca(const uint8_t* p, size_t n, std::vector<Collision>& out) {
    out.clear();
    if (n < 16) return;
    uint32_t count;
    std::memcpy(&count, p + 4, 4);
    size_t o = 16;
    for (uint32_t i = 0; i + 1 < count && o + 16 <= n; i++, o += 16) {
        int16_t x, z; uint16_t w, d, flg, typ; uint32_t fmask;
        std::memcpy(&x, p + o, 2); std::memcpy(&z, p + o + 2, 2);
        std::memcpy(&w, p + o + 4, 2); std::memcpy(&d, p + o + 6, 2);
        std::memcpy(&flg, p + o + 8, 2); std::memcpy(&typ, p + o + 10, 2);
        std::memcpy(&fmask, p + o + 12, 4);
        Collision c;
        c.index = (int)i; c.x = x; c.z = z; c.w = w; c.d = d;
        c.shape = flg & 0x0F; c.floor_mask = fmask; c.raw_type = typ; c.raw_flag = flg;
        out.push_back(c);
    }
}

static void scan_script_doors(const std::vector<uint8_t>& raw, size_t start, size_t end, RoomFile& r,
                              bool main_script = false) {
    if (start == 0 || end <= start || end > raw.size()) return;
    // procedure table: first u16 = offset of procedure 0 = table size
    uint16_t first = rd<uint16_t>(raw, start);
    size_t nprocs = first / 2;
    if (nprocs == 0 || first > end - start) return;
    std::vector<size_t> offs;
    for (size_t i = 0; i < nprocs; i++) offs.push_back(start + rd<uint16_t>(raw, start + i * 2));
    if (main_script) {
        r.proc_message.assign(nprocs, -1);
        r.main_procs.clear();
        for (size_t i = 0; i < nprocs; i++) r.main_procs.push_back({offs[i], (i + 1 < nprocs) ? offs[i + 1] : end});
    }
    std::vector<std::vector<int>> calls(nprocs);     // gosub targets, for procedures whose message is in a helper
    for (size_t i = 0; i < nprocs; i++) {
        size_t p = offs[i], e = (i + 1 < nprocs) ? offs[i + 1] : end;
        while (p < e && p < raw.size()) {
            uint8_t op = raw[p];
            int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
            if (sz <= 0) break;
            if (main_script && op == OP_MESSAGE_ON && p + 3 <= raw.size() && r.proc_message[i] < 0)
                r.proc_message[i] = raw[p + 2];
            if (main_script && op == 0x18 && p + 1 < raw.size()) calls[i].push_back(raw[p + 1]);   // gosub
            if (op == OP_DOOR_AOT_SE && p + 32 <= raw.size()) {
                ScriptDoor d;
                int16_t x = rd<int16_t>(raw, p + 6), z = rd<int16_t>(raw, p + 8);
                uint16_t w = rd<uint16_t>(raw, p + 10), dd = rd<uint16_t>(raw, p + 12);
                d.quad = rect_quad(x, z, w, dd);
                d.stage = raw[p + 22]; d.room = raw[p + 23];
                d.aot = raw[p + 1]; d.lock_id = raw[p + 29]; d.lock_type = raw[p + 30]; d.anim = raw[p + 27]; d.tex = raw[p + 26];
                d.parked = raw[p + 3] == 0 || w == 0 || dd == 0;
                r.script_doors.push_back(d);
            } else if (op == OP_DOOR_AOT_SET_4P && p + 40 <= raw.size()) {
                ScriptDoor d;
                for (int k = 0; k < 4; k++)
                    d.quad.push_back({(double)rd<int16_t>(raw, p + 6 + k * 4), (double)rd<int16_t>(raw, p + 8 + k * 4)});
                d.stage = raw[p + 30]; d.room = raw[p + 31];
                d.aot = raw[p + 1]; d.lock_id = raw[p + 37]; d.lock_type = raw[p + 38]; d.anim = raw[p + 35]; d.tex = raw[p + 34];
                d.parked = raw[p + 3] == 0;
                r.script_doors.push_back(d);
            }
            p += sz;
        }
    }
    // a procedure that shows no message itself takes the first one of what it calls
    if (main_script)
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < nprocs; i++)
                if (r.proc_message[i] < 0)
                    for (int c : calls[i])
                        if (c >= 0 && c < (int)nprocs && r.proc_message[c] >= 0) { r.proc_message[i] = r.proc_message[c]; break; }
}

RoomFile load_room(const std::string& dir, int stage0, int room, int player) {
    RoomFile r;
    r.stage = stage0; r.room = room; r.player = player;
    char name[32];
    std::snprintf(name, sizeof name, "ROOM%X%02X%d.RDT", stage0 + 1, room & 0xFF, player);
    const char* subs[] = {"Rdt", "RDT", "rdt", "Rdu", "RDU"};
    FILE* f = nullptr;
    for (const char* s : subs) {
        std::string path = dir + "/Pl" + std::to_string(player) + "/" + s + "/" + name;
        f = std::fopen(path.c_str(), "rb");
        if (!f) {   // file names are lower case in some installs
            std::string low = name;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            path = dir + "/Pl" + std::to_string(player) + "/" + s + "/" + low;
            f = std::fopen(path.c_str(), "rb");
        }
        if (f) break;
    }
    if (!f) return r;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    r.raw.resize(n > 0 ? n : 0);
    if (n > 0 && std::fread(r.raw.data(), 1, n, f) != (size_t)n) r.raw.clear();
    std::fclose(f);
    auto& b = r.raw;
    if (b.size() < 8 + 23 * 4) return r;
    uint32_t off[23];
    for (int i = 0; i < 23; i++) off[i] = rd<uint32_t>(b, 8 + i * 4);
    auto chunk_end = [&](uint32_t s) {
        uint32_t e = (uint32_t)b.size();
        for (int i = 0; i < 23; i++) if (off[i] > s && off[i] < e) e = off[i];
        return e;
    };
    // cameras (RID)
    int ncut = b[1];
    for (int i = 0; i < ncut && off[7] && off[7] + (i + 1) * 0x20 <= b.size(); i++) {
        size_t o = off[7] + i * 0x20;
        Camera c;
        c.flag = rd<uint16_t>(b, o); c.dist = rd<uint16_t>(b, o + 2);
        c.fx = rd<int32_t>(b, o + 4); c.fy = rd<int32_t>(b, o + 8); c.fz = rd<int32_t>(b, o + 12);
        c.tx = rd<int32_t>(b, o + 16); c.ty = rd<int32_t>(b, o + 20); c.tz = rd<int32_t>(b, o + 24);
        r.cameras.push_back(c);
    }
    // collision (SCA)
    if (off[6] && off[6] < b.size()) parse_sca(&b[off[6]], chunk_end(off[6]) - off[6], r.collisions);
    // English messages
    if (off[14] && off[14] + 2 <= b.size()) {
        uint32_t s = off[14], e = chunk_end(s);
        uint16_t first = rd<uint16_t>(b, s);
        size_t nm = first / 2;
        if (first && first <= e - s) {
            for (size_t i = 0; i < nm; i++) {
                uint32_t mo = s + rd<uint16_t>(b, s + i * 2);
                uint32_t me = (i + 1 < nm) ? s + rd<uint16_t>(b, s + (i + 1) * 2) : e;
                if (mo < me && me <= b.size()) {
                    r.messages.push_back(decode_text(&b[mo], me - mo, 0));
                    std::vector<uint8_t> raw(b.begin() + mo, b.begin() + me);
                    for (size_t k = 0; k < raw.size(); k++)
                        if (raw[k] == 0xFE || raw[k] == 0xFB) { raw.resize(std::min(raw.size(), k + 2)); break; }
                    r.message_bytes.push_back(raw);
                } else {
                    r.messages.push_back("");
                    r.message_bytes.push_back({});
                }
            }
        }
    }
    // script door declarations (init and main scripts)
    if (off[16]) {
        scan_script_doors(b, off[16], chunk_end(off[16]), r);
        size_t s0 = off[16], e0 = chunk_end(off[16]);
        if (s0 + 2 <= b.size()) {
            size_t first = rd<uint16_t>(b, s0);
            size_t n = first / 2;
            r.init_start = s0 + first;
            r.init_end = n > 1 && s0 + 4 <= b.size() ? s0 + rd<uint16_t>(b, s0 + 2) : e0;
            if (r.init_end > e0 || r.init_end < r.init_start) r.init_end = e0;
        }
    }
    if (off[17]) scan_script_doors(b, off[17], chunk_end(off[17]), r, true);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- collision shapes
static bool in_tri(double px, double pz, Pt a, Pt b, Pt c) {
    auto sign = [](double x1, double z1, Pt p2, Pt p3) {
        return (x1 - p3.x) * (p2.z - p3.z) - (p2.x - p3.x) * (z1 - p3.z);
    };
    double d1 = sign(px, pz, a, b), d2 = sign(px, pz, b, c), d3 = sign(px, pz, c, a);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

bool Collision::contains(double px, double pz) const {
    double X = x, Z = z, W = w, D = d;
    switch (shape) {
        case 1: return in_tri(px, pz, {X, Z + D}, {X + W, Z + D}, {X + W, Z});
        case 2: return in_tri(px, pz, {X, Z}, {X, Z + D}, {X + W, Z + D});
        case 3: return in_tri(px, pz, {X, Z}, {X + W, Z + D}, {X + W, Z});
        // bio2.exe's pushers 0x4E14B0 (the fourth half-box) and 0x4E17B0 (a diamond whose
        // corners are the box's side midpoints, 612's three consoles)
        case 4: return in_tri(px, pz, {X, Z}, {X + W, Z}, {X, Z + D});
        case 5: {
            double hw = W / 2, hd = D / 2;
            if (hw <= 0 || hd <= 0) return false;
            return std::fabs(px - (X + hw)) / hw + std::fabs(pz - (Z + hd)) / hd <= 1.0;
        }
        case 6: { double r = W / 2; return (px - (X + r)) * (px - (X + r)) + (pz - (Z + r)) * (pz - (Z + r)) <= r * r; }
        case 7: {   // capsule along x
            double r = D / 2, cz = Z + r;
            if (px >= X + r && px <= X + W - r && pz >= Z && pz <= Z + D) return true;
            for (double cx : {X + r, X + W - r})
                if ((px - cx) * (px - cx) + (pz - cz) * (pz - cz) <= r * r) return true;
            return false;
        }
        case 8: {   // capsule along z
            double r = W / 2, cx = X + r;
            if (pz >= Z + r && pz <= Z + D - r && px >= X && px <= X + W) return true;
            for (double cz : {Z + r, Z + D - r})
                if ((px - cx) * (px - cx) + (pz - cz) * (pz - cz) <= r * r) return true;
            return false;
        }
        default: return px >= X && px <= X + W && pz >= Z && pz <= Z + D;
    }
}

// A body of half-width r at (px, pz) against one shape. Boxes match the game exactly (bio2.exe
// 0x4E0A00 pushes a square body out of the box grown by r); round shapes use their outline
// grown by r; triangles are sampled.
bool Collision::overlaps(double px, double pz, double r) const {
    double X = x, Z = z, W = w, D = d;
    if (px + r <= X || px - r >= X + W || pz + r <= Z || pz - r >= Z + D) return false;
    switch (shape) {
        case 0: case 9: case 10: return true;
        case 6: {
            double rr = W / 2;
            return std::hypot(px - (X + rr), pz - (Z + rr)) < rr + r;
        }
        case 7: case 8: {
            double rr = shape == 7 ? D / 2 : W / 2;
            double sx0 = X + rr, sx1 = X + W - rr, sz0 = Z + rr, sz1 = Z + D - rr;
            if (shape == 7) sz0 = sz1 = Z + rr; else sx0 = sx1 = X + rr;
            if (sx1 < sx0) sx0 = sx1 = (sx0 + sx1) / 2;
            if (sz1 < sz0) sz0 = sz1 = (sz0 + sz1) / 2;
            double cx = std::min(std::max(px, sx0), sx1), cz = std::min(std::max(pz, sz0), sz1);
            // a margin round the ends: the player stuck against the end of 612's long rounded counter
            // where the round outline said there was room (not along the sides: 610's channel)
            bool at_end = shape == 7 ? (px < sx0 || px > sx1) : (pz < sz0 || pz > sz1);
            return std::hypot(px - cx, pz - cz) < rr + r + (at_end ? 200 : 0);
        }
        default: {
            double pts[9][2] = {{px, pz}};
            for (int k = 0; k < 8; k++) {
                double a = k * PI / 4;
                pts[k + 1][0] = px + r * std::cos(a);
                pts[k + 1][1] = pz + r * std::sin(a);
            }
            for (auto& q : pts) if (contains(q[0], q[1])) return true;
            return false;
        }
    }
}

// A box on one floor that covers an active stairway rising from that floor: the space under
// the stairs (room 111: the crank switches the stairs and this box on together). The game lets
// the player onto the stairs there, so it is not a wall. (Needs a real-game check.)
bool stair_underside(const Collision& c, const std::vector<Collision>& cols) {
    if (c.shape != 0 || !c.floor_mask || (c.floor_mask & (c.floor_mask - 1))) return false;
    for (const auto& st : cols) {
        if ((st.shape != 11 && st.shape != 12) || !(st.raw_flag & PLAYER_BLOCK_BIT) || !st.floor_mask) continue;
        if ((st.floor_mask & (0u - st.floor_mask)) != c.floor_mask) continue;    // rises from the box's floor
        double ox = std::min<double>(c.x + c.w, st.x + st.w) - std::max(c.x, st.x);
        double oz = std::min<double>(c.z + c.d, st.z + st.d) - std::max(c.z, st.z);
        if (ox > 0 && oz > 0 && ox * oz >= 0.9 * double(st.w) * st.d) return true;
    }
    return false;
}

const Collision* blocking_at(const std::vector<Collision>& cols, const std::vector<int>& floors,
                             double px, double pz, double radius) {
    for (const auto& c : cols) {
        if (c.walkable() || !c.blocks_player()) continue;
        if (stair_underside(c, cols)) continue;
        bool all = true;
        for (int f : floors) if (!c.on_floor(f)) { all = false; break; }
        if (!all) continue;
        if (c.overlaps(px, pz, radius)) return &c;
    }
    return nullptr;
}

// ---------------------------------------------------------------- directions
void facing_vector(int dir, double& fx, double& fz) {
    double t = ((dir % DIR_FULL + DIR_FULL) % DIR_FULL) / double(DIR_FULL) * 2 * PI;
    fx = std::cos(t);
    fz = -std::sin(t);
}

int dir_towards(double fx, double fz, double tx, double tz) {
    double t = std::atan2(-(tz - fz), tx - fx);
    int d = (int)std::lround(t / (2 * PI) * DIR_FULL);
    return ((d % DIR_FULL) + DIR_FULL) % DIR_FULL;
}

int angle_diff(int a, int b) {
    int d = ((b - a) % DIR_FULL + DIR_FULL) % DIR_FULL;
    if (d >= DIR_FULL / 2) d -= DIR_FULL;
    return d;
}

std::string compass(int dir) {
    double fx, fz;
    facing_vector(dir, fx, fz);
    double deg = std::fmod(std::atan2(fx, fz) * 180 / PI + 360, 360);
    static const char* names[] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    return names[int((deg + 22.5) / 45) % 8];
}

std::string relative_direction(int pd, double px, double pz, double tx, double tz) {
    int want = dir_towards(px, pz, tx, tz);
    double deg = angle_diff(pd, want) / double(DIR_FULL) * 360;
    double a = std::fabs(deg);
    if (a <= 20) return "straight ahead";
    if (a >= 160) return "behind you";
    std::string side = deg > 0 ? "right" : "left";
    if (a <= 60) return "ahead to your " + side;
    if (a <= 120) return "to your " + side;
    return "behind you to the " + side;
}

std::string metres(double units) {
    double m = units / 1000.0;
    if (m < 1) return "less than a metre";
    if (m < 1.5) return "1 metre";
    return std::to_string((int)std::lround(m)) + " metres";
}

bool point_in_quad(double px, double pz, const Quad& q) {
    bool inside = false;
    size_t n = q.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = q[i].x, zi = q[i].z, xj = q[j].x, zj = q[j].z;
        double dz = (zj - zi) != 0 ? (zj - zi) : 1e-9;
        if (((zi > pz) != (zj > pz)) && (px < (xj - xi) * (pz - zi) / dz + xi)) inside = !inside;
    }
    return inside;
}

Pt quad_centre(const Quad& q) {
    Pt c;
    for (auto& p : q) { c.x += p.x; c.z += p.z; }
    if (!q.empty()) { c.x /= q.size(); c.z /= q.size(); }
    return c;
}

Quad rect_quad(double x, double z, double w, double d) {
    return {{x, z}, {x + w, z}, {x + w, z + d}, {x, z + d}};
}

// ---------------------------------------------------------------- navigation grid
bool supported_at(const std::vector<Collision>& cols, int floor, double px, double pz) {
    if (floor <= 0) return true;
    for (const auto& c : cols) {
        if (c.shape == 11 || c.shape == 12) {
            if (!c.floor_mask) continue;
            int low = 0;
            while (low < 31 && !(c.floor_mask & (1u << low))) low++;
            int high = low + stair_rise(c);
            if (low <= floor && floor <= high && c.contains(px, pz)) return true;
            // the landing just past either end of a flight (409: two slopes meet at a corner
            // landing that has no collision box of its own): solid on that end's level
            const double m = 450;
            if ((floor == low || floor == high) && px >= c.x - m && px <= c.x + c.w + m && pz >= c.z - m && pz <= c.z + c.d + m)
                return true;
        } else if (c.shape != 4 && c.shape != 5 && c.shape < 13 && c.on_floor(floor - 1) && c.contains(px, pz)) {
            return true;
        }
    }
    return false;
}

NavGrid::NavGrid(const std::vector<Collision>& cols, int floor, int cell_, double radius, bool need_support)
    : floor_(floor), cell(cell_), radius_(radius) {
    double mnx = 1e9, mnz = 1e9, mxx = -1e9, mxz = -1e9;
    for (auto& c : cols) {
        mnx = std::min(mnx, (double)c.x); mnz = std::min(mnz, (double)c.z);
        mxx = std::max(mxx, (double)c.x + c.w); mxz = std::max(mxz, (double)c.z + c.d);
        if (c.on_floor(floor)) cols_.push_back(c);
    }
    if (cols.empty()) { mnx = mnz = -16000; mxx = mxz = 16000; }
    minx = std::max(-32768.0, mnx - cell);
    minz = std::max(-32768.0, mnz - cell);
    double maxx = std::min(32767.0, mxx + cell), maxz = std::min(32767.0, mxz + cell);
    w = int((maxx - minx) / cell) + 1;
    h = int((maxz - minz) / cell) + 1;
    free_.assign((size_t)w * h, 0);
    std::vector<int> fl{floor};
    // active stairs and slopes (player bit set) on this floor: a body touching one is on it, and
    // then only what stands on both of its levels blocks (like the game; room 111's stairwell)
    std::vector<const Collision*> stairs;
    for (auto& c : cols_)
        if ((c.shape == 11 || c.shape == 12) && (c.raw_flag & PLAYER_BLOCK_BIT)) stairs.push_back(&c);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            Pt p = to_world(i, j);
            std::vector<int> here = fl;
            for (auto* c : stairs)
                if (p.x >= c->x - radius_ && p.x <= c->x + c->w + radius_ && p.z >= c->z - radius_ && p.z <= c->z + c->d + radius_) {
                    int low = 0;
                    while (low < 31 && !(c->floor_mask & (1u << low))) low++;
                    int high = low + stair_rise(*c);
                    here = {low, high};
                    break;
                }
            free_[(size_t)j * w + i] = blocking_at(cols_, here, p.x, p.z, radius_) ? 0 : 1;
            if (need_support && floor > 0 && free_[(size_t)j * w + i] && !supported_at(cols, floor, p.x, p.z))
                free_[(size_t)j * w + i] = 0;
        }
}

bool NavGrid::nearest_free(int i, int j, int& oi, int& oj, int maxr) const {
    if (is_free(i, j)) { oi = i; oj = j; return true; }
    for (int r = 1; r < maxr; r++) {
        int best = -1, bi = 0, bj = 0;
        for (int di = -r; di <= r; di++)
            for (int dj : {-r, r}) {
                int cand[2][2] = {{i + di, j + dj}, {i + dj, j + di}};
                for (auto& c : cand)
                    if (is_free(c[0], c[1])) {
                        int d = (c[0] - i) * (c[0] - i) + (c[1] - j) * (c[1] - j);
                        if (best < 0 || d < best) { best = d; bi = c[0]; bj = c[1]; }
                    }
            }
        if (best >= 0) { oi = bi; oj = bj; return true; }
    }
    return false;
}

bool NavGrid::find_path(double sx, double sz, double tx, double tz, int goal_r, std::vector<Pt>& out) const {
    out.clear();
    auto s0 = to_cell(sx, sz), g0 = to_cell(tx, tz);
    int si, sj, gi, gj;
    if (!nearest_free(s0.first, s0.second, si, sj) || !nearest_free(g0.first, g0.second, gi, gj)) return false;
    std::vector<float> cost((size_t)w * h, 1e30f);
    std::vector<int> came((size_t)w * h, -1);
    typedef std::pair<float, int> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    int start = sj * w + si;
    cost[start] = 0;
    q.push({0, start});
    static const int D[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    while (!q.empty()) {
        int cur = q.top().second;
        q.pop();
        int ci = cur % w, cj = cur / w;
        if (std::abs(ci - gi) <= goal_r && std::abs(cj - gj) <= goal_r) {
            std::vector<Pt> path;
            for (int c = cur; c >= 0; c = (c == start ? -1 : came[c])) path.push_back(to_world(c % w, c / w));
            std::reverse(path.begin(), path.end());
            smooth(path);
            out = path;
            return true;
        }
        for (auto& dd : D) {
            int ni = ci + dd[0], nj = cj + dd[1];
            if (!is_free(ni, nj)) continue;
            if (dd[0] && dd[1] && !(is_free(ci + dd[0], cj) && is_free(ci, cj + dd[1]))) continue;
            float nc = cost[cur] + ((dd[0] && dd[1]) ? 1.414f : 1.0f);
            if (!avoid_.empty()) {
                Pt wp = to_world(ni, nj);
                if (in_avoid(wp.x, wp.z)) nc += 40.0f;     // strongly prefer going around
            }
            int n = nj * w + ni;
            if (nc < cost[n]) {
                cost[n] = nc;
                came[n] = cur;
                float hh = (float)std::hypot(ni - gi, nj - gj);
                q.push({nc + hh, n});
            }
        }
    }
    return false;
}

std::vector<std::pair<int, int>> NavGrid::reachable(double sx, double sz) const {
    std::vector<std::pair<int, int>> out;
    auto s0 = to_cell(sx, sz);
    int si, sj;
    if (!nearest_free(s0.first, s0.second, si, sj)) return out;
    std::vector<uint8_t> seen((size_t)w * h, 0);
    std::queue<int> q;
    q.push(sj * w + si);
    seen[sj * w + si] = 1;
    while (!q.empty()) {
        int c = q.front(); q.pop();
        int ci = c % w, cj = c / w;
        out.push_back({ci, cj});
        static const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& d : D) {
            int ni = ci + d[0], nj = cj + d[1];
            if (is_free(ni, nj) && !seen[nj * w + ni]) { seen[nj * w + ni] = 1; q.push(nj * w + ni); }
        }
    }
    return out;
}

bool NavGrid::line_free(Pt a, Pt b) const {
    int steps = int(std::hypot(b.x - a.x, b.z - a.z) / 100) + 1;
    std::vector<int> fl{floor_};
    bool start_inside = in_avoid(a.x, a.z), end_inside = in_avoid(b.x, b.z);
    for (int k = 0; k <= steps; k++) {
        double t = double(k) / steps;
        double x = a.x + (b.x - a.x) * t, zz = a.z + (b.z - a.z) * t;
        if (blocking_at(cols_, fl, x, zz, radius_)) return false;
        // smoothing must not cut back through a place the path went around
        if (!avoid_.empty() && !start_inside && !end_inside && in_avoid(x, zz)) return false;
    }
    return true;
}

void NavGrid::smooth(std::vector<Pt>& path) const {
    if (path.size() <= 2) return;
    std::vector<Pt> out{path[0]};
    size_t i = 0;
    while (i < path.size() - 1) {
        size_t j = path.size() - 1;
        while (j > i + 1 && !line_free(path[i], path[j])) j--;
        out.push_back(path[j]);
        i = j;
    }
    path = out;
}

double NavGrid::clearance(const std::vector<Pt>& path) const {
    double best = 1e18;
    for (size_t i = 0; i + 1 < path.size(); i++) {
        Pt a = path[i], b = path[i + 1];
        int steps = int(std::hypot(b.x - a.x, b.z - a.z) / 100) + 1;
        for (int k = 0; k <= steps; k++) {
            double t = double(k) / steps, x = a.x + (b.x - a.x) * t, z = a.z + (b.z - a.z) * t;
            for (auto& c : avoid_) best = std::min(best, std::hypot(x - c.first.x, z - c.first.z));
        }
    }
    return best;
}

}  // namespace re2


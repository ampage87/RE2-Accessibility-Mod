#include "mod.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include "re2_addr.h"
#include "tables.inc"

namespace re2 {

static const char* kCategories[] = {"All", "Exits", "Items", "Objects", "Enemies"};
static const int kCategoryCount = 5;
static const int kLighter = 47;   // item id
// A door's name: doors whose animation climbs (4) or descends (5) a flight are stairs to the room.
// (door model 22 is a ladder: 200's to the 2nd floor, 405's to the hidden passage)
static std::string door_prefix(int anim, int tex) {
    if (anim == 4) return tex == 22 ? "Ladder up to " : "Stairs up to ";
    if (anim == 5) return tex == 22 ? "Ladder down to " : "Stairs down to ";
    return "Door to ";
}
static bool is_door_name(const std::string& n) {
    for (const char* p : {"Door to ", "Stairs up to ", "Stairs down to ", "Ladder up to ", "Ladder down to "})
        if (n.rfind(p, 0) == 0) return true;
    return false;
}
static std::string door_dest(const std::string& n) {
    for (const char* p : {"Door to ", "Stairs up to ", "Stairs down to ", "Ladder up to ", "Ladder down to "})
        if (n.rfind(p, 0) == 0) return n.substr(std::strlen(p));
    return n;
}

static std::string kind_label(int sce) {
    switch (sce) {
        case SCE_DOOR: return "Exit";
        case SCE_ITEM: return "Item";
        case SCE_MESSAGE: return "Examine";
        case SCE_EVENT: case SCE_AUTO: return "Interactive spot";
        case SCE_SAVE: return "Typewriter";
        case SCE_ITEMBOX: return "Item box";
        case SCE_FLAG_CHG: return "Item-use spot";
        case SCE_HIKIDASHI: return "Search spot";
        case SCE_WINDOWS: return "Window";
        case SCE_WATER: return "Water";
        default: return "Object";
    }
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Mod::Mod(Host& host) : h_(host), reader_(host.mem()) {
    load_data();
    unlimited_health_ = h_.get_setting("unlimited_health", 0) != 0;
    unlimited_ammo_ = h_.get_setting("unlimited_ammo", 0) != 0;
    run_enabled_ = h_.get_setting("auto_walk_run", 1) != 0;
    ad_enabled_ = h_.get_setting("audio_description", 1) != 0;
    ad_offset_ms_ = h_.get_setting("audio_description_offset_ms", 0);
    h_.log(std::string("assists: unlimited health ") + (unlimited_health_ ? "on" : "off") +
           ", unlimited ammo " + (unlimited_ammo_ ? "on" : "off"));
}

const char* Mod::hotkey_help() {
    return "Minus and equals: previous and next thing in the catalog. "
           "Left and right bracket: change catalog category. "
           "Backspace: directions to the selected thing. "
           "Backslash: walk there. Shift backslash: turn to face it. "
           "T: turn to the nearest enemy. Shift T: next enemy. "
           "Apostrophe: where am I. H: health. Y: time left on a countdown. I: inventory. E: enemies. Shift E: get away from the nearest enemy. Semicolon: walls around you. "
           "Comma: compass. Period: explore the room. "
           "Grave accent: repeat. F1: voice. Shift F1: speech output, your screen reader or SAPI. F2: ducking on or off. Shift F2: how low the game goes while the mod talks. F3 and F4: speech rate. Shift F3 and F4: volume, up to 200 percent. "
           "F5: unlimited health on or off. F6: unlimited ammo on or off. "
           "M: the map from where you are. Shift M: open the map of every room. "
           "F10: version. F11: save a diagnostic snapshot and a picture of the screen to the log folder. Slash: this help.";
}

void Mod::say(const std::string& t, bool interrupt) {
    if (t.empty()) return;
    last_said_ = t;
    // a typed page (Extreme Battle briefing, results) is still being read: queue behind it
    // instead of cutting it off (about 70 ms a character at the usual rates)
    if (interrupt && hold_until_ms_ && h_.ms() < hold_until_ms_) interrupt = false;
    h_.speak(t, interrupt);
}

void Mod::say_long(const std::string& t) {
    say(t, true);
    hold_until_ms_ = h_.ms() + 70ull * t.size();
}

// ---------------------------------------------------------------- data files
void Mod::load_data() {
    std::string dir = h_.mod_data_dir();
    {
        std::ifstream f(dir + "/room_names.txt");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq));
            for (auto& c : k) c = (char)std::toupper((unsigned char)c);
            room_names_[k] = trim(line.substr(eq + 1));
        }
    }
    {   // files.txt: "== FILE 00 ==" headers, pages separated by "--"
        std::ifstream f(dir + "/files.txt");
        std::string line, page;
        int cur = -1;
        auto flush = [&]() {
            if (cur >= 0 && !trim(page).empty()) files_[cur].push_back(trim(page));
            page.clear();
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("== FILE ", 0) == 0) {
                flush();
                cur = (int)std::strtol(line.c_str() + 8, nullptr, 16);
                if ((int)files_.size() <= cur) files_.resize(cur + 1);
                continue;
            }
            if (line == "--") { flush(); continue; }
            if (cur >= 0) page += (page.empty() ? "" : " ") + trim(line);
        }
        flush();
    }
    {   // menus.txt: key=Label 1|Label 2|...
        std::ifstream f(dir + "/menus.txt");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::vector<std::string> labels;
            std::stringstream ss(line.substr(eq + 1));
            std::string lab;
            while (std::getline(ss, lab, '|')) labels.push_back(trim(lab));
            menu_labels_[trim(line.substr(0, eq))] = labels;
        }
    }
    h_.log("data: " + std::to_string(room_names_.size()) + " room names, " + std::to_string(files_.size()) +
           " files, " + std::to_string(menu_labels_.size()) + " menu label sets");
}

std::string Mod::room_name(const std::string& code) const {
    auto it = room_names_.find(code);
    return it != room_names_.end() ? it->second : "room " + code;
}

std::string Mod::ui_label(const std::string& key, int index, const std::string& fallback) {
    auto it = menu_labels_.find(key);
    if (it != menu_labels_.end() && index >= 0 && index < (int)it->second.size() && !it->second[index].empty())
        return it->second[index];
    return fallback;
}

void Mod::ensure_room() {
    std::string key = s_.room_code + "/" + std::to_string(s_.character);
    if (key == room_key_) { sync_collisions(); return; }
    room_key_ = key;
    category_ = 0;   // each room starts with the whole catalog
    room_ = load_room(h_.game_data_dir(), s_.stage, s_.room, s_.character);
    obj_sig_.clear();
    obj_cols_.clear();
    clear_grids();
    h_.log("room " + s_.room_code + (room_.ok ? " loaded: " : " NOT FOUND: ") + std::to_string(room_.collisions.size()) +
           " collision shapes, " + std::to_string(room_.messages.size()) + " messages, " +
           std::to_string(room_.script_doors.size()) + " script doors");
}

// Scripts switch collision entries on and off as the story moves (sca_id_set, bio2.exe
// 0x4E6DF0 keeps the low 7 bits and takes the rest from the script): a fire, a fallen
// shelf, a door blocked by a car. The live table is the room image's SCA chunk
// ([[0x98861C] + 0x20]); entry i's flag word is at +0x18 + 16 i. Copy changed flags into
// the room so routes and wall checks follow the game.
void Mod::sync_collisions() {
    if (!room_.ok || room_.collisions.empty()) return;
    MemoryReader& mem = h_.mem();
    uint32_t img = mem.u32(addr::P_ROOM);
    if (!img) return;
    uint32_t sca = mem.u32(img + 0x20);
    if (!sca) return;
    bool changed = false;
    for (auto& c : room_.collisions) {
        uint8_t e[16];
        if (!mem.read(sca + 0x10 + 16 * c.index, e, sizeof e)) return;
        int16_t x, z; uint16_t f;
        std::memcpy(&x, e, 2); std::memcpy(&z, e + 2, 2); std::memcpy(&f, e + 8, 2);
        if (x != c.x || z != c.z) return;           // the image is another room's (door in progress)
        if (f != c.raw_flag) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "[SCA] entry %d flag %04X -> %04X", c.index, c.raw_flag, f);
            h_.log(buf);
            c.raw_flag = f;
            changed = true;
            sca_changed_ms_ = h_.ms();
        }
    }
    // room objects with a collision box (the library's sliding shelves, the archive room's
    // stepladder, the statues) are walls too, and move
    std::vector<Collision> oc;
    std::string sig;
    for (auto& ob : s_.objects) {
        if (!ob.solid) continue;
        Collision c;
        c.index = 900 + ob.index;
        c.shape = 0;
        c.x = (int)std::lround(ob.cx - ob.hx); c.z = (int)std::lround(ob.cz - ob.hz);
        c.w = (int)std::lround(2 * ob.hx); c.d = (int)std::lround(2 * ob.hz);
        for (int f = 0; f < 16; f++) {
            double top = -(f + 1) * 1800.0, bottom = -f * 1800.0;           // floor band f (y up is negative)
            if (std::min(bottom, ob.ybottom) - std::max(top, ob.ytop) > 100) c.floor_mask |= 1u << f;
        }
        if (!c.floor_mask) c.floor_mask = 1u << ob.floor;
        c.raw_flag = PLAYER_BLOCK_BIT;
        oc.push_back(c);
        char b[64];
        std::snprintf(b, sizeof b, "%d:%d:%d:%d:%d|", ob.index, c.x / 50, c.z / 50, c.w / 50, c.d / 50);
        sig += b;
    }
    if (sig != obj_sig_) {
        // logged once things settle (a sliding shelf changes this every frame while it moves)
        obj_log_pending_ = !obj_sig_.empty() || !sig.empty();
        obj_moved_ms_ = h_.ms();
        obj_sig_ = sig;
        obj_cols_ = oc;
        changed = true;
    }
    if (obj_log_pending_ && h_.ms() - obj_moved_ms_ > 1000) {
        obj_log_pending_ = false;
        h_.log("[OBJ] collision boxes " + (obj_sig_.empty() ? std::string("none") : obj_sig_));
    }
    if (changed) clear_grids();
}

// the walls a script can switch, as they stand (not automatic doors, which open as you come)
std::string Mod::sca_signature() {
    nav_collisions();     // finds the automatic doors
    std::string sig;
    for (auto& c : room_.collisions) {
        if (auto_door_cols_.count(c.index)) continue;
        sig += std::to_string(c.index) + ":" + std::to_string(c.blocks_player() ? 1 : 0) + std::to_string(c.walkable() ? 1 : 0) + "|";
    }
    return sig;
}

// the room's collision shapes plus the objects' boxes, for routes
const std::vector<Collision>& Mod::nav_collisions() {
    nav_cols_ = room_.collisions;
    // automatic doors (615's sliding glass doors): a wall a script switches with sca_id_set and
    // a flag zone the player walks into beside it (that opens it) - plan through it; walking
    // up to it opens it
    if (sca_room_ != room_key_) {
        sca_room_ = room_key_;
        auto_door_cols_.clear();
        sca_ids_.clear();
        const auto& b = room_.raw;
        auto scan = [&](size_t st, size_t en) {
            for (size_t pc = st; pc < en && pc < b.size();) {
                uint8_t op = b[pc];
                int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
                if (sz <= 0 || pc + sz > b.size()) break;
                if (op == 0x37 && sz >= 2) sca_ids_.insert(b[pc + 1]);
                pc += sz;
            }
        };
        for (auto& pr : room_.main_procs) scan(pr.first, pr.second);
        if (room_.init_end > room_.init_start) scan(room_.init_start, room_.init_end);
    }
    for (auto& c : nav_cols_) {
        if (!sca_ids_.count(c.index) || !c.blocks_player()) continue;
        for (auto& a : s_.aots) {
            // a flag zone (615) or a plain trigger zone (703's train car doors)
            // (not a zone that fires when you face something: 616's closed shutter has one, sat 0x21)
            if ((a.sce != SCE_FLAG_CHG && a.sce != SCE_NORMAL) || !a.player_triggers() || a.manual() || (a.sat & 0x20) || a.quad.empty()) continue;
            double mnx = 1e9, mxx = -1e9, mnz = 1e9, mxz = -1e9;
            for (auto& q : a.quad) { mnx = std::min(mnx, q.x); mxx = std::max(mxx, q.x); mnz = std::min(mnz, q.z); mxz = std::max(mxz, q.z); }
            const double m = 800;
            if (mxx + m < c.x || mnx - m > c.x + c.w || mxz + m < c.z || mnz - m > c.z + c.d) continue;
            c.raw_flag &= ~PLAYER_BLOCK_BIT;
            auto_door_cols_.insert(c.index);
            break;
        }
    }
    nav_cols_.insert(nav_cols_.end(), obj_cols_.begin(), obj_cols_.end());
    return nav_cols_;
}

void Mod::clear_grids() {
    for (auto& g : grids_) delete g.second;
    grids_.clear();
    stair_ends_.clear();
    stair_ends_done_ = false;
}

// Navigation grids per floor. The normal grid keeps the player's whole body clear of walls;
// the tight grid (a smaller body) finds the narrow gaps the game lets the player squeeze
// through by sliding along walls, used only when the normal grid finds no way.
NavGrid& Mod::grid(int floor, int tight) {
    double body = s_.body_r > 0 ? s_.body_r : PLAYER_RADIUS;
    if (std::fabs(body - grid_body_) > 1) { clear_grids(); grid_body_ = body; }
    // standing somewhere the support test does not recognise as floor: plan without it
    bool support = floor != s_.floor || supported_at(room_.collisions, floor, s_.x, s_.z);
    int key = floor * 8 + tight * 2 + (support ? 0 : 1);
    auto it = grids_.find(key);
    if (it == grids_.end()) {
        // 0: the body and a margin; 1: just the body (the game's own box test, bio2.exe 0x4E0A00:
        // a corridor exactly as wide as the body lets you through); 2: a smaller body for the
        // gaps the game lets you slide through
        double r = tight == 2 ? std::max(200.0, body - 130) : tight == 1 ? body : body + 20;
        it = grids_.emplace(key, new NavGrid(nav_collisions(), floor, 100, r, support)).first;
    }
    return *it->second;
}

// ---------------------------------------------------------------- frame
void Mod::frame() {
    p_ = s_;
    s_ = reader_.read();
    frame_movie();
    frame_intro_probe();
    frame_text_scan();
    if (!s_.valid) return;
    if (!have_prev_) { p_ = s_; have_prev_ = true; }
    apply_assists();
    log_ui_probe();
    frame_message();
    if (frame_terminal() || frame_options() || frame_pc_menu()) {
        ui_mode_ = 5;
        prev_ui_mode_ = 5;
    } else {
        if (prev_ui_mode_ == 5) prev_ui_mode_ = s_.in_game ? 2 : -1;   // "Back to the game" after a save screen
        frame_ui();
    }
    // the game's map screen closed: the mod's map that opened with it closes too
    if (game_map_open_ && !(ui_mode_ == 2 && (s_.status.type == 3 || s_.status.type == 4))) {
        game_map_open_ = false;
        map_mode_ = false;
        cancel_frames_ = 0;
    }
    if (travel_after_map_ && s_.in_game && ui_mode_ == 0 && !game_map_open_) {
        travel_after_map_ = false;
        if (!travel_to_.empty()) { ensure_room(); travel_step(true); }
    }
    if (ui_mode_ == 3 || !s_.in_game) game_inv_valid_ = false;   // the item box moves things in and out
    if (s_.in_game && ui_mode_ == 0) {
        ensure_room();
        frame_game();
    } else {
        if (walking_ || face_dir_ >= 0) stop_walk();
        if (room_pending_) room_quiet_ = true;   // a menu opened before the room summary: skip it
    }
    frame_countdown();
    frame_results();
    frame_ex_char();
    frame_ex_stage();
    frame_ex_text();
    if (s_.in_game) frame_puzzle();
    if (s_.in_game) frame_travel();
    if (s_.in_game) frame_via();
    // the story hands control to another character (Sherry in Claire's scenario, Ada in Leon's)
    {
        int pl = h_.mem().u8(addr::PL_ID);
        if (s_.in_game && p_.in_game && last_pl_id_ >= 0 && pl != last_pl_id_) {
            static const char* kWho[] = {"Leon", "Claire", "Leon", "Claire", "Leon", "Claire", "Hunk", "Tofu", "Ada", "Sherry"};
            if (pl >= 0 && pl < 10) say(std::string("You are now playing as ") + kWho[pl] + ".");
        }
        if (s_.in_game) last_pl_id_ = pl;
        // Sherry's part of Claire's story keeps Claire's PL_ID; the player's model is Sherry's
        // (15, seen in the South Control Room)
        int model = h_.mem().u8(addr::PLAYER + addr::W_ID);
        if (s_.in_game && p_.in_game && last_model_ >= 0 && model != last_model_ && ui_mode_ == 0) {
            if (model == 15) say("You are now playing as Sherry.");
            else if (last_model_ == 15 && pl >= 0 && pl < 6) say(std::string("You are now playing as ") + (pl & 1 ? "Claire" : "Leon") + ".");
        }
        if (s_.in_game && ui_mode_ == 0) last_model_ = model;
        // a partner joins or leaves (script flag 1.3: Sherry with Claire, Ada with Leon)
        if (s_.in_game && ui_mode_ == 0 && pl < 6) {
            int with = reader_.flag(1, 3) ? 1 : 0;
            if (partner_ >= 0 && with != partner_ && partner_room_ == (s_.stage * 256 + s_.room)) {
                const char* who = (pl & 1) ? "Sherry" : "Ada";
                say(std::string(who) + (with ? " is with you now." : " is no longer with you."));
            }
            partner_ = with;
            partner_room_ = (s_.stage * 256 + s_.room);
        }
    }
    uint32_t buttons = (s_.in_game && ui_mode_ == 0) ? steer() : 0;
    // a journey picked on the map answers the door's own question ("Will you go down the
    // ladder?") with Yes; nothing else is answered for the player
    if (confirm_frames_ > 0) {
        if (s_.message.active && s_.message.routine == 4 && s_.message.cursor == 0) { buttons |= BTN_ACTION; confirm_frames_--; }
        else if (!s_.message.active) confirm_frames_ = 0;
    }
    if (page_frames_ > 0) {
        if (s_.message.active && s_.message.routine == 2) { buttons |= BTN_ACTION; page_frames_--; }
        else page_frames_ = 0;
    }
    if (press_action_frames_ > 0 && (s_.message.active || ui_mode_ != 0)) press_action_frames_ = 0;
    if (press_action_frames_ > 0 && s_.in_game && ui_mode_ == 0 && !walking_ && face_dir_ < 0 && !s_.stop_flg &&
        !event_running()) {
        press_action_frames_--;
        buttons |= BTN_ACTION;
    }
    if (cancel_frames_ > 0 && game_map_open_) { buttons |= BTN_CANCEL; cancel_frames_--; }
    h_.set_buttons(buttons);
}

// Assist options: keep health full, keep the equipped weapon loaded. Damage still plays the
// "hurt" cue so the player knows they are being attacked.
void Mod::apply_assists() {
    MemoryReader& m = h_.mem();
    // unlimited health also turns the game's instant kills into a hit that leaves 1 point
    // (the refill below then tops it up); put back when the assist is off
    if (unlimited_health_ != kill_patched_) {
        static const uint8_t kPat[7] = {0x66, 0xC7, 0x05, 0x46, 0xA0, 0x98, 0x00};
        int done = 0;
        for (uint32_t site : addr::KILL_SITES) {
            std::vector<uint8_t> b = m.bytes(site, 9);
            if (b.size() != 9 || std::memcmp(b.data(), kPat, 7) != 0) continue;
            uint16_t v = unlimited_health_ ? 1 : 0xFFFF;
            if (m.write(site + 7, &v, 2)) done++;
        }
        // damage clamp
        static const uint8_t kSub[6] = {0x66, 0x2B, 0xF1, 0x83, 0xF8, 0x01};
        static const uint8_t kCave[13] = {0x66, 0x2B, 0xF1,        // sub si,cx
                                          0x7F, 0x04,              // jg  +4
                                          0x66, 0xBE, 0x01, 0x00,  // mov si,1
                                          0x83, 0xF8, 0x01,        // cmp eax,1
                                          0xC3};                   // ret
        int32_t rel = (int32_t)addr::DMG_CAVE - (int32_t)(addr::DMG_SUB + 5);
        uint8_t kCall[6] = {0xE8, (uint8_t)rel, (uint8_t)(rel >> 8), (uint8_t)(rel >> 16), (uint8_t)(rel >> 24), 0x90};
        std::vector<uint8_t> site = m.bytes(addr::DMG_SUB, 6), cave = m.bytes(addr::DMG_CAVE, 15);
        if (site.size() == 6 && cave.size() == 15) {
            bool site_orig = !std::memcmp(site.data(), kSub, 6), site_ours = !std::memcmp(site.data(), kCall, 6);
            bool cave_free = true;
            for (int i = 0; i < 15; i++) if (cave[i] != 0x90) cave_free = false;
            bool cave_ours = !std::memcmp(cave.data(), kCave, 13);
            if (unlimited_health_ && site_orig && (cave_free || cave_ours)) {
                if (m.write(addr::DMG_CAVE, kCave, 13) && m.write(addr::DMG_SUB, kCall, 6)) done++;
            } else if (!unlimited_health_ && site_ours) {
                static const uint8_t nops[13] = {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
                if (m.write(addr::DMG_SUB, kSub, 6)) { m.write(addr::DMG_CAVE, nops, 13); done++; }
            }
        }
        // cockroach swarm kill: jbe -> jmp
        {
            std::vector<uint8_t> j = m.bytes(addr::ROACH_KILL_JBE, 2);
            if (j.size() == 2 && j[1] == 0x33 && (j[0] == 0x76 || j[0] == 0xEB)) {
                uint8_t want = unlimited_health_ ? 0xEB : 0x76;
                if (j[0] != want && m.write(addr::ROACH_KILL_JBE, &want, 1)) done++;
            }
        }
        // alligator swallow: 0x401 -> 0x101
        {
            std::vector<uint8_t> b = m.bytes(addr::ALLIGATOR_SWALLOW, 7);
            static const uint8_t kHead[3] = {0xC7, 0x46, 0x04};
            if (b.size() == 7 && !std::memcmp(b.data(), kHead, 3) && b[3] == 0x01 && (b[4] == 0x04 || b[4] == 0x01) && !b[5] && !b[6]) {
                uint8_t want = unlimited_health_ ? 0x01 : 0x04;
                if (b[4] != want && m.write(addr::ALLIGATOR_SWALLOW + 4, &want, 1)) done++;
            }
        }
        kill_patched_ = unlimited_health_;
        if (done) h_.log(std::string("instant kills ") + (unlimited_health_ ? "softened" : "restored") + " at " + std::to_string(done) + " places (with damage clamp, cockroach swarm and alligator swallow)");
    }
    if (!s_.in_game) { full_ammo_.clear(); return; }
    // player state diagnostics: routine bytes (+4..+7), the 'held/dead' byte (+0x1D3), life,
    // cockroach counters and game over; logged when the routine or the held byte changes
    {
        uint32_t rt = m.u32(addr::PLAYER + 4);
        uint8_t held = m.u8(addr::PLAYER + 0x1D3);
        char sig[48];
        std::snprintf(sig, sizeof sig, "%08X/%02X%s", rt, held, (s_.system_flg & addr::SYS_GAME_OVER) ? " GO" : "");
        if (pl_diag_sig_ != sig) {
            pl_diag_sig_ = sig;
            char line[160];
            std::snprintf(line, sizeof line, "[PL] routine %08X held %02X life %d/%d roaches %u/%u sys %08X",
                          rt, held, s_.hp, s_.max_hp, m.u32(0x689594), m.u32(0x689590), s_.system_flg);
            h_.log(line);
        }
    }
    // unlimited health also keeps poison away
    if (unlimited_health_ && s_.poisoned) {
        uint16_t z16 = 0; uint8_t z8 = 0;
        if (m.write(addr::POISON_TIMER, &z16, 2) && m.write(addr::POISON_TICK, &z8, 1)) {
            s_.poisoned = false;
            h_.log("poison cleared (unlimited health)");
        }
    }
    if (unlimited_health_ && s_.max_hp > 0 && s_.hp < s_.max_hp && (s_.hp > 0 || !(s_.system_flg & addr::SYS_GAME_OVER))) {
        int16_t full = (int16_t)s_.max_hp;
        if (m.write(addr::PLAYER + addr::W_LIFE, &full, 2)) {
            h_.cue("hurt");
            s_.hp = s_.max_hp;
        }
    }
    if (unlimited_ammo_ && s_.equipped > 1) {
        for (size_t i = 0; i < s_.inventory.size(); i++) {
            InvSlot& it = s_.inventory[i];
            if (it.id != s_.equipped) continue;
            int& full = full_ammo_[it.id];
            if (it.count > full) full = it.count;            // first sight, reload, or more ammo combined in
            else if (it.count < full) {
                uint8_t v = (uint8_t)full;
                if (m.write(addr::ITEM_WORK + (uint32_t)i * 4 + 1, &v, 1)) it.count = full;
            }
            break;
        }
    }
}

void Mod::toggle_assist(bool health) {
    bool& on = health ? unlimited_health_ : unlimited_ammo_;
    on = !on;
    h_.put_setting(health ? "unlimited_health" : "unlimited_ammo", on ? 1 : 0);
    if (!health) full_ammo_.clear();
    std::string name = health ? "Unlimited health" : "Unlimited ammo";
    std::string extra = (!health && on) ? " Reload once so the weapon is full; it then stays full." : "";
    say(name + (on ? " on." : " off.") + extra, true);
}

// Spoken name of the highlighted title-menu choice (see re2_addr.h for how the screens work).
std::string Mod::title_label(const TitleScreen& t, std::string& heading) {
    static const char* kMain[] = {"Load game", "Original mode", "Arrange mode", "Special", "Option"};
    bool claire = (s_.status_flg & 0x80000000u) != 0;
    switch (t.screen) {
        case 0:
            heading = "Main menu.";
            return t.cursor < 5 ? kMain[t.cursor] : "";
        case 1: case 2:
            heading = std::string(t.screen == 1 ? "Original mode." : "Arrange mode.") + " Choose difficulty.";
            // without Hard: Normal/Easy (left or right) above Exit. With Hard unlocked
            // (TI_HARD_OK, bio2.exe 0x504573): Normal and Easy on the top row, Hard and Exit
            // below them; down from Normal goes to Hard, down from Easy to Exit
            if (t.cursor == 0) return std::string(t.option == 1 ? "Easy" : "Normal") + ", left or right to change";
            if (t.cursor == 1) return "Exit";
            if (t.cursor == 2) return t.hard_ok ? "Hard, right for Exit" : "Hard";
            return "";
        case 5: case 6: case 7:
            heading = "Choose character.";
            if (t.cursor == 0) return std::string(claire ? "Claire" : "Leon") + ", left or right to change";
            if (t.cursor == 1) return "Exit";
            return "";
        case 3: {
            heading = "Special.";
            // the highlighted word pulses brighter than the others
            static const std::pair<int, const char*> kWords[] = {
                {9, "4th Survivor"}, {10, "To-fu"}, {11, "Extreme battle"}, {12, "Exit"}, {13, "Gallery"},
                {39, "Gallery"}, {27, "Result"}, {33, "Level 1"}, {34, "Level 2"}, {35, "Level 3"},
                {36, "Exit"}, {14, "Game start"}, {15, "Exit"}, {26, "Game start"}, {28, "Exit"}};
            int best = 0x20; const char* w = nullptr;
            for (auto& kv : kWords)
                if (t.sprite_level[kv.first] > best && t.sprite_level[kv.first] <= 0x60) { best = t.sprite_level[kv.first]; w = kv.second; }
            return w ? w : "";
        }
        default:
            return "";
    }
}

// The 4th Survivor / To-fu title (title routine 4, bio2.exe 0x505670; routine 3, 0x505460, loads
// its picture: common\data\title_o.adt, HUNK, or title_o2.adt, To-fu, when System_flg bit 0
// is set). The game comes here when the mode is chosen from Special and again after the mode's
// ending. Sub-routine 0 shows the picture and waits for any key; 1 (0x5057F0) takes input:
// up / down move the cursor (+7) between 0, game start (sets System_flg 0x20000 and starts
// the mode again), and 1, exit (clears System_flg bits 0 and 3 and goes back to the main menu).
std::string Mod::ex_time_text(uint32_t secs) {
    if (secs >= 360000) return "over 99 hours";
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    std::string t;
    if (h) t = std::to_string(h) + (h == 1 ? " hour " : " hours ");
    t += std::to_string(m) + (m == 1 ? " minute " : " minutes ") + std::to_string(s) + (s == 1 ? " second" : " seconds");
    return t;
}

static const char* ex_char_name(int c) {
    static const char* kNames[] = {"Leon", "Claire", "Ada", "Chris"};
    return c >= 0 && c < 4 ? kNames[c] : "unknown";
}

// Extreme Battle title (routine 6) and its menus; see re2_addr.h.
void Mod::frame_ex_menu(const TitleScreen& t) {
    MemoryReader& m = h_.mem();
    if (t.main_rtn != 6) { ex_menu_sig_.clear(); return; }
    std::string head, label;
    int levels = m.u8(addr::EX_LEVELS);
    if (t.sub_rtn0 == 0) {
        head = "Extreme Battle title.";
        label = "Press any key.";
    } else if (t.sub_rtn0 == 1 && t.screen == 0 && t.step == 1) {
        static const char* kMenu[] = {"Load game", "Game start", "Result, your records", "Exit to the main menu"};
        head = "Extreme Battle.";
        label = t.cursor < 4 ? kMenu[t.cursor] : "";
        if (t.cursor == 1 && levels == 0) label += ", Level 1";
    } else if (t.sub_rtn0 == 1 && t.screen == 0 && t.step == 2 && m.u8(addr::TI_SUB4) == 2) {
        head = "Choose level.";
        if (t.cursor == 0) {
            label = "Level " + std::to_string(m.u8(addr::EX_LEVEL_SEL) + 1);
            if (levels > 0) label += ", left or right to change, " + std::to_string(levels + 1) + " levels open";
        } else {
            label = "Exit";
        }
    }
    if (label.empty()) return;
    std::string sig = head + "|" + label;
    if (sig == ex_menu_sig_) return;
    bool new_head = ex_menu_sig_.compare(0, head.size() + 1, head + "|") != 0;
    say(new_head ? head + " " + label : label, true);
    ex_menu_sig_ = sig;
}

// The Extreme Battle records (Result on its menu) and the result after a run: title routine 7,
// task 0x4DD4F0 (bio2.exe 0x4DDB40 records, 0x4DDFF0 result: level, character, three stage
// times and their total against the best for that level; "New record", "Try next level",
// "You are great" after Level 3).
void Mod::frame_ex_records(const TitleScreen& t) {
    MemoryReader& m = h_.mem();
    int st = m.u8(addr::EX_RES_STATE), mode = m.u8(addr::EX_RES_MODE);
    bool up = !s_.in_game && t.main_rtn == 7 && st == 2 && (mode == 1 || mode == 2);
    if (!up) { ex_rec_said_ = 0; return; }
    if (ex_rec_said_ == mode) return;
    ex_rec_said_ = mode;
    auto rec_total = [&](int lv) -> uint32_t {
        uint32_t a = addr::EX_RECORDS + lv * 16;
        return m.u32(a + 4) + m.u32(a + 8) + m.u32(a + 12);
    };
    std::string text;
    if (mode == 1) {
        text = "Result, your records.";
        int levels = m.u8(addr::EX_LEVELS);
        for (int lv = 0; lv <= levels && lv < 3; lv++) {
            uint32_t a = addr::EX_RECORDS + lv * 16;
            text += " Level " + std::to_string(lv + 1) + ": ";
            if (m.u16(a + 2)) text += std::string(ex_char_name(m.u16(a))) + ", " + ex_time_text(rec_total(lv)) + ".";
            else text += "no record.";
        }
    } else {
        int lv = m.u8(addr::EX_LEVEL);            // 1..3
        uint32_t t1 = m.u32(addr::EX_TIMES), t2 = m.u32(addr::EX_TIMES + 4), t3 = m.u32(addr::EX_TIMES + 8);
        uint32_t total = t1 + t2 + t3;
        text = "Result. Level " + std::to_string(lv) + ", " + ex_char_name(m.u8(addr::EX_CHAR)) + ". Your time: " + ex_time_text(total) + ".";
        text += " Stages: " + ex_time_text(t1) + "; " + ex_time_text(t2) + "; " + ex_time_text(t3) + ".";
        int li = lv >= 1 && lv <= 3 ? lv - 1 : 0;
        uint32_t a = addr::EX_RECORDS + li * 16;
        if (m.u16(a + 2)) {
            uint32_t best = rec_total(li);
            text += " Best: " + std::string(ex_char_name(m.u16(a))) + ", " + ex_time_text(best) + ".";
            if (total <= best) text += " New record!";
        } else {
            text += " New record!";
        }
        if (lv == 3) text += " You are great!";
        else if (m.u8(addr::EX_LEVELS) < lv) text += " Try the next level!";
    }
    h_.log("[EXTREME] " + text);
    say_long(text);
}

// Extreme Battle character select (task 0x4EA320, runs before the level loads).
void Mod::frame_ex_char() {
    MemoryReader& m = h_.mem();
    if (!(s_.system_flg & 0x1000000) || m.u8(addr::EX_CHAR_STATE) != 2) { ex_char_sel_ = -1; return; }
    static const char* kSlot[] = {"Ada", "Leon", "Claire", "Chris"};
    int sel = (int8_t)m.u8(addr::EX_CHAR_SEL);
    if (sel < 0 || sel > 3 || sel == ex_char_sel_) return;
    std::string name = kSlot[sel];
    if (ex_char_sel_ < 0) {
        int lock = m.u8(addr::EX_CHAR_LOCK);
        int open = 4 - ((lock & 2) ? 1 : 0) - ((lock & 4) ? 1 : 0);
        say("Choose character. " + name + ", left or right to change, " + std::to_string(open) + " characters open.", true);
    } else {
        say(name, true);
    }
    ex_char_sel_ = sel;
}

// A text from the game's own text tables in the exe (0xFE ends it).
std::string Mod::exe_text(uint32_t a) {
    MemoryReader& m = h_.mem();
    std::vector<uint8_t> b;
    for (int i = 0; i < 300; i++) {
        uint8_t c = m.u8(a + i);
        b.push_back(c);
        if (c == 0xFE) break;
    }
    return decode_text(b.data(), b.size(), 0);
}

// Extreme Battle stage clear: the stage's time is written to EX_TIMES (bio2.exe 0x4CB332..0x4CB388).
void Mod::frame_ex_stage() {
    MemoryReader& m = h_.mem();
    bool ex = (s_.system_flg & 0x1000000) != 0;
    for (int i = 0; i < 3; i++) {
        uint32_t v = m.u32(addr::EX_TIMES + i * 4);
        if (ex && ex_times_ok_ && v && v != ex_times_[i] && s_.in_game) {
            std::string obj = exe_text((m.u8(addr::EX_STAGE_FLAGS) & 0x1F) ? addr::EX_OBJ_BOMBS : addr::EX_OBJ_PRECINCT);
            std::string t = "Stage clear. Clear time: " + ex_time_text(v) + ".";
            if (!obj.empty()) t += " " + obj;
            t += " Press any button.";
            h_.log("[EXTREME] " + t);
            say(t);
        }
        ex_times_[i] = v;
    }
    ex_times_ok_ = true;
}

// Extreme Battle's typed pages: the briefing at the start of a run and the "Mission complete"
// text at the end. Each page is read when its typing starts (state goes to 1).
void Mod::frame_ex_text() {
    MemoryReader& m = h_.mem();
    bool ex = (s_.system_flg & 0x1000000) != 0;
    struct Src { uint32_t tab, base, state; int lines; int* prev; };
    Src srcs[] = {{addr::EX_INTRO_TAB, addr::EX_INTRO_BASE, addr::EX_INTRO_STATE, 10, &ex_intro_prev_},
                  {addr::EX_END_TAB, addr::EX_END_BASE, addr::EX_END_STATE, 11, &ex_end_prev_}};
    for (auto& src : srcs) {
        int st = (int)m.u32(src.state);
        if (ex && st == 1 && *src.prev != 1 && *src.prev != -2) {
            int base = m.u8(src.base);
            std::string page;
            for (int i = 0; i < src.lines; i++) {
                uint32_t p = m.u32(src.tab + (base + i) * 4);
                if (!p) continue;
                std::string line = exe_text(p);
                if (line.empty()) { if (!page.empty() && page.back() != '\n') page += "\n"; continue; }
                if (!page.empty() && page.back() != '\n') page += " ";
                page += line;
            }
            // lines are wrapped mid-sentence; blank lines end a paragraph
            for (auto& c : page) if (c == '\n') c = ' ';
            if (!page.empty()) {
                h_.log("[EXTREME] page: " + page);
                hold_until_ms_ = 0;
                say_long(page);
            }
        }
        *src.prev = st;
    }
}

void Mod::frame_ex_title(const TitleScreen& t) {
    frame_ex_menu(t);
    frame_ex_records(t);
    if (t.main_rtn != 4) { ex_title_sub_ = -1; ex_title_cur_ = -1; return; }
    bool tofu = (s_.system_flg & 0x1) != 0;
    std::string mode = tofu ? "To-fu" : "4th Survivor";
    if (t.sub_rtn0 == 0 && ex_title_sub_ != 0) {
        say(mode + " title. Press any key.", true);
    } else if (t.sub_rtn0 == 1) {
        std::string label = t.cursor == 0 ? "Game start, plays " + mode + " again" : t.cursor == 1 ? "Exit to the main menu" : "";
        if (ex_title_sub_ != 1) say(mode + ". " + label + ". Up and down to choose.", true);
        else if (t.cursor != ex_title_cur_ && !label.empty()) say(label, true);
        ex_title_cur_ = t.cursor;
    }
    ex_title_sub_ = t.sub_rtn0;
}

// Which screen is up. 0 = playing, 1 = title/menus before a game, 2 = status screen,
// 3 = item box, 4 = other status-screen modes.
void Mod::frame_ui() {
    int mode = 0;
    if (!s_.in_game) mode = 1;
    else if (s_.status.main_rtn == 1) mode = s_.status.type == 1 ? 3 : 2;
    else if (s_.system_flg & 0x4000) {
        // a status-type screen opening or closing, or a door. The bit can also stay set while
        // the player walks freely (Leon B, the main hall after the fountain's Spade key): after
        // 3 seconds with no stop flags and no message, that is play, not a screen
        if (!sys4000_ms_) sys4000_ms_ = h_.ms();
        bool stuck = h_.ms() - sys4000_ms_ > 3000 && !s_.stop_flg && !s_.message.active;
        if (stuck && !sys4000_logged_) { sys4000_logged_ = true; h_.log("[UI] System_flg 0x4000 stays set during play; treating it as play"); }
        if (!stuck) mode = 4;
    }
    if (!(s_.system_flg & 0x4000)) { sys4000_ms_ = 0; sys4000_logged_ = false; }
    ui_mode_ = mode;
    bool changed = mode != prev_ui_mode_;
    if (mode == 1) {
        const TitleScreen& t = s_.title;
        if (changed) {
            // the 4th Survivor / To-fu title says its own name
            if (s_.title.main_rtn < 3) say("Title screen.", true);
            title_screen_ = -1; title_label_.clear(); title_start_said_ = false;
        }
        // Each title screen runs in steps (TI_STEP): 0 set-up, then input. The main menu takes
        // input in step 1; the character, difficulty and special screens fade in during step 1
        // and take input in step 2 (bio2.exe 0x504240 / 0x5044D0 / 0x504900). Choosing a
        // character switches straight to the difficulty screen (screen - 4) still in step 2.
        frame_ex_title(t);
        if (t.main_rtn == 2 && t.sub_rtn0 == 1 && (t.step == 1 || t.step == 2)) {
            std::string heading;
            std::string label = title_label(t, heading);
            bool new_screen = t.screen != title_screen_;
            if (!label.empty() && (new_screen || label != title_label_)) {
                say(new_screen && !heading.empty() ? heading + " " + label : label, true);
                title_label_ = label;
            }
            if (!label.empty()) title_screen_ = t.screen;
        }
        // screen 4 = leaving the title; TI_SEL_MODE 1 = a new game was chosen (0 load, 2 options,
        // which have their own announcements). The fade to the game takes a few seconds.
        if (t.screen == 4 && t.sel_mode == 1 && !title_start_said_) {
            title_start_said_ = true;
            say("Starting the game.", true);
        }
        ui_sig_.clear();
    } else if (mode == 2 || mode == 3) {
        frame_status(changed);
    }
    // only after the status screen or the item box (mode 4 is also the door animation)
    if (mode == 0 && (prev_ui_mode_ == 2 || prev_ui_mode_ == 3) && s_.in_game && status_type_ != 2)
        say("Back to the game.", true);
    // typewriter save / load slot cursor
    if (s_.save_cursor != p_.save_cursor && (s_.system_flg & (addr::SYS_SAVE_GAME | addr::SYS_LOAD_GAME)))
        say("Slot " + std::to_string(s_.save_cursor + 1) + ".", true);
    prev_ui_mode_ = mode;
}

// Computer terminals (the police station's door-lock computer and others). The game types
// text into a 12 x 30 character grid; the player types a name on the keyboard. Each line is
// read once it stops changing; letters the player types are echoed as they appear.
bool Mod::frame_terminal() {
    MemoryReader& m = h_.mem();
    uint32_t work = m.u32(addr::PC_WORK_PTR);
    int state = m.u8(addr::PC_STATE);
    if (!s_.in_game || !work || state == 0 || s_.stop_flg == 0) {
        if (term_active_) { term_active_ = false; term_lines_.clear(); term_said_.clear(); }
        kb_active_ = false; kb_col_ = kb_row_ = kb_blink_ = -1;
        return false;
    }
    std::vector<uint8_t> grid = m.bytes(work + addr::PC_GRID, addr::PC_ROWS * addr::PC_COLS);
    if (grid.size() != (size_t)(addr::PC_ROWS * addr::PC_COLS)) return false;
    if (!term_active_) {
        term_active_ = true;
        term_lines_.assign(addr::PC_ROWS, "");
        term_said_.assign(addr::PC_ROWS, "");
        term_changed_ms_.assign(addr::PC_ROWS, 0);
        term_msg_ = 0;
        say("Computer.", true);
        h_.log("terminal open, state " + std::to_string(state));
    }
    uint64_t now = h_.ms();
    for (int r = 0; r < addr::PC_ROWS; r++) {
        std::string line;
        for (int c = 0; c < addr::PC_COLS; c++) {
            uint8_t ch = grid[r * addr::PC_COLS + c];
            line += (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
        }
        size_t e = line.find_last_not_of(' ');
        line = e == std::string::npos ? "" : line.substr(0, e + 1);
        if (line != term_lines_[r]) {
            // a single new letter after the ">" prompt is the player typing: echo it at once
            const std::string& old = term_lines_[r];
            if (line.size() == old.size() + 1 && line.compare(0, old.size(), old) == 0 && old.find('>') != std::string::npos &&
                term_said_[r] == old) {
                say(std::string(1, line.back()), true);
                term_said_[r] = line;
            }
            term_lines_[r] = line;
            term_changed_ms_[r] = now;
        } else if (line != term_said_[r] && now - term_changed_ms_[r] > 350) {
            // the game finished typing this line: say what is new on it
            std::string t = line;
            if (!term_said_[r].empty() && line.compare(0, term_said_[r].size(), term_said_[r]) == 0)
                t = line.substr(term_said_[r].size());
            term_said_[r] = line;
            std::string spoken;                       // runs of spaces read as one
            for (char ch : t) if (!(ch == ' ' && (spoken.empty() || spoken.back() == ' '))) spoken += ch;
            if (!spoken.empty() && spoken.find_first_not_of("-=_> ") != std::string::npos) say(spoken);
            h_.log("[TERMINAL] row " + std::to_string(r) + ": " + line);
        }
    }
    frame_keyboard(work);
    // the room message the terminal shows underneath ("Enter your user name." etc.)
    uint32_t mp = m.u32(addr::PC_MESSAGE_PTR);
    if (mp && mp != term_msg_) {
        term_msg_ = mp;
        std::vector<uint8_t> text;
        for (uint32_t a = mp; text.size() < 512; a++) {
            uint8_t b;
            if (!m.read(a, &b, 1)) break;
            text.push_back(b);
            if (b == 0xFE) break;
            if (b == 0xFB) {          // a yes/no question ends the message (some have no end code after it)
                if (m.read(a + 1, &b, 1)) text.push_back(b);
                break;
            }
        }
        std::string t = decode_text(text.data(), text.size(), 0);
        if (!t.empty()) { say(t); h_.log("[TERMINAL] message: " + t); }
    }
    return true;
}

// The terminal's on-screen keyboard (bio2.exe 0x4D2240): 10 columns x 3 rows of keys, the
// cursor's column at work +2 and row at +3. Key k = row * 10 + column types code k + 1 (k < 9),
// k (10..18) or k - 1 (20..28), shown as the letter code + 0x3F; column 9 is Backspace on the
// top row and Enter below it (one tall key). The game cycles a blink value at +0x2DA every
// frame while the keyboard takes input.
static std::string kb_key_name(int col, int row) {
    if (col == 9) return row == 0 ? "Backspace" : "Enter";
    int k = row * 10 + col;
    int code = k < 9 ? k + 1 : (k < 19 ? k : k - 1);
    char ch = (char)(code + 0x3F);
    if (ch == '@') return "At sign";
    return std::string(1, ch);
}

static std::string spell(const std::string& t) {
    std::string o;
    for (char c : t) { if (!o.empty()) o += ' '; o += c; }
    return o;
}

void Mod::frame_keyboard(uint32_t work) {
    MemoryReader& m = h_.mem();
    int col = m.u8(work + 2), row = m.u8(work + 3), blink = m.u8(work + 0x2DA);
    uint64_t now = h_.ms();
    if (blink != kb_blink_) { if (kb_blink_ >= 0) kb_seen_ms_ = now; kb_blink_ = blink; }
    bool active = kb_seen_ms_ && now - kb_seen_ms_ < 500 && col <= 9 && row <= 2;
    // the text typed so far: what follows the ">" prompt on the last prompt line
    std::string typed;
    bool have_prompt = false;
    for (int r = (int)term_lines_.size() - 1; r >= 0; r--) {
        size_t g = term_lines_[r].find('>');
        if (g != std::string::npos) {
            typed = term_lines_[r].substr(g + 1);
            have_prompt = true;
            break;
        }
    }
    size_t a = typed.find_first_not_of(' ');
    typed = a == std::string::npos ? "" : typed.substr(a);
    if (active && !kb_active_) {
        // after the screen's lines have been read
        for (size_t r = 0; r < term_lines_.size(); r++)
            if (term_lines_[r] != term_said_[r]) return;
        kb_active_ = true;
        kb_col_ = col; kb_row_ = row; kb_line_ = typed;
        bool any_key = false;          // the door-lock computer's "[press any key]>" (0x4D16FA)
        for (auto& l : term_lines_) if (l.find("press any key") != std::string::npos) any_key = true;
        kb_any_key_ = any_key;
        if (any_key) {
            say("Press action to continue.");
            h_.log("[KEYBOARD] press any key");
            return;
        }
        say("Keyboard. Arrows choose a letter, action types it. Backspace and Enter are at the right end. On " +
            kb_key_name(col, row) + ".");
        h_.log("[KEYBOARD] open at column " + std::to_string(col) + " row " + std::to_string(row));
        return;
    }
    if (!active) {
        if (kb_active_ && kb_seen_ms_ && now - kb_seen_ms_ > 1500) kb_active_ = false;
        return;
    }
    if (kb_any_key_) return;
    if (col != kb_col_ || row != kb_row_) {
        std::string name = kb_key_name(col, row);
        if (name == "Enter") name += typed.empty() ? ", nothing typed" : ", typed " + spell(typed);
        say(name, true);
        kb_col_ = col; kb_row_ = row;
    }
    if (have_prompt && typed != kb_line_) {
        if (typed.size() + 1 == kb_line_.size() && kb_line_.compare(0, typed.size(), typed) == 0)
            say("Deleted " + kb_line_.substr(typed.size()) + (typed.empty() ? ", empty" : ""), true);
        h_.log("[KEYBOARD] typed \"" + typed + "\"");
        kb_line_ = typed;
    }
}

// A key's name for the options menu's key config (Windows virtual-key codes).
static std::string vk_name(int vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    if (vk >= 0x60 && vk <= 0x69) return "Numpad " + std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F);
    switch (vk) {
        case 0: return "none";
        case 0x08: return "Backspace"; case 0x09: return "Tab"; case 0x0D: return "Enter";
        case 0x10: return "Shift"; case 0x11: return "Control"; case 0x12: return "Alt";
        case 0x13: return "Pause"; case 0x14: return "Caps Lock"; case 0x1B: return "Escape";
        case 0x20: return "Space"; case 0x21: return "Page Up"; case 0x22: return "Page Down";
        case 0x23: return "End"; case 0x24: return "Home"; case 0x25: return "Left arrow";
        case 0x26: return "Up arrow"; case 0x27: return "Right arrow"; case 0x28: return "Down arrow";
        case 0x2D: return "Insert"; case 0x2E: return "Delete";
        case 0x6A: return "Numpad star"; case 0x6B: return "Numpad plus"; case 0x6D: return "Numpad minus";
        case 0x6E: return "Numpad point"; case 0x6F: return "Numpad slash";
        case 0xBA: return "Semicolon"; case 0xBB: return "Equals"; case 0xBC: return "Comma";
        case 0xBD: return "Minus"; case 0xBE: return "Period"; case 0xBF: return "Slash";
        case 0xC0: return "Grave"; case 0xDB: return "Left bracket"; case 0xDC: return "Backslash";
        case 0xDD: return "Right bracket"; case 0xDE: return "Apostrophe";
        default: { char b[16]; std::snprintf(b, sizeof b, "key %02X", vk); return b; }
    }
}

// The PC options menu: Ctrl in the game (and Option on the title screen) opens it. It is
// drawn with pictures, so the mod reads its state and cursors (bio2.exe 0x44A0B0):
//   top menu: Key config, Screen brightness (a test picture), Sound, Exit
//   key config: ten rows, each a game function with its key; Enter then a key changes it;
//               the last row is Exit or Default (left / right)
//   sound: speaker, quality, music volume, effects volume, Exit (left / right change)
bool Mod::frame_options() {
    MemoryReader& m = h_.mem();
    int st = m.u8(addr::OPT_STATE);
    if (st == 0 || st > 0x17 || !(s_.system_flg & addr::SYS_OPTIONS)) {
        if (opt_group_ >= 0) { opt_group_ = -1; opt_said_.clear(); h_.log("[OPTIONS] closed"); }
        return false;
    }
    bool opened = opt_group_ < 0;
    int group = 0;                                    // 1 top, 2 keys, 3 brightness, 4 sound, 5 save question
    if (st == 1) group = 1;
    else if (st == 2 || (st >= 9 && st <= 12) || st == 14) group = 2;
    else if (st == 15) group = 5;
    else if (st == 3 || st == 7) group = 3;
    else if (st == 4 || st == 8) group = 4;
    if (group == 0) { if (opened) opt_group_ = 0; return true; }   // opening, closing, leaving a page
    bool new_group = group != opt_group_;
    if (new_group) {
        char b[64];
        std::snprintf(b, sizeof b, "[OPTIONS] state %d", st);
        h_.log(b);
    }
    std::string text, head;
    static const char* kTop[] = {"Key config", "Screen brightness", "Sound", "Exit"};
    static const char* kRow[] = {"Up", "Down", "Left", "Right", "Aim", "Action", "Run and cancel",
                                 "Status screen", "Options", "Map"};
    static const int kRowKey[] = {0, 1, 2, 3, 10, 11, 5, 13, 12, 7};
    int row = m.s32(addr::OPT_ROW);
    if (group == 1) {
        int c = m.s32(addr::OPT_TOP_CUR);
        if (new_group && opt_group_ <= 0) head = "Options. Up and down to choose, Enter to open, Escape to leave. ";
        if (c >= 0 && c < 4) text = kTop[c];
        if (c == 2) text += std::string(", ") + (m.s32(addr::OPT_SPEAKER) == 1 ? "mono" : "stereo");
    } else if (group == 2) {
        if (new_group) head = "Key config. Up and down to choose, Enter then the new key to change one. ";
        if (row >= 0 && row < 10) {
            int vk = m.u8(addr::OPT_KEYS + kRowKey[row]);
            text = st == 12 ? "Press the new key for " + std::string(kRow[row]) + "."
                            : std::string(kRow[row]) + ": " + vk_name(vk) + ".";
        } else if (row == 10) {
            text = m.u8(addr::OPT_KEY_CHOICE) ? "Default keys, Enter to restore them. Left or right for Exit."
                                              : "Exit. Left or right for Default keys.";
        }
    } else if (group == 5) {
        if (new_group) head = "Keep the new keys? ";
        text = m.u8(addr::OPT_KEY_CHOICE) ? "No." : "Yes.";
    } else if (group == 3) {
        if (new_group) text = "Screen brightness picture: colour bars under a red line. The Japanese text asks you to set "
                              "the monitor's brightness so the bars below the line can't be seen. Enter or Escape to go back.";
        else text = opt_said_;
    } else if (group == 4) {
        if (new_group) head = "Sound. Up and down to choose, left and right to change. ";
        switch (row) {
            case 0: text = std::string("Speakers: ") + (m.s32(addr::OPT_SPEAKER) == 1 ? "mono" : "stereo"); break;
            case 1: {
                int bits = m.s32(addr::OPT_SND_BITS), rate = m.s32(addr::OPT_SND_RATE);
                text = "Quality: " + std::to_string(bits) + " bit, " + (rate == 44100 ? "44" : rate == 22050 ? "22" : std::to_string(rate / 1000)) + " kilohertz";
                break;
            }
            case 2: text = "Music volume: " + std::to_string(m.u8(addr::OPT_BGM_VOL)); break;
            case 3: text = "Effects volume: " + std::to_string(m.u8(addr::OPT_SE_VOL)); break;
            case 4: text = "Exit"; break;
            default: break;
        }
    }
    if (!text.empty() && (new_group || text != opt_said_)) say(head + text, true);
    if (!text.empty()) opt_said_ = text;
    opt_group_ = group;
    return true;
}

// The PC save / load screens (and their confirmation boxes). Their text is drawn through the
// game's text function, which the host reports; the list cursor is read from memory.
//   list:   strings centred (x 0) and stacked: the folder title first, then one row per entry
//           ("Create New", saved games, "Exit"); SV_ROW_SEL is the highlighted row on screen.
//   dialog: a centred question with choices drawn at their own x; the choice drawn in colour 3
//           is highlighted.
//   footer: a help line at the bottom (y 0x1A0, or 0xD0 at low resolution).
bool Mod::frame_pc_menu() {
    std::vector<ScreenText> items = h_.screen_text();
    if (items.empty()) {
        if (pc_active_) { pc_active_ = false; pc_sig_.clear(); pc_dialog_sig_.clear(); }
        return false;
    }
    bool opened = !pc_active_;
    pc_active_ = true;
    std::string sig;
    for (auto& t : items) sig += std::to_string(t.x) + "," + std::to_string(t.y) + "," + std::to_string(t.colour) + ":" + t.text + "|";
    if (sig != pc_sig_) {
        std::string line = "[SCREEN]";
        for (auto& t : items) line += " (" + std::to_string(t.x) + "," + std::to_string(t.y) + ",c" + std::to_string(t.colour) + ") " + t.text + " |";
        h_.log(line);
    }
    auto is_footer = [](const ScreenText& t) { return t.x == 0 && (t.y == 0x1A0 || t.y == 0xD0); };
    std::vector<const ScreenText*> centred, choices;
    const ScreenText* footer = nullptr;
    for (auto& t : items) {
        if (t.text.empty()) continue;
        if (is_footer(t)) footer = &t;
        else if (t.x == 0) centred.push_back(&t);
        else choices.push_back(&t);
    }
    std::sort(centred.begin(), centred.end(), [](const ScreenText* a, const ScreenText* b) { return a->y < b->y; });
    MemoryReader& m = h_.mem();
    int row_sel = (int)m.s32(addr::SV_ROW_SEL), top = (int)m.s32(addr::SV_TOP_ROW);
    if (!choices.empty()) {
        // a confirmation box: the question is the centred line drawn just before the choices
        const ScreenText* question = nullptr;
        for (auto& t : items) {
            if (&t == choices[0]) break;
            if (t.x == 0 && !t.text.empty() && !is_footer(t)) question = &t;
        }
        std::string dsig;
        for (auto* c : choices) dsig += c->text + "|";
        const ScreenText* hl = nullptr;
        for (auto* c : choices) if (c->colour == 3) hl = c;
        std::string hls = hl ? hl->text : "";
        if (dsig != pc_dialog_sig_) {
            std::string t = question ? question->text + " " : "";
            for (size_t i = 0; i < choices.size(); i++) t += (i ? ", " : "") + choices[i]->text;
            t += ".";
            if (!hls.empty()) t += " " + hls + ".";
            say(t, true);
            pc_dialog_sig_ = dsig;
        } else if (hls != pc_hl_ && !hls.empty()) {
            say(hls, true);
        }
        pc_hl_ = hls;
    } else {
        bool from_dialog = !pc_dialog_sig_.empty();
        if (from_dialog) { pc_dialog_sig_.clear(); pc_hl_.clear(); }
        // the list: first centred line is the title, the rest are rows
        std::string title = centred.empty() ? "" : centred[0]->text;
        std::vector<std::string> rows;
        for (size_t i = 1; i < centred.size(); i++) rows.push_back(centred[i]->text);
        std::string cur = (row_sel >= 0 && row_sel < (int)rows.size()) ? rows[row_sel] : "";
        bool moved = row_sel != pc_row_ || top != pc_top_;
        if (opened || title != pc_title_) {
            std::string t = title.empty() ? "Save files." : title + ".";
            if (!cur.empty()) t += " " + cur + ".";
            if (opened && footer) t += " " + footer->text;
            say(t, true);
        } else if (!moved && !from_dialog && cur != pc_cur_ && !cur.empty() &&
                   cur.compare(0, std::min(pc_cur_.size(), cur.size()), pc_cur_.substr(0, std::min(pc_cur_.size(), cur.size()))) == 0 &&
                   (cur.size() > pc_cur_.size() || pc_typing_ms_)) {
            // a name being typed in (REbirth types the new save's name a letter at a time):
            // say it once it stops changing
            pc_typing_ms_ = h_.ms();
        } else if ((moved || cur != pc_cur_ || from_dialog) && !cur.empty()) {
            pc_typing_ms_ = 0;
            say(cur, true);
        }
        if (pc_typing_ms_ && h_.ms() - pc_typing_ms_ > 600 && cur == pc_cur_) {
            pc_typing_ms_ = 0;
            say(cur, true);
        }
        pc_title_ = title;
        pc_cur_ = cur;
    }
    pc_row_ = row_sel;
    pc_top_ = top;
    pc_sig_ = sig;
    return true;
}

// A document being read as it is picked up (status type 2, screen 4). The game shows the
// pages as pictures; the text comes from data/files.txt. The document is the file item the
// player is standing at; ST_F_PAGE_NO is the page on screen.
void Mod::frame_file_reader(bool opened) {
    const StatusScreen& st = s_.status;
    if (opened) {
        // Which document: the file item the player is using, or the one an event in front of
        // them hands over (a desk drawer, a printer); else the nearest untaken file item, even
        // one the room keeps hidden until an event gives it.
        reading_file_ = -1;
        auto is_file = [](const Aot& a) { return a.sce == SCE_ITEM && a.item >= 0x68 && a.item <= 0x80; };
        std::string how;
        for (const Aot* a : actionable_for(p_)) {
            if (is_file(*a)) { reading_file_ = a->item - 0x68; how = "item in front"; break; }
            if (a->sce == SCE_EVENT && (a->data[0] == 255 || a->data[2] == 24)) {
                Outcome o = event_outcome(a->data[3]);
                for (auto& b : s_.aots)
                    if (b.id == o.aot && is_file(b)) { reading_file_ = b.item - 0x68; how = "event in front"; }
                if (o.item >= 0x68 && o.item <= 0x80) { reading_file_ = o.item - 0x68; how = "event gives"; }
                if (reading_file_ >= 0) break;
            }
        }
        if (reading_file_ < 0) {
            double best = 1e18;
            for (auto& a : s_.aots) {
                if (!is_file(a) || a.item_taken) continue;
                bool hidden_spot = std::fabs(a.centre.x) < 50 && std::fabs(a.centre.z) < 50;
                double d = hidden_spot ? 50000 : std::hypot(a.centre.x - s_.x, a.centre.z - s_.z);
                if (d < best) { best = d; reading_file_ = a.item - 0x68; how = hidden_spot ? "hidden item" : "nearest item"; }
            }
        }
        if (reading_file_ < 0) {
            // the file just picked up: the game has already marked it taken (60A: the lab
            // security manual on the console)
            double best = 3000;
            for (auto& a : s_.aots) {
                if (!is_file(a)) continue;
                double d = std::hypot(a.centre.x - s_.x, a.centre.z - s_.z);
                if (d < best) { best = d; reading_file_ = a.item - 0x68; how = "item just taken"; }
            }
        }
        h_.log("reading file " + std::to_string(reading_file_) + " (" + how + ")");
        {   // the status work while a document is up, to find the game's own record of which one
            std::vector<uint8_t> w = h_.mem().bytes(addr::ST_MAIN_RTN, 0x40);
            std::string hex;
            char b[4];
            for (uint8_t v : w) { std::snprintf(b, sizeof b, "%02X", v); hex += b; }
            h_.log("[FILE] status work " + hex);
        }
        reading_page_ = -1;
    }
    if (reading_file_ < 0 || reading_file_ >= (int)files_.size() || files_[reading_file_].empty()) {
        if (opened) say("Reading a document. Its text is not available.", true);
        return;
    }
    const auto& pages = files_[reading_file_];
    int page = st.file_page;
    if (page == reading_page_) return;
    int n = (int)pages.size() - 1;               // pages[0] is the title
    std::string t;
    if (reading_page_ < 0) t = pages[0] + ". " + std::to_string(n) + (n == 1 ? " page. " : " pages. ");
    if (page + 1 <= n) t += "Page " + std::to_string(page + 1) + ". " + pages[page + 1];
    else t += "End of the document.";
    reading_page_ = page;
    say(t, true);
}

// The item box: state 0 = choosing one of your slots, 1 = choosing in the box (64 places,
// a window of five; the middle one, ST_IBOX_SEL + 2, is the one swapped), 2/3 = the box list
// scrolling, 4 = swapping.
std::string Mod::box_text(int index) const {
    index &= 63;
    int id = h_.mem().u8(addr::ITEM_WORK + (11 + index) * 4);
    int n = h_.mem().u8(addr::ITEM_WORK + (11 + index) * 4 + 1);
    std::string t = id ? item_name(id) : "Empty";
    if (id && ((n > 1 && id <= 30) || (id >= 2 && id <= 19))) t += ", " + std::to_string(n);
    return t + ". Box " + std::to_string(index + 1) + ".";
}

void Mod::frame_item_box(bool opened) {
    const StatusScreen& st = s_.status;
    const StatusScreen& ps = p_.status;
    int sel = (st.ibox_sel + 2) & 63, psel = (ps.ibox_sel + 2) & 63;
    bool state_changed = opened || st.item_state != ps.item_state;
    if (st.item_state == 0) {
        if (state_changed || st.cursor != ps.cursor) {
            std::string head = opened ? "Item box. Your items: " : (ps.item_state != 0 ? "Your items: " : "");
            int c = st.cursor;
            say(head + slot_text(c) + ". " + (c == 10 ? std::string("Top box") : "Slot " + std::to_string(c + 1)) + ".", true);
        }
    } else if (st.item_state >= 1 && st.item_state <= 3) {   // the box list (2 and 3 while it scrolls)
        if ((state_changed && ps.item_state == 0) || opened) say("In the box: " + box_text(sel), true);
        else if (sel != psel) say(box_text(sel), true);
    }
}

// One inventory slot as spoken: "Handgun bullets, 30", "Browning handgun, 13, equipped", "Empty".
std::string Mod::slot_text(int slot) const {
    const InvSlot* it = nullptr;
    if (slot >= 0 && slot < (int)s_.inventory.size()) it = &s_.inventory[slot];
    else if (slot == 10) it = &s_.top_slot;
    if (!it || !it->id) return "Empty";
    std::string t = item_name(it->id);
    if (it->part == 2) return t + ", second half";   // a two-slot weapon's right-hand slot
    if ((it->count > 1 && it->id <= 30) || (it->id >= 2 && it->id <= 19)) t += ", " + std::to_string(it->count);
    if (it->id == s_.equipped) t += ", equipped";
    return t;
}

// The status screen (inventory): top menu, item slots, command menu, combining, files, map.
void Mod::frame_status(bool opened) {
    const StatusScreen& st = s_.status;
    const StatusScreen& ps = p_.status;
    status_type_ = st.type;
    if (st.type == 2) {            // picking something up: the game's own message speaks for it
        if (st.sub_rtn0 == 4) frame_file_reader(st.sub_rtn0 != ps.sub_rtn0 || opened);
        return;
    }
    if (st.type == 1) {          // item box (bio2.exe 0x4FC110)
        frame_item_box(opened);
        return;
    }
    // the map on its own: the map key (A by default) opens it (type 3), and some events show
    // it (type 4, picking up an area map)
    if (st.type == 3 || st.type == 4) {
        // the game's map is a picture: open the mod's map with it (the same as Shift M)
        if (opened && !game_map_open_) {
            game_map_open_ = true;
            ensure_room();
            open_map("Game map screen, with the mod's map. Escape closes both. ");
        }
        return;
    }
    static const char* kTabs[] = {"File", "Map", "Item", "Exit"};
    int screen = st.sub_rtn0;
    bool new_screen = opened || screen != ps.sub_rtn0;
    std::string head;
    // as on the status screen: the character's name and the ECG's condition and colour
    if (opened) head = player_name() + ", " + condition_text() + ", ";
    if (screen == 2) {                                   // top menu
        if (new_screen || st.menu0 != ps.menu0) {
            std::string t = st.menu0 < 4 ? kTabs[st.menu0] : "Option";
            say(head + (new_screen ? "Menu: " : "") + t + (new_screen ? ", left or right to change" : "") + ".", true);
        }
    } else if (screen == 3) {                            // items
        bool new_state = new_screen || st.item_state != ps.item_state;
        if (st.item_state == 0) {
            if (new_state || st.cursor != ps.cursor) {
                int c = st.cursor;
                std::string where = c == 10 ? "Top box" : "Slot " + std::to_string(c + 1);
                say(head + slot_text(c) + ". " + where + ".", true);
            }
        } else if (st.item_state == 1) {
            if (new_state || st.menu1 != ps.menu1) {
                int item = (st.cursor >= 0 && st.cursor < (int)s_.inventory.size()) ? s_.inventory[st.cursor].id
                          : st.cursor == 10 ? s_.top_slot.id : 0;
                bool weapon = item > 0 && item < 0x14;
                std::string t;
                switch (st.menu1) {
                    case 3: t = weapon ? "Equip" : "Use"; break;
                    case 2: t = "Check"; break;
                    case 1: t = "Combine"; break;
                    default: t = weapon ? "Use" : "Equip"; break;
                }
                say(std::string(new_state ? item_name(item) + ": " : "") + t + ".", true);
            }
        } else if (st.item_state == 2 && st.item_cmd == 1 && st.item_step == 0) {   // combine: pick the second item
            if (new_state || st.item_cmd != ps.item_cmd || st.cursor_sub != ps.cursor_sub) {
                int c = st.cursor_sub;
                bool first = new_state || st.item_cmd != ps.item_cmd;
                say(std::string(first ? "Combine with: " : "") + slot_text(c) + ". " +
                    (c == 10 ? "Top box" : "Slot " + std::to_string(c + 1)) + ".", true);
            }
        }
    } else if (screen == 4) {
        if (new_screen) say(head + "Map. You are in " + room_name(s_.room_code) +
                            ". Back in the game, M tells the map from where you are and Shift M opens the mod's map of every room.", true);
    } else if (screen == 5) {
        if (new_screen) say(head + "Files.", true);
        // file reading: values logged by [UI] lines until the file screen layout is confirmed
        if (!new_screen && (st.file_page != ps.file_page || st.file_sel != ps.file_sel) && st.file_sel >= 0) {
            int fileno = -1, n = 0;
            for (int b = 0; b < 32; b++)
                if (s_.file_flags & (0x80000000u >> b)) { if (n == st.file_sel) { fileno = b; break; } n++; }
            if (fileno >= 0 && fileno < (int)files_.size() && !files_[fileno].empty()) {
                const auto& pages = files_[fileno];
                int pg = std::min<int>(st.file_page, (int)pages.size() - 1);
                std::string text = pg == 0 ? pages[0] : pages[pg];
                if (st.file_sel != ps.file_sel) text = pages[0] + ". " + std::to_string(pages.size() - 1) + " pages.";
                say(text, true);
            }
        }
    }
}

void Mod::frame_message() {
    Message& m = s_.message;
    if (m.active && !p_.message.active) {
        // the text pointer advances as the game types the message; walk back to its start.
        // Messages begin with the 0xFA start code (and never contain it), so the nearest
        // 0xFA at or before the typing position is the start; else the byte after 0xFE.
        MemoryReader& mem = h_.mem();
        uint32_t ptr = m.ptr, start = ptr;
        for (uint32_t back = 0; back < 24 && ptr > back; back++) {
            uint8_t b;
            if (!mem.read(ptr - back, &b, 1)) break;
            if (b == 0xFA) { start = ptr - back; break; }
            if (b == 0xFE && back > 0) { start = ptr - back + 1; break; }
        }
        puzzle_patch_message(start);    // may add the mod's own question to a puzzle's message
        // read up to the end code, a byte at a time near the end of readable memory
        std::vector<uint8_t> text;
        for (uint32_t a = start; text.size() < 2048; a++) {
            uint8_t b;
            if (!mem.read(a, &b, 1)) break;
            text.push_back(b);
            if (b == 0xFE) break;
            if (b == 0xFB) {          // a yes/no question ends the message (some have no end code after it)
                if (mem.read(a + 1, &b, 1)) text.push_back(b);
                break;
            }
        }
        bool yn = false;
        int item = h_.mem().u8(addr::MESS_ITEM);
        // 0xFB's parameter picks the set of choices (bio2.exe 0x4C9358: table 0x52E528, labels
        // 0x5326A0): 0 Yes/No, 2 the digits 1-6 of a key pad (20B's safe), 3 Up/Down...
        msg_choice_type_ = 0;
        for (size_t k = 0; k + 1 < text.size(); k++)
            if (text[k] == 0xFB) { msg_choice_type_ = text[k + 1] <= 5 ? text[k + 1] : 0; break; }
        msg_text_ = text.empty() ? "" : decode_text(text.data(), text.size(), item, &yn);
        // The game shows a message a page at a time (0xFD ends a page; the player presses
        // action to go on) and asks a yes/no question (0xFB) on the last page. Each page is read
        // as the game shows it, so a question is heard when its choices are on screen.
        msg_page_texts_.clear();
        msg_page_ = 0;
        {
            size_t from = 0;
            for (size_t k = 0; k <= text.size(); k++) {
                if (k == text.size() || text[k] == 0xFD || text[k] == 0xFE) {
                    std::string pg = k > from ? decode_text(text.data() + from, k - from, item) : "";
                    if (!pg.empty() || k == text.size() || text[k] == 0xFE) msg_page_texts_.push_back(pg);
                    if (k == text.size() || text[k] == 0xFE) break;
                    from = k + 2;            // skip the page break and its parameter
                    k++;
                }
            }
            while (msg_page_texts_.size() > 1 && msg_page_texts_.back().empty()) msg_page_texts_.pop_back();
        }
        msg_yesno_ = yn;
        msg_start_ = start;
        m.text = msg_text_;
        m.yesno = yn;
        if (!msg_text_.empty()) h_.log("message at " + std::to_string(start) + ": " + msg_text_);
        // the basement reserve power panel (room 213): a sighted player watches a meter rise and
        // fall as each of the five switches moves; say what it shows
        if (s_.room_code == "213" && msg_text_.rfind("-Reserve Power", 0) == 0 && !msg_page_texts_.empty()) {
            msg_page_texts_.back() += " The panel's meter starts at 0. Each switch follows as a question: Up moves it up "
                                      "and adds 36, Down moves it down and takes away 14. The meter must end at exactly 80.";
            power_meter_ = h_.mem().s16(addr::SCE_VARS + 5 * 2);   // speak changes from here on
        }
        // Sherry's water tank (room 306): three boxes must lie along the north wall in three
        // columns so that the filling water floats them into a bridge to the Club key (main
        // procedure 10 checks them, flag 4.147). While the tank is drained (flag 4.146 off) and
        // that is not done, the lever's Yes also lines them up (frame_tank).
        if (s_.room_code == "306" && msg_text_.rfind("A lever to fill and drain water", 0) == 0 && !msg_page_texts_.empty() &&
            !reader_.flag(4, 146) && !reader_.flag(4, 147)) {
            msg_page_texts_.back() += " Yes also lines up the three boxes below, so the water floats them into a bridge to the Club key.";
            tank_state_ = 1;
        }
        // the media room's faucets (20D): "Light the queen, the king and the jack" means the
        // fireplace first (the Lighter, used from the items), then the faucets etched 12, 13, 11
        if (s_.room_code == "20D" && !msg_page_texts_.empty()) {
            bool lit = reader_.flag(5, 10);
            bool lighter = false, in_box = false;
            for (auto& it : s_.inventory) if (it.id == kLighter) lighter = true;
            for (int b = 0; b < 64 && !lighter; b++)
                if (h_.mem().u8(addr::ITEM_WORK + (11 + b) * 4) == kLighter) in_box = true;
            std::string where = in_box ? " It is in your item box." : " You are not carrying it now.";
            std::string light = lighter ? "First light the fireplace: face it and use the Lighter from your items."
                                        : "First light the fireplace with the Lighter: face it and use the Lighter from your items." + where;
            if (msg_text_.rfind("A picture of a man on a carriage", 0) == 0)
                msg_page_texts_.back() += std::string(" Mod note: in playing cards the Queen is 12, the King 13 and the Jack 11, "
                                          "the numbers etched on the three faucets. ") + (lit ? "The fireplace is lit. " : light + " ") +
                                          "Then turn on faucet 12, then 13, then 11.";
            else if (msg_text_.rfind("You can light a fire here", 0) == 0)
                msg_page_texts_.back() += " " + std::string(lighter ? "Face it and use the Lighter from your items."
                                                                    : "It needs the Lighter, used from your items." + where);
            else if (msg_text_.rfind("The number 1", 0) == 0 && !msg_yesno_ && !lit)
                msg_page_texts_.back() += " The faucet won't turn until the fireplace is lit.";
            else if (msg_text_.rfind("The number 1", 0) == 0 && msg_yesno_) {
                int done = h_.mem().s16(addr::SCE_VARS + 4 * 2);   // the script counts faucets turned in order in var 4
                if (done >= 0 && done <= 3)
                    msg_page_texts_.back() = "Faucets turned in the right order so far: " + std::to_string(done) +
                                             " of 3 (12, 13, 11). " + msg_page_texts_.back();
            }
        }
        // the library's sliding shelves (room 112): the bronze plate's picture shows where the
        // three shelves must stand; each shelf's switch moves it one place along nine
        if (s_.room_code == "112" && !msg_page_texts_.empty()) {
            if (msg_text_.rfind("A bronze plate with a picture", 0) == 0)
                msg_page_texts_.back() += " The picture shows the three shelves standing at places 2, 5 and 7 "
                                          "of the nine places along the wall.";
            else if (msg_text_.rfind("A switch to slide the shelf", 0) == 0) {
                int place = h_.mem().s16(addr::SCE_VARS + 7 * 2) + 1;   // the script copies the shelf's place into var 7
                if (place >= 1 && place <= 9)
                    msg_page_texts_.back() = "This shelf stands at place " + std::to_string(place) +
                        " of 9. Left slides it one place toward place 1, Right one place toward place 9. " + msg_page_texts_.back();
                // the switch does nothing until the power switch is on (procedure 7, flag 4.23)
                if (!reader_.flag(4, 23))
                    msg_page_texts_.back() = "The shelves have no power yet: turn on the power switch first. " + msg_page_texts_.back();
            }
        }
        // the parking lot's hand brake (216): say where the push starts
        if (s_.room_code == "216" && !msg_page_texts_.empty() && msg_text_.rfind("The hand brake is loose", 0) == 0) {
            bool spot = false;
            for (auto& a : s_.aots) if (push_spot(a)) spot = true;
            if (spot) msg_page_texts_.back() += " Mod note: walk to \"Push the vehicle\" in the catalog; walking there starts the push.";
        }
        if (!msg_text_.empty()) {
            bool last = msg_page_texts_.size() <= 1;
            std::string first = msg_page_texts_.empty() ? msg_text_ : msg_page_texts_[0];
            say(first + (yn && last ? " " + choices_prompt() : ""), false);
        }
    }
    if (m.active && m.routine == 4) puzzle_cursor_ = m.cursor;
    if (s_.room_code == "213" && power_meter_ >= 0) {
        int v = h_.mem().s16(addr::SCE_VARS + 5 * 2);
        if (v != power_meter_ && v >= 0 && v <= 100) { power_meter_ = v; say("Meter " + std::to_string(v) + ".", false); }
        if (!event_running() && !m.active) power_meter_ = -1;
    }
    if (m.active && p_.message.active) {
        m.yesno = p_.message.yesno;
        // the next page starts typing after a page wait (routine 2 -> 1)
        if (m.routine == 1 && p_.message.routine == 2 && msg_page_ + 1 < (int)msg_page_texts_.size()) {
            msg_page_++;
            bool last = msg_page_ + 1 == (int)msg_page_texts_.size();
            say(msg_page_texts_[msg_page_] + (msg_yesno_ && last ? " " + choices_prompt() : ""), true);
        }
        // the choices: the highlighted one when they appear (after the text), then each move
        if (m.routine == 4 && p_.message.routine != 4) say(choice_label(m.cursor), false);
        else if (m.routine == 4 && m.cursor != p_.message.cursor) say(choice_label(m.cursor), true);
    }
}

void Mod::log_ui_probe() {
    // Records every change in the menu-related memory, so a test session shows how the
    // game's menus map onto these values (see docs/TESTING.md).
    const GameState& s = s_;
    char buf[600];
    std::snprintf(buf, sizeof buf,
                  "sys=%08X st=%08X stop=%08X ingame=%d | title rtn=%d/%d mode=%d cur=%d | status rtn=%d/%d type=%d "
                  "cur=%d/%d m=%d/%d st=%d/%d/%d file=%d/%d/%d map=%d ibox=%d | save=%d front=%08X | sv=%d/%d/%d pc=%d | mess=%d/%d/%d",
                  s.system_flg, s.status_flg, s.stop_flg, s.in_game ? 1 : 0, s.title.main_rtn, s.title.sub_rtn0,
                  s.title.sel_mode, s.title.cursor, s.status.main_rtn, s.status.sub_rtn0, s.status.type, s.status.cursor,
                  s.status.cursor_sub, s.status.menu0, s.status.menu1, s.status.item_state, s.status.item_cmd,
                  s.status.item_step, s.status.file_book, s.status.file_sel,
                  s.status.file_page, s.status.map_no, s.status.ibox_sel, s.save_cursor, s.front_func,
                  (int)h_.mem().s32(addr::SV_TOP_ROW), (int)h_.mem().s32(addr::SV_ROW_SEL),
                  (int)h_.mem().u8(addr::SV_DIALOG_SEL), (int)h_.mem().u8(addr::PC_STATE),
                  s.message.active ? 1 : 0, s.message.routine, s.message.cursor);
    // log when a menu value changes; the system/status/stop flags alone change constantly in
    // play (walking, the camera), so they are shown but do not trigger a line
    std::string line = buf;
    char flags[40];
    std::snprintf(flags, sizeof flags, "%08X/%08X", s.system_flg & 0x7FFFFFFFu, s.stop_flg);
    std::string sig = flags + line.substr(line.find('|'));
    static std::string last;
    if (sig != last) { h_.log("[UI] " + line); last = sig; }
}

// The choices a question offers (bio2.exe 0x52E528 sets, labels 0x5326A0, the highlighted
// spelling): the 0xFB code's parameter picks the set.
static const std::vector<std::vector<const char*>>& choice_sets() {
    static const std::vector<std::vector<const char*>> k = {
        {"Yes", "No"}, {"Left", "Right", "No"}, {"1", "2", "3", "4", "5", "6"},
        {"Up", "Down"}, {"Left", "Right", "Cancel"}, {"Yes", "No", "Original"}};
    return k;
}

std::string Mod::choice_label(int cursor) const {
    const auto& set = choice_sets()[msg_choice_type_ < (int)choice_sets().size() ? msg_choice_type_ : 0];
    if (cursor >= 0 && cursor < (int)set.size()) return set[cursor];
    return cursor == 0 ? "Yes" : "No";
}

std::string Mod::choices_prompt() const {
    if (msg_choice_type_ == 0) return "Yes or no.";
    const auto& set = choice_sets()[msg_choice_type_ < (int)choice_sets().size() ? msg_choice_type_ : 0];
    if (msg_choice_type_ == 2) return "Choose a number from 1 to 6.";
    std::string t = "Choose ";
    for (size_t i = 0; i < set.size(); i++) t += (i ? (i + 1 == set.size() ? " or " : ", ") : "") + std::string(set[i]);
    return t + ".";
}

// ---------------------------------------------------------------- in-game announcements
// A zombie lying on the floor that has not got up yet (20B's in Claire's scenario: pose 2,
// AI flag 0x40; awake by the game's flags, but it stays down until the player comes close).
// While it has not moved since the room was entered it is no reason to stop a walk: routes
// keep a little away from it and pass.
bool Mod::lying_still(const Enemy& e) const {
    if (e.type == 0x2A && e.hp < 20000) return true;      // Mister X knocked down (see above)
    if (e.type < 0x10 || e.type > 0x1F) return false;    // zombies only (crows perch in pose 2)
    if (!((e.pose & 0xFF) == 2 || (e.pose & 0x4000))) return false;
    auto it = enemy_first_pos_.find(e.slot);
    return it != enemy_first_pos_.end() && std::hypot(e.x - it->second.x, e.z - it->second.z) < 150;
}

bool Mod::in_play(const Enemy& e) const {
    if (e.dormant()) return false;          // asleep until a script wakes it: not a threat yet
    if (std::fabs(e.x) >= 31000 || std::fabs(e.z) >= 31000) return false;
    // held far above or below the floor until a scripted entrance (20C: Mister X at y -9000
    // above the corridor before he breaks in)
    // (only him: a spider high on a wall, 405 at y -7200, is in play and can drop or spit)
    if ((e.type == 0x2A || e.type == 0x2B) && std::fabs(e.y - s_.y) > 6000) return false;
    if (room_.collisions.empty()) return true;
    double mnx = 1e9, mxx = -1e9, mnz = 1e9, mxz = -1e9;
    for (auto& c : room_.collisions) {
        if (c.w >= 30000 || c.d >= 30000) continue;
        mnx = std::min(mnx, (double)c.x); mxx = std::max(mxx, (double)c.x + c.w);
        mnz = std::min(mnz, (double)c.z); mxz = std::max(mxz, (double)c.z + c.d);
    }
    return e.x >= mnx - 1000 && e.x <= mxx + 1000 && e.z >= mnz - 1000 && e.z <= mxz + 1000;
}

bool Mod::on_stairs() const {
    for (auto& c : room_.collisions)
        if ((c.shape == 11 || c.shape == 12) && s_.x >= c.x && s_.x <= c.x + c.w && s_.z >= c.z && s_.z <= c.z + c.d)
            return true;
    return false;
}

std::vector<const Aot*> Mod::actionable() const { return actionable_for(s_); }

std::vector<const Aot*> Mod::actionable_for(const GameState& st) const {
    const GameState& s_ = st;
    std::vector<std::pair<int, const Aot*>> out;
    double fx, fz;
    facing_vector(s_.dir, fx, fz);
    double px = s_.x + fx * 800, pz = s_.z + fz * 800;
    for (const auto& a : s_.aots) {
        if (!a.player_triggers() || a.sce == SCE_AUTO || a.sce == SCE_NORMAL || a.sce == SCE_DAMAGE ||
            a.sce == SCE_STATUS)
            continue;
        if (a.sce == SCE_ITEM && a.item_taken) continue;
        if (a.floor >= 0 && a.floor < 0x80 && a.floor != s_.game_floor) continue;   // bio2.exe 0x4E921B: same floor only
        bool inside = point_in_quad(s_.x, s_.z, a.quad) || point_in_quad(px, pz, a.quad);
        if (!inside) continue;
        if (a.sat & SAT_FRONT) {
            bool ok = point_in_quad(s_.x + fx * 400, s_.z + fz * 400, a.quad) ||
                      std::hypot(a.centre.x - s_.x, a.centre.z - s_.z) < 300 ||
                      std::abs(angle_diff(s_.dir, dir_towards(s_.x, s_.z, a.centre.x, a.centre.z))) < 1024;
            if (!ok) continue;
        }
        if (!(a.manual() || a.sce == SCE_DOOR || a.sce == SCE_ITEM || a.sce == SCE_MESSAGE || a.sce == SCE_SAVE ||
              a.sce == SCE_ITEMBOX))
            continue;
        static const int prio[16] = {5, 1, 0, 5, 2, 2, 4, 5, 5, 3, 3, 5, 5, 2, 5, 5};
        out.push_back({prio[a.sce & 15], &a});
    }
    std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::vector<const Aot*> r;
    for (auto& o : out) r.push_back(o.second);
    return r;
}

// 603 (the pump room): the crate on the upper west walkway (object 1) is pushed off the
// walkway's east end; it lands in object trigger aot 11 below, whose proc sets 4.107 and turns
// on the ledge it makes (collision 33): the climb up to the east walkway and Power Room U
const int kCrateObj = 1, kCrateEntry = 2001, kCrateFlag = 107;
const double kCrateX = -13950, kCrateZ = -3210;

// the room's climb and drop boxes, plus climbable room objects (be_flg 0x100: 603's crate once
// it has fallen) as boxes to climb onto (the archive room's stepladder has its own help)
std::vector<Collision> Mod::climb_cols() const {
    std::vector<Collision> climbs = room_.collisions;
    if (s_.room_code == "203") return climbs;
    for (auto& ob : s_.objects) {
        if (!ob.solid || !(ob.flags & 0x100) || ob.floor < 0 || ob.floor > 30) continue;
        if (ob.index == kCrateObj && crate_up()) continue;
        Collision c;
        c.index = 900 + ob.index;
        c.shape = 9;
        c.x = (int)std::lround(ob.cx - ob.hx); c.z = (int)std::lround(ob.cz - ob.hz);
        c.w = (int)std::lround(2 * ob.hx); c.d = (int)std::lround(2 * ob.hz);
        c.floor_mask = 1u << ob.floor;
        c.raw_flag = PLAYER_BLOCK_BIT | CLIMB_BIT;
        climbs.push_back(c);
    }
    return climbs;
}

std::string Mod::climbable() const {
    double fx, fz;
    facing_vector(s_.dir, fx, fz);
    for (const auto& c : climb_cols()) {
        if ((c.shape != 9 && c.shape != 10) || !c.climbable()) continue;
        for (double d : {300.0, 500.0, 700.0}) {
            double x = s_.x + fx * d, z = s_.z + fz * d;
            // the game only looks at boxes on the player's own level (bio2.exe 0x4E03B0 tests the
            // floor mask): 603's drop from the east walkway is over the step up to it
            if (!c.contains(x, z) || !c.on_floor(s_.floor)) continue;
            if (c.shape == 9) return "climb up";
            if (s_.floor > 0) return "drop down";
        }
    }
    return "";
}

const Enemy* Mod::aim_target() const {
    double fx, fz;
    facing_vector(s_.dir, fx, fz);
    const Enemy* best = nullptr;
    double bd = 1e18;
    for (auto& e : s_.enemies) {
        if (!e.alive || !e.hostile() || !in_play(e)) continue;
        double dx = e.x - s_.x, dz = e.z - s_.z, along = dx * fx + dz * fz;
        if (along > 0 && std::fabs(dx * fz - dz * fx) < 900 && along < bd) { bd = along; best = &e; }
    }
    return best;
}

// The alligator in the Garbage Passage (40A, ROOM40A1.RDT): once it appears (flag 4.51) the
// doors lock; the gas cylinder on the wall at the Stairwell end can then be released (event 9,
// flag 4.155 once it lies on the floor). The alligator bites it (its +0x226 goes 1..6 while the
// cylinder is in its jaws) and a shot then blows it up. The safety lock device unlocks the doors.
// The partner following the player: pEnemy[1] while script flag 1.3 is set (Claire's or Leon's
// part; not while playing as Sherry or Ada)
bool Mod::partner_pos(double& x, double& z) const {
    if (partner_ != 1) return false;
    int pl = h_.mem().u8(addr::PL_ID);
    if (pl >= 6 || h_.mem().u8(addr::PLAYER + addr::W_ID) == 15) return false;
    uint32_t p = h_.mem().u32(addr::P_ENEMY + 4);
    if (!p || !h_.mem().u32(p + addr::W_BE_FLG)) return false;
    x = h_.mem().s32(p + addr::W_POS_X);
    z = h_.mem().s32(p + addr::W_POS_Z);
    return true;
}

void Mod::frame_alligator() {
    if (s_.room_code != "40A") { gator_hint_ = gator_released_ = gator_mouth_ = false; gator_sig_.clear(); return; }
    const Enemy* g = nullptr;
    for (auto& e : s_.enemies) if (e.type == 0x23) { g = &e; break; }
    bool fight = reader_.flag(4, 51);
    if (g) {
        char sig[96];
        std::snprintf(sig, sizeof sig, "routine %08X cylinder %d life %d", g->ptr ? h_.mem().u32(g->ptr + 4) : 0, g->cylinder, g->hp);
        if (gator_sig_ != sig) { gator_sig_ = sig; h_.log(std::string("[BOSS] alligator ") + sig); }
    }
    if (!g || !g->alive || !fight || s_.stop_flg) return;
    if (!gator_hint_) {
        gator_hint_ = true;
        say(std::string("The alligator. The doors are locked. ") +
            "Go to the gas cylinder release on the wall and press action to drop the cylinder, then keep away from the alligator. "
            "When it bites the cylinder, shoot. Or use the safety lock device to unlock the doors and leave.", false);
        return;
    }
    bool released = reader_.flag(4, 155);
    if (released && !gator_released_) {
        gator_released_ = true;
        say("Gas cylinder released. It is on the floor. Keep the alligator coming toward it; when it bites the cylinder, shoot.", true);
    }
    // 1..6: biting it (about a second), 7: holding it (a shot then blows it up, 0.8.9 log)
    bool mouth = g->cylinder == 7;
    if (mouth && !gator_mouth_) say("The alligator has the cylinder in its jaws. Aim and fire now!", true);
    gator_mouth_ = mouth;
    // distance every few seconds while it is coming
    uint64_t now = h_.ms();
    if (now - gator_dist_ms_ > 4000) {
        gator_dist_ms_ = now;
        double d = std::hypot(g->x - s_.x, g->z - s_.z);
        if (d < 12000) say("Alligator " + locate(Pt{g->x, g->z}) + ".", false);
    }
}

void Mod::frame_game() {
    // room change (also the first frame of play after the title screen or a load)
    std::string here = s_.room_code + "/" + std::to_string(s_.character);
    if (here != announced_room_) {
        announced_room_ = here;
        sel_id_ = -1;
        stop_walk();
        close_enemies_.clear();
        enemy_alert_ms_.clear();
        enemy_first_pos_.clear();
        enemy_dormant_.clear();
        near_hazards_.clear();
        hazard_count_ = -1;
        clash_numbers_.clear();
        aot_names_.clear();
        catalog_order_.clear();
        next_order_ = 0;
        in_front_.clear();
        category_ = 0;
        enemy_hp_.clear();
        for (auto& e : s_.enemies) enemy_hp_[e.slot] = e.hp;
        bool first = !visited_rooms_.count(s_.room_code);
        visited_rooms_.insert(s_.room_code);
        room_brief_ = !first;
        room_pending_ = true;
        room_quiet_ = false;
        room_changed_ms_ = aot_sig_ms_ = h_.ms();
        press_action_frames_ = 0;   // a press left over from the door must not use the door back (404)
        if (via_state_ && s_.room_code != via_room_) via_state_ = 0;   // a ladder's walk ends with its room
        page_frames_ = 0;
        aot_sig_.clear();
        known_.clear();
        on_stairs_prev_ = on_stairs();
        stairs_y_ = s_.y;
        prev_ammo_ = -1;
        return;
    }
    // The room summary waits until the room has set up: during the door animation the AOT table
    // still holds the last room's doors and items, and the new room's scripts add theirs a
    // moment later. The catalog is numbered from what is there once it settles.
    uint64_t now_ms = h_.ms();
    if (room_pending_) {
        std::string sig;
        for (auto& a : s_.aots) {
            char b[64];
            std::snprintf(b, sizeof b, "%d:%d:%.0f:%.0f:%d|", a.id, a.sce, a.centre.x, a.centre.z, a.item_taken ? 1 : 0);
            sig += b;
        }
        if (sig != aot_sig_) { aot_sig_ = sig; aot_sig_ms_ = now_ms; }
        bool settled = now_ms - room_changed_ms_ >= 300 && now_ms - aot_sig_ms_ >= 400 && !s_.stop_flg;
        if (settled || now_ms - room_changed_ms_ > 5000) {
            room_pending_ = false;
            catalog_order_.clear();
            next_order_ = 0;
            sel_id_ = -1;
            for (auto& e : entries(0)) known_[entry_key(e)] = e.name;
            last_catalog_check_ms_ = now_ms;
            room_settled_ms_ = now_ms;
            sca_base_ = sca_signature();
            if (!room_quiet_) announce_room(room_brief_);
            map_mode_ = false;
            // travelling to a room picked on the map (not when a journey begun while this room was
            // settling is already walking here)
            if (!travel_to_.empty() && !(travel_state_ == 1 && walking_)) travel_step(travel_state_ == 2);
        }
    } else if (now_ms - last_catalog_check_ms_ >= 500 && !s_.stop_flg && !s_.message.active) {
        // things that appear (or change) after arriving: a printout, a dropped key, an opened case
        last_catalog_check_ms_ = now_ms;
        std::vector<std::string> fresh;
        std::set<std::string> known_names;      // a second spot under a name already listed is no news
        auto cur = entries(0);
        for (auto& e : cur) { auto k = known_.find(entry_key(e)); if (k != known_.end() && k->second == e.name) known_names.insert(e.name); }
        for (auto& e : cur) {
            if (e.type == Entry::ENEMY || e.type == Entry::PERSON) continue;
            // stairs only when a script just switched the walls (111: the crank lowers the stairs)
            if (e.type == Entry::STAIRS && !(sca_changed_ms_ && now_ms - sca_changed_ms_ < 10000)) { known_[entry_key(e)] = e.name; continue; }
            auto it = known_.find(entry_key(e));
            // a note on the name changing (", use the Rook plug here" after one plug went in) is no news
            auto base = [](const std::string& n) { size_t u = n.find(", use the "); return u == std::string::npos ? n : n.substr(0, u); };
            if (it != known_.end() && it->second != e.name && base(it->second) == base(e.name)) { it->second = e.name; continue; }
            if (it == known_.end() || it->second != e.name) {
                auto fs = front_said_ms_.find(e.name);
                bool just_said = fs != front_said_ms_.end() && now_ms - fs->second < 5000;   // "in front of you" said it
                if (e.name != "Something to examine" && e.name != "Interactive spot" && !just_said &&
                    !known_names.count(e.name)) fresh.push_back(e.name);
                known_[entry_key(e)] = e.name;
            }
        }
        // a burst of "new" things just after arriving is the room finishing its setup, not news
        bool settling = now_ms - room_settled_ms_ < 4000 && (fresh.size() > 2 || now_ms - room_settled_ms_ < 1500);
        if (settling && !fresh.empty()) h_.log("[CATALOG] late room setup, " + std::to_string(fresh.size()) + " entries added quietly");
        if (!fresh.empty() && puzzle_state_ == 0 && !settling) {      // the puzzle help says what happened itself
            std::string t = "New: ";
            for (size_t i = 0; i < fresh.size(); i++) t += (i ? ", " : "") + fresh[i];
            say(t + ".");
            h_.log("[CATALOG] " + t);
        }
    }
    // a script switched the room's walls (408: the Valve handle lowers the bridge; 111: the
    // crank lowers the stairs): once it settles and stays changed, say so
    if (!room_pending_ && sca_changed_ms_ > room_settled_ms_ && now_ms - sca_changed_ms_ > 1500 && !event_running() && !s_.stop_flg) {
        std::string sig = sca_signature();
        if (!sca_base_.empty() && sig != sca_base_) { say("The way through this room has changed.", false); h_.log("[SCA] layout changed"); }
        sca_base_ = sig;
        sca_changed_ms_ = room_settled_ms_;     // handled
    }
    // stairs, climbing and drops
    bool st = on_stairs();
    if (st && !on_stairs_prev_) { stairs_y_ = p_.y; say("Stairs."); }
    else if (!st && on_stairs_prev_) {
        double dy = s_.y - stairs_y_;
        if (dy <= -900) say("Top of the stairs.");
        else if (dy >= 900) say("Bottom of the stairs.");
    } else if (!st && std::fabs(s_.y - p_.y) >= 900 && !s_.stop_flg) {
        say(s_.y < p_.y ? "Up a level." : "Down a level.");
        level_said_ms_ = h_.ms();
    }
    on_stairs_prev_ = st;
    // a lift (603's, 506's turn table) moves the player a little each frame during its event:
    // compare the level before and after
    {
        bool busy = s_.stop_flg || event_running();
        if (busy && !lift_busy_) lift_y_ = s_.y;
        if (!busy && lift_busy_ && !room_pending_ && !st && h_.ms() - level_said_ms_ > 3000) {
            double dy = s_.y - lift_y_;
            if (dy <= -900) say("Up a level.");
            else if (dy >= 900) say("Down a level.");
        }
        lift_busy_ = busy;
    }
    // health
    frame_alligator();
    // a change of condition is said at once, cutting off other speech, with the word only (a
    // sighted player sees the stance change, not the ECG colour, which needs the status screen)
    if (s_.in_game && p_.in_game && s_.hp > 0 && p_.hp > 0) {
        std::string c = condition_word(condition_level(s_.hp, s_.max_hp, s_.poisoned));
        std::string pc = condition_word(condition_level(p_.hp, p_.max_hp, p_.poisoned));
        if (c != pc) {
            hold_until_ms_ = 0;
            if (s_.poisoned && !p_.poisoned) say("Condition: Poison! A Blue Herb cures it.", true);
            else say("Condition: " + c + "!", true);
        } else if (s_.hp < p_.hp && !(s_.poisoned && p_.hp - s_.hp <= 2)) {
            h_.cue("hurt");   // poison takes a point at a time
        }
    }
    if (s_.hp <= 0 && p_.hp > 0 && !said_dead_) { said_dead_ = true; say("You died.", true); }
    if (s_.hp > 0) said_dead_ = false;
    // things within reach
    std::set<int> now;
    std::set<std::string> said_front, was_front;
    for (auto* a : actionable()) if (in_front_.count(a->id)) was_front.insert(describe(*a, true));
    for (auto* a : actionable()) {
        now.insert(a->id);
        if (in_front_.count(a->id) || walking_) continue;
        // two spots for one thing (a door with an event over it, 400's floodgate) say it once
        std::string d = describe(*a, true);
        if (was_front.count(d) || !said_front.insert(d).second) continue;
        // a door under a script spot (30A's ladder): the game takes the lower number first
        bool covered = false;
        for (auto* b : actionable())
            if (b->id < a->id && b != a && (a->sce == SCE_DOOR) != (b->sce == SCE_DOOR) &&
                (a->sce == SCE_DOOR || b->sce == SCE_DOOR) && std::hypot(b->centre.x - a->centre.x, b->centre.z - a->centre.z) < 800)
                covered = true;
        if (covered) continue;
        // a spot the script swaps for another one just after it was said (400's floodgate)
        auto fs = front_said_ms_.find(d);
        if (fs != front_said_ms_.end() && h_.ms() - fs->second < 5000) continue;
        front_said_ms_[d] = h_.ms();
        say(d + " in front of you.");
    }
    in_front_ = now;
    // standing where a carried item can be used (111: the Cog wheel at the clock's switch)
    {
        std::string use_here;
        for (const auto& a : s_.aots) {
            if (a.sce != SCE_FLAG_CHG || !a.player_triggers() || a.quad.empty() || !a.data[2]) continue;
            double fx, fz;
            facing_vector(s_.dir, fx, fz);
            // sat 0x20: the game tests the point 620 in front of the player
            bool in = point_in_quad(s_.x, s_.z, a.quad) || ((a.sat & 0x20) && point_in_quad(s_.x + fx * 620, s_.z + fz * 620, a.quad));
            if (a.floor != s_.floor || !in) continue;
            for (auto& it : s_.inventory)
                if (it.id == a.data[2] && use_here.find(item_name(a.data[2])) == std::string::npos) {
                    std::string n = item_name(a.data[2]);
                    if (use_here.empty()) use_here = n;
                    else {
                        size_t orr = use_here.rfind(" or ");
                        if (orr != std::string::npos) use_here = use_here.substr(0, orr) + ", " + use_here.substr(orr + 4);
                        use_here += " or " + n;
                    }
                }
        }
        if (!use_here.empty() && use_here != use_here_said_ && !walking_) say("You can use the " + use_here + " here.");
        if (!walking_ || use_here.empty()) use_here_said_ = use_here;
    }
    std::string cl = climbable();
    if (!cl.empty() && cl != prev_climb_ && !(cl == last_climb_said_ && h_.ms() - climb_said_ms_ < 4000)) {
        say("You can " + cl + " here.");
        last_climb_said_ = cl;
        climb_said_ms_ = h_.ms();
    }
    prev_climb_ = cl;
    // wall bump: forward or back held, no movement
    // the aim key the player is physically holding (game memory has no reliable aim flag)
    uint32_t held = h_.player_buttons();
    bool aiming = (held & BTN_AIM) != 0 && !walking_ && face_dir_ < 0;
    bool moving_keys = !aiming && (s_.key & (0x1000 | 0x4000)) != 0;
    if (moving_keys && !s_.stop_flg && !event_running() && !s_.message.active && std::lround(s_.x) == std::lround(p_.x) && std::lround(s_.z) == std::lround(p_.z)) {
        if (++bump_frames_ == 4 && !bumped_) {
            bumped_ = true;
            h_.cue("wall");
            uint64_t now = h_.ms();
            if (!walking_ && now - blocked_said_ms_ > 3000) { say("Blocked."); blocked_said_ms_ = now; }
        }
    } else {
        if (std::lround(s_.x) != std::lround(p_.x) || std::lround(s_.z) != std::lround(p_.z)) bumped_ = false;
        bump_frames_ = 0;
    }
    visited_cells_[s_.room_code].insert({int(std::floor(s_.x / 1500)), int(std::floor(s_.z / 1500))});
    // danger areas: warn when coming close (once until the player has moved away again)
    {
        std::set<int> live;
        for (auto* a : hazards()) {
            live.insert(a->id);
            double d = hazard_distance(*a, s_.x, s_.z);
            if (d < 900 && !near_hazards_.count(a->id) && !s_.stop_flg) {
                bool any_near = false;
                for (int id : near_hazards_) if (id >= 0) any_near = true;
                near_hazards_.insert(a->id);
                if (!any_near) say(d <= 0 ? "In a danger area!" : "Danger area " + locate(a->centre) + ".", true);
            } else if (d > 2000) near_hazards_.erase(a->id);
        }
        for (auto it = near_hazards_.begin(); it != near_hazards_.end();)
            it = live.count(*it) ? std::next(it) : near_hazards_.erase(it);
        int n = (int)live.size();
        if (hazard_count_ > 0 && n == 0 && !room_pending_) say("The danger area is gone.");
        hazard_count_ = room_pending_ ? -1 : n;
    }
    // enemies (several at once, like a flock of crows or arms through a window, get one line)
    std::vector<const Enemy*> woke, close;
    for (auto& e : s_.enemies) {
        auto pd = enemy_dormant_.find(e.slot);
        if (pd == enemy_dormant_.end() || pd->second != e.dormant()) {
            if (pd != enemy_dormant_.end() && !e.dormant() && e.alive && e.hostile()) {
                woke.push_back(&e);
                close_enemies_.insert(e.slot);              // the wake-up line is its alert
                enemy_alert_ms_[e.slot] = h_.ms();
            }
            char buf[96];
            std::snprintf(buf, sizeof buf, "[ENEMY] slot %d %s pose %04X be %08X %s", e.slot, e.name().c_str(), e.pose,
                          e.be_flg, e.dormant() ? "asleep" : "active");
            h_.log(buf);
            enemy_dormant_[e.slot] = e.dormant();
        }
        auto it = enemy_hp_.find(e.slot);
        // (not while a room is still setting up its enemies: 509's G is written into the slot in
        // stages and its life drops from the placeholder, which said "G down." on arrival)
        if (it != enemy_hp_.end() && e.hp < it->second && e.hostile() && !room_pending_ && h_.ms() - room_changed_ms_ > 2000) {
            if (!e.alive) say(e.name() + " down.", true);
            else h_.cue("hit");
            // Mister X cannot be killed: his life starts at 20000 and some, and below 20000 he
            // goes down (bio2.exe 0x480370) and stays down while you are in the room
            if (e.type == 0x2A && e.hp < 20000 && it->second >= 20000)
                say("Mister X is down. He stays down while you are in this room; get past him now.", true);
        }
        enemy_hp_[e.slot] = e.hp;
        if (!enemy_first_pos_.count(e.slot) && !room_pending_) enemy_first_pos_[e.slot] = {e.x, e.z};
        if (!e.alive || !in_play(e) || !e.hostile()) { close_enemies_.erase(e.slot); continue; }
        double d = std::hypot(e.x - s_.x, e.z - s_.z);
        if (room_pending_ || event_running()) continue;   // the room summary or a cutscene first; alerts follow
        // one overhead, right above the player (500: the G on the sky tram's roof reaches down
        // through it): say so each time it comes over you, so you can step away or aim up
        if (e.y - s_.y < -1500 && d < 1200 && !lying_still(e)) {
            auto ov = overhead_ms_.find(e.slot);
            if (ov == overhead_ms_.end() || h_.ms() - ov->second > 4000) {
                say(e.name() + " right above you.", true);
                overhead_ms_[e.slot] = h_.ms();
            }
        }
        if (d < 5000 && !close_enemies_.count(e.slot)) {
            close_enemies_.insert(e.slot);
            uint64_t now = h_.ms();
            auto last = enemy_alert_ms_.find(e.slot);
            // a flock (109's crows) closes in one by one: one line for them, not one each
            auto kind = kind_alert_ms_.find(e.name());
            bool flock = kind != kind_alert_ms_.end() && now - kind->second < 4000;
            if ((last == enemy_alert_ms_.end() || now - last->second > 15000) && !flock) {
                enemy_alert_ms_[e.slot] = now;
                kind_alert_ms_[e.name()] = now;
                close.push_back(&e);
            } else if (flock) {
                enemy_alert_ms_[e.slot] = now;
            }
        } else if (d > 10000) close_enemies_.erase(e.slot);
    }
    auto group_line = [&](std::vector<const Enemy*>& es, const char* what) {
        if (es.empty()) return;
        std::sort(es.begin(), es.end(), [&](const Enemy* a, const Enemy* b) {
            return std::hypot(a->x - s_.x, a->z - s_.z) < std::hypot(b->x - s_.x, b->z - s_.z);
        });
        const Enemy& n = *es[0];
        std::string where = metres(std::hypot(n.x - s_.x, n.z - s_.z)) + ", " +
                            relative_direction(s_.dir, s_.x, s_.z, n.x, n.z) + level_note(n);
        // just after arriving the room summary (and a journey's "You have reached") is still
        // being read: queue the alert behind it instead of cutting it off
        bool cut = h_.ms() - room_settled_ms_ > 3000;
        if (es.size() == 1) {
            std::string nm = n.name() + (lying_still(n) && !*what ? " lying on the floor," : "");
            say(nm + (*what ? std::string(" is ") + what + ", " : " ") + where + ".", cut);
            return;
        }
        bool same = true;
        for (auto* e : es) if (e->name() != n.name()) same = false;
        std::string name = same ? n.name() : "enemies";
        if (same && !name.empty() && name.back() != 's') {
            if (name.size() > 2 && (name.compare(name.size() - 2, 2, "ch") == 0 || name.compare(name.size() - 2, 2, "sh") == 0)) name += "es";
            else if (name == "G embryo" || name == "Zombie dog") name += "s";
            else name += "s";
        }
        say(std::to_string(es.size()) + " " + name + (*what ? std::string(" ") + what : std::string(" close")) +
            ", nearest " + where + ".", cut);
    };
    // a swarm (24 cockroaches in the crawl pipe): one line now and then, never cutting off
    // what the player is listening to (the catalog), so they can still pick a way out
    int active = 0;
    for (auto& e : s_.enemies) if (e.alive && e.hostile() && in_play(e) && !lying_still(e)) active++;
    if (active >= 10) {
        uint64_t now = h_.ms();
        if ((!close.empty() || !woke.empty()) && now - swarm_alert_ms_ > 10000) {
            swarm_alert_ms_ = now;
            auto& es = close.empty() ? woke : close;
            std::sort(es.begin(), es.end(), [&](const Enemy* a, const Enemy* b) {
                return std::hypot(a->x - s_.x, a->z - s_.z) < std::hypot(b->x - s_.x, b->z - s_.z);
            });
            const Enemy& n = *es[0];
            say(std::to_string(active) + " enemies about, nearest " + n.name() + ", " +
                metres(std::hypot(n.x - s_.x, n.z - s_.z)) + ", " + relative_direction(s_.dir, s_.x, s_.z, n.x, n.z) + level_note(n) + ".", false);
        }
        woke.clear(); close.clear();
    }
    group_line(woke, "getting up");
    group_line(close, "");
    // an item that left the inventory (used on something: a stone in a slot, a jewel in a
    // statue). Compared with the inventory last seen while playing, so the status screen in
    // between does not hide it; an item box visit or a combination (something new appeared)
    // resets it. Keys say so themselves ("You used the key"), ammunition just runs out.
    {
        std::set<int> now;
        std::map<int, int> counts;
        for (auto& it : s_.inventory) if (it.id) { now.insert(it.id); counts[it.id] += it.part == 2 ? 0 : std::max(1, (int)it.count); }
        if (game_inv_valid_ && !room_pending_ && ui_mode_ == 0) {
            // picked up: something new or more of something, and nothing went down
            std::vector<int> up;
            bool down = false;
            for (auto& kv : counts) { auto f = game_counts_.find(kv.first); if (f == game_counts_.end() || f->second < kv.second) up.push_back(kv.first); }
            for (auto& kv : game_counts_) { auto f = counts.find(kv.first); if (f == counts.end() || f->second < kv.second) down = true; }
            if (!down && up.size() == 1) {
                int id = up[0];
                if (id <= 30 && game_counts_.count(id)) say("Picked up " + item_name(id) + ", now " + std::to_string(counts[id]) + ".");
                else say("Picked up " + item_name(id) + ".");
            }
        }
        game_counts_ = counts;
        if (game_inv_valid_ && !room_pending_) {
            bool appeared = false;
            for (int id : now) if (!game_inv_.count(id)) appeared = true;
            if (!appeared)
                for (int id : game_inv_) {
                    if (now.count(id)) continue;
                    bool key = (id >= 89 && id <= 100) || id == 31 || id == 52 || id == 53;
                    if (key || id <= 30) continue;          // keys, weapons, ammunition
                    say(item_name(id) + " used.");
                }
        }
        game_inv_ = now;
        game_inv_valid_ = !room_pending_;
    }
    // ammunition in the equipped weapon
    int ammo = -1;
    if (s_.equipped > 1)
        for (auto& it : s_.inventory) if (it.id == s_.equipped) { ammo = it.count; break; }
    if (s_.equipped != prev_equipped_) { prev_equipped_ = s_.equipped; prev_ammo_ = -1; }   // a weapon change is not a reload
    if (prev_ammo_ >= 0 && ammo >= 0 && ammo != prev_ammo_) {
        int spare = 0;
        for (auto& w : kAmmoFor)
            if (w[0] == s_.equipped)
                for (auto& it : s_.inventory) if (it.id == w[1]) spare += it.count;
        if (ammo == 0) say(spare ? "Magazine empty." : "Out of ammo.", true);
        else if (ammo > prev_ammo_) say("Reloaded, " + std::to_string(ammo) + ".");
        else if (ammo <= 3) say(std::to_string(ammo) + " left.");
    }
    prev_ammo_ = ammo;
    // aiming (aim button held)
    // once per raise of the weapon; a quick re-raise at the same target stays quiet
    if (aiming && !was_aiming_ && ammo == 0) {
        // an empty weapon only clicks: say so (and whether there is ammunition to load)
        int spare = 0;
        for (auto& w : kAmmoFor)
            if (w[0] == s_.equipped)
                for (auto& it : s_.inventory) if (it.id == w[1]) spare += it.count;
        say(item_name(s_.equipped) + " is empty" + (spare ? "; you carry " + std::to_string(spare) + " rounds for it." : ", and you have no ammunition for it."), true);
    }
    if (aiming && !was_aiming_) {
        const Enemy* t = aim_target();
        int slot = t ? t->slot : -1;
        uint64_t now = h_.ms();
        if (slot != aim_said_slot_ || now - aim_said_ms_ > 3000) {
            if (t) {
                // a spider on the ceiling, a crow on a ledge: the gun points level unless the
                // player also holds up (or down) while aiming
                std::string lv = level_note(*t);
                std::string tip = lv == ", above you" ? " Hold up as well to aim up." : lv == ", below you" ? " Hold down as well to aim down." : "";
                say("Aiming at " + t->name() + ", " + metres(std::hypot(t->x - s_.x, t->z - s_.z)) + lv + "." + tip);
            }
            else {
                // where the nearest one is, so the player knows which way to turn (T faces it)
                const Enemy* n = nullptr;
                double nd = 20000;
                for (auto& e : s_.enemies) {
                    if (!e.alive || !e.hostile() || !in_play(e) || lying_still(e)) continue;
                    double d = std::hypot(e.x - s_.x, e.z - s_.z);
                    if (d < nd) { nd = d; n = &e; }
                }
                if (n) say("Aiming. No target in front; " + n->name() + " " + metres(nd) + ", " +
                           relative_direction(s_.dir, s_.x, s_.z, n->x, n->z) + level_note(*n) + ".");
                else say("Aiming. No target in front.");
            }
            aim_said_slot_ = slot;
            aim_said_ms_ = now;
        }
    }
    was_aiming_ = aiming;
}

// ---------------------------------------------------------------- catalog
bool Mod::hidden(const Aot& a) const {
    double mnx = 1e9, mxx = -1e9, mnz = 1e9, mxz = -1e9;
    for (auto& p : a.quad) { mnx = std::min(mnx, p.x); mxx = std::max(mxx, p.x); mnz = std::min(mnz, p.z); mxz = std::max(mxz, p.z); }
    if (mxx - mnx < 60 || mxz - mnz < 60) return true;
    if (!room_.collisions.empty()) {
        double bx0 = 1e9, bx1 = -1e9, bz0 = 1e9, bz1 = -1e9;
        for (auto& c : room_.collisions) {
            bx0 = std::min(bx0, (double)c.x); bx1 = std::max(bx1, (double)c.x + c.w);
            bz0 = std::min(bz0, (double)c.z); bz1 = std::max(bz1, (double)c.z + c.d);
        }
        if (a.centre.x < bx0 - 2000 || a.centre.x > bx1 + 2000 || a.centre.z < bz0 - 2000 || a.centre.z > bz1 + 2000)
            return true;
    }
    return false;
}

// A short name for an examine spot, taken from the start of its message: "It's a control
// monitor. It can't be operated..." -> "control monitor"; "Paper dated Sep. 18th..." -> "paper".
// Returns "" when the message does not start by naming a thing ("There is nothing useful here").
static std::string capitalise(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

std::string Mod::examine_noun(const std::string& msg) {
    // the way the player came, closed behind them (700's lift: "I can't go back now!")
    if (msg.rfind("I can't go back", 0) == 0) return "way back, closed";
    // a label in quotes first ("\u201CFan regulator\u201D You can stop...", "\"Oil pressure data
    // transmitter\""): that is the thing's name
    {
        size_t a = msg.find_first_not_of(" -");
        bool curly = a != std::string::npos && msg.compare(a, 3, "\xE2\x80\x9C") == 0;
        bool plain = a != std::string::npos && msg[a] == '"';
        if (curly || plain) {
            size_t st = a + (curly ? 3 : 1);
            size_t e = curly ? msg.find("\xE2\x80\x9D", st) : msg.find('"', st);
            if (e != std::string::npos && e > st) {
                std::string t = msg.substr(st, e - st);
                // a notice rather than a name ("Left back: Darkroom.", "Don't leave the Darkroom
                // door open."): call it a sign; examining it reads it
                int words = 1;
                for (char c : t) if (c == ' ') words++;
                char last = t.empty() ? 0 : t.back();
                // a name with a full stop ("Clearance identification device.", 407): a short
                // label ending in a thing's word is still a name
                if (last == '.' && words <= 4 && t.find(':') == std::string::npos && t.find('\'') == std::string::npos) {
                    std::string lw = t.substr(0, t.size() - 1);
                    size_t sp = lw.rfind(' ');
                    std::string tail = sp == std::string::npos ? lw : lw.substr(sp + 1);
                    for (auto& c : tail) c = (char)std::tolower((unsigned char)c);
                    static const char* things[] = {"device", "machine", "panel", "switch", "terminal", "regulator", "transmitter",
                                                   "unit", "controller", "system", "console", "lever", "valve", "generator"};
                    for (auto* th : things)
                        if (tail == th) { for (auto& c : lw) c = (char)std::tolower((unsigned char)c); return lw; }
                }
                if (t.find(':') != std::string::npos || last == '.' || last == '!' || last == '?' || words > 5 || e - st > 40)
                    return "sign";
                for (auto& c : t) c = (char)std::tolower((unsigned char)c);
                return t;
            }
        }
    }
    // a thing named with what it is for ("A device to open the tunnel gate.", 700; "There's an
    // activation lever for the train motor.", 703): the whole phrase is the name
    {
        std::string sent = msg.substr(0, msg.find_first_of(".!?"));
        std::string l = sent;
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        for (const char* lead : {"there's an ", "there's a ", "it's an ", "it's a ", "an ", "a "})
            if (l.rfind(lead, 0) == 0) { l = l.substr(std::strlen(lead)); break; }
        int words = 1;
        for (char c : l) if (c == ' ') words++;
        if (words <= 8 && l.size() < 50)
            for (const char* th : {"device to ", "lever for the ", "device for the "}) {
                size_t k = l.find(th);
                if (k != std::string::npos && k < 25 && l.find(' ', k + std::strlen(th)) != std::string::npos) return l;
            }
    }
    // "It's a switch for the lift." (603): a lift switch
    {
        size_t k = msg.find("switch for the ");
        if (k != std::string::npos) {
            std::string what = msg.substr(k + 15);
            what = what.substr(0, what.find_first_of(".!?, "));
            if (!what.empty()) return what + " switch";
        }
    }
    // a person doing something ("Ada is operating a control panel.", 503): who, and at what
    for (const char* who : {"Ada ", "Sherry ", "Ben ", "Annette ", "Marvin ", "Leon ", "Claire "}) {
        size_t n = std::strlen(who);
        if (msg.compare(0, n, who) != 0) continue;
        std::string rest = msg.substr(0, msg.find_first_of(".!?"));
        size_t art = std::string::npos;
        for (const char* a : {" a ", " an ", " the "}) {
            size_t k = rest.rfind(a);
            if (k != std::string::npos && (art == std::string::npos || k > art)) art = k + std::strlen(a);
        }
        if (art == std::string::npos) break;
        std::string thing = rest.substr(art);
        for (auto& c : thing) c = (char)std::tolower((unsigned char)c);
        return std::string(who, n - 1) + " at the " + thing;
    }
    std::istringstream ss(msg);
    std::vector<std::string> words;
    std::string w;
    while (ss >> w && words.size() < 12) {
        size_t k = w.find_first_not_of(".-\"\xE2\x80\x9C\x9D");   // leading "..." "-" and quotes
        if (k == std::string::npos) continue;
        w = w.substr(k);
        while (w.size() > 1 && w.back() == '-') w.pop_back();          // "-VAM-" -> "VAM"
        for (auto& c : w) if (c == '/') c = '-';      // "open/close switch" -> "open-close switch"
        words.push_back(w);
    }
    auto low = [](std::string x) {
        std::string o;
        for (char c : x) if (std::isalpha((unsigned char)c) || c == '\'' || c == '-') o += (char)std::tolower((unsigned char)c);
        return o;
    };
    size_t i = 0;
    auto skip = [&](std::initializer_list<const char*> seq) {
        size_t j = i;
        for (const char* t : seq) { if (j >= words.size() || low(words[j]) != t) return false; j++; }
        i = j;
        return true;
    };
    // leading phrases that come before the thing's name
    // "You can light a fire here." -> "Place to light a fire"
    if (words.size() >= 3 && low(words[0]) == "you" && low(words[1]) == "can") {
        std::string t = "place to";
        for (size_t j = 2; j < words.size() && j < 7; j++) {
            std::string w2 = low(words[j]);
            if (w2 == "here" || w2.empty()) break;
            t += " " + w2;
            char last = words[j].back();
            if (last == '.' || last == ',') break;
        }
        if (t != "place to") return t;
    }
    bool nothing_but = skip({"nothing", "but"});
    bool pronoun = !nothing_but && (skip({"it", "looks", "like"}) || skip({"it", "seems", "to", "be"}) ||
                   skip({"it", "appears", "to", "be"}) || skip({"it's"}) || skip({"it", "is"}) || skip({"that's"}));
    skip({"there's"}) || skip({"there", "is"}) || skip({"there", "are"}) || skip({"there", "was"}) ||
        skip({"there", "were"}) || skip({"there", "appear", "to", "be"}) || skip({"there", "appears", "to", "be"}) ||
        skip({"these", "are"}) || skip({"this", "is"});
    // "Many of the books are on profiling", "None of these books appear to be useful" -> books
    bool some_of = skip({"many", "of", "the"}) || skip({"none", "of", "these"}) || skip({"none", "of", "the"}) ||
                   skip({"some", "of", "the"}) || skip({"most", "of", "the"}) || skip({"all", "of", "the"});
    skip({"possibly"});
    // "Will you go up the ladder?" -> "ladder up" (401 has one each way at the same spot)
    for (size_t j = 0; j + 4 < words.size() + 1 && j < 6; j++) {
        if (j + 4 <= words.size() && low(words[j]) == "go" && (low(words[j + 1]) == "up" || low(words[j + 1]) == "down") &&
            low(words[j + 2]) == "the" && j >= 2 && low(words[j - 2]) == "will" && low(words[j - 1]) == "you" && j + 3 < words.size()) {
            std::string n = low(words[j + 3]);
            if (!n.empty()) return n + " " + low(words[j + 1]);
        }
    }
    // "Will you use the computer?" / "Will you go up the ladder?"
    if (skip({"will", "you"})) {
        while (i < words.size() && low(words[i]) != "the" && low(words[i]) != "a" && low(words[i]) != "an" &&
               low(words[i]) != "it" && low(words[i]).back() != '?')
            i++;
    }
    bool article = some_of || skip({"a"}) || skip({"an"}) || skip({"the"}) || skip({"some"}) || skip({"more"});
    if (pronoun && !article) return "";   // "It's locked", "It's too dark": a state, not a thing
    static const char* kStop[] = {"is", "are", "was", "were", "has", "have", "had", "dated", "and", "with", "of", "that",
                                  "it", "it's", "can", "can't", "which", "for", "on", "in", "to", "from", "here", "there",
                                  "looks", "seems", "by", "but", "made", "used", "leads", "fills", "blocks", "need", "than",
                                  "been", "isn't", "doesn't", "probably", "about", "more", "appears", "will", "has", "for",
                                  "says", "turns", "could", "won't", "too", "slightly", "ok", "the", "appear", "wrapped", "covered",
                                  "lying", "hanging", "placed", "filled", "written", "painted", "sitting", "stacked", "at"};
    static const char* kNone[] = {"nothing", "something", "someone", "anything", "no", "useless", "nobody",
                                  "i", "you", "he", "she", "they", "we", "my", "his", "her", "he's", "she's", "if",
                                  "all", "both", "this", "these", "complete", "miserable", "many", "none", "must",
                                  "sort", "kind", "already", "out", "you've", "i've", "it's", "that", "there"};
    std::vector<std::string> out;
    // "Nothing useful on the shelf." / "Nothing inside the locker": the place is the name
    if (!words.empty() && (low(words[0]) == "nothing" || low(words[0]) == "there's") && !nothing_but) {
        for (size_t j = 1; j + 2 < words.size() + 1 && j + 1 < words.size(); j++) {
            std::string p = low(words[j]), q = low(words[j + 1]);
            if ((p == "on" || p == "in" || p == "inside" || p == "under" || p == "behind") && (q == "the" || q == "this")) {
                if (j + 2 < words.size()) {
                    std::string n = words[j + 2];
                    while (!n.empty() && std::ispunct((unsigned char)n.back())) n.pop_back();
                    if (n.size() >= 3) return low(n);
                }
            }
        }
    }
    for (; i < words.size() && out.size() < 3; i++) {
        std::string raw = words[i], lw = low(raw);
        bool digit = false, alpha = false;
        for (char c : raw) { if (std::isdigit((unsigned char)c)) digit = true; if (std::isalpha((unsigned char)c)) alpha = true; }
        // a number after "number" ("The number 12 has been etched"), or a code mixing letters
        // and digits ("A police B1 map"), is part of the name; dates and other numbers end it
        if (digit && ((!out.empty() && out.back() == "number") || alpha)) {
            std::string t;
            for (char c : raw) if (std::isalnum((unsigned char)c)) t += (char)std::toupper((unsigned char)c);
            out.push_back(t);
            continue;
        }
        if (digit || raw[0] == '"' || (unsigned char)raw[0] == 0xE2) break;   // dates, codes, quoted writing
        if (lw.empty()) break;
        // "a button below the picture": keep a short place phrase
        if (!out.empty() && (lw == "below" || lw == "above" || lw == "under" || lw == "beside" || lw == "behind") &&
            i + 2 < words.size() && low(words[i + 1]) == "the") {
            std::string n = words[i + 2];
            while (!n.empty() && std::ispunct((unsigned char)n.back())) n.pop_back();
            out.push_back(lw); out.push_back("the"); out.push_back(low(n));
            break;
        }
        bool stop = false;
        for (const char* t : kStop) if (lw == t) stop = true;
        if (stop) break;
        if (lw == "useless" || lw == "broken" || lw == "old") continue;   // judgements stay for examining
        // an acronym written with dots ("S.T.A.R.S.") is one word, not the end of a sentence
        {
            std::string letters;
            int dots = 0;
            bool acro = true;
            for (char c : raw) {
                if (c == '.') dots++;
                else if (std::isalpha((unsigned char)c)) letters += (char)std::toupper((unsigned char)c);
                else acro = false;
            }
            if (acro && dots >= 2 && letters.size() >= 2 && letters.size() <= (size_t)dots + 1) {
                out.push_back(letters);
                continue;
            }
        }
        out.push_back(lw);
        if ((lw == "bunch" || lw == "pile" || lw == "stack" || lw == "pair" || lw == "set") && i + 2 < words.size() &&
            low(words[i + 1]) == "of") { out.push_back("of"); out.push_back(low(words[i + 2])); break; }
        char last = raw.back();
        if (last == '.' || last == ',' || last == '!' || last == '?' || last == ';' || last == ':') break;
    }
    if (out.empty()) return "";
    for (const char* t : kNone) if (out[0] == t) return "";
    size_t letters = 0;
    for (auto& o : out) letters += o.size();
    if (letters < 3) return "";
    std::string r;
    for (size_t k = 0; k < out.size(); k++) r += (k ? " " : "") + out[k];
    return r;
}

// What an event's script would do if the player used it now: walks the main-script procedure
// with the current flags and reports the first message it shows, the first AOT it fires
// (aot_on, which is how events hand over items) and any item it gives directly.
// The door an event opens when the player says Yes (302's ladder: "Will you climb up?" then
// aot_on of a hidden door to the kennel): the first aot_on of a door anywhere in the procedure.
int Mod::event_door(int proc) const {
    if (proc < 0 || proc >= (int)room_.main_procs.size()) return -1;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[proc].first, end = room_.main_procs[proc].second;
    for (int steps = 0; steps < 600 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        if (op == 0x47)
            for (auto& b : s_.aots) if (b.id == raw[pc + 1] && b.sce == SCE_DOOR) return b.id;
        pc += sz;
    }
    return -1;
}

// A lift: the event sets the player's floor member (17) while working on the player
// (work_set 1, 0), moving them up or down a level (506's turn table)
bool Mod::event_lift(int proc) const {
    if (proc < 0 || proc >= (int)room_.main_procs.size()) return false;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[proc].first, end = room_.main_procs[proc].second;
    bool on_player = false;
    for (int steps = 0; steps < 600 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        if (op == 0x2E && sz >= 3) on_player = raw[pc + 1] == 1 && raw[pc + 2] == 0;   // work_set
        if (op == 0x34 && sz >= 2 && raw[pc + 1] == 17 && on_player) return true;      // member_set(17, ...)
        pc += sz;
    }
    return false;
}

Mod::Outcome Mod::event_outcome(int proc, int depth) const {
    Outcome o;
    if (depth > 3 || proc < 0 || proc >= (int)room_.main_procs.size()) return o;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[proc].first, end = room_.main_procs[proc].second;
    std::vector<size_t> fails;            // where each open "if" goes when a condition fails
    auto have = [&](int item) {
        for (auto& it : s_.inventory) if (it.id == item) return true;
        return false;
    };
    for (int steps = 0; steps < 600 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        auto u8 = [&](int k) { return (int)raw[pc + k]; };
        auto u16 = [&](int k) { return (int)(raw[pc + k] | (raw[pc + k + 1] << 8)); };
        bool cond = true, is_cond = false;
        switch (op) {
            case 0x01: case 0x19: return o;                                   // evt_end, return
            case 0x06: fails.push_back(pc + sz + u16(2)); break;              // if
            case 0x07: if (!fails.empty()) fails.pop_back(); pc = pc + u16(2); continue;   // else: skip it
            case 0x08: if (!fails.empty()) fails.pop_back(); break;           // endif
            case 0x0D: case 0x0F: case 0x11: case 0x13: case 0x17: return o; // loops, switch, goto: stop
            case 0x18: {                                                      // gosub
                Outcome sub = event_outcome(u8(1), depth + 1);
                if (sub.message >= 0 || sub.aot >= 0 || sub.item > 0) return sub;
                break;
            }
            case 0x21: is_cond = true; cond = reader_.flag(u8(1), u8(2)) == (u8(3) != 0); break;   // ck
            case 0x5E: is_cond = true; cond = have(u8(1)); break;             // keep_item_ck
            case 0x88: is_cond = true; cond = !have(u8(1)); break;            // sce_item_ck_lost
            case 0x2B: o.message = u8(2); return o;                           // message_on
            case 0x47: o.aot = u8(1); return o;                               // aot_on
            case 0x76: o.item = u8(1); return o;                              // sce_item_get
            default: break;
        }
        if (is_cond && !cond) {
            if (fails.empty()) return o;
            pc = fails.back();
            fails.pop_back();
            continue;
        }
        pc += sz;
    }
    return o;
}

// The message an examine spot or event would show now (-1 if none known).
int Mod::aot_message(const Aot& a) const {
    if (a.sce == SCE_MESSAGE) return a.data[0] < room_.messages.size() ? a.data[0] : -1;
    if (a.sce == SCE_EVENT && (a.data[0] == 255 || a.data[2] == 24)) {
        Outcome o = event_outcome(a.data[3]);
        if (o.message >= 0) return o.message;
        if (a.data[3] < room_.proc_message.size()) return room_.proc_message[a.data[3]];
    }
    return -1;
}

// the archive room (203) stepladder help: see frame_ladder
namespace {
const int kLadderObj = 0, kCrankAot = 5, kReportsMsg = 3;
const double kLadderX = -18673, kLadderZ = -25200;        // against the south shelf (box 7, z -25868)
const double kLadderStandX = -18673, kLadderStandZ = -23800;
const char* kLadderQuestion = "Push the stepladder here?";
}

// A spot that runs its event when the player walks onto it (no button) and drops the player
// to the floor below: the library's weak floor on the upper walkway (room 112, AOT 9,
// procedure 17), the only way to the area behind the shelves.
bool Mod::proc_drops_player(int proc) const {
    if (proc < 0 || proc >= (int)room_.main_procs.size()) return false;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[proc].first, end = room_.main_procs[proc].second;
    bool on_player = false, falls = false;
    for (int steps = 0; steps < 400 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        if (op == 0x2E) on_player = raw[pc + 1] == 1 && raw[pc + 2] == 0;          // work_set: the player
        if (op == 0x2F && on_player && raw[pc + 1] == 1) falls = true;            // speed_set: vertical speed
        if (op == 0x30 && falls) return true;                                     // add_speed: moves
        pc += sz;
    }
    return false;
}

bool Mod::walk_on_spot(const Aot& a) const {
    return (a.sce == SCE_EVENT && !a.manual() && a.player_triggers() && a.data[0] == 0xFF && a.data[2] == 0x18 &&
            proc_drops_player(a.data[3])) || push_spot(a) || scene_spot(a);
}

// A place that starts a scene when walked into (no button): the event's procedure stops the
// player (set(2, 7, 1)). The story can wait on one (509: the turn table's next scene starts on
// the far side of the platform; 409: Ada's scene), so they are listed
bool Mod::scene_spot(const Aot& a) const {
    if (a.sce != SCE_EVENT || a.manual() || !a.player_triggers() || a.data[0] != 0xFF || a.data[2] != 0x18 ||
        a.data[3] >= room_.main_procs.size() || a.quad.empty()) return false;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[a.data[3]].first, end = room_.main_procs[a.data[3]].second;
    for (int steps = 0; steps < 40 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        if (op == 0x22 && sz >= 4 && raw[pc + 1] == 2 && raw[pc + 2] == 7 && raw[pc + 3] == 1) return true;
        pc += sz;
    }
    return false;
}

// The parking lot's vehicle (216, Leon): after the scene with the woman, walking up beside it
// (AOT 3, an event run by walking in, procedure 2: set(4, 89, 1)) starts pushing it together
bool Mod::push_spot(const Aot& a) const {
    if (s_.room_code != "216" || a.sce != SCE_EVENT || a.manual() || !a.player_triggers() ||
        a.data[0] != 0xFF || a.data[2] != 0x18 || a.data[3] >= room_.main_procs.size()) return false;
    const auto& raw = room_.raw;
    size_t pc = room_.main_procs[a.data[3]].first, end = room_.main_procs[a.data[3]].second;
    for (int steps = 0; steps < 200 && pc < end && pc < raw.size(); steps++) {
        uint8_t op = raw[pc];
        int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
        if (sz <= 0 || pc + sz > raw.size()) break;
        if (op == 0x22 && sz >= 4 && raw[pc + 1] == 4 && raw[pc + 2] == 89 && raw[pc + 3] == 1) return true;
        pc += sz;
    }
    return false;
}

std::string Mod::describe(const Aot& a, bool shortform) const {
    if (push_spot(a)) return "Push the vehicle, walk up to it";
    if (scene_spot(a) && !proc_drops_player(a.data[3])) return "Scene trigger, walk into it";
    if (walk_on_spot(a)) return "Weak floor, walk onto it to drop down";
    if (a.sce == SCE_DOOR) {
        // a door into another part of this same room that a script made out of something with a
        // name (21B: the elevator behind the painting): keep calling it that
        if (a.door_stage == s_.stage && a.door_room == s_.room) {
            auto it = aot_names_.find(a.id);
            if (it != aot_names_.end() && !is_door_name(it->second)) return it->second;
            // after loading a save the script's name is gone: the known ones by room
            if (s_.stage == 1 && s_.room == 0x1B) return "Elevator";
            if (!a.door_locked) return "Way to another part of this room";
        }
        std::string t = door_prefix(a.door_anim, a.door_tex) + room_name(a.door_stage, a.door_room);
        if (a.door_locked) {
            t += ", locked";
            // the door's lock mark (the game shows it on the door and in its message): which key
            if (a.lock_type > 0 && a.lock_type < 0xFE && a.lock_type < kItemCount && kItemNames[a.lock_type][0])
                t += ", needs the " + item_name(a.lock_type);
        }
        return t;
    }
    if (a.sce == SCE_ITEM) {
        std::string t = item_name(a.item);
        // the archive room's crank is out of reach on top of a shelf (see frame_ladder)
        if (s_.stage == 1 && s_.room == 3 && a.id == kCrankAot) return t + ", on top of the shelf";
        // ammunition and ink ribbons come in amounts, weapons with their load; for keys the
        // number is the game's own bookkeeping (Diamond key "2")
        bool counted = (a.item >= 2 && a.item <= 30);
        if (a.amount > 1 && counted) t += ", " + std::to_string(a.amount);
        return t;
    }
    // scripts often turn a door into an event (locked, cutscene, "won't open")
    if (a.sce == SCE_EVENT || a.sce == SCE_MESSAGE) {
        // the door this event itself turns on (505: the security panel's event makes AOT 0 a door
        // to the second turn table; the room's script also has a door there to the first one)
        int own = -1;
        if (a.sce == SCE_EVENT && (a.data[0] == 255 || a.data[2] == 24) && a.data[3] < room_.main_procs.size()) {
            const auto& raw = room_.raw;
            size_t pc = room_.main_procs[a.data[3]].first, end = room_.main_procs[a.data[3]].second;
            for (int steps = 0; steps < 600 && pc < end && pc < raw.size(); steps++) {
                uint8_t op = raw[pc];
                int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
                if (sz <= 0 || pc + sz > raw.size()) break;
                if (op == 0x47 && sz >= 2) { own = raw[pc + 1]; break; }      // aot_on
                pc += sz;
            }
        }
        std::vector<const ScriptDoor*> order;
        for (auto& d : room_.script_doors) if (own >= 0 && d.aot == own) order.push_back(&d);
        for (auto& d : room_.script_doors) if (!(own >= 0 && d.aot == own)) order.push_back(&d);
        for (auto* dp : order) {
            const auto& d = *dp;
            Pt c = quad_centre(d.quad);
            if (d.stage == s_.stage && d.room == s_.room) continue;   // a way within this room (21B's elevator)
            if (point_in_quad(a.centre.x, a.centre.z, d.quad) || std::hypot(c.x - a.centre.x, c.z - a.centre.z) < 600) {
                std::string t = door_prefix(d.anim, d.tex) + room_name(d.stage, d.room);
                // the event in front of it says why it won't open ("It's locked", "sealed")
                int msg = aot_message(a);
                if (msg >= 0 && msg < (int)room_.messages.size()) {
                    const std::string& m = room_.messages[msg];
                    // a ladder, hatch or lift that leads there: "Emergency ladder to ..."
                    std::string noun = examine_noun(m);
                    bool offer = m.find("Will you") != std::string::npos;   // "Will you push the button?"
                    bool way = noun.find("elevator") != std::string::npos || noun.find("ladder") != std::string::npos ||
                               noun.find("lift") != std::string::npos || noun.find("hatch") != std::string::npos ||
                               noun.find("manhole") != std::string::npos || noun.find("hole") != std::string::npos ||
                               noun.find("vent") != std::string::npos || noun.find("duct") != std::string::npos ||
                               noun.find("pipe") != std::string::npos;
                    if ((offer || way) && !noun.empty() && noun.find("door") == std::string::npos &&
                        m.find("door") == std::string::npos && m.find("knob") == std::string::npos) {
                        bool here = d.stage == s_.stage && d.room == s_.room;   // the script moves you on later
                        std::string rn = room_name(d.stage, d.room), rl = rn, nl = noun;
                        for (auto& c : rl) c = (char)std::tolower((unsigned char)c);
                        for (auto& c : nl) c = (char)std::tolower((unsigned char)c);
                        // "Elevator to Elevator" (602): the room is named after the thing
                        if (!here && rl.find(nl) != std::string::npos) t = rn;
                        else t = capitalise(noun) + (here ? "" : " to " + rn);
                    }
                    if (m.find("too small") != std::string::npos) t += ", too small for you to enter";
                    else if (m.find("for me to enter") != std::string::npos || m.find("can't enter") != std::string::npos) t += ", can't enter yet";
                    else if (m.find("locked") != std::string::npos) t += ", locked";
                    else if (m.find("sealed") != std::string::npos || m.find("won't budge") != std::string::npos ||
                             m.find("won't open") != std::string::npos)
                        t += ", won't open";
                }
                return t;
            }
        }
    }
    // an item found by searching a spot (401: the first aid spray in the old locker)
    if (a.sce == SCE_EVENT && a.item > 0 && a.item < kItemCount && kItemNames[a.item][0]) return item_name(a.item) + ", search the spot";
    std::string label = kind_label(a.sce);
    if (s_.room_code == "10C" && a.id == 6 && a.sce == SCE_EVENT && !const_cast<Mod*>(this)->statues_solved())
        return "Statue holding the red jewel";
    if (s_.room_code == "10C" && a.id == 6 && a.sce == SCE_EVENT) return "Statue inscription";
    // the media room's faucet puzzle (20D): the picture asks to light the queen, the king and
    // the jack; the faucets are etched 12, 13 and 11 (playing-card values), turned in that order
    if (s_.room_code == "20D" && a.sce == SCE_EVENT && a.id >= 4 && a.id <= 6) {
        static const char* kFaucet[] = {"Faucet 12, the Queen", "Faucet 13, the King", "Faucet 11, the Jack"};
        return kFaucet[a.id - 4];
    }
    if (s_.room_code == "20D" && a.sce == SCE_EVENT && a.id == 3) return "Fireplace";
    // the alligator's corridor (40A): the gas cylinder on the wall, released once the alligator
    // has appeared; it then lies on the floor; the safety lock unlocks the two doors
    if (s_.room_code == "40A") {
        if (a.id == 2 && a.sce == SCE_EVENT) return "Gas cylinder release";
        if (a.id == 2) return "Gas cylinder on the wall";
        if (a.id == 3) return "Gas cylinder on the floor";
        if (a.id == 4 && a.sce == SCE_EVENT) return "Safety lock device, unlocks the doors";
    }
    // an event names itself through what its script does now: the item it hands over, or the
    // message it shows ("Will you use the computer?")
    if (a.sce == SCE_EVENT && (a.data[0] == 255 || a.data[2] == 24)) {
        Outcome o = event_outcome(a.data[3]);
        if (o.aot >= 0)
            for (auto& b : s_.aots) {
                if (b.id != o.aot) continue;
                if (b.sce == SCE_ITEM && !b.item_taken) return item_name(b.item);
                // a ladder, hatch or gap that the script turns into a door when used
                if (b.sce == SCE_DOOR) return "Way to " + room_name(b.door_stage, b.door_room);
                break;
            }
        if (o.item > 0) return item_name(o.item);
        int msg = o.message;
        // a ladder or hatch whose Yes opens a door elsewhere: say where it goes
        int door = event_door(a.data[3]);
        if (door >= 0)
            for (auto& b : s_.aots)
                if (b.id == door && !(b.door_stage == s_.stage && b.door_room == s_.room)) {
                    std::string noun = msg >= 0 && msg < (int)room_.messages.size() ? examine_noun(room_.messages[msg]) : "";
                    std::string lead = noun.rfind("ladder", 0) == 0 || noun.rfind("Ladder", 0) == 0 ? capitalise(noun) : "Way";
                    return lead + " to " + room_name(b.door_stage, b.door_room);
                }
        if (event_lift(a.data[3])) return "Lift";
        if (msg < 0 && a.data[3] < room_.proc_message.size()) msg = room_.proc_message[a.data[3]];
        // what it says now is about something else not being ready ("The tunnel gate isn't open
        // yet!", 703's lever): name the thing from its first message and say what is missing
        if (msg >= 0 && msg < (int)room_.messages.size() && a.data[3] < room_.proc_message.size()) {
            const std::string& m = room_.messages[msg];
            int first = room_.proc_message[a.data[3]];
            bool not_yet = m.find(" yet") != std::string::npos && (m.find("n't") != std::string::npos || m.find(" not ") != std::string::npos);
            if (not_yet && first >= 0 && first != msg && first < (int)room_.messages.size()) {
                std::string noun = examine_noun(room_.messages[first]);
                if (!noun.empty()) {
                    std::string why = m;
                    while (!why.empty() && (why.back() == '!' || why.back() == '.' || why.back() == ' ')) why.pop_back();
                    if (!why.empty()) why[0] = (char)std::tolower((unsigned char)why[0]);
                    return capitalise(noun) + ", " + why;
                }
            }
        }
        if (msg >= 0 && msg < (int)room_.messages.size()) {
            std::string noun = examine_noun(room_.messages[msg]);
            // "Will you move the lever?": keep the fuller name its first message gives
            // ("activation lever for the train motor"), so the entry keeps its name
            int first = a.data[3] < room_.proc_message.size() ? room_.proc_message[a.data[3]] : -1;
            if (!noun.empty() && first >= 0 && first != msg && first < (int)room_.messages.size() &&
                room_.messages[msg].find("Will you") != std::string::npos) {
                std::string full = examine_noun(room_.messages[first]);
                std::string fl = full, nl = noun;
                for (auto& c : fl) c = (char)std::tolower((unsigned char)c);
                for (auto& c : nl) c = (char)std::tolower((unsigned char)c);
                if (full.size() > noun.size() && fl.find(nl) != std::string::npos) noun = full;
            }
            if (!noun.empty()) return capitalise(noun);
            return "Something to examine";
        }
    }
    if (a.sce == SCE_MESSAGE && a.data[0] < room_.messages.size()) {
        // name the thing without giving away what examining it says
        std::string noun = examine_noun(room_.messages[a.data[0]]);
        return noun.empty() ? "Something to examine" : "Examine " + noun;
    }
    (void)shortform;
    return label;
}

std::vector<Entry> Mod::entries(int cat) {
    std::vector<Entry> out;
    if (cat != 4) {
        for (const auto& a : s_.aots) {
            if (!a.player_triggers()) continue;
            if (a.sce == SCE_AUTO || a.sce == SCE_NORMAL || a.sce == SCE_DAMAGE || a.sce == SCE_STATUS ||
                a.sce == SCE_WATER || a.sce == SCE_MOVE)
                continue;
            if (hidden(a)) continue;
            bool walk_on = walk_on_spot(a);
            if (!(a.manual() || a.sce == SCE_DOOR || a.sce == SCE_ITEM || walk_on)) continue;
            if ((a.sce == SCE_ITEM || (a.sce == SCE_EVENT && a.item > 0)) && a.item_taken) continue;
            if (cat == 1 && a.sce != SCE_DOOR) continue;
            if (cat == 2 && a.sce != SCE_ITEM) continue;
            if (cat == 3 && (a.sce == SCE_DOOR || a.sce == SCE_ITEM)) continue;
            Entry e;
            e.type = Entry::AOT; e.id = a.id; e.kind = a.sce; e.name = describe(a, false);
            aot_names_[a.id] = e.name;
            e.pos = a.centre; e.quad = a.quad; e.locked = a.door_locked; e.floor = a.floor;
            e.walk_on = walk_on;
            out.push_back(e);
        }
    }
    // item-use spots (flag-change zones naming an item, 111: the Cog wheel at the clock's switch):
    // when the player carries that item, say so on the spot's entry, or list the spot itself
    std::map<int, std::vector<std::string>> use_notes;
    if (cat == 0 || cat == 3) {
        for (const auto& a : s_.aots) {
            if (a.sce != SCE_FLAG_CHG || !a.player_triggers() || a.quad.empty() || !a.data[2]) continue;
            bool have = false;
            for (auto& it : s_.inventory) if (it.id == a.data[2]) have = true;
            if (!have) continue;
            std::string what = item_name(a.data[2]);
            if (what.empty()) continue;
            Entry* best = nullptr;
            double bd = 1500;
            for (auto& o : out) {
                if (o.type != Entry::AOT || o.kind == SCE_DOOR || o.kind == SCE_ITEM) continue;
                if (o.floor != a.floor && o.floor >= 0 && a.floor >= 0) continue;
                double d = std::hypot(o.pos.x - a.centre.x, o.pos.z - a.centre.z);
                if (d < bd) { bd = d; best = &o; }
            }
            if (best) {
                // several items for one spot (303's panel takes the four plugs): listed after the
                // names are final (numbered ones too)
                auto& v = use_notes[entry_key(*best)];
                if (std::find(v.begin(), v.end(), what) == v.end()) v.push_back(what);
                continue;
            }
            Entry e;
            e.type = Entry::AOT; e.id = a.id; e.kind = a.sce; e.name = "Spot to use the " + what;
            e.pos = a.centre; e.quad = a.quad; e.floor = a.floor; e.walk_on = true;
            out.push_back(e);
        }
    }
    // Leon B's escape train (704): once the train is moving (4.137), a script waits for the player
    // to walk west past x -6000 into the front car, then the last fight's scene starts (proc 2
    // -> 6). It is not an AOT; list it until the scene has turned the door into "I can't put my
    // friends in danger" (aot 0 becomes a message)
    if ((cat == 0 || cat == 3) && s_.room_code == "704" && reader_.flag(4, 137)) {
        bool door = false;
        for (auto& a : s_.aots) if (a.id == 0 && a.sce == SCE_DOOR) door = true;
        if (door && s_.x > -6500) {
            Entry e;
            e.type = Entry::AOT; e.id = 2002; e.kind = SCE_EVENT;
            e.name = "Scene trigger, walk into it";
            e.pos = {-7500, -23500}; e.floor = 0; e.walk_on = true;
            e.quad = {{-8100, -24100}, {-6900, -24100}, {-6900, -22900}, {-8100, -22900}};
            out.push_back(e);
        }
    }
    if ((cat == 0 || cat == 3) && crate_up()) {
        const SceneObject& ob = s_.objects[kCrateObj];
        Entry e;
        e.type = Entry::AOT; e.id = kCrateEntry; e.kind = 0;
        e.name = "Crate on the upper walkway, the mod pushes it off the end";
        e.pos = {ob.cx ? ob.cx : ob.x, ob.cz ? ob.cz : ob.z}; e.floor = ob.floor;
        out.push_back(e);
    }
    // stairs (collision shape 12): the far end, up or down from where the player is
    if (cat == 0 || cat == 3) {
        for (const auto& w : stairways()) {
            if (!w.stairs) continue;
            bool going_up;
            if (s_.floor == w.low) going_up = true;
            else if (s_.floor == w.high) going_up = false;
            else if (s_.floor > w.low && s_.floor < w.high) going_up = std::hypot(s_.x - w.low_end.x, s_.z - w.low_end.z) < std::hypot(s_.x - w.high_end.x, s_.z - w.high_end.z);
            else continue;                                   // stairs on another storey
            Entry e;
            e.type = Entry::STAIRS; e.id = w.index; e.kind = going_up ? 1 : 0;
            e.name = going_up ? "Stairs up" : "Stairs down";
            e.pos = going_up ? w.high_end : w.low_end;
            e.start_end = going_up ? w.low_end : w.high_end;
            out.push_back(e);
        }
        // ledges and boxes to climb onto or drop down from (collision shapes 9 and 10), one per
        // ledge, the side nearest the player: a drop is often the only way on (306's water tank)
        std::map<int, Entry> ledges;
        for (const auto& w : stairways()) {
            if (w.index < 1000) continue;
            int c = (w.index - 1000) / 4;
            bool up;
            if (w.oneway == 1 && s_.floor == w.low) up = true;
            else if (w.oneway == -1 && s_.floor == w.high) up = false;
            else continue;
            Entry e;
            e.type = Entry::STAIRS; e.id = w.index; e.kind = up ? 2 : 3;
            e.name = up ? "Ledge to climb up" : "Ledge to drop down";
            e.start_end = up ? w.low_end : w.high_end;
            e.pos = up ? w.high_end : w.low_end;
            auto it = ledges.find(c);
            if (it == ledges.end() || std::hypot(e.start_end.x - s_.x, e.start_end.z - s_.z) <
                                          std::hypot(it->second.start_end.x - s_.x, it->second.start_end.z - s_.z))
                ledges[c] = e;
        }
        int nup = 0, ndown = 0, iup = 0, idown = 0;
        for (auto& kv : ledges) (kv.second.kind == 2 ? nup : ndown)++;
        for (auto& kv : ledges) {
            Entry e = kv.second;
            if (e.kind == 2 && nup > 1) e.name += " " + std::to_string(++iup);
            if (e.kind == 3 && ndown > 1) e.name += " " + std::to_string(++idown);
            out.push_back(e);
        }
    }
    if (cat == 0 || cat == 4) {
        for (const auto& en : s_.enemies) {
            if (!en.alive || !in_play(en)) continue;
            if (en.type == 0x3F) continue;            // scenery (see Enemy::hostile)
            if (!en.hostile() && cat == 4) continue;
            Entry e;
            e.type = en.hostile() ? Entry::ENEMY : Entry::PERSON; e.id = en.slot; e.kind = en.type; e.name = en.name();
            e.pos = {en.x, en.z};
            out.push_back(e);
        }
    }
    // the partner following the player (pEnemy[1]): listed so the player can walk back to them
    if (cat == 0) {
        double px, pz;
        if (partner_pos(px, pz)) {
            bool listed = false;
            std::string who = (h_.mem().u8(addr::PL_ID) & 1) ? "Sherry" : "Ada";
            for (auto& o : out)
                if (o.type == Entry::PERSON && (std::hypot(o.pos.x - px, o.pos.z - pz) < 500 ||
                                                (o.name == who && std::hypot(o.pos.x - px, o.pos.z - pz) < 2500))) listed = true;
            if (!listed) {
                Entry e;
                e.type = Entry::PERSON; e.id = 1; e.kind = 0;
                e.name = (h_.mem().u8(addr::PL_ID) & 1) ? "Sherry" : "Ada";
                e.pos = {px, pz};
                out.push_back(e);
            }
        }
    }
    // a door the player cannot use from this level, under a script event that can be (405:
    // the hidden passage's door is set on floor 7; the event at the ladder's foot climbs and
    // opens it): list the event, under the door's name
    for (size_t i = 0; i < out.size();) {
        bool drop = false;
        if (out[i].type == Entry::AOT && out[i].kind == SCE_DOOR && out[i].floor >= 0 && out[i].floor < 0x80 &&
            std::abs(out[i].floor - s_.game_floor) >= 5)       // far off (401: a walkway three up is a real level)
            for (auto& ev : out)
                if (ev.type == Entry::AOT && (ev.kind == SCE_EVENT || ev.kind == SCE_MESSAGE) && ev.floor == s_.game_floor &&
                    std::hypot(ev.pos.x - out[i].pos.x, ev.pos.z - out[i].pos.z) < 1500) {
                    // a message there says why not yet ("The fan is spinning too fast"): keep its name
                    if (ev.kind == SCE_EVENT && !is_door_name(ev.name)) ev.name = out[i].name;
                    drop = true;
                    break;
                }
        if (drop) out.erase(out.begin() + i); else i++;
    }
    // a script event over a real door (a guard that runs first) is the same exit: list it once
    for (size_t i = 0; i < out.size();) {
        bool dup = false;
        if (out[i].type == Entry::AOT && (out[i].kind == SCE_EVENT || out[i].kind == SCE_MESSAGE))
            for (auto& d : out)
                if (d.type == Entry::AOT && d.kind == SCE_DOOR && d.name.compare(0, out[i].name.size(), out[i].name) == 0 &&
                    std::hypot(d.pos.x - out[i].pos.x, d.pos.z - out[i].pos.z) < 800) { dup = true; break; }
        // two script spots on one doorway (30A: a message and an event): list the first
        if (!dup && out[i].type == Entry::AOT && out[i].kind != SCE_DOOR && out[i].kind != SCE_ITEM &&
            is_door_name(out[i].name))
            for (size_t j = 0; j < out.size(); j++)
                if (j != i && out[j].type == Entry::AOT && out[j].kind != SCE_DOOR && out[j].name == out[i].name &&
                    out[j].id < out[i].id && std::hypot(out[j].pos.x - out[i].pos.x, out[j].pos.z - out[i].pos.z) < 800) { dup = true; break; }
        // an event over an item that hands over that item (215's Red card key): list the item
        if (!dup && out[i].type == Entry::AOT && out[i].kind == SCE_EVENT)
            for (auto& it : out)
                if (it.type == Entry::AOT && it.kind == SCE_ITEM && it.name == out[i].name &&
                    std::hypot(it.pos.x - out[i].pos.x, it.pos.z - out[i].pos.z) < 800) { dup = true; break; }
        // a door with an event over it that leads to the same room (110's emergency ladder):
        // the game checks the lower AOT number first, so that one is what using it does
        if (!dup && out[i].type == Entry::AOT && out[i].kind == SCE_DOOR) {
            std::string dest = door_dest(out[i].name);
            size_t cut = dest.find(", locked");
            if (cut != std::string::npos) dest = dest.substr(0, cut);
            for (auto& ev : out)
                if (ev.type == Entry::AOT && (ev.kind == SCE_EVENT || ev.kind == SCE_MESSAGE) && ev.id < out[i].id &&
                    ev.name.size() > dest.size() && ev.name.compare(ev.name.size() - dest.size(), dest.size(), dest) == 0 &&
                    std::hypot(ev.pos.x - out[i].pos.x, ev.pos.z - out[i].pos.z) < 800) { dup = true; break; }
        }
        // two door records for one doorway (the room script sets one per story state)
        if (out[i].type == Entry::AOT && out[i].kind == SCE_DOOR)
            for (size_t j = 0; j < i; j++)
                if (out[j].type == Entry::AOT && out[j].kind == SCE_DOOR && out[j].name == out[i].name &&
                    std::hypot(out[j].pos.x - out[i].pos.x, out[j].pos.z - out[i].pos.z) < 800) { dup = true; break; }
        if (dup) out.erase(out.begin() + i); else i++;
    }
    // two doors with the same name on different levels (400: one to the South Control Room
    // from the lower walkway, one from the high walkway): say which level
    std::vector<std::string> door_names;
    for (auto& e : out) door_names.push_back(e.name);
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i].type != Entry::AOT || out[i].kind != SCE_DOOR) continue;
        int lower = 0, higher = 0;
        for (size_t j = 0; j < out.size(); j++)
            if (j != i && out[j].type == Entry::AOT && out[j].kind == SCE_DOOR && door_names[j] == door_names[i]) {
                if (out[j].floor < out[i].floor) lower++;
                else if (out[j].floor > out[i].floor) higher++;
            }
        if (lower && !higher) out[i].name += ", upper level";
        else if (higher && !lower) out[i].name += ", lower level";
    }
    // two things with the same name (two reliefs, two shelves): add what sets them apart,
    // the rest of what examining them says ("Woman relief: a jewel is in place")
    std::vector<std::string> base_names;
    for (auto& e : out) base_names.push_back(e.name);
    std::map<size_t, std::string> rests;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i].type != Entry::AOT || out[i].kind == SCE_DOOR || out[i].kind == SCE_ITEM) continue;
        bool clash = false;
        for (size_t j = 0; j < out.size(); j++)
            if (j != i && base_names[j] == base_names[i] && out[j].type == Entry::AOT) clash = true;
        if (!clash) continue;
        const Aot* a = nullptr;
        for (auto& b : s_.aots) if (b.id == out[i].id) a = &b;
        int msg = a ? aot_message(*a) : -1;
        if (msg < 0 || msg >= (int)room_.messages.size()) continue;
        const std::string& m = room_.messages[msg];
        size_t dot = m.find_first_of(".!?");
        if (dot == std::string::npos) continue;
        std::string rest = m.substr(dot + 1);
        size_t k = rest.find_first_not_of(" ");
        if (k == std::string::npos) continue;
        rest = rest.substr(k);
        size_t end = rest.find_first_of(".!?");
        if (end != std::string::npos) rest = rest.substr(0, end);
        if (rest.size() > 60) rest = rest.substr(0, rest.rfind(' ', 60));
        for (const char* q : {"\xE2\x80\x9C", "\xE2\x80\x9D", "\""}) {     // quotes add nothing spoken
            size_t qp;
            while ((qp = rest.find(q)) != std::string::npos) rest.erase(qp, std::strlen(q));
        }
        if (rest.rfind("Will you", 0) == 0) continue;      // the question is the same for each
        if (!rest.empty()) { rest[0] = (char)std::tolower((unsigned char)rest[0]); rests[i] = rest; }
    }
    // a long rest that is the same for each (615's five chemical shelves, 702's two generator
    // sockets) is left out: they are numbered below
    for (auto& kv : rests) {
        bool same = false;
        for (auto& kv2 : rests)
            if (kv2.first != kv.first && base_names[kv2.first] == base_names[kv.first] && kv2.second == kv.second) same = true;
        if (!same || kv.second.size() <= 45) out[kv.first].name += ": " + kv.second;
    }
    // still the same name (three shelf switches, two empty shelves): number them
    {
        std::map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].type == Entry::AOT && out[i].kind != SCE_DOOR && out[i].kind != SCE_ITEM) groups[out[i].name].push_back(i);
        // numbers are kept for the rest of the visit, so a name does not change while its thing
        // moves or another one is briefly switched off (the library's sliding shelves)
        for (auto& g : groups) {
            auto v = g.second;
            std::sort(v.begin(), v.end(), [&](size_t a, size_t b) { return out[a].id < out[b].id; });
            auto& nums = clash_numbers_[g.first];
            bool numbered = v.size() >= 2;
            for (size_t i : v) if (nums.count(out[i].id)) numbered = true;
            if (!numbered) continue;
            for (size_t i : v) {
                if (!nums.count(out[i].id)) { int n = (int)nums.size() + 1; nums[out[i].id] = n; }
                out[i].name += " " + std::to_string(nums[out[i].id]);
            }
        }
    }
    for (auto& e : out) {
        auto u = use_notes.find(entry_key(e));
        if (u == use_notes.end()) continue;
        std::string list;
        for (size_t i = 0; i < u->second.size(); i++)
            list += (i == 0 ? "" : (i + 1 == u->second.size() ? " or " : ", ")) + u->second[i];
        e.name += ", use the " + list + " here";
    }
    // A stable order: things keep the place they were given when first seen in this room
    // (nearest first at that moment); things that appear later go to the end.
    double x = s_.x, z = s_.z;
    std::vector<Entry*> fresh;
    for (auto& e : out)
        if (!catalog_order_.count(entry_key(e))) fresh.push_back(&e);
    std::stable_sort(fresh.begin(), fresh.end(), [x, z](const Entry* a, const Entry* b) {
        return std::hypot(a->pos.x - x, a->pos.z - z) < std::hypot(b->pos.x - x, b->pos.z - z);
    });
    for (auto* e : fresh) catalog_order_[entry_key(*e)] = next_order_++;
    // enemies after everything else when there are many (a swarm would bury the exits)
    int n_enemies = 0;
    for (auto& e : out) if (e.type == Entry::ENEMY) n_enemies++;
    bool swarm = n_enemies >= 6;
    std::stable_sort(out.begin(), out.end(), [this, swarm](const Entry& a, const Entry& b) {
        if (swarm && (a.type == Entry::ENEMY) != (b.type == Entry::ENEMY)) return b.type == Entry::ENEMY;
        return catalog_order_[entry_key(a)] < catalog_order_[entry_key(b)];
    });
    return out;
}

bool Mod::selected(Entry& out) {
    if (sel_id_ < 0) return false;
    for (auto& e : entries(0))
        if (e.type == sel_type_ && e.id == sel_id_) { out = e; return true; }
    return false;
}

std::string Mod::locate(const Pt& p) const {
    return metres(std::hypot(p.x - s_.x, p.z - s_.z)) + ", " + relative_direction(s_.dir, s_.x, s_.z, p.x, p.z);
}

void Mod::cycle(int step) {
    auto list = entries(category_);
    if (list.empty()) {
        say(std::string("No ") + (category_ == 0 ? "points of interest" : kCategories[category_]) + " here.", true);
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].type == sel_type_ && list[i].id == sel_id_) { idx = (int)i; break; }
    if (idx < 0) idx = step > 0 ? 0 : (int)list.size() - 1;
    else idx = (idx + step + (int)list.size()) % (int)list.size();
    const Entry& e = list[idx];
    sel_type_ = e.type; sel_id_ = e.id;
    say(e.name + ", " + std::to_string(idx + 1) + " of " + std::to_string(list.size()), true);
}

void Mod::announce_room(bool brief) {
    auto list = entries(0);
    int exits = 0, items = 0, enemies = 0, stairs = 0;
    for (auto& e : list) {
        if (e.type == Entry::PERSON) continue;
        if (e.type == Entry::STAIRS) { if (e.kind < 2) stairs++; continue; }
        if (e.type == Entry::ENEMY) enemies++;
        else if (e.walk_on) continue;
        else if (e.kind == SCE_DOOR || is_door_name(e.name) || e.name.rfind("Way to", 0) == 0 ||
                 ((e.kind == SCE_EVENT || e.kind == SCE_MESSAGE) && e.name.find(" to ") != std::string::npos &&
                  e.name.rfind("Something to", 0) != 0 && e.name.rfind("Place to", 0) != 0 && e.name.find(": ") == std::string::npos))
            exits++;          // doors, and ladders, lifts and gaps a script opens
        else if (e.kind == SCE_ITEM) items++;
    }
    std::string t = room_name(s_.room_code) + ".";
    std::vector<std::string> parts;
    if (!brief) {
        parts.push_back(std::to_string(exits) + (exits == 1 ? " exit" : " exits"));
        if (items) parts.push_back(std::to_string(items) + (items == 1 ? " item" : " items"));
        if (stairs) parts.push_back(stairs == 1 ? "stairs" : std::to_string(stairs) + " stairways");
        size_t hz = hazards().size();
        if (hz) parts.push_back(hz == 1 ? "a danger area" : "danger areas");
    }
    if (enemies) parts.push_back(std::to_string(enemies) + (enemies == 1 ? " enemy" : " enemies") + " nearby");
    for (size_t i = 0; i < parts.size(); i++) t += (i ? ", " : " ") + parts[i];
    if (!parts.empty()) t += ".";
    // enemies about and nothing to fight with (Sherry has no weapons to equip)
    if (enemies && s_.equipped == 0) {
        bool has_weapon = false;
        for (auto& it : s_.inventory) if (it.id > 0 && it.id < 0x14) has_weapon = true;
        if (has_weapon) t += " No weapon equipped.";
    }
    say(t, true);
}

// ---------------------------------------------------------------- puzzle help
// The statue puzzle in the 2nd-floor west stairway corridor (room 10C): two statues must be
// pushed across the hall onto the floor marks so that they face the statue holding the red
// jewel. When the player reads the inscription on that statue, the mod adds a question to the
// game's own message ("Will you push the statues into place?"). On Yes it places the statues
// where the game itself puts them once solved (room script, main procedure 0), the game's
// object triggers (AOTs 3 and 4, procedures 4 and 5) do the rest, and the mod then walks the
// player to the jewel and presses action, so the game asks "Will you take the Red jewel?".
namespace {
struct StatueSpot { int obj; double x, z; int flag_group, flag_index; };
const StatueSpot kStatues[] = {{1, -17000, -12500, 4, 38}, {2, -26000, -12500, 4, 39}};
const int kJewelAot = 5, kJewelItem = 51, kJewelFlag = 32, kInscription = 1;
const char* kPushQuestion = "Will you push the statues into place?";
}

// Walking to something on another level by a ladder inside the room (401): at the ladder press
// action and answer its question with Yes, then once on the new level walk on to the thing.
void Mod::frame_via() {
    if (via_state_ == 0) return;
    uint64_t now = h_.ms();
    if (via_state_ == 1) {
        if (walking_ || face_dir_ >= 0) return;
        bool at = false;
        for (auto* a : actionable()) if (a->id == via_ladder_id_) at = true;
        // facing it from as close as the walkway allows (401: the warehouse door answers first
        // until it is opened; the presses go on below)
        if (!at && walk_target_.id == via_ladder_id_ && std::hypot(walk_target_.pos.x - s_.x, walk_target_.pos.z - s_.z) < 2500) at = true;
        if (walk_target_.id == via_ladder_id_ && at && !s_.message.active && !s_.stop_flg) {
            press_action_frames_ = 2;
            via_state_ = 2; via_ms_ = now; via_floor_ = s_.floor; via_confirmed_ = false; via_pages_ = 0;
            via_x_ = s_.x; via_z_ = s_.z;
            h_.log("[WALK] at the ladder: pressing action");
        } else {
            via_state_ = 0;
        }
    } else if (via_state_ == 2) {
        if (s_.message.active && s_.message.routine == 2 && page_frames_ == 0 && via_pages_ < 4 && now - via_page_ms_ > 600) {
            via_pages_++; via_page_ms_ = now; page_frames_ = 2;
        }
        if (s_.message.active && s_.message.yesno && s_.message.routine == 4 && s_.message.cursor == 0 && !via_confirmed_) {
            via_confirmed_ = true;
            confirm_frames_ = 2;
        }
        if (s_.message.active || s_.stop_flg || event_running() || room_pending_ || ui_mode_ == 4) { via_ms_ = now; return; }
        if (s_.floor != via_floor_ || std::hypot(s_.x - via_x_, s_.z - via_z_) > 3000) {
            via_state_ = 0;
            Entry t = via_target_;
            for (auto& e : entries(0)) if (e.type == t.type && e.id == t.id) t = e;
            if (plan_walk(t, false)) { walking_ = true; }
            return;
        }
        if (now - via_ms_ > 1500 && via_tries_ < 2) {
            // something else at the same spot answered first (401: the warehouse door over the
            // ladder down until it is opened): press again
            // (opening it can also open the way: walk up to the ladder again, then press)
            via_tries_++;
            via_confirmed_ = false;
            via_ms_ = now;
            h_.log("[WALK] at the ladder: trying again");
            int tries = via_tries_;
            for (auto& e : entries(0))
                if (e.type == Entry::AOT && e.id == via_ladder_id_ && plan_walk(e, true)) { via_state_ = 1; via_tries_ = tries; return; }
            press_action_frames_ = 2;
            return;
        }
        if (now - via_ms_ > 5000) { via_state_ = 0; say("The ladder did not take you there.", false); }
    }
}

// A script has frozen the player (FG_STOP 7) or is running a cutscene (FG_STATUS 27).
bool Mod::event_running() const { return reader_.flag(2, 7) || reader_.flag(1, 27); }

bool Mod::statues_solved() {
    for (auto& st : kStatues) if (!reader_.flag(st.flag_group, st.flag_index)) return false;
    return true;
}

// Adds "question" as a yes/no page after room message idx, in place: the space after it holds
// the next messages, which are put back (puzzle_restore) as soon as the message closes.
bool Mod::patch_question(uint32_t start, int idx, const char* question, const char* what) {
    MemoryReader& m = h_.mem();
    uint32_t room = m.u32(addr::P_ROOM);
    uint32_t tab = room ? m.u32(room + 0x40) : 0;          // English message table
    if (!tab || idx + 1 >= (int)room_.messages.size()) return false;
    auto at_msg = [&](int i) { return tab + m.u16(tab + 2 * i); };
    uint32_t a1 = at_msg(idx), a2 = at_msg(idx + 1);
    if (start != a1) return false;
    std::vector<uint8_t> b = m.bytes(a1, a2 > a1 && a2 - a1 < 1024 ? a2 - a1 : 0);
    size_t k = 0;
    while (k < b.size() && b[k] != 0xFE) k++;
    if (k >= b.size() || a1 + k + 2 != a2) { h_.log(std::string("[PUZZLE] ") + what + ": message layout not as expected"); return false; }
    size_t room_left = 2;
    uint32_t next_at = a2;
    for (int i = idx + 1; i < (int)room_.messages.size() && i < (int)room_.message_bytes.size(); i++) {
        if (at_msg(i) != next_at) break;
        std::vector<uint8_t> nb = m.bytes(next_at, room_.message_bytes[i].size());
        if (nb != room_.message_bytes[i]) break;
        room_left += nb.size();
        next_at += (uint32_t)nb.size();
    }
    std::vector<uint8_t> patch{0xFD, 0x00};
    auto q = encode_text(question);
    patch.insert(patch.end(), q.begin(), q.end());
    for (uint8_t c : {0xFC, 0xFB, 0x20, 0xFE, 0x00}) patch.push_back(c);
    if (patch.size() > room_left) { h_.log(std::string("[PUZZLE] ") + what + ": no room for the question"); return false; }
    uint32_t at = a1 + (uint32_t)k;
    puzzle_backup_ = m.bytes(at, patch.size());
    if (puzzle_backup_.size() != patch.size() || !m.write(at, patch.data(), patch.size())) {
        h_.log(std::string("[PUZZLE] ") + what + ": could not add the question");
        return false;
    }
    puzzle_patch_addr_ = at;
    puzzle_cursor_ = 0;
    h_.log(std::string("[PUZZLE] ") + what + ": asking");
    return true;
}

// The archive room (203): the crank lies on top of the shelf at the south end of the east
// aisle, out of reach. A sighted player pushes the stepladder (object 0, pushable and
// climbable) down the aisle until it stops against that shelf, climbs it and takes the crank.
// Examining the shelf below the crank ("A pile of investigation reports.") gets the mod's
// question; on Yes the mod puts the ladder where pushing would leave it, walks to it, climbs,
// faces the crank and presses action so the game asks about it, then climbs down again.

bool Mod::crank_on_shelf() const {
    for (auto& a : s_.aots) if (a.id == kCrankAot && a.sce == SCE_ITEM && !a.item_taken) return true;
    return false;
}

bool Mod::puzzle_patch_message(uint32_t start) {
    if (s_.room_code == "203" && ladder_state_ == 0 && puzzle_state_ == 0 && crank_on_shelf() && s_.floor == 0) {
        const SceneObject* ob = kLadderObj < (int)s_.objects.size() ? &s_.objects[kLadderObj] : nullptr;
        bool placed = ob && std::hypot(ob->x - kLadderX, ob->z - kLadderZ) < 300;
        if (!placed && patch_question(start, kReportsMsg, kLadderQuestion, "stepladder")) { ladder_state_ = 1; return true; }
        return false;
    }
    if (s_.room_code != "10C" || puzzle_state_ != 0 || room_.messages.size() < 4) return false;
    if (statues_solved() || reader_.flag(8, kJewelFlag)) return false;
    MemoryReader& m = h_.mem();
    uint32_t room = m.u32(addr::P_ROOM);
    uint32_t tab = room ? m.u32(room + 0x40) : 0;          // English message table
    if (!tab) return false;
    uint32_t a1 = tab + m.u16(tab + 2 * kInscription), a2 = tab + m.u16(tab + 2 * (kInscription + 1));
    uint32_t a3 = tab + m.u16(tab + 2 * (kInscription + 2));
    if (start != a1) {
        char buf[120];
        std::snprintf(buf, sizeof buf, "[PUZZLE] message at %08X is not the inscription (%08X)", start, a1);
        h_.log(buf);
        return false;
    }
    std::vector<uint8_t> b = m.bytes(a1, a2 > a1 && a2 - a1 < 1024 ? a2 - a1 : 0);
    size_t k = 0;
    while (k < b.size() && b[k] != 0xFE) k++;
    if (k >= b.size() || a1 + k + 2 != a2) { h_.log("[PUZZLE] statue message layout not as expected"); return false; }
    // the space after it holds the next two messages (only used by the floor marks); they are
    // put back as soon as this message closes
    std::vector<uint8_t> next = m.bytes(a2, room_.message_bytes[kInscription + 1].size());
    if (next != room_.message_bytes[kInscription + 1]) { h_.log("[PUZZLE] next message differs; not asking"); return false; }
    size_t room_left = next.size() + 2;
    if (a3 == a2 + next.size()) room_left += room_.message_bytes[kInscription + 2].size();
    std::vector<uint8_t> patch{0xFD, 0x00};
    auto q = encode_text(kPushQuestion);
    patch.insert(patch.end(), q.begin(), q.end());
    for (uint8_t c : {0xFC, 0xFB, 0x20, 0xFE, 0x00}) patch.push_back(c);
    if (patch.size() > room_left) { h_.log("[PUZZLE] no room for the question"); return false; }
    uint32_t at = a1 + (uint32_t)k;
    puzzle_backup_ = m.bytes(at, patch.size());
    if (puzzle_backup_.size() != patch.size() || !m.write(at, patch.data(), patch.size())) {
        h_.log("[PUZZLE] could not add the question");
        return false;
    }
    puzzle_patch_addr_ = at;
    puzzle_state_ = 1;
    puzzle_cursor_ = 0;
    h_.log("[PUZZLE] statues: asking");
    return true;
}

void Mod::puzzle_restore() {
    if (puzzle_patch_addr_ && !puzzle_backup_.empty())
        h_.mem().write(puzzle_patch_addr_, puzzle_backup_.data(), puzzle_backup_.size());
    puzzle_patch_addr_ = 0;
    puzzle_backup_.clear();
}

void Mod::frame_ladder() {
    if (ladder_state_ == 0) return;
    uint64_t now = h_.ms();
    auto log = [&](const std::string& t) { h_.log("[PUZZLE] stepladder: " + t); };
    if (s_.room_code != "203") { puzzle_restore(); ladder_state_ = 0; return; }
    auto give_up = [&](const std::string& t) { say(t, true); log("gave up"); ladder_state_ = 0; };
    const SceneObject* ob = kLadderObj < (int)s_.objects.size() ? &s_.objects[kLadderObj] : nullptr;
    switch (ladder_state_) {
        case 1:   // asked: wait for the answer
            if (p_.message.active && !s_.message.active) {
                puzzle_restore();
                bool yes = !reader_.flag(11, 31) && puzzle_cursor_ == 0;
                log(std::string("answer ") + (yes ? "yes" : "no"));
                ladder_state_ = yes ? 2 : 0;
                ladder_ms_ = now;
                ladder_stepped_ = false;
            }
            break;
        case 2: {  // the message gone: put the ladder against the shelf and walk to it
            if (s_.stop_flg || s_.message.active || ui_mode_ != 0 || now - ladder_ms_ < 500) break;
            uint32_t o = addr::OBJ_WORK + kLadderObj * addr::OBJ_SIZE;
            int32_t x = (int32_t)kLadderX, z = (int32_t)kLadderZ;
            if (!h_.mem().write(o + addr::W_POS_X, &x, 4) || !h_.mem().write(o + addr::W_POS_Z, &z, 4)) {
                give_up("The stepladder could not be moved. Push it yourself: it stands in the east aisle; push it south until it stops.");
                break;
            }
            log("moved");
            say("The stepladder is under the crank now. Climbing it.");
            ladder_state_ = 3;
            ladder_ms_ = now;
            break;
        }
        case 3: {  // the ladder's new place is in the routes (next frame); walk to its north side
            if (now - ladder_ms_ < 200) break;
            Entry e;
            e.type = Entry::AOT; e.id = 2000; e.kind = 0; e.name = "the stepladder";
            e.pos = {kLadderStandX, kLadderStandZ}; e.walk_on = true;
            if (!plan_walk(e, false)) { give_up("Walk to the north side of the stepladder, face south and press action to climb it."); break; }
            ladder_state_ = 4;
            ladder_ms_ = now;
            break;
        }
        case 4:   // walking there; then face the ladder
            if (!walking_ && face_dir_ < 0) {
                double cx = ob ? ob->cx : kLadderX, cz = ob ? ob->cz : kLadderZ;
                start_turn(dir_towards(s_.x, s_.z, cx, cz), "the stepladder");
                ladder_state_ = 5;
                ladder_ms_ = now;
            } else if (now - ladder_ms_ > 20000) { stop_walk(); give_up("Could not get to the stepladder."); }
            break;
        case 5:   // facing it: climb
            if (face_dir_ < 0) { press_action_frames_ = 2; ladder_state_ = 6; ladder_ms_ = now; }
            break;
        case 6:   // climbing: wait to be on top
            if (s_.floor >= 1 && !s_.stop_flg) {
                log("on top");
                say("On the stepladder.");
                double cx = 0, cz = 0;
                for (auto& a : s_.aots) if (a.id == kCrankAot) { cx = a.centre.x; cz = a.centre.z; }
                start_turn(dir_towards(s_.x, s_.z, cx ? cx : s_.x, cz ? cz : s_.z - 1000), "the crank");
                ladder_state_ = 7;
                ladder_ms_ = now;
            } else if (now - ladder_ms_ > 5000) give_up("Could not climb the stepladder. Face it and press action.");
            break;
        case 7: {  // facing the crank: press action so the game asks
            if (face_dir_ >= 0) break;
            // the game looks 620 units ahead (bio2.exe 0x4E916C); if the crank is further, step
            // to the shelf end of the ladder's top first
            double fx, fz;
            facing_vector(s_.dir, fx, fz);
            const Aot* crank = nullptr;
            for (auto& a : s_.aots) if (a.id == kCrankAot) crank = &a;
            if (crank && !point_in_quad(s_.x + fx * 620, s_.z + fz * 620, crank->quad) && !ladder_stepped_ && ob) {
                ladder_stepped_ = true;
                walk_target_ = Entry(); walk_target_.type = Entry::AOT; walk_target_.id = 2001; walk_target_.walk_on = true;
                walk_target_.name = "the crank";
                walk_ = {Pt{ob->cx, ob->cz - ob->hz + 250}};
                walk_floor_ = s_.floor;
                walking_ = true;
                partial_ = false;
                walk_started_ms_ = now;
                log("stepping to the shelf end");
                ladder_state_ = 12;
                ladder_ms_ = now;
                break;
            }
            press_action_frames_ = 2; ladder_state_ = 8; ladder_ms_ = now;
            break;
        }
        case 12:  // stepped forward: face the crank again
            if (!walking_ && face_dir_ < 0) {
                double cx = s_.x, cz = s_.z - 1000;
                for (auto& a : s_.aots) if (a.id == kCrankAot) { cx = a.centre.x; cz = a.centre.z; }
                start_turn(dir_towards(s_.x, s_.z, cx, cz), "the crank");
                ladder_state_ = 7;
            } else if (now - ladder_ms_ > 4000) { stop_walk(); ladder_state_ = 7; }
            break;
        case 8:
            if (s_.message.active) { press_action_frames_ = 0; ladder_state_ = 9; ladder_ms_ = now; log("asked about the crank"); }
            else if (now - ladder_ms_ > 3000) give_up("Press action facing the shelf to take the crank.");
            break;
        case 9:   // the pick-up question and the item screen; then climb down
            if (s_.message.active || ui_mode_ != 0 || s_.stop_flg) { ladder_ms_ = now; break; }
            if (now - ladder_ms_ < 700) break;
            if (crank_on_shelf()) { give_up("The crank is still on the shelf. Press action to take it, or turn around and press action to climb down."); break; }
            start_turn(dir_towards(s_.x, s_.z, kLadderStandX, kLadderStandZ), "the way down");
            ladder_state_ = 10;
            break;
        case 10:
            if (face_dir_ < 0) { press_action_frames_ = 2; ladder_state_ = 11; ladder_ms_ = now; }
            break;
        case 11:
            if (s_.floor == 0 && !s_.stop_flg) { log("down"); say("Back on the floor."); ladder_state_ = 0; }
            else if (now - ladder_ms_ > 5000) give_up("Turn away from the shelf and press action to climb down.");
            break;
        default: ladder_state_ = 0; break;
    }
}

// The water tank (306): see frame_message. On Yes the boxes go to where the game's own check
// (main procedure 10) wants them, against the north wall; the fill then floats them up.
namespace {
const double kTankBoxes[3][2] = {{-20617, -26081}, {-22428, -26081}, {-24259, -26081}};
}

void Mod::frame_tank() {
    if (tank_state_ == 0) return;
    if (s_.room_code != "306") { tank_state_ = 0; return; }
    if (tank_state_ == 1 && p_.message.active && !s_.message.active) {
        bool yes = !reader_.flag(11, 31) && puzzle_cursor_ == 0;
        h_.log(std::string("[PUZZLE] water tank: answer ") + (yes ? "yes" : "no"));
        tank_state_ = 0;
        if (!yes) return;
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            uint32_t o = addr::OBJ_WORK + i * addr::OBJ_SIZE;
            int32_t x = (int32_t)kTankBoxes[i][0], z = (int32_t)kTankBoxes[i][1];
            ok = ok && h_.mem().write(o + addr::W_POS_X, &x, 4) && h_.mem().write(o + addr::W_POS_Z, &z, 4);
        }
        h_.log(std::string("[PUZZLE] water tank: boxes ") + (ok ? "lined up" : "could not be moved"));
        if (ok) say("The boxes are lined up along the far wall. When the tank has filled, walk to the Club key across them.");
        else say("The boxes could not be moved.", true);
    }
}

void Mod::frame_puzzle() {
    frame_ladder();
    frame_tank();
    if (puzzle_state_ == 0) return;
    uint64_t now = h_.ms();
    if (s_.room_code != "10C") {                     // left the room: give up quietly
        puzzle_restore();
        puzzle_state_ = 0;
        return;
    }
    auto log = [&](const std::string& t) { h_.log("[PUZZLE] " + t); };
    switch (puzzle_state_) {
        case 1:   // asked: wait for the answer
            if (p_.message.active && !s_.message.active) {
                puzzle_restore();
                bool yes = !reader_.flag(11, 31) && puzzle_cursor_ == 0;
                log(std::string("answer ") + (yes ? "yes" : "no"));
                if (!yes) { puzzle_state_ = 0; break; }
                say("Pushing the statues into place.");
                puzzle_state_ = 2;
                puzzle_ms_ = now;
            }
            break;
        case 2:   // the inscription event finishing
            if (!s_.stop_flg && !s_.message.active && ui_mode_ == 0 && now - puzzle_ms_ > 500) {
                puzzle_state_ = 3;
                puzzle_ms_ = now;
            } else if (now - puzzle_ms_ > 10000) { log("event did not end"); puzzle_state_ = 0; }
            break;
        case 3: {  // move the next statue that is not in place yet
            const StatueSpot* next = nullptr;
            for (auto& st : kStatues) if (!reader_.flag(st.flag_group, st.flag_index)) { next = &st; break; }
            if (!next) { puzzle_state_ = 5; puzzle_ms_ = now; puzzle_saw_stop_ = false; break; }
            uint32_t o = addr::OBJ_WORK + next->obj * addr::OBJ_SIZE;
            int32_t x = (int32_t)next->x, z = (int32_t)next->z;
            bool ok = h_.mem().write(o + addr::W_POS_X, &x, 4) && h_.mem().write(o + addr::W_POS_Z, &z, 4);
            log("statue " + std::to_string(next->obj) + (ok ? " moved" : " could not be moved"));
            if (!ok) { say("The statues could not be moved. Push them yourself.", true); puzzle_state_ = 0; break; }
            puzzle_state_ = 4;
            puzzle_ms_ = now;
            puzzle_obj_ = next - kStatues;
            break;
        }
        case 4: {  // wait for the game to accept it
            const StatueSpot& st = kStatues[puzzle_obj_];
            if (reader_.flag(st.flag_group, st.flag_index)) { puzzle_state_ = 3; puzzle_ms_ = now; }
            else if (now - puzzle_ms_ > 4000) {
                char buf[160];
                const SceneObject* ob = st.obj < (int)s_.objects.size() ? &s_.objects[st.obj] : nullptr;
                std::snprintf(buf, sizeof buf, "statue %d not accepted: at (%.0f,%.0f) floor %d, in AOT %d, active %d", st.obj,
                              ob ? ob->x : 0, ob ? ob->z : 0, ob ? ob->floor : -1, ob ? ob->aot_hit : -1, ob ? ob->active : 0);
                log(buf);
                say("The statue did not lock into place. You may need to push it yourself.", true);
                puzzle_state_ = 0;
            }
            break;
        }
        case 5: {  // the jewel is released (a short cutscene)
            // the jewel cutscene (procedures 6-8) freezes the player with flags 2.7 and 1.27 and
            // only gives control back at its very end; wait for that, then a moment more
            if (s_.stop_flg || event_running()) { puzzle_saw_stop_ = true; puzzle_idle_ms_ = now; }
            const Aot* jewel = nullptr;
            for (auto& a : s_.aots) if (a.id == kJewelAot && a.sce == SCE_ITEM && a.item == kJewelItem) jewel = &a;
            if (jewel && !s_.stop_flg && !event_running() && !s_.message.active && ui_mode_ == 0 &&
                now - puzzle_ms_ > 800 && now - puzzle_idle_ms_ > 1000) {
                log("jewel released");
                Entry e;
                e.type = Entry::AOT; e.id = jewel->id; e.kind = jewel->sce; e.name = describe(*jewel, false);
                e.pos = jewel->centre; e.quad = jewel->quad; e.floor = jewel->floor;
                say("The statues are in place.");
                bool here = false;
                for (auto* a : actionable()) if (a->id == jewel->id) here = true;
                if (here) { puzzle_state_ = 6; puzzle_ms_ = now; }
                else if (plan_walk(e, false)) { puzzle_state_ = 6; puzzle_ms_ = now; }
                else { say("Walk to the Red jewel to take it."); puzzle_state_ = 0; }
            } else if (now - puzzle_ms_ > 30000) { log("jewel not released"); puzzle_state_ = 0; }
            break;
        }
        case 6:   // walking to the jewel; then press action so the game asks
            if (!walking_ && face_dir_ < 0) {
                press_action_frames_ = 2;
                puzzle_state_ = 7;
                puzzle_ms_ = now;
            } else if (now - puzzle_ms_ > 20000) { stop_walk(); puzzle_state_ = 0; }
            break;
        case 7:
            if (s_.message.active) press_action_frames_ = 0;
            if (s_.message.active || now - puzzle_ms_ > 2000) {
                log(s_.message.active ? "asked to take the jewel" : "no pick-up question");
                puzzle_state_ = 0;
            }
            break;
        default: puzzle_state_ = 0; break;
    }
}

// ---------------------------------------------------------------- hotkeys
void Mod::hotkey(Hotkey k) {
    if (term_active_ && k != HK_REPEAT && k != HK_HELP && k != HK_VERSION && k != HK_DIAG) return;   // typing a name
    if (k == HK_REPEAT) { if (!last_said_.empty()) h_.speak(last_said_, true); return; }
    if (k == HK_HELP && game_map_open_ && map_mode_) { map_hotkey(k); return; }
    if (k == HK_HELP) { say(hotkey_help(), true); return; }
    if (k == HK_VERSION) { say(std::string("RE2 accessibility mod version ") + RE2ACCESS_VERSION, true); return; }
    if (k == HK_DIAG) { key_diag(); return; }
    if (k == HK_UNLIMITED_HEALTH) { toggle_assist(true); return; }
    if (k == HK_UNLIMITED_AMMO) { toggle_assist(false); return; }
    if (!s_.valid) { say("Game not ready.", true); return; }
    if (k == HK_HEALTH) { key_health(); return; }
    if (k == HK_TIMER) {
        if (cd_on_ && cd_up_) say("Time so far: " + countdown_text(cd_var_) + ".", true);
        else if (cd_on_ && cd_secs_ >= 0) say(countdown_text((int)std::ceil(cd_secs_)) + " left.", true);
        else if (cd_secs_ > 0) say("Countdown stopped for now, " + countdown_text((int)std::ceil(cd_secs_)) + " left.", true);
        else say("No countdown running.", true);
        return;
    }
    if (k == HK_INVENTORY) { key_inventory(); return; }
    // the game's map screen: the mod's map keys work there
    if (game_map_open_) {
        if (k == HK_MAP_MODE) { key_map_mode(); return; }
        if (map_mode_ && map_hotkey(k)) return;
    }
    if (!s_.in_game || ui_mode_ != 0) {
        if (s_.message.active && !msg_text_.empty()) { say(msg_text_, true); return; }
        say("Not available on this screen.", true);
        return;
    }
    ensure_room();
    if (k == HK_MAP) { key_map_here(); return; }
    if (k == HK_MAP_MODE) { key_map_mode(); return; }
    if (map_hotkey(k)) return;
    switch (k) {
        case HK_NEXT: cycle(1); break;
        case HK_PREV: cycle(-1); break;
        case HK_CAT_NEXT: case HK_CAT_PREV:
            category_ = (category_ + (k == HK_CAT_NEXT ? 1 : kCategoryCount - 1)) % kCategoryCount;
            sel_id_ = -1;
            {
                auto n = entries(category_).size();
                say(std::string(kCategories[category_]) + ", " + std::to_string(n), true);
            }
            break;
        case HK_WHERE: key_where(); break;
        case HK_GO: key_go(); break;
        case HK_FACE: key_face(); break;
        case HK_TARGET: key_target(false); break;
        case HK_TARGET_NEXT: key_target(true); break;
        case HK_ROOM: announce_room(false); break;
        case HK_ENEMIES: key_enemies(); break;
        case HK_SCAN: key_scan(); break;
        case HK_COMPASS: say("Facing " + compass(s_.dir) + ".", true); break;
        case HK_EXPLORE: key_explore(); break;
        case HK_GET_AWAY: key_get_away(); break;
        default: break;
    }
}

void Mod::key_where() {
    Entry e;
    if (!selected(e)) { say("Nothing selected.", true); return; }
    say(e.name + ": " + locate(e.pos) + ".", true);
}

// The escape countdowns (Leon and Claire's lab and train, 617/701/703/704): a script sets status
// flag 1.4 (the countdown runs) and script variable 35 to the seconds left (save(35, 300), 703's
// save(35, 120)); scripts clear 1.4 while a scene plays. The status screen shows variable 35 as the
// time (bio2.exe 0x4B6E48). If the game counts the variable down the mod follows it; if it does
// not, the mod counts down itself while the game is being played.
std::string Mod::countdown_text(int secs) const {
    if (secs < 0) secs = 0;
    int m = secs / 60, sec = secs % 60;
    std::string t;
    if (m) t = std::to_string(m) + (m == 1 ? " minute" : " minutes");
    if (sec || !m) t += (t.empty() ? "" : " ") + std::to_string(sec) + (sec == 1 ? " second" : " seconds");
    return t;
}

// The results screens at the end of a scenario (bio2.exe ending task 0x4B57C0; text drawn by
// 0x4B69B0). Every line on them is an entry of a 33-entry table at RES_TABLE (12 bytes each:
// flag byte at +0, bit 0 = shown; +3 = string number; +4/+6 = x, y). The game sets the flags
// as each line fades in, so a line is read when its flag turns on. Values: rank RES_RANK
// (u16, 0 S .. 4 D), hours/minutes/seconds RES_HOURS.. and saves RES_SAVES (bio2.exe 0x4B6E10).
// The same screens follow the 4th Survivor and To-fu endings (labels 31 / 32).
std::string Mod::result_line(int i) {
    MemoryReader& m = h_.mem();
    static const char* kRank = "SABCD";
    static const char* kWeapon[] = {"Sub machine gun", "Rocket launcher", "Gatling gun"};
    switch (i) {
        case 0: case 2: return "Agent survived and successfully obtained the G virus. The virus went into the hands of Umbrella.";
        case 1: case 3: return "End of one nightmare, prelude to another.";
        case 4: case 5: case 6: case 7: case 8: return std::string("Ranking: ") + kRank[i - 4] + ".";
        case 9: return "Clear game: Leon, scenario A.";
        case 10: return "Clear game: Claire, scenario A.";
        case 11: return "Clear game: Leon, scenario B.";
        case 12: return "Clear game: Claire, scenario B.";
        case 13: return "Congratulations!";
        case 16: {
            int h = m.u8(addr::RES_HOURS), mi = m.u8(addr::RES_HOURS + 1), sec = m.u8(addr::RES_HOURS + 2);
            if (h > 98) return "Total time: over 99 hours.";
            std::string t;
            if (h) t = std::to_string(h) + (h == 1 ? " hour " : " hours ");
            t += std::to_string(mi) + (mi == 1 ? " minute " : " minutes ") + std::to_string(sec) + (sec == 1 ? " second" : " seconds");
            return "Total time: " + t + ".";
        }
        case 17: {
            if (s_.status_flg & 0x2000000) return "Number of saves: not counted.";
            int n = m.u8(addr::RES_SAVES);
            return n > 98 ? "Number of saves: over 99." : "Number of saves: " + std::to_string(n) + ".";
        }
        case 18: case 19: case 20: return std::string(kWeapon[i - 18]) + ".";
        case 21: case 22: return "You will receive fewer points by using a special weapon.";
        case 23: return "Weapon get!";
        case 24: return "Check the item box!";
        case 25: case 26: return "Make new save data to play the other scenario.";
        case 27: return "S.T.A.R.S. member Brad became a living dead. He's roaming around somewhere in the police station possessing the special key. Find him and help him rest in peace.";
        case 28: return "Reach the police station without getting any items!";
        case 31: return "Clear game: 4th Survivor.";
        case 32: return "Clear game: To-fu.";
        default: return "";   // 14 RANKING, 15 CLEAR GAME, 29/30 Japanese copies of 27/28
    }
}

void Mod::frame_results() {
    MemoryReader& m = h_.mem();
    std::string said;
    for (int i = 0; i < addr::RES_COUNT; i++) {
        bool on = (m.u8(addr::RES_TABLE + i * addr::RES_SIZE) & 1) != 0;
        if (on && !res_on_[i] && res_ready_) {
            std::string t = result_line(i);
            char b[64];
            std::snprintf(b, sizeof b, "[RESULT] entry %d shown", i);
            h_.log(b + (t.empty() ? std::string() : ": " + t));
            // a line and its Japanese twin can come up together; say each text once
            if (!t.empty() && said.find(t) == std::string::npos && (h_.ms() - res_ms_ > 3000 || t != res_last_)) {
                say(t);
                said += t;
                res_last_ = t;
                res_ms_ = h_.ms();
            }
        }
        res_on_[i] = on;
    }
    res_ready_ = true;   // lines already up when the mod starts are not read
}

void Mod::frame_countdown() {
    uint64_t now = h_.ms();
    bool on = s_.in_game && reader_.flag(1, 4);
    int v = h_.mem().s16(addr::SCE_VARS + 35 * 2);
    char buf[120];
    if (on && !cd_on_) {
        cd_on_ = true;
        cd_ms_ = now;
        if (v > 0 && (cd_secs_ < 0 || v != cd_var_)) { cd_secs_ = v; cd_said_ = -1; }
        cd_var_ = v;
        std::snprintf(buf, sizeof buf, "[TIMER] countdown on: var 35 = %d, counting from %.0f", v, cd_secs_);
        h_.log(buf);
        if (cd_secs_ > 0 && cd_said_ < 0) {
            cd_said_ = (int)std::ceil(cd_secs_);
            say("Countdown: " + countdown_text(cd_said_) + " left. Y: time left.");
        }
        return;
    }
    if (!on) {
        if (cd_on_) {
            cd_on_ = false;
            std::snprintf(buf, sizeof buf, "[TIMER] countdown stopped: var 35 = %d, %.0f left", v, cd_secs_);
            h_.log(buf);
        }
        if (!s_.in_game) { cd_secs_ = -1; cd_var_ = -1; cd_var_live_ = false; cd_up_ = false; }   // back at the title
        return;
    }
    // running
    if (v != cd_var_) {
        if (cd_var_ >= 0 && v > cd_var_ && v - cd_var_ <= 2) {
            // the variable goes up a second at a time: a clock (the 4th Survivor and To-fu modes
            // run flag 1.4 with variable 35 counting up from 0), not a countdown. Never spoken.
            if (!cd_up_) {
                cd_up_ = true;
                h_.log("[TIMER] var 35 counts up: a clock, not a countdown");
            }
            cd_secs_ = v;
            cd_said_ = -1;
        } else if (cd_var_ > 0 && v >= 0 && v < cd_var_ && cd_var_ - v <= 2) {
            cd_var_live_ = true;            // the game counts the variable down: follow it
            cd_secs_ = v;
        } else if (v > 0 && !cd_up_) {
            cd_secs_ = v;                   // a script set a new time
            cd_said_ = -1;
            say("Countdown: " + countdown_text(v) + " left.");
            cd_said_ = v;
        }
        cd_var_ = v;
    } else if (!cd_up_ && !cd_var_live_ && ui_mode_ == 0 && cd_secs_ > 0) {
        cd_secs_ = std::max(0.0, cd_secs_ - (now - cd_ms_) / 1000.0);
    }
    cd_ms_ = now;
    if (now - cd_log_ms_ > 10000) {
        cd_log_ms_ = now;
        std::snprintf(buf, sizeof buf, "[TIMER] var 35 = %d, %.0f %s (%s)", v, cd_secs_, cd_up_ ? "on the clock" : "left",
                      cd_up_ ? "counting up" : cd_var_live_ ? "the game's" : "the mod's count");
        h_.log(buf);
    }
    // spoken at each whole minute, then at 30 and 10 seconds (queued, not cutting speech off)
    int r = (int)std::ceil(cd_secs_);
    if (!cd_up_ && cd_said_ > 0 && r < cd_said_) {
        static const int kMarks[] = {600, 540, 480, 420, 360, 300, 240, 180, 120, 60, 30, 10};
        for (int t : kMarks)
            if (cd_said_ > t && r <= t) {
                say(countdown_text(t) + " left.");
                break;
            }
        cd_said_ = r;
    }
}

std::string Mod::condition_text() {
    int lv = condition_level(s_.hp, s_.max_hp, s_.poisoned);
    return condition_word(lv) + ", " + condition_colour(lv);
}

// The character being played, by the player's model (OBJ W_ID; 0-9 are Leon's and Claire's
// costumes, even Leon, odd Claire; 11 Chris, 12 Hunk, 13 Tofu, 14 Ada, 15 Sherry), else PL_ID.
std::string Mod::player_name() {
    int model = h_.mem().u8(addr::PLAYER + addr::W_ID);
    if (model <= 9) return model & 1 ? "Claire" : "Leon";
    switch (model) {
        case 11: return "Chris";
        case 12: return "Hunk";
        case 13: return "Tofu";
        case 14: return "Ada";
        case 15: return "Sherry";
    }
    static const char* kWho[] = {"Leon", "Claire", "Leon", "Claire", "Leon", "Claire", "Hunk", "Tofu", "Ada", "Sherry"};
    int pl = h_.mem().u8(addr::PL_ID);
    return pl >= 0 && pl < 10 ? kWho[pl] : "Leon";
}

void Mod::key_health() {
    // only what the status screen's ECG shows: the condition and its colour, no numbers
    say(condition_text() + ".", true);
}

void Mod::key_inventory() {
    std::vector<std::string> parts;
    for (auto& it : s_.inventory) {
        if (!it.id || it.part == 2) continue;        // a two-slot weapon is listed once
        std::string t = item_name(it.id);
        if ((it.count > 1 && it.id <= 30) || (it.id >= 2 && it.id <= 19)) t += " " + std::to_string(it.count);
        if (it.id == s_.equipped) t += ", equipped";
        parts.push_back(t);
    }
    if (parts.empty()) { say("Inventory empty.", true); return; }
    std::string t;
    for (size_t i = 0; i < parts.size(); i++) t += (i ? "; " : "") + parts[i];
    say(t + ".", true);
}

// " below you" / " above you" for an enemy on another level (a balcony, a stairwell)
std::string Mod::level_note(const Enemy& e) const {
    double dy = e.y - s_.y;                // the game's y grows downwards
    if (dy > 1500) return ", below you";
    if (dy < -1500) return ", above you";
    return "";
}

// nearest first, but enemies on the player's own level before those above or below
static double enemy_rank(const Enemy& e, double x, double y, double z) {
    return std::hypot(e.x - x, e.z - z) + (std::fabs(e.y - y) > 1500 ? 1e6 : 0);
}

void Mod::key_enemies() {
    std::vector<const Enemy*> es;
    for (auto& e : s_.enemies) if (e.alive && e.hostile() && in_play(e)) es.push_back(&e);
    if (es.empty()) { say("No enemies detected.", true); return; }
    std::sort(es.begin(), es.end(), [&](const Enemy* a, const Enemy* b) {
        return enemy_rank(*a, s_.x, s_.y, s_.z) < enemy_rank(*b, s_.x, s_.y, s_.z);
    });
    std::string t = std::to_string(es.size()) + (es.size() == 1 ? " enemy: " : " enemies: ");
    for (size_t i = 0; i < es.size() && i < 5; i++)
        t += (i ? "; " : "") + es[i]->name() + " " + locate({es[i]->x, es[i]->z}) + level_note(*es[i]);
    if (es.size() > 5) t += "; and " + std::to_string(es.size() - 5) + " more";
    say(t + ".", true);
}

void Mod::key_scan() {
    static const char* labels[] = {"ahead", "right", "behind", "left"};
    std::string t = "Walls: ";
    std::vector<int> fl{s_.floor};
    for (int k = 0; k < 4; k++) {
        double fx, fz;
        facing_vector(s_.dir + k * 1024, fx, fz);
        int dist = -1;
        for (int step = 1; step < 40; step++)
            if (blocking_at(room_.collisions, fl, s_.x + fx * step * 250, s_.z + fz * step * 250, 150)) { dist = step * 250; break; }
        t += std::string(k ? ", " : "") + labels[k] + " " + (dist > 0 ? metres(dist) : "open");
    }
    say(t + ".", true);
}

void Mod::key_diag() {
    char buf[256];
    std::snprintf(buf, sizeof buf, "DIAG room=%s cut=%d pos=(%.0f,%.0f,%.0f) dir=%d floor=%d hp=%d/%d aots=%zu enemies=%zu pl=%d model=%d",
                  s_.room_code.c_str(), s_.cut, s_.x, s_.y, s_.z, s_.dir, s_.floor, s_.hp, s_.max_hp, s_.aots.size(),
                  s_.enemies.size(), h_.mem().u8(addr::PL_ID), h_.mem().u8(addr::PLAYER + addr::W_ID));
    h_.log(buf);
    for (auto& a : s_.aots) {
        std::snprintf(buf, sizeof buf, "DIAG aot %d sce=%d sat=%02X floor=%d centre=(%.0f,%.0f) door=%d/%d lock=%d/%d item=%d x%d %s",
                      a.id, a.sce, a.sat, a.floor, a.centre.x, a.centre.z, a.door_stage, a.door_room, a.lock_id,
                      a.lock_type, a.item, a.amount, describe(a, false).c_str());
        h_.log(buf);
    }
    for (auto& e : s_.enemies) {
        std::snprintf(buf, sizeof buf, "DIAG enemy slot %d type %02X %s pos=(%.0f,%.0f) hp=%d/%d", e.slot, e.type,
                      e.name().c_str(), e.x, e.z, e.hp, e.max_hp);
        h_.log(buf);
    }
    for (size_t i = 0; i < s_.inventory.size(); i++) {
        std::snprintf(buf, sizeof buf, "DIAG inv %zu id=%d count=%d %s", i, s_.inventory[i].id, s_.inventory[i].count,
                      item_name(s_.inventory[i].id).c_str());
        h_.log(buf);
    }
    say("Diagnostic snapshot saved to the log.", true);
}

// ---------------------------------------------------------------- movement
bool Mod::crate_up() const {
    if (s_.room_code != "603" || reader_.flag(4, kCrateFlag) || kCrateObj >= (int)s_.objects.size()) return false;
    const SceneObject& ob = s_.objects[kCrateObj];
    return ob.active && ob.y < -3000;
}

void Mod::drop_crate() {
    stop_walk();
    if (!crate_up()) { say("Arrived.", true); return; }
    uint32_t o = addr::OBJ_WORK + kCrateObj * addr::OBJ_SIZE;
    int32_t x = (int32_t)kCrateX, y = 0, z = (int32_t)kCrateZ;
    uint8_t fl = 0;
    MemoryReader& m = h_.mem();
    if (!m.write(o + addr::W_POS_X, &x, 4) || !m.write(o + addr::W_POS_Y, &y, 4) || !m.write(o + addr::W_POS_Z, &z, 4) ||
        !m.write(o + addr::W_FLOOR, &fl, 1)) {
        say("The crate could not be moved. Push it east along the walkway and off the end.", true);
        return;
    }
    h_.log("[PUZZLE] 603 crate: dropped to the floor below");
    say(std::string("Pushed the crate off the end of the walkway; it fell to the floor below, by the steps. ") +
        (s_.floor >= 3 ? "Take the lift down, then pick Door to Power Room U: " : "Pick Door to Power Room U: ") +
        "auto-walk climbs onto the crate and up the steps to it.", true);
}

void Mod::start_push() {
    walking_ = false;
    walk_.clear();
    push_to_ = walk_target_.pos;
    push_frames_ = 120;
    say("Pushing.", true);
}

void Mod::start_turn(int dir, const std::string& label) {
    walking_ = false;
    walk_.clear();
    face_dir_ = dir;
    face_label_ = label;
    walk_started_ms_ = h_.ms();
}

void Mod::stop_walk(const std::string& why) {
    bool was = walking_ || face_dir_ >= 0;
    stair_action_pending_ = false;
    walking_ = false;
    getaway_ = false;
    push_frames_ = 0;
    partner_wait_ = false;
    nudge_frames_ = 0;
    walk_.clear();
    face_dir_ = -1;
    if (was && !why.empty()) say(why, true);
}

void Mod::key_face() {
    Entry e;
    if (!selected(e)) { say("Nothing selected.", true); return; }
    start_turn(dir_towards(s_.x, s_.z, e.pos.x, e.pos.z), e.name);
    say("Turning to " + e.name + ".", true);
}

void Mod::key_target(bool next) {
    std::vector<const Enemy*> es;
    for (auto& e : s_.enemies) if (e.alive && e.hostile() && in_play(e)) es.push_back(&e);
    if (es.empty()) { say("No enemies.", true); return; }
    std::sort(es.begin(), es.end(), [&](const Enemy* a, const Enemy* b) {
        return enemy_rank(*a, s_.x, s_.y, s_.z) < enemy_rank(*b, s_.x, s_.y, s_.z);
    });
    size_t pick = 0;
    if (next) {
        for (size_t i = 0; i < es.size(); i++)
            if (es[i]->slot == target_slot_) { pick = (i + 1) % es.size(); break; }
    }
    const Enemy* e = es[pick];
    target_slot_ = e->slot;          // separate from the catalog selection, so Go still goes there
    say(e->name() + ", " + locate({e->x, e->z}) + level_note(*e) + ". " + std::to_string(pick + 1) + " of " + std::to_string(es.size()) + ".", true);
    start_turn(dir_towards(s_.x, s_.z, e->x, e->z), e->name());
}

// Stairs (shape 12) and slopes (shape 11) join floors: they rise (type >> 6) & 31 floors from the
// lowest floor in their mask; (type >> 4) & 3 says which edge is the low one.
std::vector<Mod::Stairway> Mod::stairways() const {
    std::vector<Stairway> out;
    for (const auto& c : room_.collisions) {
        if ((c.shape != 11 && c.shape != 12) || !c.floor_mask) continue;
        if (!(c.raw_flag & PLAYER_BLOCK_BIT)) continue;     // switched off by a script (111 before the crank)
        Stairway w;
        w.index = c.index;
        w.stairs = c.shape == 12;
        while (w.low < 31 && !(c.floor_mask & (1u << w.low))) w.low++;
        w.high = w.low + stair_rise(c);
        int kind = (c.raw_type >> 4) & 3;
        double cx = c.x + c.w / 2.0, cz = c.z + c.d / 2.0;
        w.low_end = {cx, cz}; w.high_end = {cx, cz};
        if (kind == 0) { w.low_end.x = c.x - 400; w.high_end.x = c.x + c.w + 400; }
        else if (kind == 1) { w.low_end.x = c.x + c.w + 400; w.high_end.x = c.x - 400; }
        else if (kind == 2) { w.low_end.z = c.z - 400; w.high_end.z = c.z + c.d + 400; }
        else { w.low_end.z = c.z + c.d + 400; w.high_end.z = c.z - 400; }
        out.push_back(w);
    }
    // climbing: shape 9 boxes are climbed onto from their floor, shape 10 ledges dropped from
    // (bio2.exe 0x4CDE9A; the player's bit 0x8000). Each side the player can face gives one
    // way across: stand outside the side, face in, press action, land about a metre further.
    double r = (s_.body_r > 0 ? s_.body_r : PLAYER_RADIUS) + 20;
    std::vector<Collision> climbs = climb_cols();
    for (const auto& c : climbs) {
        if ((c.shape != 9 && c.shape != 10) || !c.climbable() || !c.floor_mask) continue;
        int f = 0;
        while (f < 31 && !(c.floor_mask & (1u << f))) f++;
        int from = f, to = c.shape == 9 ? f + 1 : f - 1;
        if (to < 0) continue;
        double cx = c.x + c.w / 2.0, cz = c.z + c.d / 2.0;
        const double dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++) {
            const double* d = dirs[k];
            // the approach: just outside the side the player walks in from; along a long edge
            // part of it may be behind a wall (306's tank ledge), so try along the edge too
            std::vector<int> ff{from};
            Pt a{0, 0};
            bool free_side = false;
            for (double t : {0.0, -0.25, 0.25, -0.4, 0.4}) {
                double along = t * (d[0] != 0 ? c.d : c.w);
                Pt q{cx - d[0] * (c.w / 2.0 + r + 150) + (d[0] == 0 ? along : 0),
                     cz - d[1] * (c.d / 2.0 + r + 150) + (d[0] != 0 ? along : 0)};
                if (!blocking_at(room_.collisions, ff, q.x, q.z, r - 80)) { a = q; free_side = true; break; }
            }
            if (!free_side) continue;
            Pt l{0, 0};
            bool ok = false;
            for (double dist : {900.0, 1200.0, 1500.0, 700.0, 1800.0, 2200.0}) {
                Pt q{a.x + d[0] * dist, a.z + d[1] * dist};
                std::vector<int> tf{to};
                if (!blocking_at(room_.collisions, tf, q.x, q.z, r - 80)) { l = q; ok = true; break; }
            }
            if (!ok) continue;
            Stairway w;
            w.index = 1000 + c.index * 4 + k;
            w.stairs = false;
            if (to > from) { w.low = from; w.high = to; w.low_end = a; w.high_end = l; w.oneway = 1; }
            else { w.low = to; w.high = from; w.low_end = l; w.high_end = a; w.oneway = -1; }
            out.push_back(w);
        }
    }
    return out;
}

std::vector<int> Mod::stair_floors() {
    std::set<int> fl;
    for (auto& w : stairways()) { fl.insert(w.low); fl.insert(w.high); }
    std::vector<int> v(fl.begin(), fl.end());
    std::sort(v.begin(), v.end(), [&](int a, int b) { return std::abs(a - s_.floor) < std::abs(b - s_.floor); });
    return v;
}

Pt Mod::stand_point(const Entry& e, int floor) {
    // a free spot inside the trigger area, closest to its centre, that the player can get to
    std::vector<int> fl{floor};
    double r = (s_.body_r > 0 ? s_.body_r : PLAYER_RADIUS) - 60;
    double mnx = 1e9, mxx = -1e9, mnz = 1e9, mxz = -1e9;
    for (auto& p : e.quad) { mnx = std::min(mnx, p.x); mxx = std::max(mxx, p.x); mnz = std::min(mnz, p.z); mxz = std::max(mxz, p.z); }
    if (e.quad.empty()) { mnx = mxx = e.pos.x; mnz = mxz = e.pos.z; }
    // where the player can walk from here (only known for the level the player is on)
    std::set<std::pair<int, int>> reach;
    NavGrid* g = nullptr;
    if (floor == s_.floor) {
        g = &grid(floor, 2);
        for (auto& c : g->reachable(s_.x, s_.z)) reach.insert(c);
    }
    auto reachable = [&](double x, double z) {
        if (!g || reach.empty()) return true;
        auto c = g->to_cell(x, z);
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++)
                if (reach.count({c.first + di, c.second + dj})) return true;
        return false;
    };
    // well inside the trigger area (the game's check is strict, and the player drifts a little
    // when stopping), else anywhere inside it
    // the game checks the lower AOT number first: where another spot covers this one (401's
    // warehouse door over the ladder down), stand where only this one is, when there is room
    std::vector<const Aot*> over;
    if (e.type == Entry::AOT)
        for (auto& a : s_.aots)
            if (a.id < e.id && a.player_triggers() && a.manual() && a.quad.size() >= 3 &&
                (a.floor >= 0x80 || e.floor < 0 || a.floor == e.floor) && !(a.sce == SCE_ITEM && a.item_taken))
                over.push_back(&a);
    auto shadowed = [&](double x, double z) {
        for (auto* a : over) if (point_in_quad(x, z, a->quad)) return true;
        return false;
    };
    // a lift's switch is worked from on the lift: its "standing on it" zone (603: a flag zone
    // under the player, sat 0x40, that the switch's event checks) must hold the spot too
    if (e.type == Entry::AOT && e.name.rfind("Lift", 0) == 0) {
        const Aot* ride = nullptr;
        for (auto& a : s_.aots)
            if (a.sce == SCE_FLAG_CHG && (a.sat & 0x40) && a.data[0] != 10 && a.quad.size() >= 3 &&
                (a.floor >= 0x80 || a.floor == floor) && std::hypot(a.centre.x - e.pos.x, a.centre.z - e.pos.z) < 2500)
                ride = &a;
        if (ride) {
            double best = 1e18; Pt bp = e.pos;
            for (double x = mnx - 1500; x <= mxx + 1500; x += 50)
                for (double z = mnz - 1500; z <= mxz + 1500; z += 50) {
                    // well inside it: the player drifts a little when stopping
                    bool deep = point_in_quad(x - 150, z, ride->quad) && point_in_quad(x + 150, z, ride->quad) &&
                                point_in_quad(x, z - 150, ride->quad) && point_in_quad(x, z + 150, ride->quad);
                    if (!deep || blocking_at(room_.collisions, fl, x, z, r) || !reachable(x, z)) continue;
                    double d = std::hypot(x - e.pos.x, z - e.pos.z);
                    if (d < best) { best = d; bp = {x, z}; }
                }
            if (best < 1e18) return bp;
        }
    }
    for (int pass = 0; pass < 4; pass++) {
        double margin = (pass % 2 == 0) ? 200.0 : 0.0;
        bool avoid_over = pass < 2;
        if (avoid_over && over.empty()) continue;
        double best = 1e18; Pt bp = e.pos;
        for (double x = mnx; x <= mxx; x += 50)
            for (double z = mnz; z <= mxz; z += 50) {
                if (avoid_over && shadowed(x, z)) continue;
                if (!e.quad.empty()) {
                    if (!point_in_quad(x, z, e.quad)) continue;
                    if (margin > 0 && !(point_in_quad(x - margin, z, e.quad) && point_in_quad(x + margin, z, e.quad) &&
                                        point_in_quad(x, z - margin, e.quad) && point_in_quad(x, z + margin, e.quad)))
                        continue;
                }
                if (blocking_at(room_.collisions, fl, x, z, r)) continue;
                if (!reachable(x, z)) continue;
                double d = std::hypot(x - e.pos.x, z - e.pos.z);
                if (d < best) { best = d; bp = {x, z}; }
            }
        if (best < 1e18) return bp;
    }
    // the trigger sits inside a wall (most doors): the reachable free spot closest to it
    for (int pass = 0; pass < 2; pass++) {
        double best = 1e18; Pt bp = e.pos;
        for (double x = mnx - 1500; x <= mxx + 1500; x += 50)
            for (double z = mnz - 1500; z <= mxz + 1500; z += 50)
                if (!blocking_at(room_.collisions, fl, x, z, r) && (pass == 1 || reachable(x, z))) {
                    double dx = std::max({mnx - x, 0.0, x - mxx}), dz = std::max({mnz - z, 0.0, z - mxz});
                    double d = std::hypot(dx, dz) * 10 + std::hypot(x - e.pos.x, z - e.pos.z) * 0.01;
                    if (d < best) { best = d; bp = {x, z}; }
                }
        if (best < 1e18) return bp;
    }
    return e.pos;
}

// Damage areas the player can walk into (fire in the 2nd-floor helicopter corridor and the like).
std::vector<const Aot*> Mod::hazards() const {
    std::vector<const Aot*> out;
    for (auto& a : s_.aots)
        if (a.sce == SCE_DAMAGE && (a.sat & SAT_PL) && !a.quad.empty()) out.push_back(&a);
    return out;
}

double Mod::hazard_distance(const Aot& a, double x, double z) const {
    if (point_in_quad(x, z, a.quad)) return 0;
    double best = 1e18;
    for (size_t i = 0; i < a.quad.size(); i++) {
        Pt p = a.quad[i], q = a.quad[(i + 1) % a.quad.size()];
        double dx = q.x - p.x, dz = q.z - p.z, l2 = dx * dx + dz * dz;
        double t = l2 > 0 ? std::max(0.0, std::min(1.0, ((x - p.x) * dx + (z - p.z) * dz) / l2)) : 0;
        best = std::min(best, std::hypot(x - (p.x + t * dx), z - (p.z + t * dz)));
    }
    return best;
}

std::vector<std::pair<Pt, double>> Mod::hazard_circles() const {
    std::vector<std::pair<Pt, double>> out;
    double body = s_.body_r > 0 ? s_.body_r : PLAYER_RADIUS;
    for (auto* a : hazards()) {
        double r = 0;
        for (auto& p : a->quad) r = std::max(r, std::hypot(p.x - a->centre.x, p.z - a->centre.z));
        out.push_back({a->centre, r * 0.8 + body});
    }
    return out;
}

// Does the way (from the player) touch a damage area?
bool Mod::path_hazard(const std::vector<Pt>& path) const {
    auto hz = hazards();
    if (hz.empty()) return false;
    double body = (s_.body_r > 0 ? s_.body_r : PLAYER_RADIUS) * 0.7;
    Pt prev{s_.x, s_.z};
    for (auto& q : path) {
        double len = std::hypot(q.x - prev.x, q.z - prev.z);
        int n = std::max(1, int(len / 100));
        for (int k = 0; k <= n; k++) {
            double x = prev.x + (q.x - prev.x) * k / n, z = prev.z + (q.z - prev.z) * k / n;
            for (auto* a : hz) if (hazard_distance(*a, x, z) < body) return true;
        }
        prev = q;
    }
    return false;
}

// Active enemies to walk around (not the one we are walking towards).
std::vector<std::pair<Pt, double>> Mod::danger_circles(const Entry& target) const {
    std::vector<std::pair<Pt, double>> out;
    for (auto& e : s_.enemies) {
        if (!e.alive || !e.hostile() || !in_play(e) || lying_still(e)) continue;
        if (target.type == Entry::ENEMY && target.id == e.slot) continue;
        // one that is right on top of the target (the G embryo inside the G creature) is the target too
        if (target.type == Entry::ENEMY && std::hypot(e.x - target.pos.x, e.z - target.pos.z) < 1000) continue;
        if (std::hypot(e.x - s_.x, e.z - s_.z) > 15000) continue;
        // unarmed (Sherry) keeps further away: the way round is better than a grab
        out.push_back({{e.x, e.z}, s_.equipped == 0 ? kEnemyKeepAway * 1.5 : kEnemyKeepAway});
    }
    return out;
}

// The floor a catalog entry is on: AOTs carry their floor; the rest are on the player's.
int Mod::goal_floor(const Entry& e) const {
    if (e.type != Entry::AOT || e.floor < 0 || e.floor > 31) return s_.floor;
    // only trust the AOT floor when the room has something on that floor
    bool used = false;
    for (auto& c : room_.collisions) if (c.on_floor(e.floor)) { used = true; break; }
    return used ? e.floor : s_.floor;
}

// Finds a way to the entry: on this floor with the normal grid, then the tight grid, then
// over stairs or a slope to the entry's floor. "how" says which; goal is the end point.
bool Mod::route(const Entry& e, std::vector<Pt>& path, std::string& how, Pt& goal) {
    auto danger = danger_circles(e);
    for (auto& c : hazard_circles()) danger.push_back(c);
    // sleeping enemies (a zombie lying on the floor wakes when you pass): keep a little away when
    // there is room, without counting them as a reason to stop
    for (auto& en : s_.enemies)
        if (en.alive && en.hostile() && (en.dormant() || lying_still(en)) && std::fabs(en.x) < 31000 && std::fabs(en.z) < 31000 &&
            !(e.type == Entry::ENEMY && e.id == en.slot) && std::hypot(en.x - s_.x, en.z - s_.z) < 15000)
            danger.push_back({{en.x, en.z}, 1300});
    int f = s_.floor, gf = goal_floor(e);
    auto find = [&](int floor, int tight, Pt from, Pt to, int goal_r, std::vector<Pt>& out) {
        NavGrid& g = grid(floor, tight);
        g.set_avoid(danger);
        if (!g.find_path(from.x, from.z, to.x, to.z, goal_r, out)) return false;
        // finish on the exact spot when the last stretch is clear
        if (!out.empty() && g.line_free(out.back(), to)) out.push_back(to);
        // a path found with a smaller body hugs corners the real body catches on (112: the end
        // of the shelf rail beside the power switch); move each turn to where there is most room
        if (tight > 0 && out.size() > 2) {
            const auto& cols = nav_collisions();
            std::vector<int> fl{floor};
            auto room_at = [&](double x, double z) {
                double r = g.radius();
                if (blocking_at(cols, fl, x, z, r)) return -1.0;
                for (double rr = r + 25; rr <= PLAYER_RADIUS + 60; rr += 25)
                    if (blocking_at(cols, fl, x, z, rr)) return rr - 25;
                return PLAYER_RADIUS + 60.0;
            };
            // the least room along a stretch (its start is where the player stands now: only
            // the part beyond the first 150 counts)
            auto stretch_room = [&](Pt a, Pt b, bool skip_start) {
                double len = std::hypot(b.x - a.x, b.z - a.z), least = 1e9;
                int steps = std::max(1, int(len / 100));
                for (int k = 0; k <= steps; k++) {
                    double t = double(k) / steps;
                    if (skip_start && t * len < 150) continue;
                    least = std::min(least, room_at(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t));
                }
                return least;
            };
            for (size_t i = 1; i + 1 < out.size(); i++) {
                // the tighter of its two stretches first, then the other one (a turn between a
                // narrow corridor and an open one still moves off the corner on the open side)
                Pt best = out[i];
                double r1 = stretch_room(out[i - 1], best, i == 1), r2 = stretch_room(best, out[i + 1], false);
                double best_lo = std::min(r1, r2), best_hi = std::max(r1, r2), best_d = 0;
                for (int dx = -600; dx <= 600; dx += 50)
                    for (int dz = -600; dz <= 600; dz += 50) {
                        Pt c{out[i].x + dx, out[i].z + dz};
                        double d = std::hypot((double)dx, (double)dz);
                        if (room_at(c.x, c.z) < 0) continue;
                        double a1 = stretch_room(out[i - 1], c, i == 1), a2 = stretch_room(c, out[i + 1], false);
                        double lo = std::min(a1, a2), hi = std::max(a1, a2);
                        if (lo < best_lo || (lo == best_lo && (hi < best_hi || (hi == best_hi && d >= best_d)))) continue;
                        best = c; best_lo = lo; best_hi = hi; best_d = d;
                    }
                out[i] = best;
            }
        }
        else if (out.size() >= 2 && g.line_free(out[out.size() - 2], to)) out.back() = to;
        return true;
    };
    // a way onto stairs (or a ledge): line up first, a step out from the end along the
    // stair's own line, then go straight to the end. Stairs are walls until the player is
    // lined up at an end and presses action; their mouths can be only a body wide (116).
    auto approach = [&](int floor, int tight, Pt from, Pt near_pt, Pt far_pt, std::vector<Pt>& out) {
        double ux = near_pt.x - far_pt.x, uz = near_pt.z - far_pt.z, len = std::hypot(ux, uz);
        if (len > 1) {
            ux /= len; uz /= len;
            Pt pre{near_pt.x + ux * 800, near_pt.z + uz * 800};
            NavGrid& g = grid(floor, tight);
            auto c = g.to_cell(pre.x, pre.z);
            if (g.is_free(c.first, c.second) && find(floor, tight, from, pre, 0, out) && !out.empty() &&
                std::hypot(out.back().x - pre.x, out.back().z - pre.z) < 150) {
                out.push_back(near_pt);
                return true;
            }
        }
        return find(floor, tight, from, near_pt, 1, out);
    };
    Pt here{s_.x, s_.z};
    if (e.type == Entry::STAIRS) {
        for (int tight : {0, 1, 2}) {
            std::vector<Pt> p;
            if (approach(f, tight, here, e.start_end, e.pos, p)) {
                p.push_back(e.pos);
                path = p; how = tight ? "stairs tight" : "stairs"; goal = e.pos;
                return true;
            }
        }
        return false;
    }
    // a spot to walk onto counts once inside its area: a free place in it (216: the middle of
    // the push zone is under the vehicle's side), not its exact centre
    bool point_goal = e.type == Entry::ENEMY || e.type == Entry::PERSON;
    int goal_r = (e.type == Entry::ENEMY || e.type == Entry::PERSON) ? 3 : 0;
    std::vector<Pt> same_loose; Pt same_to{0, 0};
    if (gf == f) {
        Pt to = point_goal ? e.pos : stand_point(e, f);
        std::vector<Pt> loose;
        for (int tight : {0, 1, 2}) {
            std::vector<Pt> p;
            if (!find(f, tight, here, to, goal_r, p)) continue;
            double short_by = p.empty() ? 0 : std::hypot(p.back().x - to.x, p.back().z - to.z);
            // the normal body stops short of a narrow way in (610's channel to the activator):
            // the tight one may get there
            if (!tight && short_by > 300) { loose = p; continue; }
            if (tight && !loose.empty() && short_by > std::hypot(loose.back().x - to.x, loose.back().z - to.z) - 200) {
                path = loose; how = "direct"; goal = to; return true;
            }
            path = p; how = tight ? "tight" : "direct"; goal = to; return true;
        }
        // well short of it: a way over a ledge or stairs may get there (600: drop down first)
        if (!loose.empty() && std::hypot(loose.back().x - to.x, loose.back().z - to.z) < 1500) {
            path = loose; how = "direct"; goal = to; return true;
        }
        same_loose = loose; same_to = to;
    }
    // over stairs or a slope: walk to its near end, cross it, and plan again from the far end.
    // The AOT's floor byte is usually right; when no way matches it (room 200's door to the
    // soda machine corridor says floor 1 but stands on the ground floor), any level will do.
    double best = 1e18;
    // (same_loose: the best partial way on this level, if nothing else gets there)
    for (int pass = 0; pass < 2 && best >= 1e18; pass++)
    for (const auto& w : stairways()) {
        Pt near_pt, far_pt; int arrive;
        if (f == w.low && w.oneway >= 0) { near_pt = w.low_end; far_pt = w.high_end; arrive = w.high; }
        else if (f == w.high && w.oneway <= 0) { near_pt = w.high_end; far_pt = w.low_end; arrive = w.low; }
        else continue;
        if (pass == 0 && gf != f && arrive != gf) continue;
        if (pass == 1 && arrive == gf) continue;      // already tried
        if (pass == 1 && std::abs(arrive - gf) > 1) continue;   // not to the spot under or over it (400)
        for (int tight : {0, 1, 2}) {
            std::vector<Pt> p1, p2;
            if (!approach(f, tight, here, near_pt, far_pt, p1)) continue;
            // the grid snaps a blocked end to the nearest open cell, which can be across a
            // wall (room 30A: the west side of the drop-down ledge is the lower corridor)
            if (!p1.empty() && std::hypot(p1.back().x - near_pt.x, p1.back().z - near_pt.z) > 450) continue;
            Pt to = point_goal ? e.pos : stand_point(e, arrive);
            if (!find(arrive, tight, far_pt, to, goal_r, p2)) continue;
            if (!p2.empty() && std::hypot(p2.front().x - far_pt.x, p2.front().z - far_pt.z) > 450) continue;
            double len = 0;
            Pt prev = here;
            for (auto& q : p1) { len += std::hypot(q.x - prev.x, q.z - prev.z); prev = q; }
            len += std::hypot(far_pt.x - prev.x, far_pt.z - prev.z);
            if (len < best) {
                best = len;
                path = p1;
                path.push_back(far_pt);
                how = std::string(w.stairs ? "via stairs" : w.index >= 1000 ? "via climb" : "via slope") + (tight ? " tight" : "");
                goal = to;
            }
            break;
        }
    }
    if (best < 1e18) return true;
    // the AOT's floor byte can be one off from where it can be used (room 305's door to the
    // treatment room says floor 3, the walkway is floor 2): try this level as it stands
    // a ladder or doorway whose floor byte is far off (405's ladder up to the hidden passage
    // says floor 7, its foot is on the ground): its foot is on this level
    bool way_out = e.type == Entry::AOT && (e.kind == SCE_DOOR || e.name.rfind("Ladder", 0) == 0) && std::abs(gf - f) >= 5;
    if (gf != f && (std::abs(gf - f) == 1 || way_out)) {
        Pt to = point_goal ? e.pos : stand_point(e, f);
        for (int tight : {0, 1, 2}) {
            std::vector<Pt> p;
            if (find(f, tight, here, to, goal_r, p)) { path = p; how = tight ? "same level tight" : "same level"; goal = to; return true; }
        }
    }
    // two crossings: down one stairway and up another (room 200: from the landing by the
    // 2nd-floor door, down to the hall and up again to the reception counter)
    auto ways = stairways();
    for (int tight : {0, 1, 2}) {
        for (const auto& w1 : ways) {
            Pt n1, f1; int a1;
            if (f == w1.low && w1.oneway >= 0) { n1 = w1.low_end; f1 = w1.high_end; a1 = w1.high; }
            else if (f == w1.high && w1.oneway <= 0) { n1 = w1.high_end; f1 = w1.low_end; a1 = w1.low; }
            else continue;
            std::vector<Pt> p1;
            if (!approach(f, tight, here, n1, f1, p1)) continue;
            if (!p1.empty() && std::hypot(p1.back().x - n1.x, p1.back().z - n1.z) > 450) continue;
            for (const auto& w2 : ways) {
                if (w2.index == w1.index) continue;
                Pt n2, f2; int a2;
                if (a1 == w2.low && w2.oneway >= 0) { n2 = w2.low_end; f2 = w2.high_end; a2 = w2.high; }
                else if (a1 == w2.high && w2.oneway <= 0) { n2 = w2.high_end; f2 = w2.low_end; a2 = w2.low; }
                else continue;
                // back to this level, or far from the goal's: no use (400: down a ledge and up again)
                if (std::abs(a2 - gf) > 1) continue;
                std::vector<Pt> p2, p3;
                if (!find(a1, tight, f1, n2, 1, p2)) continue;
                if (!p2.empty() && (std::hypot(p2.back().x - n2.x, p2.back().z - n2.z) > 450 ||
                                    std::hypot(p2.front().x - f1.x, p2.front().z - f1.z) > 450)) continue;
                Pt to = point_goal ? e.pos : stand_point(e, a2);
                if (!find(a2, tight, f2, to, goal_r, p3)) continue;
                double len = 0;
                Pt prev = here;
                auto add = [&](const std::vector<Pt>& v) { for (auto& q : v) { len += std::hypot(q.x - prev.x, q.z - prev.z); prev = q; } };
                add(p1); add({f1}); add(p2); add({f2});
                if (len < best) {
                    best = len;
                    // walk to the first crossing; the level change re-plans the rest from there
                    path = p1;
                    path.push_back(f1);
                    how = std::string("via two stairways") + (tight ? " tight" : "");
                    goal = to;
                }
            }
        }
        if (best < 1e18) return true;
    }
    // more crossings (701: up two flights to the walkway, along it, down two flights): a search
    // over stairway ends, fewest crossings first; walk to the first one, the rest is planned
    // again from each new level
    {
        struct Node { Pt p; int floor; int first = -1; };   // first: index into ways of the first crossing
        std::vector<Node> frontier{{here, f, -1}};
        std::set<std::pair<int, int>> used;                // (way, direction) already crossed
        for (int depth = 0; depth < 6 && !frontier.empty(); depth++) {
            std::vector<Node> next;
            for (auto& n : frontier) {
                for (size_t wi = 0; wi < ways.size(); wi++) {
                    const auto& w = ways[wi];
                    Pt nr, fr; int arr, dirn;
                    if (n.floor == w.low && w.oneway >= 0) { nr = w.low_end; fr = w.high_end; arr = w.high; dirn = 0; }
                    else if (n.floor == w.high && w.oneway <= 0) { nr = w.high_end; fr = w.low_end; arr = w.low; dirn = 1; }
                    else continue;
                    if (used.count({(int)wi, dirn})) continue;
                    std::vector<Pt> p1;
                    bool ok = false;
                    for (int tight : {0, 1, 2}) {
                        p1.clear();
                        if (depth == 0 ? approach(n.floor, tight, n.p, nr, fr, p1) : find(n.floor, tight, n.p, nr, 1, p1)) {
                            if (!p1.empty() && std::hypot(p1.back().x - nr.x, p1.back().z - nr.z) > 450) continue;
                            ok = true;
                            break;
                        }
                    }
                    if (!ok) continue;
                    used.insert({(int)wi, dirn});
                    int first = n.first >= 0 ? n.first : (int)wi;
                    if (depth == 0) {
                        // keep the way onto the first crossing
                        multi_first_[wi] = p1;
                        multi_first_[wi].push_back(fr);
                    }
                    // can the goal be reached from the far end?
                    if (std::abs(arr - gf) <= 1) {
                        Pt to = point_goal ? e.pos : stand_point(e, arr);
                        std::vector<Pt> p2;
                        if (find(arr, false, fr, to, goal_r, p2) || find(arr, true, fr, to, goal_r, p2)) {
                            path = multi_first_[first];
                            how = "via " + std::to_string(depth + 1) + " stairways";
                            goal = to;
                            return true;
                        }
                    }
                    next.push_back({fr, arr, first});
                }
            }
            frontier.swap(next);
        }
    }
    if (!same_loose.empty()) { path = same_loose; how = "direct"; goal = same_to; return true; }
    return false;
}

bool Mod::plan_walk(const Entry& e, bool replanning) {
    // 603's crate: pushing it along the walkway and off the end is done for the player, from
    // wherever they are (like the archive room's stepladder)
    if (e.type == Entry::AOT && e.id == kCrateEntry) { drop_crate(); return true; }
    if (!replanning) walk_unreachable_ = false;
    if (!replanning) { ledge_tries_ = 0; stair_tries_ = 0; }
    std::vector<Pt> path;
    std::string how;
    Pt goal = e.pos;
    bool ok = route(e, path, how, goal);
    partial_ = false;
    NavGrid& g = grid(s_.floor);
    char buf[200];
    if (!ok) {
        // as close as possible on this floor (some ways open later, e.g. after a script)
        NavGrid& gt = grid(s_.floor, 2);
        int gf = goal_floor(e);
        Pt to = e.type == Entry::AOT && gf == s_.floor ? stand_point(e, s_.floor) : e.pos;
        auto reach = gt.reachable(s_.x, s_.z);
        double best = 1e18; Pt bp;
        for (auto& c : reach) {
            Pt p = gt.to_world(c.first, c.second);
            double d = std::hypot(p.x - to.x, p.z - to.z);
            if (d < best) { best = d; bp = p; }
        }
        std::snprintf(buf, sizeof buf, "[WALK] %s from (%.0f,%.0f) floor %d to (%.0f,%.0f) floor %d: no way, closest %.0f",
                      e.name.c_str(), s_.x, s_.z, s_.floor, to.x, to.z, gf, best);
        h_.log(buf);
        {
            double x0 = 1e9, x1 = -1e9, z0 = 1e9, z1 = -1e9;
            for (auto& c : reach) { Pt p = gt.to_world(c.first, c.second); x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.z); z1 = std::max(z1, p.z); }
            std::string ws = "[WALK] reach " + std::to_string(reach.size()) + " cells x " + std::to_string((int)x0) + ".." + std::to_string((int)x1) +
                             " z " + std::to_string((int)z0) + ".." + std::to_string((int)z1) + "; ways:";
            for (auto& w : stairways()) {
                char b2[120];
                std::snprintf(b2, sizeof b2, " %d(%s %d-%d %.0f,%.0f>%.0f,%.0f)", w.index, w.stairs ? "st" : "sl", w.low, w.high,
                              w.low_end.x, w.low_end.z, w.high_end.x, w.high_end.z);
                ws += b2;
            }
            h_.log(ws);
        }
        // a hint when the room has a spot that drops you somewhere (the library's weak floor)
        std::string hint;
        Entry ladder; bool have_ladder = false;
        for (auto& o : entries(0)) if (o.walk_on && o.id != e.id) { hint = " The " + o.name.substr(0, o.name.find(',')) + " may be the way."; std::transform(hint.begin() + 5, hint.begin() + 6, hint.begin() + 5, ::tolower); break; }
        // a ladder within the room toward that level (401: "Ladder up" to the high walkway)
        if (hint.empty() && e.type == Entry::AOT && gf != s_.floor) {
            const char* dir = gf > s_.floor ? " up" : " down";
            for (auto& o : entries(0))
                if (o.id != e.id && o.type == Entry::AOT && o.name.find(" to ") == std::string::npos &&
                    o.name.size() > std::strlen(dir) && o.name.compare(o.name.size() - std::strlen(dir), std::strlen(dir), dir) == 0) {
                    hint = " Try the " + o.name + ".";
                    std::transform(hint.begin() + 9, hint.begin() + 10, hint.begin() + 9, ::tolower);
                    ladder = o; have_ladder = true;
                    break;
                }
        }
        // another part of this room behind a wall, entered by its own door from the next room
        // (20E: the interrogation room and the observation room behind the mirror, each with a
        // door to the Media Room Corridor): leave by one door and come back in by the other
        if (hint.empty() && gf == s_.floor && !reach.empty()) {
            auto there = gt.reachable(to.x, to.z);
            std::set<std::pair<int, int>> here(reach.begin(), reach.end());
            bool apart = !there.empty();
            for (auto& c : there) if (here.count(c)) { apart = false; break; }
            if (apart) {
                std::set<std::pair<int, int>> far_set(there.begin(), there.end());
                auto near_cells = [&](const std::set<std::pair<int, int>>& cells, const Pt& p) {
                    auto c = gt.to_cell(p.x, p.z);
                    for (int di = -12; di <= 12; di++)
                        for (int dj = -12; dj <= 12; dj++)
                            if (cells.count({c.first + di, c.second + dj})) return true;
                    return false;
                };
                for (auto& d1 : s_.aots) {
                    if (d1.sce != SCE_DOOR || d1.door_locked || !near_cells(here, d1.centre)) continue;
                    for (auto& d2 : s_.aots)
                        if (d2.sce == SCE_DOOR && d2.id != d1.id && d2.door_stage == d1.door_stage && d2.door_room == d1.door_room &&
                            near_cells(far_set, d2.centre)) {
                            hint = " It is in another part of this room: go out to " + room_name(d1.door_stage, d1.door_room) +
                                   " and come back in by its other door.";
                            break;
                        }
                    if (!hint.empty()) break;
                }
                if (!hint.empty()) {
                    if (!replanning) say("Can't reach it from here." + hint, true);
                    return false;
                }
            }
        }
        // the same level but a part of the room only a ladder reaches (60E: the stairway's two
        // shafts, both at the same height to the game): a ladder from this part of the room
        if (hint.empty() && !have_ladder && gf == s_.floor && !reach.empty()) {
            for (auto& o : entries(0)) {
                if (o.id == e.id || o.type != Entry::AOT || o.name.rfind("Ladder", 0) != 0 || o.name.find(" to ") != std::string::npos)
                    continue;
                double nearest = 1e18;
                for (auto& c : reach) {
                    Pt p = gt.to_world(c.first, c.second);
                    nearest = std::min(nearest, std::hypot(p.x - o.pos.x, p.z - o.pos.z));
                }
                if (nearest < 1500) { ladder = o; have_ladder = true; hint = " Try the " + o.name + "."; break; }
            }
        }
        // take that ladder first, then carry on to the thing (frame_via)
        if (have_ladder && !replanning && via_state_ == 0) {
            via_target_ = e;
            via_ladder_id_ = ladder.id;
            via_tries_ = 0;
            via_room_ = s_.room_code;
            say("On another level: taking the " + std::string(1, (char)std::tolower((unsigned char)ladder.name[0])) + ladder.name.substr(1) + " first.", true);
            if (plan_walk(ladder, true)) { via_state_ = 1; return true; }
            return false;
        }
        if (have_ladder && gf == s_.floor) {
            if (!replanning) say("Can't reach it from here yet; it is on another part of the room." + hint, true);
            return false;
        }
        if (e.type == Entry::AOT && gf != s_.floor && std::abs(gf - s_.floor) > 1) {
            // on another level with no known way: walking to the spot above or below it helps nobody
            if (!replanning) say("Can't reach it from here yet; it is on another level." + hint, true);
            return false;
        }
        if (reach.empty() || best < 400 || !gt.find_path(s_.x, s_.z, bp.x, bp.z, 0, path)) {
            if (!replanning) say("No path found.", true);
            return false;
        }
        if (!replanning)
            say("Can't reach it from here yet; the way is blocked. Walking to the closest point, " + metres(best) + " short." + hint, true);
        partial_ = true;
        walk_unreachable_ = true;
        how = "partial";
    } else {
        std::snprintf(buf, sizeof buf, "[WALK] %s from (%.0f,%.0f) floor %d (y %.0f, game floor %d) to (%.0f,%.0f): %s, %zu points",
                      e.name.c_str(), s_.x, s_.z, s_.floor, s_.y, s_.game_floor, goal.x, goal.z, how.c_str(), path.size());
        if (!replanning || how != last_how_) {
            std::string pts(buf);
            for (auto& q : path) { char b2[40]; std::snprintf(b2, sizeof b2, " (%.0f,%.0f)", q.x, q.z); pts += b2; }
            h_.log(pts);
        }
        std::vector<Pt> full{{s_.x, s_.z}};
        full.insert(full.end(), path.begin(), path.end());
        if (path_hazard(full)) {
            // the only way crosses fire or the like: say so, and go only when asked again
            int key = entry_key(e);
            bool confirmed = key == hazard_ok_key_ && h_.ms() - hazard_ok_ms_ < 15000;
            if (!replanning && !confirmed) {
                hazard_ok_key_ = key; hazard_ok_ms_ = h_.ms();
                h_.log("[WALK] route crosses a danger area; waiting for a second request");
                say("The way to " + e.name + " goes through a danger area that will hurt you, like fire. "
                    "Walk there again to go anyway.", true);
                return false;
            }
            if (!replanning) say("Walking to " + e.name + ", through the danger area.", true);
        } else if (!replanning) say("Walking to " + e.name + ".", true);
    }
    last_how_ = how;
    // does the route keep clear of every enemy, or is there no way around?
    std::vector<Pt> full{{s_.x, s_.z}};
    full.insert(full.end(), path.begin(), path.end());
    auto danger = danger_circles(e);
    g.set_avoid(danger);
    walk_unsafe_ = !danger.empty() && g.clearance(full) < kEnemyKeepAway * 0.6;
    walk_.assign(path.size() > 1 ? path.begin() + 1 : path.begin(), path.end());
    walking_ = true;
    face_dir_ = -1;
    walk_target_ = e;
    last_replan_ms_ = h_.ms();
    walk_floor_ = s_.floor;
    stuck_ = 0;
    walk_started_ms_ = h_.ms();
    return true;
}

void Mod::key_go() {
    Entry e;
    if (!selected(e)) { say("Nothing selected.", true); return; }
    if (s_.message.active) { say("A message is still open. Press action to close it first.", true); return; }
    bool ok = plan_walk(e, false);
    // the door a map journey takes from here: carry on travelling
    if (!travel_to_.empty() && e.type == Entry::AOT && e.id == travel_door_id_) travel_state_ = ok ? 1 : 0;
    else if (!travel_to_.empty()) { travel_state_ = 0; }
}

// Run from the nearest enemy: to the reachable open place farthest from it, by a way that keeps
// clear of it, not into a corner (a boss in a closed room: 303's G, Mister X)
void Mod::key_get_away() {
    const Enemy* foe = nullptr;
    double fd = 20000;
    for (auto& e : s_.enemies) {
        if (!e.alive || !e.hostile() || !in_play(e) || lying_still(e) || !level_note(e).empty()) continue;
        double d = std::hypot(e.x - s_.x, e.z - s_.z);
        if (d < fd) { fd = d; foe = &e; }
    }
    if (!foe) { say("No enemy close to get away from.", true); return; }
    NavGrid& g = grid(s_.floor);
    g.set_avoid({});
    auto reach = g.reachable(s_.x, s_.z);
    const auto& cols = nav_collisions();
    std::vector<int> fl{s_.floor};
    Pt ep{foe->x, foe->z};
    double best = -1e18; Pt bp{s_.x, s_.z};
    int k = 0;
    for (auto& c : reach) {
        if ((k++ % 3) != 0) continue;
        Pt p = g.to_world(c.first, c.second);
        double from_foe = std::hypot(p.x - ep.x, p.z - ep.z);
        double from_me = std::hypot(p.x - s_.x, p.z - s_.z);
        if (from_foe < fd + 1500 || from_me < 2000 || from_me > 12000) continue;
        // the way must not pass the enemy: the straight line keeps it on the far side
        double vx = p.x - s_.x, vz = p.z - s_.z, ex = ep.x - s_.x, ez = ep.z - s_.z;
        double t = std::max(0.0, std::min(1.0, (vx * ex + vz * ez) / (vx * vx + vz * vz)));
        if (std::hypot(s_.x + vx * t - ep.x, s_.z + vz * t - ep.z) < std::min(fd, 2500.0)) continue;
        // room round it to turn and shoot from
        double room = 0;
        for (double r = 300; r <= 1500; r += 300) { if (blocking_at(cols, fl, p.x, p.z, r)) break; room = r; }
        double score = from_foe + 3 * room - 0.3 * from_me;
        if (score > best) { best = score; bp = p; }
    }
    if (best <= -1e17) { say("No way to get away from the " + foe->name() + " here.", true); return; }
    std::vector<Pt> path;
    g.set_avoid({{ep, std::min(fd * 0.8, 2500.0)}});
    bool ok = g.find_path(s_.x, s_.z, bp.x, bp.z, 1, path);
    g.set_avoid({});
    if (!ok) { say("No way to get away from the " + foe->name() + " here.", true); return; }
    walk_.assign(path.size() > 1 ? path.begin() + 1 : path.begin(), path.end());
    walking_ = true; partial_ = false; face_dir_ = -1;
    walk_target_ = Entry(); walk_target_.id = -1;
    walk_floor_ = s_.floor; stuck_ = 0;
    walk_started_ms_ = h_.ms();
    getaway_ = true; getaway_slot_ = foe->slot;
    say("Getting away from the " + foe->name() + ".", true);
}

void Mod::key_explore() {
    NavGrid& g = grid(s_.floor);
    auto reach = g.reachable(s_.x, s_.z);
    auto& seen = visited_cells_[s_.room_code];
    double best = -1; Pt bp;
    // first the places where walking makes something happen (216's parking lot: Ada comes to
    // help push the car once Leon walks along the far side): a script zone the player sets off
    // by walking into it, not yet walked into
    {
        std::set<std::pair<int, int>> reach_set(reach.begin(), reach.end());
        double near = 1e18;
        int zone = -1;
        for (auto& a : s_.aots) {
            if (a.sce != SCE_EVENT || a.manual() || !a.player_triggers() || a.quad.empty() || hidden(a)) continue;
            if (!(a.data[0] == 0xFF && a.data[2] == 0x18)) continue;
            if (explored_zones_[s_.room_code].count(a.id)) continue;
            // the nearest place inside the zone that can be walked to, a little way in
            for (auto& c : reach) {
                Pt p = g.to_world(c.first, c.second);
                if (!point_in_quad(p.x, p.z, a.quad)) continue;
                bool deep = true;
                for (auto dd : {Pt{250, 0}, Pt{-250, 0}, Pt{0, 250}, Pt{0, -250}})
                    if (!point_in_quad(p.x + dd.x, p.z + dd.z, a.quad)) deep = false;
                if (!deep) continue;
                double d = std::hypot(p.x - s_.x, p.z - s_.z);
                if (d < near) { near = d; bp = p; best = d; zone = a.id; }
            }
        }
        if (zone >= 0) explored_zones_[s_.room_code].insert(zone);
    }
    if (best < 0)
    for (auto& c : reach) {
        Pt p = g.to_world(c.first, c.second);
        if (seen.count({int(std::floor(p.x / 1500)), int(std::floor(p.z / 1500))})) continue;
        double d = std::hypot(p.x - s_.x, p.z - s_.z);
        if (d > best) { best = d; bp = p; }
    }
    if (best < 0) {
        // other levels: a ledge or stairs in the catalog leads on (509: the drop to the turn table)
        std::string other;
        for (auto& e : entries(0))
            if (e.type == Entry::STAIRS) { other = e.name; break; }
        say("You have been everywhere you can reach on this level." +
            (other.empty() ? std::string() : " " + other + " is in the catalog."), true);
        return;
    }
    std::vector<Pt> path;
    if (!g.find_path(s_.x, s_.z, bp.x, bp.z, 1, path)) { say("Nothing new to explore.", true); return; }
    walk_.assign(path.size() > 1 ? path.begin() + 1 : path.begin(), path.end());
    walking_ = true; partial_ = true; face_dir_ = -1;
    walk_target_ = Entry(); walk_target_.id = -1;
    walk_floor_ = s_.floor; stuck_ = 0;
    say("Exploring, " + locate(bp) + ".", true);
}

uint32_t Mod::steer() {
    // turning on the spot (face a target)
    if (face_dir_ >= 0) {
        int diff = angle_diff(s_.dir, face_dir_);
        if (std::abs(diff) <= 64 || h_.ms() - walk_started_ms_ > 4000) {
            face_dir_ = -1;
            say("Facing " + face_label_ + ".", true);
            // a spot the game checks at the point in front of the player (sat 0x20): when that
            // point falls just short (615's vaccine machine), step up to it
            if (walk_target_.type == Entry::AOT)
                for (auto& a : s_.aots)
                    if (a.id == walk_target_.id && (a.sat & 0x20) && !a.quad.empty()) { nudge_frames_ = 20; nudge_aot_ = a.id; }
            // a thing with a carried item's use spot on it (408's oil pressure data transmitter
            // and its Valve handle spot): step up until the game would take the item there
            if (walk_target_.type == Entry::AOT && walk_target_.name.find(", use the ") != std::string::npos)
                for (auto& a : s_.aots)
                    if (a.sce == SCE_FLAG_CHG && (a.sat & 0x20) && !a.quad.empty() && a.floor == s_.floor &&
                        std::hypot(a.centre.x - walk_target_.pos.x, a.centre.z - walk_target_.pos.z) < 1500) {
                        bool have = false;
                        for (auto& it : s_.inventory) if (it.id == a.data[2]) have = true;
                        if (have) { nudge_frames_ = 30; nudge_aot_ = a.id; break; }
                    }
            return 0;
        }
        return diff > 0 ? BTN_RIGHT : BTN_LEFT;
    }
    if (push_frames_ > 0 && !walking_) {
        // pushing the vehicle (216): face the push spot and keep walking into it until the
        // game's scene starts
        if (event_running() || s_.stop_flg || s_.message.active) { push_frames_ = 0; return 0; }
        if (--push_frames_ == 0) { say("The push did not start. Keep walking into the vehicle from here, or try a little to either side.", true); return 0; }
        int want = dir_towards(s_.x, s_.z, push_to_.x, push_to_.z);
        int diff = angle_diff(s_.dir, want);
        if (std::abs(diff) > 64) return diff > 0 ? BTN_RIGHT : BTN_LEFT;
        return BTN_UP;
    }
    if (nudge_frames_ > 0 && !walking_) {
        const Aot* a = nullptr;
        for (auto& x : s_.aots) if (x.id == nudge_aot_) a = &x;
        double fx, fz;
        facing_vector(s_.dir, fx, fz);
        bool inside = a && point_in_quad(s_.x + fx * 620, s_.z + fz * 620, a->quad) &&
                      point_in_quad(s_.x + fx * 520, s_.z + fz * 520, a->quad);
        if (!a || inside || s_.message.active || s_.stop_flg) { nudge_frames_ = 0; return 0; }
        nudge_frames_--;
        return BTN_UP;
    }
    if (!walking_) return 0;
    // a spot that acts when walked onto (the weak floor) has done its job once its event runs
    if (walk_target_.walk_on && (s_.stop_flg || event_running())) {
        walking_ = false;
        walk_.clear();
        walk_target_.id = -1;
        return 0;
    }
    if (s_.stop_flg || s_.message.active || event_running()) {             // event or message: wait
        if (!walk_wait_ms_) walk_wait_ms_ = h_.ms();
        else if (h_.ms() - walk_wait_ms_ > 2000 && !walk_wait_logged_) {
            walk_wait_logged_ = true;
            char b[120];
            std::snprintf(b, sizeof b, "[WALK] waiting: stop %08X, message %d, flags 2.7 %d 1.27 %d", s_.stop_flg,
                          s_.message.active ? 1 : 0, reader_.flag(2, 7) ? 1 : 0, reader_.flag(1, 27) ? 1 : 0);
            h_.log(b);
        }
        return 0;
    }
    walk_wait_ms_ = 0; walk_wait_logged_ = false;
    // enemies: keep re-planning around them while they move; stop only when one is right
    // next to us, or when no route keeps clear of it
    bool near_enemy = false;
    for (auto& e : s_.enemies) {
        if (!e.alive || !e.hostile() || !in_play(e) || lying_still(e)) continue;
        if (walk_target_.type == Entry::ENEMY && walk_target_.id == e.slot) continue;
        if (walk_target_.type == Entry::ENEMY && std::hypot(e.x - walk_target_.pos.x, e.z - walk_target_.pos.z) < 1000) continue;
        if (!level_note(e).empty()) continue;   // on the ceiling or another level: not in the way
        double d = std::hypot(e.x - s_.x, e.z - s_.z);
        if (d < 10000) near_enemy = true;
        // with no weapon (Sherry) stopping only helps the enemy: keep going and get away
        bool armed = s_.equipped > 0;
        if (!armed || getaway_) continue;
        // a boss chases: stopping beside it helps nobody (702: walking to the rocket launcher
        // with the Super Tyrant on your heels). Keep going and say how close it is
        bool boss = e.type == 0x2A || e.type == 0x2B || (e.type >= 0x30 && e.type <= 0x37);
        if (boss) {
            if (d < 2500 && h_.ms() - boss_warn_ms_ > 3000) {
                boss_warn_ms_ = h_.ms();
                say(e.name() + " " + metres(d) + ", " + relative_direction(s_.dir, s_.x, s_.z, e.x, e.z) + ".", false);
            }
            continue;
        }
        if (d < 1300) {
            stop_walk("Stopped. " + e.name() + " right next to you, " + relative_direction(s_.dir, s_.x, s_.z, e.x, e.z) + ".");
            return 0;
        }
        if (walk_unsafe_ && d < 3000) {
            stop_walk("Stopped. No way around the " + e.name() + ", " + relative_direction(s_.dir, s_.x, s_.z, e.x, e.z) + ".");
            return 0;
        }
    }
    bool crossing = walk_target_.type == Entry::STAIRS && walk_.size() <= 1;   // on the last stretch over stairs
    // re-plan around moving enemies, but keep the way chosen while it still keeps clear of them
    // (401: a zombie by the desks flipped the way between north and south every half second)
    bool threatened = true;
    if (near_enemy && !walk_.empty()) {
        auto danger = danger_circles(walk_target_);
        NavGrid& gg = grid(s_.floor);
        gg.set_avoid(danger);
        std::vector<Pt> rest{{s_.x, s_.z}};
        rest.insert(rest.end(), walk_.begin(), walk_.end());
        double keep = s_.equipped == 0 ? kEnemyKeepAway * 1.5 : kEnemyKeepAway;
        threatened = gg.clearance(rest) < keep * 0.8;
    }
    if (near_enemy && !crossing && walk_target_.id >= 0 && h_.ms() - last_replan_ms_ > (threatened ? 500 : 3000)) {
        Entry t = walk_target_;
        bool part = partial_;
        if (!plan_walk(t, true)) { stop_walk("Lost the path."); return 0; }
        partial_ = part || partial_;
    }
    // stairs or a climb changed level: plan again from here
    if (walk_target_.id >= 0 && walk_target_.type != Entry::STAIRS && !on_stairs() && s_.floor != walk_floor_) {
        if (walk_floor_ >= 0) ledge_tries_ = 0;   // a real change of level
        Entry t = walk_target_;
        if (!plan_walk(t, true)) { stop_walk("Lost the path."); return 0; }
    }
    if (walk_.empty()) {
        walking_ = false;
        if (getaway_) {
            getaway_ = false;
            for (auto& e : s_.enemies)
                if (e.slot == getaway_slot_ && e.alive && in_play(e)) {
                    say("Away. " + e.name() + " " + metres(std::hypot(e.x - s_.x, e.z - s_.z)) + ", " +
                        relative_direction(s_.dir, s_.x, s_.z, e.x, e.z) + ".", true);
                    return 0;
                }
            say("Away.", true);
            return 0;
        }
        if (partial_) { say("As close as you can get for now.", true); return 0; }
        if (walk_target_.id >= 0 && walk_target_.walk_on && walk_target_.name.rfind("Push the vehicle", 0) == 0 &&
            !event_running() && !s_.stop_flg) {
            // 216: lean on the vehicle, walking into it toward the push spot, for a few seconds
            start_push();
            return 0;
        }
        if (walk_target_.type == Entry::AOT && walk_target_.id == kCrateEntry) { drop_crate(); return 0; }
        if (walk_target_.id >= 0 && walk_target_.walk_on) { say("Arrived.", true); return 0; }   // a spot, not a thing to face
        if (walk_target_.id >= 0) {
            start_turn(dir_towards(s_.x, s_.z, walk_target_.pos.x, walk_target_.pos.z), walk_target_.name);
            face_label_ = walk_target_.name;
            say("Arrived.", true);
        }
        return 0;
    }
    // a partner following (Sherry with Claire, Ada with Leon) is slower: walk instead of running
    // when they fall behind, stop and wait when they are far behind, and wait for them by a door
    // (the game will not let the player leave without them: "I can't leave Sherry alone")
    partner_slow_ = false;
    {
        double px, pz;
        if (!(walk_target_.type == Entry::PERSON && walk_target_.id == 1) && partner_pos(px, pz)) {
            double d = std::hypot(px - s_.x, pz - s_.z);
            bool to_door = walk_target_.type == Entry::AOT && walk_target_.kind == SCE_DOOR &&
                           std::hypot(walk_.back().x - s_.x, walk_.back().z - s_.z) < 2500;
            bool danger = false;
            for (auto& e : s_.enemies)
                if (e.alive && e.hostile() && in_play(e) && !lying_still(e) && level_note(e).empty() &&
                    std::hypot(e.x - s_.x, e.z - s_.z) < 4000) danger = true;
            const char* who = (h_.mem().u8(addr::PL_ID) & 1) ? "Sherry" : "Ada";
            if (partner_wait_) {
                if (d < (to_door ? 1500 : 2200) || danger) {
                    partner_wait_ = false;
                } else if (h_.ms() - partner_wait_ms_ > 15000) {
                    partner_wait_ = false;
                    partner_ignore_until_ = h_.ms() + 20000;
                    say(std::string(who) + " is not catching up. Going on.", false);
                    h_.log("[WALK] partner not catching up, going on");
                } else {
                    return 0;
                }
            } else if (!danger && h_.ms() > partner_ignore_until_ && d > (to_door ? 2500 : 5500)) {
                partner_wait_ = true;
                partner_wait_ms_ = h_.ms();
                if (!partner_said_ms_ || h_.ms() - partner_said_ms_ > 20000) say(std::string("Waiting for ") + who + ".", false);
                partner_said_ms_ = h_.ms();
                char b[120];
                std::snprintf(b, sizeof b, "[WALK] waiting for the partner, %.0f away%s", d, to_door ? " (at a door)" : "");
                h_.log(b);
                return 0;
            }
            partner_slow_ = d > 1800;
        }
    }
    Pt t = walk_.front();
    bool last = walk_.size() == 1;
    // run while the way ahead is long and straight; walk the last stretch so we stop on the spot
    double left_to_go = std::hypot(t.x - s_.x, t.z - s_.z);
    for (size_t i = 0; i + 1 < walk_.size(); i++) left_to_go += std::hypot(walk_[i + 1].x - walk_[i].x, walk_[i + 1].z - walk_[i].z);
    // the end of a stairway is a point to reach exactly: cutting the corner leaves the player
    // against the stair's side rail at the top, which the game does not let them past (116)
    if (stair_ends_.empty() && !stair_ends_done_) {
        for (auto& w : stairways()) if (w.stairs) { stair_ends_.push_back(w.low_end); stair_ends_.push_back(w.high_end); }
        stair_ends_done_ = true;
    }
    bool precise = false;
    for (auto& q : stair_ends_) if (std::hypot(q.x - t.x, q.z - t.z) < 60) precise = true;
    // a re-plan that starts beside the near end (112, going up): the end is the next point
    // but one, so the point before it is where to line up
    if (!precise && walk_.size() >= 2) {
        bool far_next = false, near_here = false;
        for (auto& q : stair_ends_) {
            if (std::hypot(q.x - walk_[1].x, q.z - walk_[1].z) < 60) far_next = true;
            if (std::hypot(q.x - t.x, q.z - t.z) < 300) near_here = true;
        }
        precise = far_next && near_here;
    }
    double to_t = std::hypot(t.x - s_.x, t.z - s_.z);
    bool run = run_enabled_ && left_to_go > 2500 && !on_stairs() && !(precise && to_t < 3000) && !partner_slow_;
    // with nothing to fight with (Sherry), outrun what is close: run to the end of the walk
    if (s_.equipped == 0 && !on_stairs() && !precise && left_to_go > 600)
        for (auto& e : s_.enemies)
            if (e.alive && e.hostile() && in_play(e) && !lying_still(e) && level_note(e).empty() &&
                std::hypot(e.x - s_.x, e.z - s_.z) < 7000) { run = true; break; }
    if (to_t < (last || precise ? 70 : (run ? 350 : 180))) {
        walk_.erase(walk_.begin());
        // at one end of a stairway with the other end next: face along it, then action
        if (precise && !walk_.empty()) {
            bool cross = false;
            for (auto& q : stair_ends_) if (std::hypot(q.x - walk_.front().x, q.z - walk_.front().z) < 60) cross = true;
            if (cross) { stair_action_pending_ = true; stair_tries_ = 0; }
        }
        return steer();
    }
    if (stair_action_pending_) {
        int want0 = dir_towards(s_.x, s_.z, t.x, t.z);
        int diff0 = angle_diff(s_.dir, want0);
        if (std::abs(diff0) > 48) return diff0 > 0 ? BTN_RIGHT : BTN_LEFT;   // turn on the spot first
        stair_action_pending_ = false;
        stair_press_ms_ = h_.ms();
        h_.log("[WALK] at a stairway's end, lined up: pressing action");
        return BTN_ACTION;
    }
    int want = dir_towards(s_.x, s_.z, t.x, t.z);
    int diff = angle_diff(s_.dir, want);
    uint32_t b = 0;
    if (std::abs(diff) > 64) b |= diff > 0 ? BTN_RIGHT : BTN_LEFT;
    if (std::abs(diff) < 160) b |= BTN_UP;
    if ((b & BTN_UP) && run && std::abs(diff) < 100) b |= BTN_RUN;
    bool still = std::lround(s_.x) == std::lround(last_x_) && std::lround(s_.z) == std::lround(last_z_);
    // sliding along a wall or a ledge's edge moves the player without getting closer (407):
    // no progress toward the point for a while counts as stuck too
    if (std::hypot(t.x - prog_t_.x, t.z - prog_t_.z) > 1) { prog_t_ = t; prog_best_ = to_t; prog_frames_ = 0; }
    else if (to_t < prog_best_ - 40) { prog_best_ = to_t; prog_frames_ = 0; }
    else if (b & BTN_UP) prog_frames_++;
    if (prog_frames_ > 45) still = true;
    if (prog_frames_ > 45 && h_.ms() - walk_dbg_ms_ > 2000) {
        walk_dbg_ms_ = h_.ms();
        char db[200];
        std::snprintf(db, sizeof db, "[WALK] at (%.0f,%.0f) dir %d to (%.0f,%.0f) diff %d buttons %X stuck %d still %d climb '%s'",
                      s_.x, s_.z, s_.dir, t.x, t.z, diff, b, stuck_, still ? 1 : 0, climbable().c_str());
        h_.log(db);
    }
    if ((b & BTN_UP) && still) {
        // only when the way crosses a ledge here (not when brushing past one, room 400)
        bool ledge_next = false;
        for (auto& w : stairways())
            if (!w.stairs && (std::hypot(w.low_end.x - t.x, w.low_end.z - t.z) < 1500 ||
                              std::hypot(w.high_end.x - t.x, w.high_end.z - t.z) < 1500)) ledge_next = true;
        if (!climbable().empty() && stuck_ >= 3 && (!ledge_next || ledge_tries_ >= 3) && stuck_ == 3) {
            char lb[160];
            std::snprintf(lb, sizeof lb, "[WALK] stuck by a ledge, not climbing: next %d tries %d, target (%.0f,%.0f)", ledge_next ? 1 : 0, ledge_tries_, t.x, t.z);
            h_.log(lb);
        }
        if (!climbable().empty() && stuck_ >= 3 && ledge_next && ledge_tries_ < 3) {
            if (ledge_tries_++ == 0) say(climbable() == "climb up" ? "Climbing." : "Dropping down.");
            char lb[120];
            std::snprintf(lb, sizeof lb, "[WALK] at a ledge (%s), try %d: pressing action, facing %d", climbable().c_str(), ledge_tries_, s_.dir);
            h_.log(lb);
            stuck_ = 0;
            walk_floor_ = -1;                 // force a re-plan from the new level
            return BTN_ACTION;
        }
        // some stairs are taken with the action button, not walked onto (116's fire escape):
        // stopped at a stairway's end, press it
        bool at_stair_end = false;
        for (auto& q : stair_ends_) if (std::hypot(q.x - s_.x, q.z - s_.z) < 900) at_stair_end = true;
        if (at_stair_end && stuck_ >= 3 && h_.ms() - stair_press_ms_ > 1500 && stair_tries_ < 2) {
            stair_press_ms_ = h_.ms();
            stair_tries_++;
            say("Taking the stairs.");
            h_.log("[WALK] stopped at a stairway's end: pressing action");
            stuck_ = 0;
            return BTN_ACTION;
        }
        if (stuck_ > 20 && walk_.size() <= 2 && walk_target_.walk_on && walk_target_.name.rfind("Push the vehicle", 0) == 0 &&
            to_t < 3000) {
            walk_.clear(); walking_ = false; stuck_ = 0;
            start_push();
            return 0;
        }
        // pressed against the thing itself on the last stretch (a switch in a shelf, 112): that is arrival
        if (stuck_ > 20 && walk_.size() == 1 && to_t < 450 && walk_target_.id >= 0 && !walk_target_.walk_on) {
            walk_.clear(); stuck_ = 0;
            return steer();
        }
        if (++stuck_ > 45) { walk_unreachable_ = true; stop_walk("Auto-walk blocked."); return 0; }
    } else stuck_ = 0;
    last_x_ = s_.x; last_z_ = s_.z;
    return b;
}

// ---------------------------------------------------------------- movies
// The movie table holds a path per entry ("pl1\zmovie\opn1stc.bin"); the audio description
// for it lives in RE2Access\movies\opn1stc\ (cues.txt: start ms, tab, WAV file, tab, text).
std::string Mod::movie_name(int index, bool claire) {
    if (index < 0 || index >= addr::MOVIE_ENTRIES) return "";
    uint32_t p = h_.mem().u32((claire ? addr::MOVIE_TABLE_C : addr::MOVIE_TABLE_L) + index * 12);
    if (!p) return "";
    std::vector<uint8_t> b = h_.mem().bytes(p, 40);
    std::string path;
    for (uint8_t c : b) { if (!c) break; path += (char)std::tolower(c); }
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    return path;
}

bool Mod::load_movie_cues(const std::string& name) {
    movie_cues_.clear();
    std::ifstream f(h_.mod_data_dir() + "/movies/" + name + "/cues.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t a = line.find('\t');
        if (a == std::string::npos) continue;
        size_t b = line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        MovieCue c;
        c.ms = std::atoi(line.substr(0, a).c_str());
        c.file = line.substr(a + 1, b - a - 1);
        c.text = line.substr(b + 1);
        movie_cues_.push_back(c);
    }
    std::sort(movie_cues_.begin(), movie_cues_.end(), [](const MovieCue& x, const MovieCue& y) { return x.ms < y.ms; });
    return !movie_cues_.empty();
}

// Runs every frame, in game or not (the openings play from the title screen). The clock starts
// when the game's movie routine reaches "started" (state 2): REbirth's player does not update
// the game's own frame counter.
void Mod::frame_movie() {
    int st = h_.mem().u8(addr::MOVIE_STATE);
    int idx = h_.mem().u8(addr::MOVIE_INDEX);
    bool playing = (st == 2 || st == 3);
    uint64_t now = h_.ms();
    if (playing && !movie_on_) {
        bool claire = (h_.mem().u32(addr::STATUS_FLG) >> 31) != 0;
        movie_on_ = true;
        movie_index_ = idx;
        movie_t0_ = now;
        movie_next_ = 0;
        movie_name_ = movie_name(idx, claire);
        bool have = !movie_name_.empty() && load_movie_cues(movie_name_);
        h_.log("[MOVIE] start " + movie_name_ + " (entry " + std::to_string(idx) + ", " + (claire ? "Claire" : "Leon") +
               " table, state " + std::to_string(st) + ")" +
               (have ? ", " + std::to_string(movie_cues_.size()) + " description lines" : ", no audio description") +
               (ad_enabled_ ? "" : ", descriptions off"));
    } else if (movie_on_ && (!playing || idx != movie_index_)) {
        h_.stop_file();
        h_.log("[MOVIE] end " + movie_name_ + " after " + std::to_string(now - movie_t0_) + " ms, state " +
               std::to_string(st) + (h_.mem().u8(addr::MOVIE_SKIPPED) ? ", skipped" : ""));
        movie_on_ = false;
        movie_cues_.clear();
        // The text the game shows after the opening movies, before the first room (English
        // text from bio2.exe 0x540520, in the game's own text code, not a picture): the first
        // scenario's opening ends with the car crash (opn2ndc/opn2ndl), the second scenario's
        // with its other version (opn2ndrc/opn2ndrl).
        if (movie_name_ == "opn2ndc" || movie_name_ == "opn2ndl") say_transition_text(0, "after the opening");
        else if (movie_name_ == "opn2ndrc" || movie_name_ == "opn2ndrl") say_transition_text(1, "after the opening");
    }
    if (!movie_on_ || !ad_enabled_) return;
    long t = (long)(now - movie_t0_) + ad_offset_ms_;
    while (movie_next_ < movie_cues_.size() && movie_cues_[movie_next_].ms <= t) {
        const MovieCue& c = movie_cues_[movie_next_++];
        if (t - c.ms > 1500) { h_.log("[AD] late, skipped: " + c.text); continue; }   // after a hitch, don't talk over the next line
        h_.play_file(h_.mod_data_dir() + "\\movies\\" + movie_name_ + "\\" + c.file);
        h_.log("[AD] " + std::to_string(t) + " ms: " + c.text);
    }
}

// The narrated introduction before the opening movies (bio2.exe 0x4CF1F0, pictures from
// open00-open03.adt). Its sprite table pointer is at 0x9800BC (0x533600 for the first
// scenario's introduction: 28 entries) and its scene number at 0x9800B6 (u16); each scene shows
// a set of table entries chosen by the mask at 0x980AE4. First scenario: scene 2 the virus-cell
// background, 3 the S.T.A.R.S. badge then the forest photo, 5 the Umbrella logo then the lab
// photo. RE2Access\movies\intro_<table>\cues.txt maps scene -> WAV. Other tables are logged.
void Mod::frame_intro_probe() {
    uint32_t sys = h_.mem().u32(addr::SYSTEM_FLG);
    if (movie_on_ || s_.in_game || !(sys & 0x20000)) { intro_scene_ = -1; set_intro_audio(false); return; }
    uint32_t table = h_.mem().u32(0x9800BC);
    int scene = h_.mem().u16(0x9800B6);
    if (scene == intro_scene_ && table == intro_table_) { set_intro_audio(!intro_cues_.empty()); return; }
    bool new_table = table != intro_table_;
    intro_scene_ = scene;
    intro_table_ = table;
    char name[32];
    std::snprintf(name, sizeof name, "intro_%06X", (unsigned)table);
    if (new_table) {
        intro_cues_.clear();
        std::ifstream f(h_.mod_data_dir() + "/movies/" + name + "/cues.txt");
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            MovieCue c;
            c.ms = std::atoi(line.substr(0, a).c_str());     // the scene number
            c.file = line.substr(a + 1, b - a - 1);
            c.text = line.substr(b + 1);
            intro_cues_.push_back(c);
        }
    }
    h_.log(std::string("[INTRO] ") + name + " scene " + std::to_string(scene) + " line " + std::to_string(h_.mem().u8(0x9800AD)));
    set_intro_audio(!intro_cues_.empty());
    if (!ad_enabled_) return;
    for (const MovieCue& c : intro_cues_) {
        if (c.ms != scene) continue;
        h_.play_file(h_.mod_data_dir() + "\\movies\\" + name + "\\" + c.file);
        h_.log("[AD] intro scene " + std::to_string(scene) + ": " + c.text);
    }
}

void Mod::set_intro_audio(bool on) {
    if (on == intro_audio_) return;
    intro_audio_ = on;
    h_.set_intro_audio(on);
}

// ---------------------------------------------------------------- transition texts
// Five English texts in the game's text code at bio2.exe 0x540520 (u16 offsets at 0x540776):
// shown full screen between scenes, not through the message system, and no code in bio2.exe
// refers to them directly. While not in a room the mod scans .data twice a second for a
// pointer into the block, logs each one ([TEXTPTR]) and says the text it points at.
static const uint32_t kTextBlock = 0x540520, kTextBlockEnd = 0x540776;
static const uint32_t kTextStart[5] = {0x540520, 0x5405A5, 0x540641, 0x5406B0, 0x540713};
static const char* const kTextSay[5] = {
    "They were parted by an unescapable destiny. This is just the beginning of their worst nightmare.",
    "At the same time, at the same place. You have to survive this nightmare to know the true end.",
    "You once again stepped into the world of survival horror. Good luck...",
    "Fighting foes is not the only way to survive this horror.",
    "G... G-Virus... I have to bring it to Umbrella...",
};

void Mod::say_transition_text(int k, const char* why) {
    if (k < 0 || k > 4) return;
    uint64_t now = h_.ms();
    if (text_said_ms_[k] && now - text_said_ms_[k] < 20000) return;   // already said for this showing
    text_said_ms_[k] = now;
    h_.log(std::string("[TEXT] ") + std::to_string(k + 1) + " (" + why + "): " + kTextSay[k]);
    say_long(kTextSay[k]);
}

void Mod::frame_text_scan() {
    if (s_.in_game) return;
    uint64_t now = h_.ms();
    if (now - text_scan_ms_ < 500) return;
    text_scan_ms_ = now;
    std::vector<uint8_t> buf;
    for (uint32_t base = 0x51A000; base < 0x99EE58; base += 0x10000) {
        uint32_t n = std::min<uint32_t>(0x10000, 0x99EE58 - base);
        buf = h_.mem().bytes(base, n);
        if (buf.size() != n) continue;
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            uint32_t v;
            std::memcpy(&v, &buf[i], 4);
            if (v < kTextBlock || v >= kTextBlockEnd) continue;
            uint32_t at = base + i;
            if (at >= 0x540400 && at < 0x540800) continue;   // the block itself
            auto it = text_ptrs_.find(at);
            if (it != text_ptrs_.end() && it->second == v) continue;
            text_ptrs_[at] = v;
            int k = 0;
            while (k < 4 && v >= kTextStart[k + 1]) k++;
            char line[96];
            std::snprintf(line, sizeof line, "[TEXTPTR] %08X = %08X (text %d)", (unsigned)at, (unsigned)v, k + 1);
            h_.log(line);
            say_transition_text(k, "pointer");
        }
    }
}

}  // namespace re2

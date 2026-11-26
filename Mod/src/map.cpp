// The map: an accessible stand-in for the game's map screen.
//
// The game's map shows, per area and floor, the rooms (the one you are in, the ones you have
// been in, the ones you have not), and the doors between them coloured by the key that opens
// them. It only shows an area once its map has been found. The mod gives more: every room
// of the game for this character, from the room files themselves, with
//   - whether you have been in it (FG_MAP, bit = per-stage base + room; bio2.exe 0x4E4040),
//   - its doors: where each leads, and whether it is locked and which key opens it
//     (door data lock id/type; unlocked once FG_KEY[id & 0x7F] is set),
//   - the items still lying there (item_aot_set entries whose FG_ITEM flag is not set yet),
//   - a typewriter or an item box,
// and a route from the room you are in to any other room, with travel help: the door to take
// is selected in the catalog in each room on the way.
#include "mod.h"
#include "tables.inc"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>

namespace re2 {

namespace {

const int kMapBase[7] = {0, 30, 58, 72, 89, 99, 123};   // bio2.exe 0x4E4040: FG_MAP bit = base[stage] + room

uint16_t rd16(const std::vector<uint8_t>& b, size_t p) { return (uint16_t)(b[p] | (b[p + 1] << 8)); }

bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

}  // namespace

std::string Mod::map_area_of(const std::string& code, const std::string& name) const {
    int stage = code.empty() ? 0 : code[0] - '0';
    if (has(name, "1st floor")) return "Police station, 1st floor";
    if (has(name, "2nd floor")) return "Police station, 2nd floor";
    if (has(name, "3rd floor")) return "Police station, 3rd floor";
    if (name.rfind("RPD Basement", 0) == 0) return "Police station basement";
    static const std::map<std::string, std::string> kOverride = {
        {"10D", "Police station, 2nd floor"}, {"10E", "Police station, 2nd floor"}, {"114", "Police station, 2nd floor"},
        {"115", "Police station, 2nd floor"}, {"21B", "Police station, 2nd floor"}, {"108", "Police station roof"},
        {"11C", "Police station roof"}, {"111", "Police station, 3rd floor"},
    };
    auto it = kOverride.find(code);
    if (it != kOverride.end()) return it->second;
    switch (stage) {
        case 1: return "The city and the police station grounds";
        case 2: return "Police station";
        case 3: return "Underground and disposal plant";
        case 4: return "Sewers";
        case 5: return "Marshalling yard";
        case 6: return "Laboratory";
        case 7: return "Laboratory, lower levels";
        default: return "Other";
    }
}

// Reads every room file once (the first time the map is used).
void Mod::map_build() {
    if (map_built_ && map_character_ == s_.character) return;
    map_rooms_.clear();
    map_order_.clear();
    std::vector<std::pair<std::string, std::string>> parked;   // doors a script switches on later
    std::string dir = h_.game_data_dir();
    for (int stage = 0; stage < 7; stage++) {
        for (int room = 0; room < 0x30; room++) {
            RoomFile rf = load_room(dir, stage, room, s_.character);
            if (!rf.ok) continue;
            MapRoom m;
            m.code = room_code(stage, room);
            m.stage = stage; m.room = room;
            m.name = room_name(m.code);
            m.area = map_area_of(m.code, m.name);
            map_scan_items(rf, m);                     // doors set up as the room loads, items, typewriter
            // doors the story adds later (events): only where nothing leads there yet
            for (auto& d : rf.script_doors) {
                std::string to = room_code(d.stage, d.room);
                if (d.parked && d.stage >= 0 && d.stage <= 6 && to != m.code) parked.push_back({m.code, to});
                if (d.stage < 0 || d.stage > 6 || to == m.code || d.parked) continue;
                bool known = false;
                for (auto& o : m.doors) if (o.to == to) known = true;
                if (known || m.other_story.count(to)) continue;
                MapDoor md;
                md.to = to; md.lock_id = d.lock_id; md.lock_type = d.lock_type;
                m.doors.push_back(md);
            }
            map_rooms_[m.code] = m;
            map_order_.push_back(m.code);
        }
    }
    // a parked door whose room leads back here is reached by a way inside the room (21B's
    // elevator down to 308: the door there is switched on by the lift's own event)
    for (auto& pd : parked) {
        auto a = map_rooms_.find(pd.first), b = map_rooms_.find(pd.second);
        if (a == map_rooms_.end() || b == map_rooms_.end()) continue;
        bool there = false, back = false;
        for (auto& d : a->second.doors) if (d.to == pd.second) there = true;
        for (auto& d : b->second.doors) if (d.to == pd.first) back = true;
        if (!there && back) { MapDoor md; md.to = pd.second; md.via_event = true; a->second.doors.push_back(md); }
    }
    // drop doors to rooms that do not exist for this character
    for (auto& kv : map_rooms_) {
        auto& v = kv.second.doors;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const MapDoor& d) { return !map_rooms_.count(d.to); }), v.end());
    }
    map_areas_.clear();
    for (auto& c : map_order_) {
        const std::string& a = map_rooms_[c].area;
        if (std::find(map_areas_.begin(), map_areas_.end(), a) == map_areas_.end()) map_areas_.push_back(a);
    }
    static const char* kAreaOrder[] = {"The city and the police station grounds", "Police station, 1st floor",
                                       "Police station, 2nd floor", "Police station, 3rd floor", "Police station roof",
                                       "Police station basement", "Police station", "Underground and disposal plant",
                                       "Sewers", "Marshalling yard", "Laboratory", "Laboratory, lower levels"};
    auto rank = [&](const std::string& a) {
        for (int i = 0; i < (int)(sizeof(kAreaOrder) / sizeof(kAreaOrder[0])); i++) if (a == kAreaOrder[i]) return i;
        return 99;
    };
    std::stable_sort(map_areas_.begin(), map_areas_.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    map_built_ = true;
    map_character_ = s_.character;
    h_.log("[MAP] " + std::to_string(map_rooms_.size()) + " rooms in " + std::to_string(map_areas_.size()) + " areas");
}

// Items, typewriters and item boxes a room's scripts set up. Conditions that can be read now
// (character, story flags, keys) pick a branch; others (this room's own temporary flags) take
// both, so an item may be listed that is not there in this story.
void Mod::map_scan_items(const RoomFile& rf, MapRoom& m) {
    const auto& b = rf.raw;
    auto scan = [&](size_t start, size_t end, auto& self, int depth) -> void {
        if (depth > 2) return;
        struct Frame { size_t end; int mode; };   // mode 0 taking the if branch, 2 both
        std::vector<Frame> st;
        size_t pc = start;
        for (int steps = 0; steps < 3000 && pc < end && pc < b.size(); steps++) {
            while (!st.empty() && pc >= st.back().end) st.pop_back();
            uint8_t op = b[pc];
            int sz = op < sizeof(kOpSize) ? kOpSize[op] : 0;
            if (sz <= 0 || pc + sz > b.size()) return;
            if (op == 0x01 && st.empty()) return;                     // evt_end
            if (op == 0x06) {                                          // if: evaluate its conditions
                size_t fail = pc + sz + rd16(b, pc + 2);
                size_t q = pc + sz;
                int known = 1;                                         // 1 true, 0 false, 2 unknown
                bool fixed_false = false;                              // false by the character or scenario (flags 1.0, 1.1)
                while (q < b.size()) {
                    uint8_t c = b[q];
                    int csz = c < sizeof(kOpSize) ? kOpSize[c] : 0;
                    if (csz <= 0) break;
                    if (c == 0x21) {                                   // ck group index value
                        int g = b[q + 1];
                        bool v = reader_.flag(g, b[q + 2]) == (b[q + 3] != 0);
                        if (g == 5 || g == 2) known = (known == 0) ? 0 : 2;   // room and stop flags: not for other rooms
                        else if (!v) { known = 0; if (g == 1 && b[q + 2] <= 1) fixed_false = true; }
                    } else if (c == 0x23 || c == 0x5E || c == 0x88 || c == 0x3E) {
                        known = (known == 0) ? 0 : 2;
                    } else break;
                    q += csz;
                }
                if (known == 0) {
                    // the doors in a branch for the other character or scenario (404's stairs
                    // to the North Control Room are Leon's): not doors "the story adds later"
                    if (fixed_false)
                        for (size_t r = q; r < fail && r < b.size();) {
                            uint8_t c2 = b[r];
                            int s2 = c2 < sizeof(kOpSize) ? kOpSize[c2] : 0;
                            if (s2 <= 0 || r + s2 > b.size()) break;
                            if (c2 == OP_DOOR_AOT_SE || c2 == OP_DOOR_AOT_SET_4P) {
                                size_t o = c2 == OP_DOOR_AOT_SE ? 22 : 30;
                                m.other_story.insert(room_code(b[r + o], b[r + o + 1]));
                            }
                            r += s2;
                        }
                    pc = fail;
                    continue;
                }
                st.push_back({fail, known == 2 ? 2 : 0});
                pc = q;
                continue;
            }
            if (op == 0x07) {                                          // else
                size_t after = pc + rd16(b, pc + 2);
                if (!st.empty()) {
                    int mode = st.back().mode;
                    st.pop_back();
                    if (mode == 2) { st.push_back({after, 2}); pc += sz; continue; }
                }
                pc = after;
                continue;
            }
            if (op == 0x18) {                                          // gosub
                int p = b[pc + 1];
                if (p < (int)rf.main_procs.size()) self(rf.main_procs[p].first, rf.main_procs[p].second, self, depth + 1);
            }
            if (op == OP_ITEM_AOT_SET || op == OP_ITEM_AOT_SET_4P) {
                size_t base = op == OP_ITEM_AOT_SET ? 14 : 22;
                MapItem it;
                it.item = rd16(b, pc + base); it.count = rd16(b, pc + base + 2); it.flag = rd16(b, pc + base + 4);
                bool dup = false;
                for (auto& o : m.items) if (o.flag == it.flag && o.item == it.item) dup = true;
                if (!dup && it.item > 0 && it.item < kItemCount) m.items.push_back(it);
            }
            if (op == OP_DOOR_AOT_SE || op == OP_DOOR_AOT_SET_4P) {
                size_t o = op == OP_DOOR_AOT_SE ? 22 : 30;
                MapDoor md;
                md.to = room_code(b[pc + o], b[pc + o + 1]);
                md.lock_id = b[pc + o + 7]; md.lock_type = b[pc + o + 8];
                // a door with no area or that the player cannot trigger (sat 0) is a script's
                // own transition (a fall, a cutscene, a lift switched on later): not on the map
                bool parked = b[pc + 3] == 0 || (op == OP_DOOR_AOT_SE && (rd16(b, pc + 10) == 0 || rd16(b, pc + 12) == 0));
                if (b[pc + o] <= 6 && md.to != m.code && !parked) m.doors.push_back(md);
            }
            if (op == OP_AOT_SET || op == OP_AOT_SET_4P) {
                int sce = b[pc + 2];
                if (sce == SCE_SAVE) m.typewriter = true;
                if (sce == SCE_ITEMBOX) m.item_box = true;
            }
            pc += sz;
        }
    };
    if (rf.init_end > rf.init_start) scan(rf.init_start, rf.init_end, scan, 0);
    if (!rf.main_procs.empty()) scan(rf.main_procs[0].first, rf.main_procs[0].second, scan, 0);
}

bool Mod::map_visited(const MapRoom& m) const {
    if (m.code == s_.room_code) return true;
    if (m.stage < 0 || m.stage > 6) return false;
    return reader_.flag(9, kMapBase[m.stage] + m.room);
}

bool Mod::map_have(int item) const {
    for (auto& it : s_.inventory) if (it.id == item) return true;
    return false;
}

// a door from room m to room to: "open", "locked, needs the X", "locked"; passable for routes
std::string Mod::map_door_state(const MapRoom& m, const std::string& to, bool& passable) const {
    passable = false;
    // the room you are in: the doors as the game has them now
    if (m.code == s_.room_code) {
        bool seen = false, open = false;
        std::string st = "locked";
        for (auto& a : s_.aots) {
            if (a.sce != SCE_DOOR || !a.player_triggers() || room_code(a.door_stage, a.door_room) != to) continue;
            seen = true;
            if (!a.door_locked) open = true;
            else if (a.lock_type > 0 && a.lock_type < kItemCount && kItemNames[a.lock_type][0] && a.lock_type != 0xFF) {
                if (map_have(a.lock_type)) { open = true; st = "locked, you have the " + item_name(a.lock_type); }
                else st = "locked, needs the " + item_name(a.lock_type);
            }
        }
        // events: one in front of the door that keeps it shut ("It's electronically locked"),
        // or a way through (a ladder, a lift)
        if (!open)
            for (auto& a : s_.aots) {
                if ((a.sce != SCE_EVENT && a.sce != SCE_MESSAGE) || !a.player_triggers()) continue;
                std::string d = describe(a, true);
                if (!has(d, (" to " + room_name(to)).c_str())) continue;
                seen = true;
                if (has(d, "too small") || has(d, "can't enter")) st = "can't enter";
                else if (has(d, ", locked") || has(d, "won't open")) st = has(d, "won't open") ? "won't open" : "locked";
                else open = true;
            }
        if (seen) { passable = open; return open && st == "locked" ? "open" : st; }
        for (auto& d : m.doors)
            if (d.to == to && d.via_event) { passable = inroom_way() >= 0; return passable ? "open" : "not open yet"; }
    }
    std::string best = "locked";
    bool any = false;
    for (auto& d : m.doors) {
        if (d.to != to) continue;
        any = true;
        // a way through a lift or the like: known to work once the room beyond has been visited
        if (d.via_event) {
            auto t = map_rooms_.find(to);
            if (t != map_rooms_.end() && map_visited(t->second)) { passable = true; return "open"; }
            continue;
        }
        bool unlocked = d.lock_type == 0 || d.lock_type == 0xFE || reader_.flag(31, d.lock_id & 0x7F);
        if (unlocked) { passable = true; return d.lock_type == 0xFE && !reader_.flag(31, d.lock_id & 0x7F) ? "opens from this side" : "open"; }
        if (d.lock_type != 0xFF && d.lock_type < kItemCount && kItemNames[d.lock_type][0]) {
            if (map_have(d.lock_type)) { passable = true; best = "locked, you have the " + item_name(d.lock_type); }
            else if (best == "locked") best = "locked, needs the " + item_name(d.lock_type);
        }
    }
    if (!any) return "";
    return best;
}

// the room you are in: its doors as they are now (a lift switched on, a door replaced by an
// event, a ladder): door AOTs, and events named after where they lead
std::vector<std::string> Mod::map_live_neighbours() const {
    std::vector<std::string> out;
    for (auto& a : s_.aots) {
        if (!a.player_triggers()) continue;
        std::string to;
        if (a.sce == SCE_DOOR) to = room_code(a.door_stage, a.door_room);
        else if (a.sce == SCE_EVENT || a.sce == SCE_MESSAGE) {
            std::string d = describe(a, true);
            size_t best = 0;
            for (auto& kv : map_rooms_) {
                const std::string& nm = kv.second.name;
                if (nm.size() > best && has(d, (" to " + nm).c_str())) { best = nm.size(); to = kv.first; }
            }
        }
        if (!to.empty() && to != s_.room_code && map_rooms_.count(to) && std::find(out.begin(), out.end(), to) == out.end())
            out.push_back(to);
    }
    return out;
}

// a way to another part of this room that the player can use (21B's elevator)
int Mod::inroom_way() const {
    for (auto& a : s_.aots) {
        if (!a.player_triggers()) continue;
        if (a.sce == SCE_DOOR && a.door_stage == s_.stage && a.door_room == s_.room) return a.id;
        if (a.sce == SCE_EVENT || a.sce == SCE_MESSAGE) {
            std::string d = describe(a, true);
            if (d == "Elevator" || d == "Way to " + room_name(s_.room_code) || d == "Way to another part of this room") return a.id;
        }
    }
    return -1;
}

std::vector<std::string> Mod::map_neighbours(const MapRoom& m) const {
    if (m.code == s_.room_code) {
        auto live = map_live_neighbours();
        if (inroom_way() >= 0)
            for (auto& d : m.doors)
                if (d.via_event && std::find(live.begin(), live.end(), d.to) == live.end()) live.push_back(d.to);
        if (!live.empty()) return live;
    }
    std::vector<std::string> out;
    for (auto& d : m.doors)
        if (std::find(out.begin(), out.end(), d.to) == out.end()) out.push_back(d.to);
    return out;
}

std::vector<std::string> Mod::map_route(const std::string& from, const std::string& to) const {
    std::map<std::string, std::string> prev;
    std::deque<std::string> q{from};
    prev[from] = "";
    while (!q.empty()) {
        std::string c = q.front();
        q.pop_front();
        if (c == to) break;
        auto it = map_rooms_.find(c);
        if (it == map_rooms_.end()) continue;
        for (auto& n : map_neighbours(it->second)) {
            if (prev.count(n)) continue;
            bool pass;
            map_door_state(it->second, n, pass);
            if (!pass) continue;
            // a door the journey could not walk to lately (307's ladder behind the fence)
            auto bl = travel_blocked_.find(c + ">" + n);
            if (bl != travel_blocked_.end() && h_.ms() - bl->second < 600000) continue;
            prev[n] = c;
            q.push_back(n);
        }
    }
    if (!prev.count(to)) return {};
    std::vector<std::string> path;
    for (std::string c = to; !c.empty(); c = prev[c]) path.push_back(c);
    std::reverse(path.begin(), path.end());
    return path;
}

std::string Mod::map_describe(const MapRoom& m, bool full) const {
    std::string t = m.name + ". ";
    if (m.code == s_.room_code) t += "You are here. ";
    else t += map_visited(m) ? "Visited. " : "Not visited yet. ";
    if (!full) return t;
    t += m.area + ". ";
    auto ns = map_neighbours(m);
    if (ns.empty()) t += "No doors. ";
    else {
        t += std::to_string(ns.size()) + (ns.size() == 1 ? " door: " : " doors: ");
        for (size_t i = 0; i < ns.size(); i++) {
            bool pass;
            std::string st = map_door_state(m, ns[i], pass);
            auto nt = map_rooms_.find(ns[i]);
            t += (i ? "; " : "") + room_name(ns[i]) + (st == "open" ? "" : ", " + st);
            if (nt != map_rooms_.end() && !map_visited(nt->second)) t += ", not visited";
        }
        t += ". ";
    }
    std::vector<std::string> items;
    for (auto& it : m.items) {
        if (it.flag > 0 && reader_.flag(m.stage >= 4 ? 34 : 8, it.flag)) continue;
        if (it.item >= 104 && it.item <= 128 && (s_.file_flags & (0x80000000u >> (it.item - 104)))) continue;
        std::string n = item_name(it.item);
        if (it.item <= 30 && it.count > 1) n += " " + std::to_string(it.count);
        items.push_back(n);
    }
    if (!items.empty()) {
        t += "Items there: ";
        for (size_t i = 0; i < items.size(); i++) t += (i ? ", " : "") + items[i];
        t += ". ";
    }
    if (m.typewriter) t += "Typewriter. ";
    if (m.item_box) t += "Item box. ";
    return t;
}

std::vector<std::string> Mod::map_area_rooms(const std::string& area) const {
    std::vector<std::string> out;
    for (auto& c : map_order_) {
        auto it = map_rooms_.find(c);
        if (it != map_rooms_.end() && it->second.area == area) out.push_back(c);
    }
    return out;
}

// M: the map at a glance from here
void Mod::key_map_here() {
    map_build();
    auto it = map_rooms_.find(s_.room_code);
    if (it == map_rooms_.end()) { say("This room is not on the map.", true); return; }
    auto rooms = map_area_rooms(it->second.area);
    int seen = 0;
    for (auto& c : rooms) if (map_visited(map_rooms_[c])) seen++;
    std::string t = it->second.area + ": " + std::to_string(rooms.size()) + " rooms, " + std::to_string(seen) + " visited. ";
    t += map_describe(it->second, true);
    if (!travel_to_.empty()) t += "Heading to " + room_name(travel_to_) + ". ";
    t += "Shift M opens the map.";
    say(t, true);
}

// Shift M: open or close the map
void Mod::key_map_mode() {
    map_build();
    if (map_mode_) { map_mode_ = false; say("Map closed.", true); return; }
    open_map("");
}

void Mod::open_map(const std::string& lead) {
    map_build();
    auto it = map_rooms_.find(s_.room_code);
    map_mode_ = true;
    map_sel_ = it != map_rooms_.end() ? s_.room_code : (map_order_.empty() ? "" : map_order_[0]);
    map_area_ = map_sel_.empty() ? "" : map_rooms_[map_sel_].area;
    say(lead + "Map. " + map_area_ + ". " + (map_sel_.empty() ? "" : map_describe(map_rooms_[map_sel_], false)) +
        "Minus and equals: rooms. Brackets: areas. Apostrophe: about the room. Backspace: the way there. "
        "Backslash: go there. Shift M: close the map.", true);
}

bool Mod::map_hotkey(Hotkey k) {
    if (!map_mode_) return false;
    auto rooms = map_area_rooms(map_area_);
    switch (k) {
        case HK_NEXT: case HK_PREV: {
            if (rooms.empty()) return true;
            auto f = std::find(rooms.begin(), rooms.end(), map_sel_);
            int i = f == rooms.end() ? 0 : (int)(f - rooms.begin());
            i = (i + (k == HK_NEXT ? 1 : -1) + (int)rooms.size()) % (int)rooms.size();
            if (f == rooms.end()) i = k == HK_NEXT ? 0 : (int)rooms.size() - 1;
            map_sel_ = rooms[i];
            say(map_describe(map_rooms_[map_sel_], false) + std::to_string(i + 1) + " of " + std::to_string(rooms.size()), true);
            return true;
        }
        case HK_CAT_NEXT: case HK_CAT_PREV: {
            if (map_areas_.empty()) return true;
            auto f = std::find(map_areas_.begin(), map_areas_.end(), map_area_);
            int i = f == map_areas_.end() ? 0 : (int)(f - map_areas_.begin());
            i = (i + (k == HK_CAT_NEXT ? 1 : -1) + (int)map_areas_.size()) % (int)map_areas_.size();
            map_area_ = map_areas_[i];
            auto r = map_area_rooms(map_area_);
            int seen = 0;
            for (auto& c : r) if (map_visited(map_rooms_[c])) seen++;
            map_sel_ = r.empty() ? "" : r[0];
            for (auto& c : r) if (c == s_.room_code) map_sel_ = c;
            say(map_area_ + ", " + std::to_string(r.size()) + " rooms, " + std::to_string(seen) + " visited.", true);
            return true;
        }
        case HK_ROOM: {
            auto it = map_rooms_.find(map_sel_);
            if (it != map_rooms_.end()) say(map_describe(it->second, true), true);
            return true;
        }
        case HK_WHERE: {
            if (map_sel_ == s_.room_code) { say("You are in " + room_name(map_sel_) + ".", true); return true; }
            auto path = map_route(s_.room_code, map_sel_);
            if (path.empty()) {
                say("No open way to " + room_name(map_sel_) + " is known from here yet: locked doors or doors the story has not opened are in the way.", true);
                return true;
            }
            std::string t = std::to_string(path.size() - 1) + (path.size() == 2 ? " door" : " doors") + " to " + room_name(map_sel_) + ": ";
            for (size_t i = 1; i < path.size(); i++) {
                bool pass;
                std::string st = map_door_state(map_rooms_[path[i - 1]], path[i], pass);
                t += (i > 1 ? ", then " : "") + room_name(path[i]) + (st == "open" || st.empty() ? "" : " (" + st + ")");
            }
            say(t + ".", true);
            return true;
        }
        case HK_GO: {
            if (map_sel_ == s_.room_code) { say("You are already in " + room_name(map_sel_) + ".", true); return true; }
            if (s_.message.active && !game_map_open_) { say("A message is still open. Press action to close it first.", true); return true; }
            auto path = map_route(s_.room_code, map_sel_);
            if (path.empty()) { say("No open way to " + room_name(map_sel_) + " is known from here yet.", true); return true; }
            travel_to_ = map_sel_;
            travel_seen_.clear();
            map_mode_ = false;
            say("Heading to " + room_name(travel_to_) + ", " + std::to_string(path.size() - 1) +
                (path.size() == 2 ? " door." : " doors."), true);
            // from the game's map screen: close it first (cancel), then set off (frame)
            if (game_map_open_) { travel_after_map_ = true; cancel_frames_ = 3; }
            else travel_step(true);
            return true;
        }
        case HK_HELP:
            say("Map. Minus and equals: rooms in this area. Left and right bracket: areas. Apostrophe: the selected "
                "room's doors, items, typewriter and item box. Backspace: the way there from here. Backslash: travel "
                "there; in each room on the way the door to take is selected, and backslash walks to it. "
                "Shift M: close the map. M: the map from where you are.", true);
            return true;
        default:
            return false;   // other keys work as usual
    }
}

// Travelling to a room picked on the map: in each room select the door to take and walk to it.
void Mod::travel_step(bool walk) {
    if (travel_to_.empty()) return;
    if (travel_lift_room_ != s_.room_code) { travel_lift_room_ = s_.room_code; travel_lift_tried_ = false; }
    map_build();
    if (s_.room_code == travel_to_) {
        say("You have reached " + room_name(travel_to_) + ".", false);
        travel_to_.clear(); travel_state_ = 0;
        return;
    }
    // a journey that comes back to a room twice (a door the map thinks is open but is not):
    // stop instead of going back and forth
    if (walk) {
        if (travel_seen_.empty() || travel_seen_.back() != s_.room_code) travel_seen_.push_back(s_.room_code);
        int times = 0;
        for (auto& r : travel_seen_) if (r == s_.room_code) times++;
        if (times >= 3) {
            say("Travel to " + room_name(travel_to_) + " stopped: it keeps coming back to " + room_name(s_.room_code) +
                ". The way there may not be open yet.", true);
            h_.log("[MAP] travel loop at " + s_.room_code + " toward " + travel_to_);
            travel_to_.clear(); travel_state_ = 0; travel_seen_.clear();
            return;
        }
    }
    auto path = map_route(s_.room_code, travel_to_);
    if (path.size() < 2) { say("Lost the way to " + room_name(travel_to_) + ".", false); travel_to_.clear(); travel_state_ = 0; return; }
    const std::string& next = path[1];
    if (!travel_blocked_.count(s_.room_code + ">" + next)) travel_len_ = path.size();   // the way as first planned
    // the catalog entry for a door to the next room (a door, or an event over one)
    auto list = entries(0);
    const Entry* pick = nullptr;
    for (auto& e : list) {
        if (e.type != Entry::AOT) continue;
        for (auto& a : s_.aots)
            if (a.id == e.id && a.sce == SCE_DOOR && room_code(a.door_stage, a.door_room) == next) pick = &e;
        if (!pick) {
            std::string nm = room_name(next);
            if (e.name.size() >= nm.size() && e.name.find(" to " + nm) != std::string::npos) pick = &e;
        }
        if (pick) break;
    }
    // reached by a way inside this room (21B's elevator): take that
    if (!pick) {
        int way = inroom_way();
        bool via = false;
        auto mr = map_rooms_.find(s_.room_code);
        if (mr != map_rooms_.end()) for (auto& d : mr->second.doors) if (d.to == next && d.via_event) via = true;
        if (via && way >= 0)
            for (auto& e : list) if (e.type == Entry::AOT && e.id == way) { pick = &e; break; }
    }
    if (!pick) {
        say("Toward " + room_name(travel_to_) + ": go to " + room_name(next) + "; its door is not in the catalog here.", false);
        travel_state_ = 0;
        return;
    }
    category_ = 0;
    sel_type_ = pick->type; sel_id_ = pick->id;
    Entry e = *pick;
    travel_door_id_ = e.id;
    travel_next_ = next;
    if (walk) {
        say("Toward " + room_name(travel_to_) + ", " + std::to_string(path.size() - 1) +
            (path.size() == 2 ? " door to go." : " doors to go."), false);
        travel_state_ = plan_walk(e, false) ? 1 : 0;
        if (!travel_state_ && !travel_next_.empty()) {
            // no way at all to that door now: plan around it (once)
            std::string key = s_.room_code + ">" + travel_next_;
            if (!travel_blocked_.count(key) || h_.ms() - travel_blocked_[key] > 600000) {
                travel_blocked_[key] = h_.ms();
                h_.log("[MAP] travel: no path to the door to " + travel_next_ + " from " + s_.room_code);
                auto alt = map_route(s_.room_code, travel_to_);
                if (alt.size() >= 2 && alt.size() <= travel_len_ + 4) { say("That way is blocked. Trying another way.", false); travel_step(true); return; }
            }
            // a lift in this room may be the way (506's turn table): ride it, then carry on
            if (!travel_lift_tried_) {
                travel_lift_tried_ = true;
                for (auto& le : entries(0))
                    if (le.type == Entry::AOT && le.name.rfind("Lift", 0) == 0) {
                        // the lift's control on this level ("Lift 1", "Lift 2": one at each end)
                        std::vector<Pt> lp; std::string how; Pt goal;
                        if (!route(le, lp, how, goal) || lp.empty() ||
                            std::hypot(lp.back().x - goal.x, lp.back().z - goal.z) > 1500) continue;
                        travel_lift_floor_ = s_.floor;
                        travel_lift_ms_ = h_.ms();
                        travel_lift_pressed_ = false;
                        travel_lift_yes_ = false;
                        say("Taking the lift.", false);
                        Entry l = le;
                        if (plan_walk(l, false)) { travel_state_ = 3; return; }
                    }
            }
            say("Travel to " + room_name(travel_to_) + " stopped: no way to the door to " + room_name(next) + " from here.", false);
            travel_to_.clear();
        }
    } else {
        travel_state_ = 0;
        say("Toward " + room_name(travel_to_) + ": " + e.name + " selected. Backslash walks there.", false);
    }
}

// Travelling: at the door, go through it (press action); in the next room the next door.
// Anything that stops the walk (an enemy, a locked door, the player taking over) pauses it:
// the door stays selected and backslash carries on.
void Mod::frame_travel() {
    if (travel_to_.empty() || travel_state_ == 0) return;
    uint64_t now = h_.ms();
    if (travel_state_ == 3) {
        // the lift asks first ("It's a switch for the lift. Will you push it?"): Yes, once
        if (s_.message.active && s_.message.yesno && s_.message.routine == 4 && s_.message.cursor == 0 && !travel_lift_yes_) {
            travel_lift_yes_ = true;
            confirm_frames_ = 2;
            return;
        }
        if (s_.message.active && !s_.message.yesno && s_.message.routine == 2 && page_frames_ == 0 && now - travel_page_ms_ > 600) {
            travel_page_ms_ = now;
            page_frames_ = 2;           // a page before the question
        }
        // riding a lift: once on another level and still, plan the way on from there
        if (s_.floor != travel_lift_floor_ && !walking_ && !event_running() && !s_.stop_flg && !s_.message.active) {
            travel_state_ = 0;
            // the doors out of this room that could not be reached before may be open now
            for (auto it = travel_blocked_.begin(); it != travel_blocked_.end();)
                it = it->first.rfind(s_.room_code + ">", 0) == 0 ? travel_blocked_.erase(it) : std::next(it);
            travel_step(true);
        } else if (!walking_ && face_dir_ < 0 && !travel_lift_pressed_ && !event_running() && !s_.stop_flg && !s_.message.active) {
            travel_lift_pressed_ = true;          // at the lift's control: work it
            travel_lift_ms_ = now;
            press_action_frames_ = 2;
        } else if (!walking_ && !event_running() && !s_.stop_flg && now - travel_lift_ms_ > 20000) {
            travel_state_ = 0;
            say("Travel to " + room_name(travel_to_) + " paused at the lift. Backslash carries on.", false);
        }
        return;
    }
    if (travel_state_ == 1) {
        if (walking_ || face_dir_ >= 0 || via_state_ != 0) return;   // via: a ladder on the way (frame_via)
        bool at_door = false;
        for (auto* a : actionable()) if (a->id == travel_door_id_) at_door = true;
        if (walk_target_.id == travel_door_id_ && at_door && !s_.message.active && !s_.stop_flg && !event_running()) {
            press_action_frames_ = 2;
            travel_state_ = 2;
            travel_ms_ = now;
            travel_room_ = s_.room_code;
            travel_confirmed_ = false;
            travel_pages_ = 0;
        } else {
            travel_state_ = 0;
            h_.log("[MAP] travel pause: target " + std::to_string(walk_target_.id) + " door " + std::to_string(travel_door_id_) +
                   " at_door " + std::to_string(at_door) + " msg " + std::to_string(s_.message.active) + " stop " +
                   std::to_string(s_.stop_flg) + " event " + std::to_string(event_running()));
            // at the door, but it is set on another level than the player's (405's door up to the
            // hidden passage is on floor 7: it is only for arriving): it cannot be used from here
            bool wrong_level = false;
            for (auto& a : s_.aots)
                if (a.id == travel_door_id_ && a.sce == SCE_DOOR && a.floor >= 0 && a.floor < 0x80 && a.floor != s_.game_floor &&
                    std::hypot(a.centre.x - s_.x, a.centre.z - s_.z) < 2500) wrong_level = true;
            if (wrong_level) walk_unreachable_ = true;
            // could not get to that door: plan around it from now on (for a while)
            if (walk_unreachable_ && !at_door && !travel_next_.empty()) {
                travel_blocked_[s_.room_code + ">" + travel_next_] = now;
                h_.log("[MAP] travel: no way to the door to " + travel_next_ + " from " + s_.room_code);
                auto alt = map_route(s_.room_code, travel_to_);
                if (alt.size() >= 2 && alt.size() <= travel_len_ + 4) {   // not a long way round
                    say("That way is blocked. Trying another way.", false);
                    travel_step(true);
                    return;
                }
                if (wrong_level) {
                    say("That way can't be used from here. Travel to " + room_name(travel_to_) + " stopped.", false);
                    travel_to_.clear();
                    return;
                }
            }
            say("Travel to " + room_name(travel_to_) + " paused. Backslash carries on.", false);
        }
    } else if (travel_state_ == 2) {
        // waiting for the next room (frame_game's room settle continues), or a message
        // (a locked door's key question: the player answers and goes through, and it carries on)
        // through the door: the next room's settle carries on (frame_game)
        if (s_.room_code != travel_room_ || room_pending_) return;
        // the door asks first (a ladder, stairs): Yes, once
        // a message before the question ("A ladder leads down."): go on to the next page, a few times
        if (s_.message.active && s_.message.routine == 2 && travel_pages_ < 4 && page_frames_ == 0 && now - travel_page_ms_ > 600) {
            travel_pages_++;
            travel_page_ms_ = now;
            page_frames_ = 2;
            h_.log("[MAP] travel: going on past the door's message");
        }
        if (s_.message.active && s_.message.yesno && s_.message.routine == 4 && s_.message.cursor == 0 && !travel_confirmed_) {
            travel_confirmed_ = true;
            confirm_frames_ = 2;
            h_.log("[MAP] travel: answering the door's question with Yes");
        }
        if (s_.message.active || ui_mode_ != 0 || s_.stop_flg) travel_ms_ = now;   // ui mode 4: the door opening (System_flg 0x4000)
        else if (now - travel_ms_ > 4000) {
            travel_state_ = 0;
            // the door only gave a message and the player is still here ("I have to get back to
            // Ben!", 219): that way is shut for now; go round if there is another way
            if (travel_pages_ > 0 && !travel_confirmed_ && !travel_next_.empty()) {
                travel_blocked_[s_.room_code + ">" + travel_next_] = now;
                h_.log("[MAP] travel: the door to " + travel_next_ + " gave only a message; treating it as shut");
                auto alt = map_route(s_.room_code, travel_to_);
                if (alt.size() >= 2 && alt.size() <= travel_len_ + 4) {   // not a long way round
                    say("That way is shut for now. Trying another way.", false);
                    travel_step(true);
                    return;
                }
                say("That way is shut for now. Travel to " + room_name(travel_to_) + " stopped.", false);
                travel_to_.clear();
                return;
            }
            say("Travel to " + room_name(travel_to_) + " paused. Backslash carries on.", false);
        }
    }
}

}  // namespace re2

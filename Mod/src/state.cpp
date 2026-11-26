#include "state.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include "re2_addr.h"
#include "tables.inc"

namespace re2 {

std::string item_name(int id) {
    if (id > 0 && id < kItemCount && kItemNames[id][0]) return kItemNames[id];
    return "item " + std::to_string(id);
}

std::string Enemy::name() const {
    static const char* people[16] = {"Chief Irons", "Ada", "Chief Irons", "Ada", "Ben", "Sherry", "Ben", "Annette",
                                     "Robert Kendo", "Annette", "Marvin", "Mayor's daughter", "Person", "Person",
                                     "Person", "Sherry"};
    if (type >= 0x40 && type < 0x50) return people[type - 0x40];
    if (type >= 0 && type < kEnemyCount && kEnemyNames[type][0]) {
        std::string n = kEnemyNames[type];
        if (n.rfind("Zombie Dog", 0) == 0 || n.rfind("Cerberus", 0) == 0) return "Dog";
        if (n.rfind("Zombie", 0) == 0) return "Zombie";
        if (n.rfind("Licker", 0) == 0) return "Licker";
        if (n.rfind("Tyrant", 0) == 0) return type == 0x2B ? "Super Tyrant" : "Mister X";
        if (n.rfind("Birkin", 0) == 0) return "G";
        if (n.rfind("Ivy", 0) == 0) return "Plant monster";
        if (n == "G Embryo") return "G embryo";
        if (n == "G Adult") return "G creature";
        if (n == "G tentacle") return "Tentacle";
        return n;
    }
    return "Enemy";
}

// The game's condition (bio2.exe 0x502530, used by the status screen's ECG 0x4FFAB0):
// 4 Poison while the poison timer runs; 0 Fine above half of maximum life; 1 Caution above 40;
// 2 Caution above 20; 3 Danger. ECG colours (table 0x53F2CC): green, yellow, orange, red, purple.
int condition_level(int hp, int max_hp, bool poisoned) {
    if (poisoned) return 4;
    if (max_hp <= 0) max_hp = 200;
    if (hp > max_hp / 2) return 0;
    if (hp > 40) return 1;
    if (hp > 20) return 2;
    return 3;
}

std::string condition_word(int level) {
    static const char* kWord[] = {"Fine", "Caution", "Caution", "Danger", "Poison"};
    return level >= 0 && level < 5 ? kWord[level] : "Fine";
}

std::string condition_colour(int level) {
    static const char* kColour[] = {"Green", "Yellow", "Orange", "Red", "Purple"};
    return level >= 0 && level < 5 ? kColour[level] : "Green";
}

bool StateReader::flag(int group, int index) {
    uint32_t base = m_.u32(addr::FLAG_GROUPS + group * 4);
    if (!base) return false;
    uint32_t word = m_.u32(base + ((index >> 5) & 7) * 4);
    return (word & (0x80000000u >> (index & 31))) != 0;
}

GameState StateReader::read() {
    GameState s;
    uint32_t probe;
    if (!m_.read(addr::SYSTEM_FLG, &probe, 4)) return s;
    s.valid = true;
    s.system_flg = probe;
    s.status_flg = m_.u32(addr::STATUS_FLG);
    // bits 0x40000000 and 0x20000000 freeze the partner (pEnemy[1]) and the other characters
    // (bio2.exe 0x4EF125), not the player: playing as Sherry, 0x40000000 stays set all the time
    s.stop_flg = m_.u32(addr::STOP_FLG) & ~0x60000000u;
    s.key = m_.u32(addr::KEY);
    s.in_game = (s.system_flg & addr::SYS_GAME_STARTED) != 0;
    s.stage = m_.s16(addr::STAGE);
    s.room = m_.s16(addr::ROOM);
    s.cut = m_.s16(addr::CUT);
    s.character = m_.u8(addr::PL_ID) & 1;
    s.room_code = room_code(s.stage, s.room);
    uint32_t P = addr::PLAYER;
    s.x = m_.s32(P + addr::W_POS_X);
    s.y = m_.s32(P + addr::W_POS_Y);
    s.z = m_.s32(P + addr::W_POS_Z);
    s.dir = m_.s16(P + addr::W_DIR) & 0xFFF;
    // the game's floor number (what AOT floor bytes are compared with) is not always the
    // level the player stands on: at the bottom of 116's stairs it still says 1, and on stairs
    // it jumps to the far end's floor. Routes go by height (1800 units a floor band).
    s.game_floor = m_.u8(P + addr::W_FLOOR);
    s.floor = std::max(0, (int)std::floor((-(double)s.y + 900.0) / 1800.0));
    s.hp = m_.s16(P + addr::W_LIFE);
    s.poisoned = m_.u16(addr::POISON_TIMER) != 0;
    s.max_hp = m_.s16(P + addr::W_MAX_LIFE);
    s.routine0 = m_.u8(P + addr::W_ROUTINE0);
    {
        int bw = m_.u16(P + addr::W_ATARI_X), bd = m_.u16(P + addr::W_ATARI_Z);
        int b = std::max(bw, bd);
        if (b >= 150 && b <= 1200) s.body_r = b;
    }
    s.max_items = m_.u8(addr::MAX_ITEM_NUM);
    if (s.max_items <= 0 || s.max_items > 10) s.max_items = 8;
    for (int i = 0; i < s.max_items; i++) {
        InvSlot it;
        it.id = m_.u8(addr::ITEM_WORK + i * 4);
        it.count = m_.u8(addr::ITEM_WORK + i * 4 + 1);
        it.part = m_.u8(addr::ITEM_WORK + i * 4 + 2);
        s.inventory.push_back(it);
    }
    s.top_slot.id = m_.u8(addr::ITEM_WORK + 10 * 4);
    s.top_slot.count = m_.u8(addr::ITEM_WORK + 10 * 4 + 1);
    s.equipped = m_.u8(addr::ST_EQUIP_ID);
    s.file_flags = m_.u32(addr::FILE_FLG);
    s.save_cursor = m_.u8(addr::SAVE_CURSOR);
    s.front_func = m_.u32(addr::FRONT_FUNC);
    StatusScreen& st = s.status;
    st.main_rtn = m_.u8(addr::ST_MAIN_RTN); st.sub_rtn0 = m_.u8(addr::ST_SUB_RTN0);
    st.cursor = m_.u8(addr::ST_ICURSOL_MAIN); st.cursor_sub = m_.u8(addr::ST_ICURSOL_SUB);
    st.menu0 = m_.u8(addr::ST_MCURSOL0); st.menu1 = m_.u8(addr::ST_MCURSOL1);
    st.type = m_.u8(addr::ST_TYPE);
    st.file_book = (int8_t)m_.u8(addr::ST_F_SEL_BOOK); st.file_sel = (int8_t)m_.u8(addr::ST_F_SEL_FILE);
    st.file_page = m_.u8(addr::ST_F_PAGE_NO); st.map_no = m_.u8(addr::ST_MAP_NO);
    st.ibox_sel = m_.u8(addr::ST_IBOX_SEL);
    st.item_state = m_.u8(addr::ST_ITEM_STATE); st.item_cmd = m_.u8(addr::ST_ITEM_CMD);
    st.item_step = m_.u8(addr::ST_ITEM_STEP); st.weapon_menu = m_.u8(addr::ST_WEAPON_MENU) != 0;
    TitleScreen& t = s.title;
    t.main_rtn = m_.u8(addr::TI_MAIN_RTN); t.sub_rtn0 = m_.u8(addr::TI_SUB_RTN0);
    t.sel_mode = m_.u8(addr::TI_SEL_MODE); t.cursor = m_.u8(addr::TI_CUR);
    t.screen = m_.u8(addr::TI_SCREEN); t.step = m_.u8(addr::TI_STEP); t.option = m_.u8(addr::TI_OPTION);
    t.hard_ok = m_.u8(addr::TI_HARD_OK) != 0;
    for (int i = 0; i < 40; i++) t.sprite_level[i] = m_.u8(addr::TI_SPRITES + i * addr::TI_SPRITE_SIZE + 4);
    read_message(s);
    if (s.in_game) {
        read_enemies(s);
        for (int i = 0; i < addr::OBJ_COUNT; i++) {
            uint32_t o = addr::OBJ_WORK + i * addr::OBJ_SIZE;
            SceneObject ob;
            ob.index = i;
            ob.active = (m_.u32(o) & 1) != 0;
            ob.x = m_.s32(o + addr::W_POS_X); ob.y = m_.s32(o + addr::W_POS_Y); ob.z = m_.s32(o + addr::W_POS_Z);
            ob.dir = m_.s16(o + addr::W_DIR) & 0xFFF;
            ob.floor = m_.u8(o + addr::W_FLOOR);
            ob.aot_hit = m_.u8(o + addr::W_AOT_HIT);
            ob.flags = m_.u32(o);
            {
                int16_t ox = m_.s16(o + 0x94), oz = m_.s16(o + 0x96), oy = m_.s16(o + 0x98);
                int w = m_.u16(o + 0x9A), d = m_.u16(o + 0x9C), hgt = m_.u16(o + 0x9E);
                bool swap = (m_.s16(o + addr::W_DIR) & 0x400) != 0;
                ob.cx = ob.x + (swap ? oz : ox);
                ob.cz = ob.z + (swap ? ox : oz);
                ob.hx = swap ? d : w;
                ob.hz = swap ? w : d;
                double cy = ob.y + oy;
                ob.ytop = cy - hgt; ob.ybottom = cy + hgt;
                ob.solid = ob.active && w > 0 && d > 0 && hgt > 0 && w < 8000 && d < 8000 && !(ob.flags & 2);
            }
            s.objects.push_back(ob);
        }
        read_aots(s);
    }
    return s;
}

void StateReader::read_message(GameState& s) {
    Message& msg = s.message;
    uint8_t flg = m_.u8(addr::MESS_FLG);
    msg.active = (flg & 0x80) != 0;
    msg.cursor = flg & 0x0F;
    msg.routine = m_.u8(addr::MESS_RNO);
    msg.ptr = m_.u32(addr::MESS_PTR);
}

void StateReader::read_enemies(GameState& s) {
    for (int i = 2; i < addr::P_ENEMY_COUNT; i++) {
        uint32_t p = m_.u32(addr::P_ENEMY + i * 4);
        if (!p) continue;
        uint32_t be = 0;
        if (!m_.read(p + addr::W_BE_FLG, &be, 4)) continue;
        if (!be) continue;                       // slot not in use
        Enemy e;
        e.slot = i;
        e.be_flg = be;
        e.ptr = p;
        if (m_.u8(p + addr::W_ID) == 0x23) e.cylinder = m_.u8(p + addr::ALLIGATOR_CYLINDER);
        e.pose = m_.u16(p + addr::W_TYPE);
        e.type = m_.u8(p + addr::W_ID);
        e.x = m_.s32(p + addr::W_POS_X); e.y = m_.s32(p + addr::W_POS_Y); e.z = m_.s32(p + addr::W_POS_Z);
        e.dir = m_.s16(p + addr::W_DIR) & 0xFFF;
        e.floor = m_.u8(p + addr::W_FLOOR);
        e.hp = m_.s16(p + addr::W_LIFE);
        e.max_hp = m_.s16(p + addr::W_MAX_LIFE);
        e.alive = e.hp > 0 && e.hp < 30000;     // SRT: dead when life is -1 (0xFFFF)
        if (e.type < 0x10 || e.type >= 0x50) continue;   // only enemy models (0x10..0x4F)
        s.enemies.push_back(e);
    }
}

void StateReader::read_aots(GameState& s) {
    for (int id = 0; id < 32; id++) {
        uint32_t p = m_.u32(addr::SCE_AOT + id * 4);
        if (!p) continue;
        uint8_t h[48];
        if (!m_.read(p, h, sizeof h)) continue;
        Aot a;
        a.id = id;
        a.sce = h[0]; a.sat = h[1]; a.floor = h[2];
        auto s16 = [&](int o) { int16_t v; std::memcpy(&v, h + o, 2); return (double)v; };
        auto u16 = [&](int o) { uint16_t v; std::memcpy(&v, h + o, 2); return v; };
        int data_off;
        if (a.sat & SAT_4P) {
            for (int k = 0; k < 4; k++) a.quad.push_back({s16(4 + k * 4), s16(6 + k * 4)});
            data_off = 20;
        } else {
            a.quad = rect_quad(s16(4), s16(6), u16(8), u16(10));
            data_off = 12;
        }
        // sup bit 0x80: the area moves with a room object (0x40: an enemy) and is stored relative
        // to it (bio2.exe 0x4E9227; the library's shelf switches ride on the shelves)
        if (h[3] & 0x80) {
            int idx = h[3] & 0x3F;
            double ox = 0, oz = 0;
            bool ok = false;
            if (h[3] & 0x40) {
                uint32_t p = m_.u32(addr::P_ENEMY + idx * 4);
                if (p) { ox = m_.s32(p + addr::W_POS_X); oz = m_.s32(p + addr::W_POS_Z); ok = true; }
            } else if (idx < (int)s.objects.size() && s.objects[idx].active) {
                ox = s.objects[idx].x; oz = s.objects[idx].z; ok = true;
            }
            if (ok) for (auto& q : a.quad) { q.x += ox; q.z += oz; }
        }
        a.centre = quad_centre(a.quad);
        for (int k = 0; k < 6; k++) a.data[k] = h[data_off + k];
        if (a.sce == SCE_DOOR) {
            // IN_DOOR_WORK: next x,y,z,dir (8 bytes) then stage, room, cut, nfloor, tex, type, knock, key id, key type
            const uint8_t* dw = h + data_off;
            a.door_stage = dw[8]; a.door_room = dw[9];
            a.lock_id = dw[15]; a.lock_type = dw[16]; a.door_anim = dw[13]; a.door_tex = dw[12];
            bool unlocked = flag(31, a.lock_id & 0x7F);   // FG_KEY; the id byte carries bit 0x80 (save files: 0x85 -> key flag 5)
            // 0xFF: locked until a script or the other side unlocks it; both set the key flag
            a.door_locked = a.lock_type != 0 && a.lock_type != 0xFE && !unlocked;
        } else if (a.sce == SCE_ITEM || (a.sce == SCE_EVENT && h[data_off] != 0xFF && h[data_off + 2] != 24 && u16(data_off) > 0 && u16(data_off) < 0x80)) {
            // (an event set up by item_aot_set: something found by searching, 401's locker; not
            // an event whose data is a procedure to run, "6, 0, 24, 13": 703's train lever)
            a.item = u16(data_off);
            a.amount = u16(data_off + 2);
            a.item_flag = u16(data_off + 4);
            // FG_ITEM; from stage 5 on the game keeps them in group 34 (bio2.exe 0x4E531D: the
            // lab's items reuse the police station's numbers)
            a.item_taken = flag(s.stage >= 4 ? 34 : 8, a.item_flag);
        }
        s.aots.push_back(a);
    }
}

}  // namespace re2

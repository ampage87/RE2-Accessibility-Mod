// A snapshot of everything the mod reads from the game each frame.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "platform.h"
#include "room.h"

namespace re2 {

enum AotKind { SCE_AUTO = 0, SCE_DOOR = 1, SCE_ITEM = 2, SCE_NORMAL = 3, SCE_MESSAGE = 4, SCE_EVENT = 5,
               SCE_FLAG_CHG = 6, SCE_WATER = 7, SCE_MOVE = 8, SCE_SAVE = 9, SCE_ITEMBOX = 10,
               SCE_DAMAGE = 11, SCE_STATUS = 12, SCE_HIKIDASHI = 13, SCE_WINDOWS = 14 };

constexpr uint8_t SAT_PL = 1, SAT_MANUAL = 0x10, SAT_FRONT = 0x20, SAT_4P = 0x80;

struct Aot {
    int id = 0;
    int sce = 0, sat = 0, floor = 0;
    Quad quad;
    Pt centre;
    // door
    int door_stage = -1, door_room = -1, lock_id = 0, lock_type = 0, door_anim = 0, door_tex = 0;   // anim 4 stairs up, 5 stairs down
    bool door_locked = false;
    // item
    int item = 0, amount = 0, item_flag = -1;
    bool item_taken = false;
    // generic data bytes after the area (message number etc.)
    uint8_t data[6] = {0, 0, 0, 0, 0, 0};
    bool manual() const { return (sat & SAT_MANUAL) != 0; }
    bool player_triggers() const { return (sat & SAT_PL) != 0; }
};

struct Enemy {
    int slot = 0;
    int type = 0;
    double x = 0, y = 0, z = 0;
    int dir = 0, floor = 0;
    int hp = 0, max_hp = 0;
    bool alive = true;
    int pose = 0;               // W_TYPE: pose in the low byte, AI flags in the high byte
    uint32_t be_flg = 0;
    uint32_t ptr = 0;           // ENEMY_WORK address
    int cylinder = 0;           // alligator: gas cylinder state (+0x226)
    // a sleeping enemy (scripts wake it later, e.g. when an item is taken) or a hidden one
    bool dormant() const { return (pose & 0x8000) != 0 || (be_flg & 0x8) != 0; }
    // 0x40.. are people (Kendo, Ada...). 0x3F (608's main shaft, the lab) has no move routine
    // in bio2.exe's table (0x98A2D4): scenery, not an enemy
    bool hostile() const { return type >= 0x10 && type < 0x40 && type != 0x3F; }
    std::string name() const;
};

struct InvSlot { int id = 0, count = 0, part = 0; };   // part: 1/2 = halves of a two-slot weapon

struct SceneObject {          // a room object (statues, crates...): OBJECT_WORK
    int index = 0;
    bool active = false;
    double x = 0, y = 0, z = 0;
    int dir = 0, floor = 0, aot_hit = 255;
    uint32_t flags = 0;         // +0: bit 0 in use; obj_model_set's be_flg (0x80 pushable, 0x100 climbable)
    // its collision box (obj_model_set 0x4E56B0 -> +0x94/+0x96/+0x98 offset, +0x9A/+0x9C/+0x9E size;
    // 0x4CD610: a direction with bit 0x400 swaps x and z): centre and half sizes, height range
    bool solid = false;
    double cx = 0, cz = 0, hx = 0, hz = 0, ytop = 0, ybottom = 0;
};

struct Message {
    bool active = false;
    std::string text;
    bool yesno = false;
    int cursor = 0;       // yes/no cursor: 0 yes, 1 no
    int routine = 0;
    uint32_t ptr = 0;
};

struct StatusScreen {     // in-game status (inventory) screen
    int main_rtn = 0, sub_rtn0 = 0;
    int cursor = 0, cursor_sub = 0, menu0 = 0, menu1 = 0, type = 0;
    int file_book = 0, file_sel = 0, file_page = 0, map_no = 0, ibox_sel = 0;
    int item_state = 0, item_cmd = 0, item_step = 0;
    bool weapon_menu = false;
};

struct TitleScreen {
    int main_rtn = 0, sub_rtn0 = 0, sel_mode = 0, cursor = 0;
    int screen = 0, step = 0, option = 0;
    bool hard_ok = false;
    uint8_t sprite_level[40] = {};   // brightness of each menu word (highlighted words pulse)
};

struct GameState {
    bool valid = false;           // game memory readable
    bool in_game = false;         // a game is running (not title / menus before play)
    uint32_t system_flg = 0, status_flg = 0, stop_flg = 0, key = 0;
    int stage = 0, room = 0, cut = 0, character = 0;   // stage 0-based
    std::string room_code;
    double x = 0, y = 0, z = 0;
    int dir = 0, floor = 0, hp = 0, max_hp = 0;
    int game_floor = 0;        // the game's own floor byte (+0x106); floor is from the height
    double body_r = PLAYER_RADIUS;   // collision half-width (player +0x90)
    int routine0 = 0;
    bool poisoned = false;
    std::vector<SceneObject> objects;
    std::vector<Enemy> enemies;
    std::vector<Aot> aots;
    std::vector<InvSlot> inventory;
    InvSlot top_slot;             // Item_work[10], the box above the item list   // usable slots only
    int max_items = 8, equipped = 0;
    uint32_t file_flags = 0;
    Message message;
    StatusScreen status;
    TitleScreen title;
    int save_cursor = 0;
    uint32_t front_func = 0;
};

class StateReader {
public:
    explicit StateReader(MemoryReader& m) : m_(m) {}
    GameState read();
    bool flag(int group, int index);      // script flag (bit order MSB first)
private:
    MemoryReader& m_;
    void read_aots(GameState& s);
    void read_enemies(GameState& s);
    void read_message(GameState& s);
};

std::string item_name(int id);
int condition_level(int hp, int max_hp, bool poisoned);
std::string condition_word(int level);
std::string condition_colour(int level);

}  // namespace re2

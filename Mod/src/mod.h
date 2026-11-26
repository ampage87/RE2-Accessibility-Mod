// The accessibility mod: everything that decides what to say and when, and what to do for
// auto-walk and targeting. Host-independent: it runs inside the game (win_main.cpp) and in
// the simulator harness (tests/harness.cpp) unchanged.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "platform.h"
#include "room.h"
#include "state.h"

#define RE2ACCESS_VERSION "0.9.25"

namespace re2 {

struct MapDoor { std::string to; int lock_id = 0, lock_type = 0; bool via_event = false; };   // via_event: reached through a lift or other way inside the room (21B -> 308)
struct MapItem { int item = 0, count = 0, flag = 0; };
struct MapRoom {
    std::string code, name, area;
    int stage = 0, room = 0;
    std::vector<MapDoor> doors;
    std::vector<MapItem> items;
    std::set<std::string> other_story;   // doors only in the other character's or scenario's branch
    bool typewriter = false, item_box = false;
};

struct Entry {
    enum Type { AOT, ENEMY, PERSON, STAIRS } type = AOT;
    int id = 0;               // aot id or enemy slot
    int kind = 0;             // aot sce
    std::string name;
    Pt pos;
    Pt start_end;                 // stairs: the end on the player's side
    Quad quad;
    bool locked = false;
    int floor = -1;           // AOT floor (-1 = the player's)
    bool walk_on = false;     // a spot that does something when walked onto (the library's weak floor)
};

class Mod {
public:
    explicit Mod(Host& host);
    void frame();                                  // call ~30-60 times a second
    void hotkey(Hotkey k);
    const GameState& state() const { return s_; }
    bool busy() const { return walking_ || face_dir_ >= 0; }   // auto-walking or turning
    static const char* hotkey_help();
    static std::string examine_noun(const std::string& msg);   // name of an examine spot

private:
    Host& h_;
    mutable StateReader reader_;
    GameState s_, p_;                              // this frame / previous frame
    bool have_prev_ = false;
    RoomFile room_;
    std::string room_key_, announced_room_;
    std::map<std::string, std::string> room_names_;
    std::vector<std::vector<std::string>> files_;  // file index -> pages (page 0 = title)
    std::map<std::string, std::vector<std::string>> menu_labels_;
    std::string last_said_;

    // catalog
    int category_ = 0;
    Entry::Type sel_type_ = Entry::AOT;
    int sel_id_ = -1;
    int target_slot_ = -1;            // enemy chosen with T / Shift+T
    // auto-walk / turning
    std::vector<Pt> walk_;
    bool walking_ = false, partial_ = false, facing_only_ = false;
    Entry walk_target_;
    int face_dir_ = -1;
    std::string face_label_;
    int stuck_ = 0, walk_floor_ = 0;
    double last_x_ = 0, last_z_ = 0;
    uint64_t walk_started_ms_ = 0;
    std::map<int, NavGrid*> grids_;
    // announcements state
    std::set<int> close_enemies_;
    std::map<int, int> enemy_hp_;
    std::set<int> in_front_;
    std::string prev_climb_;
    int bump_frames_ = 0;
    bool bumped_ = false, on_stairs_prev_ = false;
    double stairs_y_ = 0;
    std::set<std::string> visited_rooms_;
    std::map<std::string, std::set<std::pair<int, int>>> visited_cells_;
    int prev_ammo_ = -1;
    bool room_pending_ = false, room_quiet_ = false;   // room summary waits for the room to set up
    uint64_t room_settled_ms_ = 0, room_changed_ms_ = 0, aot_sig_ms_ = 0, last_catalog_check_ms_ = 0;
    std::string aot_sig_;
    std::map<int, std::string> known_;  // catalog entries seen in this room (key -> name)
    bool room_brief_ = false;
    bool said_dead_ = false;
    // 40A alligator fight
    void frame_alligator();
    bool gator_hint_ = false, gator_released_ = false, gator_mouth_ = false;
    std::string gator_sig_;
    uint64_t gator_dist_ms_ = 0;
    bool was_aiming_ = false;
    int aim_said_slot_ = -2;
    uint64_t aim_said_ms_ = 0, blocked_said_ms_ = 0;
    std::map<int, uint64_t> enemy_alert_ms_;
    std::map<int, bool> enemy_dormant_;
    std::map<int, int> catalog_order_;     // entry key -> place in this room's catalog
    int next_order_ = 0;
    static int entry_key(const Entry& e) { return (int)e.type * 1000 + e.id; }
    uint32_t msg_start_ = 0;
    std::string msg_text_;
    int ui_mode_ = 0, prev_ui_mode_ = -1;
    std::string ui_sig_;
    int title_screen_ = -1;
    std::string title_label_;
    bool title_start_said_ = false;
    int ex_title_sub_ = -1, ex_title_cur_ = -1;
    void frame_ex_title(const TitleScreen& t);
    void frame_ex_menu(const TitleScreen& t);
    void frame_ex_records(const TitleScreen& t);
    void frame_ex_char();
    void frame_ex_stage();
    void frame_ex_text();
    std::string exe_text(uint32_t a);
    int ex_intro_prev_ = -2, ex_end_prev_ = -2;
    uint64_t hold_until_ms_ = 0;
    void say_long(const std::string& t);
    std::string condition_text();
    std::string player_name();
    std::string ex_time_text(uint32_t secs);
    std::string ex_menu_sig_;
    int ex_rec_said_ = 0, ex_char_sel_ = -1;
    uint32_t ex_times_[3] = {};
    bool ex_times_ok_ = false;
    // assist options
    bool unlimited_health_ = false, unlimited_ammo_ = false;
    std::map<int, int> full_ammo_;      // weapon id -> highest loaded count seen
    std::string title_label(const TitleScreen& t, std::string& heading);
    void apply_assists();
    std::vector<std::pair<Pt, double>> danger_circles(const Entry& target) const;
    // damage areas (fire, steam...): scripts set them as SCE_DAMAGE AOTs
    std::vector<const Aot*> hazards() const;
    std::vector<std::pair<Pt, double>> hazard_circles() const;
    double hazard_distance(const Aot& a, double x, double z) const;
    bool path_hazard(const std::vector<Pt>& path) const;
    std::set<int> near_hazards_;
    int hazard_count_ = -1;
    int hazard_ok_key_ = -1;          // a walk through a danger area the player asked for twice
    uint64_t hazard_ok_ms_ = 0;
    static constexpr double kEnemyKeepAway = 1800;   // distance auto-walk keeps from enemies
    bool walk_unsafe_ = false, run_enabled_ = true;
    uint64_t last_replan_ms_ = 0;
    std::string last_how_;
    void frame_status(bool opened);
    void frame_file_reader(bool opened);
    void frame_item_box(bool opened);
    std::string box_text(int index) const;
    int status_type_ = 0, reading_file_ = -1, reading_page_ = -1;
    bool frame_pc_menu();
    uint64_t pc_typing_ms_ = 0;
    bool frame_options();
    int opt_group_ = -1;
    std::string opt_said_;
    bool frame_terminal();
    void frame_keyboard(uint32_t work);
    bool term_active_ = false;
    std::vector<std::string> term_lines_, term_said_;
    std::vector<uint64_t> term_changed_ms_;
    uint32_t term_msg_ = 0;
    int kb_col_ = -1, kb_row_ = -1, kb_blink_ = -1;
    uint64_t kb_seen_ms_ = 0;
    bool kb_active_ = false, kb_any_key_ = false;
    std::map<std::string, uint64_t> kind_alert_ms_;
    int msg_choice_type_ = 0;
    int event_door(int proc) const;
    bool event_lift(int proc) const;
    bool push_spot(const Aot& a) const;
    bool scene_spot(const Aot& a) const;
    std::map<std::string, uint64_t> travel_blocked_;
    std::string travel_next_;
    bool walk_unreachable_ = false;
    std::map<std::string, std::set<int>> explored_zones_;
    std::string choice_label(int cursor) const;
    std::string choices_prompt() const;
    std::string kb_line_;
    bool pc_active_ = false;
    std::string pc_sig_, pc_dialog_sig_, pc_hl_, pc_title_, pc_cur_;
    int pc_row_ = -1, pc_top_ = -1;
    std::string slot_text(int slot) const;
    void toggle_assist(bool health);
    // puzzle help (statue puzzle, room 10C)
    void frame_puzzle();
    bool puzzle_patch_message(uint32_t start);
    void puzzle_restore();
    bool statues_solved();
    bool event_running() const;
    uint64_t puzzle_idle_ms_ = 0;
    std::vector<std::string> msg_page_texts_;   // the current message, page by page
    int msg_page_ = 0;
    bool msg_yesno_ = false;
    int ladder_state_ = 0, tank_state_ = 0;
    // the map (map.cpp)
    std::map<std::string, MapRoom> map_rooms_;
    std::vector<std::string> map_order_, map_areas_;
    bool map_built_ = false, map_mode_ = false;
    int map_character_ = -1;
    std::string map_sel_, map_area_, travel_to_;
    void map_build();
    void map_scan_items(const RoomFile& rf, MapRoom& m);
    std::string map_area_of(const std::string& code, const std::string& name) const;
    bool map_visited(const MapRoom& m) const;
    bool map_have(int item) const;
    std::string map_door_state(const MapRoom& m, const std::string& to, bool& passable) const;
    std::vector<std::string> map_neighbours(const MapRoom& m) const;
    std::vector<std::string> map_live_neighbours() const;
    std::vector<std::string> map_route(const std::string& from, const std::string& to) const;
    std::string map_describe(const MapRoom& m, bool full) const;
    std::vector<std::string> map_area_rooms(const std::string& area) const;
    void key_map_here();
    void key_map_mode();
    int inroom_way() const;   // AOT id of a way to another part of this room (a lift), or -1
    void open_map(const std::string& lead);
    void frame_via();
    int via_state_ = 0, via_ladder_id_ = -1, via_floor_ = 0, via_tries_ = 0, via_pages_ = 0;
    uint64_t via_page_ms_ = 0;
    bool via_confirmed_ = false;
    uint64_t via_ms_ = 0, walk_dbg_ms_ = 0;
    Pt prog_t_{1e9, 1e9};
    double prog_best_ = 0;
    int prog_frames_ = 0;
    Entry via_target_;
    std::string via_room_;
    int last_model_ = -1;
    std::string pl_diag_sig_;
    bool kill_patched_ = false;
    uint64_t swarm_alert_ms_ = 0;
    int ledge_tries_ = 0, travel_pages_ = 0, page_frames_ = 0;
    uint64_t travel_page_ms_ = 0;
    bool game_map_open_ = false, travel_after_map_ = false;
    int cancel_frames_ = 0;
    bool map_hotkey(Hotkey k);
    void travel_step(bool walk);
    std::vector<std::string> travel_seen_;
    double via_x_ = 0, via_z_ = 0;
    std::string sca_room_;
    int nudge_frames_ = 0, nudge_aot_ = -1;
    std::set<int> sca_ids_;   // rooms passed on this journey (a loop stops it)
    void frame_travel();
    int travel_state_ = 0, travel_door_id_ = -1;
    std::string travel_room_;
    bool travel_confirmed_ = false;
    int confirm_frames_ = 0;
    uint64_t walk_wait_ms_ = 0;
    std::vector<Pt> stair_ends_;
    bool stair_ends_done_ = false;
    std::map<int, Pt> enemy_first_pos_;
    bool lying_still(const Enemy& e) const;
    uint64_t stair_press_ms_ = 0;
    bool stair_action_pending_ = false;
    int stair_tries_ = 0;
    bool walk_wait_logged_ = false;
    uint64_t travel_ms_ = 0;
    void frame_tank();
    bool ladder_stepped_ = false;
    uint64_t ladder_ms_ = 0;
    void frame_ladder();
    bool crank_on_shelf() const;
    bool patch_question(uint32_t start, int idx, const char* question, const char* what);
    int puzzle_state_ = 0, puzzle_cursor_ = 0, puzzle_obj_ = 0, press_action_frames_ = 0;
    uint32_t puzzle_patch_addr_ = 0;
    std::vector<uint8_t> puzzle_backup_;
    uint64_t puzzle_ms_ = 0;
    bool puzzle_saw_stop_ = false;

    void say(const std::string& t, bool interrupt = false);
    void load_data();
    void ensure_room();
    void sync_collisions();
    int prev_equipped_ = -1;
    int power_meter_ = -1;
    int last_pl_id_ = -1;
    int partner_ = -1, partner_room_ = -1;
    // auto-walk with a partner (Sherry, Ada): keep them in range
    bool partner_pos(double& x, double& z) const;
    bool partner_wait_ = false, partner_slow_ = false;
    uint64_t partner_wait_ms_ = 0, partner_ignore_until_ = 0, partner_said_ms_ = 0;
    std::map<std::string, uint64_t> front_said_ms_;
    std::string last_climb_said_;
    uint64_t climb_said_ms_ = 0;
    std::map<std::string, std::map<int, int>> clash_numbers_;
    std::map<int, std::string> aot_names_;      // last name each AOT had in this room visit   // same-named things: name -> AOT -> number            // room 213 power panel: last meter value read (-1 = not in use)
    std::set<int> game_inv_;
    std::map<int, int> game_counts_;
    bool game_inv_valid_ = false;
    std::string room_name(const std::string& code) const;
    std::string room_name(int stage0, int room) const { return room_name(room_code(stage0, room)); }

    // catalog
    std::vector<Entry> entries(int category);
    bool hidden(const Aot& a) const;
    std::string describe(const Aot& a, bool shortform) const;
    struct Outcome { int message = -1, aot = -1, item = 0; };
    Outcome event_outcome(int proc, int depth = 0) const;
    int aot_message(const Aot& a) const;
    std::string level_note(const Enemy& e) const;
    bool selected(Entry& out);
    void cycle(int step);
    void announce_room(bool brief);
    std::string locate(const Pt& p) const;

    // movement
    NavGrid& grid(int floor, int tight = 0);
    double grid_body_ = 0;
    // stairs and slopes, plus climb-up boxes and ledges to drop from (one way: oneway 1 = only
    // low to high, -1 = only high to low; the crossing is done with the action button)
    struct Stairway { int index = 0, low = 0, high = 0; bool stairs = true; int oneway = 0; Pt low_end, high_end; };
    std::vector<Stairway> stairways() const;
    std::map<size_t, std::vector<Pt>> multi_first_;
    bool route(const Entry& e, std::vector<Pt>& path, std::string& how, Pt& goal);
    int goal_floor(const Entry& e) const;
    void clear_grids();
    const std::vector<Collision>& nav_collisions();
    bool proc_drops_player(int proc) const;
    bool walk_on_spot(const Aot& a) const;
    std::vector<Collision> obj_cols_, nav_cols_;
    std::string obj_sig_;
    bool obj_log_pending_ = false;
    uint64_t obj_moved_ms_ = 0;
    uint64_t sca_changed_ms_ = 0;
    std::string use_here_said_;
    std::map<int, uint64_t> overhead_ms_;
    bool lift_busy_ = false;           // an event or stop: the level when it began
    double lift_y_ = 0;
    uint64_t level_said_ms_ = 0;
    uint64_t boss_warn_ms_ = 0;
    uint64_t sys4000_ms_ = 0;          // since when System_flg 0x4000 has been set
    bool sys4000_logged_ = false;
    int push_frames_ = 0;               // leaning on the parking lot's vehicle (216)
    Pt push_to_{0, 0};
    void start_push();
    bool crate_up() const;
    std::vector<Collision> climb_cols() const;
    void drop_crate();        // a chasing boss's distance said while walking   // enemies announced right above the player
    size_t travel_len_ = 0;
    bool travel_lift_tried_ = false;   // a lift tried on the way in this room
    int travel_lift_floor_ = -1;
    bool travel_lift_pressed_ = false;
    bool travel_lift_yes_ = false;
    uint64_t travel_lift_ms_ = 0;
    std::string travel_lift_room_;
    std::set<int> auto_door_cols_;     // walls that are automatic doors (615)
    std::string sca_base_;             // the room's switchable walls when last announced
    std::string sca_signature();            // doors on the planned way (a way round may be a little longer)        // the item-use spot last announced      // a script switched the room's walls (new stairs appear)
    bool plan_walk(const Entry& e, bool replanning);
    Pt stand_point(const Entry& e, int floor);
    std::vector<int> stair_floors();
    void start_turn(int dir, const std::string& label);
    void stop_walk(const std::string& why = "");
    uint32_t steer();
    std::vector<const Aot*> actionable() const;
    std::vector<const Aot*> actionable_for(const GameState& st) const;
    std::string climbable() const;
    bool on_stairs() const;
    bool in_play(const Enemy& e) const;
    const Enemy* aim_target() const;

    // per-frame announcements
    void frame_game();
    void frame_message();
    void frame_ui();
    void log_ui_probe();
    std::string ui_label(const std::string& key, int index, const std::string& fallback);

    // hotkey handlers
    void key_where();
    void key_go();
    void key_face();
    void key_target(bool next);
    void key_health();
    void frame_countdown();
    // audio description of the movies (RE2Access\movies\<movie>\cues.txt + WAVs)
    struct MovieCue { int ms = 0; std::string file, text; };
    void frame_movie();
    std::string movie_name(int index, bool claire);
    bool load_movie_cues(const std::string& name);
    bool ad_enabled_ = true;
    int ad_offset_ms_ = 0;
    int movie_index_ = -1;
    std::string movie_name_;
    std::vector<MovieCue> movie_cues_;
    size_t movie_next_ = 0;
    uint64_t movie_t0_ = 0;
    bool movie_on_ = false;
    // the narrated introduction before the opening movies (pictures from open00-open14.adt):
    // each scene's pictures are described (RE2Access\\movies\\intro_<table>\\cues.txt)
    void frame_intro_probe();
    // texts the game shows between scenes (bio2.exe 0x540520, 5 texts): found by scanning
    // .data for a pointer into them while not in a room
    void frame_text_scan();
    void say_transition_text(int k, const char* why);
    uint64_t text_scan_ms_ = 0;
    uint64_t text_said_ms_[5] = {0, 0, 0, 0, 0};
    std::map<uint32_t, uint32_t> text_ptrs_;    // address -> last value seen pointing into the block
    int intro_scene_ = -1;
    uint32_t intro_table_ = 0;
    std::vector<MovieCue> intro_cues_;
    bool intro_audio_ = false;        // told the host the introduction is on (quieter descriptions)
    void set_intro_audio(bool on);
    void frame_results();
    std::string result_line(int i);
    bool res_on_[33] = {};
    bool res_ready_ = false;
    std::string res_last_;
    uint64_t res_ms_ = 0;
    std::string countdown_text(int secs) const;
    bool cd_on_ = false, cd_var_live_ = false, cd_up_ = false;
    int cd_var_ = -1, cd_said_ = -1;
    double cd_secs_ = -1;
    uint64_t cd_ms_ = 0, cd_log_ms_ = 0;
    void key_inventory();
    void key_enemies();
    void key_scan();
    void key_explore();
    void key_get_away();
    bool getaway_ = false;             // walking away from an enemy (Shift+E)
    int getaway_slot_ = -1;
    void key_diag();
};

}  // namespace re2

// Memory map of bio2.exe, Sourcenext 1.10 (the build Classic REbirth runs).
// Verified build: SHA-256 21b2a647...bf5 with the 4 GB flag and checksum cleared (our
// installed copy differs only by the 4 GB patch).
//
// Sources: BioRand's injected DLL (biorandmod), the LiveSplit autosplitter, the SRT
// provider, IntelOrca's survey tool, MeganGrass's PSX struct layouts (the PC G struct
// at 0x9864E0 follows the PSX GLOBAL layout), and our own disassembly of bio2.exe
// (message system, script handlers, status/title work, key table). Confidence notes:
//   [H] two or more sources / confirmed in code   [M] derived from struct layout   [L] guess
#pragma once
#include <cstdint>

namespace addr {

// ---- global work (G) ------------------------------------------------------------------
constexpr uint32_t G_BASE          = 0x9864E0;   // [H]
constexpr uint32_t KEY             = 0x988604;   // [H] pad bits held (u32)
constexpr uint32_t KEY_TRG         = 0x988608;   // [M] pad bits newly pressed
constexpr uint32_t P_ROOM          = 0x98861C;   // [H] pointer to the loaded room (RDT image)
constexpr uint32_t SCE_AOT         = 0x988850;   // [H] SCE_AOT* [32]; each points 2 bytes into an aot_set script instruction
constexpr uint32_t N_SCE_AOT       = 0x98E528;   // [H] u8
constexpr uint32_t SYSTEM_FLG      = 0x989E6C;   // [H] u32 (game started, door, save, game over, movie...)
constexpr uint32_t SAVE_CURSOR     = 0x989E7C;   // [M] u8 save/load slot cursor
constexpr uint32_t STATUS_FLG      = 0x989ED0;   // [H] u32 (0x40 = message window up; written by Mess_set)
constexpr uint32_t STOP_FLG        = 0x989ED4;   // [H] u32, non-zero while a message/event freezes play
constexpr uint32_t ENEMY_NUM       = 0x989EEB;   // [M] u8
constexpr uint32_t PLAYER          = 0x989EF0;   // [H] PLAYER_WORK
constexpr uint32_t P_ENEMY         = 0x98A10C;   // [M] ENEMY_WORK* [34]  (SRT reads from [2])
constexpr int      P_ENEMY_COUNT   = 34;
constexpr uint32_t OBJ_WORK        = 0x98A61C;   // [H] OBJECT_WORK[32], 0x1F8 bytes each (obj_model_set 0x4E56B0;
constexpr uint32_t OBJ_SIZE        = 0x1F8;      //     same header as entities: +0 flags, +0x38 x, +0x40 z, +0x76 dir)
constexpr int      OBJ_COUNT       = 32;
constexpr uint32_t FRONT_FUNC      = 0x98E790;   // [H] current front-end routine (item box etc.)
constexpr uint32_t GAME_TIME       = 0x680588;   // [H] u32 in-game time (frames? seconds)

// entity work offsets (player and enemies share the header layout)
constexpr uint32_t W_BE_FLG   = 0x000;   // u32
constexpr uint32_t W_ROUTINE0 = 0x004;   // u8 x4
constexpr uint32_t W_ID       = 0x008;   // u8 model/enemy type
constexpr uint32_t W_POS_X    = 0x038;   // s32
constexpr uint32_t W_POS_Y    = 0x03C;   // s32 (negative is up)
constexpr uint32_t W_POS_Z    = 0x040;   // s32
constexpr uint32_t W_DIR      = 0x076;   // s16 0..4095
constexpr uint32_t W_ATARI_X  = 0x090;   // u16 collision half-width in x (player 0x1C2)
constexpr uint32_t W_ATARI_Z  = 0x092;   // u16 collision half-width in z
constexpr uint32_t W_AOT_HIT  = 0x00B;   // u8 AOT the object is standing in this frame (script member 9), 0xFF none
constexpr uint32_t W_FLOOR    = 0x106;   // u8
constexpr uint32_t W_TYPE     = 0x10E;   // u16 pose | AI flags << 8; 0x8000 = not activated yet (sleeping)
constexpr uint32_t W_LIFE     = 0x156;   // s16
constexpr uint32_t W_MAX_LIFE = 0x162;   // s16

// ---- save data -------------------------------------------------------------------------
constexpr uint32_t SAVE_DATA       = 0x98E79C;   // [H]
constexpr uint32_t MAX_ITEM_NUM    = 0x98E9A4;   // [H] u8 usable inventory slots (8 or 10)
constexpr uint32_t PL_ID           = 0x98E9A6;   // [H] u8 0 Leon, 1 Claire
constexpr uint32_t EQUIP_ID        = 0x98E9B4;   // [M] u8
constexpr uint32_t EQUIP_NO        = 0x98E9B5;   // [M] u8
constexpr uint32_t SAVE_CNT        = 0x98E9BC;   // [H] u16
constexpr uint32_t STAGE           = 0x98EB14;   // [H] s16 (0-based: file ROOM1xx = 0)
constexpr uint32_t ROOM            = 0x98EB16;   // [H] s16
constexpr uint32_t CUT             = 0x98EB18;   // [H] s16 camera
constexpr uint32_t SCE_VARS        = 0x98EAE4;   // [H] s16[] script variables (cmp/save/copy); 0 = AOT used, 2 = item used, 26 = camera
constexpr uint32_t FILE_FLG        = 0x98ED20;   // [M] u32 files collected
constexpr uint32_t ITEM_WORK       = 0x98ED34;   // [H] {id,num,size,pad}[11]
constexpr int      ITEM_SLOTS      = 11;
constexpr uint32_t FLAG_GROUPS     = 0x53B218;   // [H] u32* table indexed by script flag group (in the exe image)

// ---- message window (MOJIDISP at 0x9814C0; found in Mess_set 0x4C89B2) -------------------
constexpr uint32_t MOJI            = 0x9814C0;
constexpr uint32_t MESS_FLG        = MOJI + 0x3B8C;   // [H] u8: 0x80 = window active; low nibble = yes/no cursor (0 yes, 1 no)
constexpr uint32_t MESS_RNO        = MOJI + 0x3B8D;   // [H] u8 display routine (1 typing, 2 page wait, 4 yes/no ...)
constexpr uint32_t MESS_ITEM       = MOJI + 0x3B8F;   // [H] u8 item id inserted into the text
constexpr uint32_t MESS_PTR        = MOJI + 0x3B94;   // [H] u32 pointer to the current text (advances while typing)
constexpr uint32_t MESS_ATTR       = MOJI + 0x3BA8;   // [H] u16
constexpr uint32_t FN_MESS_SET     = 0x4C89B2;        // [H] LONG Mess_set(Pos_xy, Attr, Mess_no, Stop_data), cdecl

// ---- status screen work (STATUS, PSX layout, at 0x691F60) -------------------------------
constexpr uint32_t STATUS          = 0x691F60;   // [M]
constexpr uint32_t ST_MAIN_RTN     = STATUS + 0x00;
constexpr uint32_t ST_SUB_RTN0     = STATUS + 0x01;   // [H] screen: 2 top menu, 3 items, 4 map, 5 files, 0 closing
// Found by disassembling the item screen (bio2.exe 0x4F8050 top menu, 0x4F8160 items):
//   top menu cursor ST_MCURSOL0: 0 File, 1 Map, 2 Item, 3 Exit
//   ST_ITEM_STATE 0 = choosing a slot (ST_ICURSOL_MAIN: 0..slots-1 in two columns, 10 = the
//   box above the list), 1 = command menu (ST_MCURSOL1: 3 Equip/Use, 2 Check, 1 Combine,
//   0 the extra entry some weapons have), 2 = carrying out the command ST_ITEM_CMD
//   (combine: ST_ICURSOL_SUB is the second item while ST_ITEM_STEP is 0)
constexpr uint32_t ST_ITEM_STATE   = STATUS + 0x02;   // [H]
constexpr uint32_t ST_ITEM_CMD     = STATUS + 0x03;   // [H]
constexpr uint32_t ST_ITEM_STEP    = STATUS + 0x04;   // [H]
constexpr uint32_t ST_WEAPON_MENU  = 0x689DF4;        // [H] 1 when the command menu has the extra weapon entry
// ---- Sourcenext PC save / load screens (bio2.exe 0x4C5A40 input, 0x4315D0 list) ----------
constexpr uint32_t SV_TOP_ROW      = 0x986390;   // [H] first list row shown (scroll)
constexpr uint32_t SV_ROWS         = 0x986394;   // [H] rows on screen
constexpr uint32_t SV_ROW_SEL      = 0x9864B4;   // [H] highlighted row on screen
constexpr uint32_t SV_DIALOG_SEL   = 0x98638C;   // [H] choice in the confirmation boxes
// Text the PC menus draw (bio2.exe 0x431470 queues strings; Classic REbirth draws them).
constexpr uint32_t TEXT_DRAW_FUNC  = 0x431470;   // [H] cdecl (x, y, const char* text, colour, 0); colour 3 = highlighted
constexpr uint32_t TEXT_QUEUE_COUNT = 0x669AF8;  // [H] strings queued this frame
// ---- computer terminals (sce 14 "windows": bio2.exe 0x4D1280 main, 0x4D27A0 typing) --------
constexpr uint32_t PC_WORK_PTR     = 0x9800A0;   // [H] -> terminal work: +0 column, +1 row, +4 text grid
constexpr uint32_t PC_GRID         = 4;          //      12 rows x 30 columns of ASCII
constexpr int      PC_ROWS = 12, PC_COLS = 30;
constexpr uint32_t PC_STATE        = 0x98E52D;   // [H] terminal state, 0 when no terminal is up
constexpr uint32_t PC_MESSAGE_PTR  = 0x689B48;   // [H] room message shown under the screen
constexpr uint32_t ST_EQUIP_ID     = STATUS + 0x0A;   // [H] SRT reads equipped item id here
constexpr uint32_t ST_ICURSOL_MAIN = STATUS + 0x0C;   // [M] inventory cursor (indexes Item_work)
constexpr uint32_t ST_ICURSOL_SUB  = STATUS + 0x0D;
constexpr uint32_t ST_MCURSOL0     = STATUS + 0x0E;   // [M] sub-menu cursor
constexpr uint32_t ST_MCURSOL1     = STATUS + 0x0F;
constexpr uint32_t ST_TYPE         = STATUS + 0x10;
constexpr uint32_t ST_F_SEL_BOOK   = STATUS + 0x11;
constexpr uint32_t ST_F_SEL_FILE   = STATUS + 0x12;   // [M] file cursor
constexpr uint32_t ST_F_PAGE_NO    = STATUS + 0x13;   // [M] page of the open file
constexpr uint32_t ST_MAP_NO       = STATUS + 0x1A;
constexpr uint32_t ST_IBOX_SEL     = STATUS + 0x24;   // [M] item box cursor

// ---- title work (TITLE_WORK, PSX layout, at 0x691B80) -----------------------------------
constexpr uint32_t TITLE           = 0x691B80;   // [M]
constexpr uint32_t TI_MAIN_RTN     = TITLE + 0;
constexpr uint32_t TI_SUB_RTN0     = TITLE + 1;
constexpr uint32_t TI_SEL_MODE     = TITLE + 6;   // [H] set on leaving (screen 4): 0 load, 1 new game, 2 options
constexpr uint32_t TI_CUR          = TITLE + 7;   // [H] menu cursor
// Found by disassembling the title code (bio2.exe 0x503D7D dispatches on TI_SCREEN):
//   0 main menu (0x504060): Load game, Original mode, Arrange mode, Special, Option
//   1/2 difficulty for Original/Arrange (0x5044D0): cursor 0 = Normal/Easy (TI_OPTION 0/1),
//       1 = Exit, 2 = Hard (only when TI_HARD_OK)
//   3 special menu (0x504900), 4 leaving the title (to load / options / game),
//   5/6 character for Original/Arrange (0x504240): cursor 0 = Leon/Claire (Status_flg bit 31),
//       1 = Exit; 7 another character choice (0x505020)
constexpr uint32_t TI_SCREEN       = TITLE + 2;   // [H]
constexpr uint32_t TI_STEP         = TITLE + 3;   // [H] 0 setup; main menu: 1 input; screens 1/2/3/5/6/7:
                                                  // 1 fade-in, 2 input; 3 leaving
constexpr uint32_t TI_OPTION       = TITLE + 0x0E;   // [H] Normal (0) / Easy (1)
constexpr uint32_t TI_HARD_OK      = 0x6805B0;    // [H] Hard difficulty unlocked
constexpr uint32_t TI_SPRITES      = 0x691BB8;    // [H] 40 menu-word sprites, 0x14 bytes, colour at +4
constexpr uint32_t TI_SPRITE_SIZE  = 0x14;

// ---- results screens after an ending (bio2.exe ending task 0x4B57C0, results 0x4B6E10) ----
constexpr uint32_t RES_TABLE       = 0x52D178;   // [H] 33 text entries, 12 bytes, +0 flags (bit 0 shown)
constexpr int      RES_COUNT       = 33;
constexpr int      RES_SIZE        = 12;
constexpr uint32_t RES_RANK        = 0x99CE20;   // [H] u16 rank 0 S, 1 A, 2 B, 3 C, 4 D
constexpr uint32_t RES_HOURS       = 0x99CE22;   // [H] u8 hours, +1 minutes, +2 seconds
constexpr uint32_t RES_SAVES       = 0x99CE25;   // [H] u8 number of saves

// ---- Extreme Battle (System_flg 0x1000000) ------------------------------------------------
// Title routine 5 (0x506F90) loads EXTITLE.adt; routine 6 (0x507100) is its title: sub 0 the
// picture (any key), sub 1 the menu: TI_SCREEN 0 = menu, TI_STEP 1 = main menu (cursor 0 Load
// game, 1 Game start, 2 Result, 3 Exit), TI_STEP 2 = level menu with sub-step TI_SUB4 2 taking
// input (cursor 0 = level row, EX_LEVEL_SEL 0..EX_LEVELS, left/right; cursor 1 = Exit).
// TI_SCREEN 1 = leaving. Routine 7 (0x507AB0) runs the records / result task 0x4DD4F0.
constexpr uint32_t TI_SUB4         = TITLE + 4;     // [H] level-menu sub-step
constexpr uint32_t EX_LEVEL_SEL    = 0x691EF3;      // [H] level chosen (0 = Level 1)
constexpr uint32_t EX_CLEARED      = 0x989E90;      // [H] u8 an Extreme Battle level was just cleared
constexpr uint32_t EX_LEVELS       = 0x989E91;      // [H] u8 extra levels unlocked (0..2)
constexpr uint32_t EX_LEVEL        = 0x989E92;      // [H] u8 level played (1..3)
constexpr uint32_t EX_CHAR         = 0x989E93;      // [H] u8 character played (0 Leon 1 Claire 2 Ada 3 Chris)
constexpr uint32_t EX_RECORDS      = 0x989E94;      // [H] 3 x 16: +0 u16 character, +2 u16 set, +4/+8/+C u32 stage times (s)
constexpr uint32_t EX_TIMES        = 0x989EC4;      // [H] 3 x u32 this run's stage times (s)
constexpr uint32_t EX_RES_STATE    = 0x698822;      // [H] records task state: 2 = showing
constexpr uint32_t EX_RES_MODE     = 0x698823;      // [H] 1 records (from the menu), 2 result of a run
constexpr uint32_t EX_CHAR_STATE   = 0x694260;      // [H] character select task 0x4EA320: 2 = taking input
constexpr uint32_t EX_CHAR_SEL     = 0x6949E4;      // [H] s8 slot: 0 Ada, 1 Leon, 2 Claire, 3 Chris (models 14, 0, 1, 11)
constexpr uint32_t EX_CHAR_LOCK    = 0x6949E7;      // [H] bit 2 Ada locked, bit 4 Chris locked

// Extreme Battle typed texts (drawn a letter at a time with a typing sound, bio2.exe 0x4CA2F9).
// Start of a run (0x4474A0): table EX_INTRO_TAB, 10 lines a page from EX_INTRO_BASE (0, then 10);
// state EX_INTRO_STATE = 1 when a page starts. Mission complete (0x4B7B00): table EX_END_TAB,
// 11 lines a page from EX_END_BASE (0, then 11); state EX_END_STATE. Stage clear (0x4CB2C0):
// "CLEAR TIME", then 0x533186 "Fight to the police precinct!" when EX_STAGE_FLAGS & 0x1F is 0,
// else 0x5331A5 "Obtain the 4 special bombs!", then "PRESS ANY BUTTON".
constexpr uint32_t EX_INTRO_TAB    = 0x525438;   // [H] u32 pointers to texts (0xFE ends)
constexpr uint32_t EX_INTRO_BASE   = 0x99D028;   // [H] u8 first line of the page
constexpr uint32_t EX_INTRO_STATE  = 0x99DA84;   // [H] u32 0 set-up, 1 typing, 2-5 page done / waiting
constexpr uint32_t EX_END_TAB      = 0x52CD80;   // [H]
constexpr uint32_t EX_END_BASE     = 0x99C871;   // [H]
constexpr uint32_t EX_END_STATE    = 0x995B40;   // [H]
constexpr uint32_t EX_STAGE_FLAGS  = 0x98149F;   // [H] u8
constexpr uint32_t EX_OBJ_PRECINCT = 0x533186;   // [H] text
constexpr uint32_t EX_OBJ_BOMBS    = 0x5331A5;   // [H] text

// ---- keyboard -> pad table (in the exe) --------------------------------------------------
constexpr uint32_t KEY_PAD_TABLE   = 0x524CE8;   // [H] u32[32], pad bit for each KeyDef entry
// Pad bits (PC): 0x1000 up, 0x4000 down, 0x8000 left, 0x2000 right,
// 0x80 action/confirm (Enter, Space, C), 0x40 cancel (Esc), 0x08 aim (X),
// 0x44 run (V: cancel+L1), 0x10 (A), 0x100 (Ctrl), 0x800 (Z, status screen).

// ---- the PC options menu (Ctrl in the game, Option on the title; bio2.exe 0x44A0B0 input,
// 0x44B180 drawing). It runs while System_flg 0x08000000 is set.
constexpr uint32_t OPT_STATE       = 0x99D001;   // [H] u8: 1 top menu, 2/9/10 key config opening, 11 key rows,
                                                 //     12 waiting for a key, 13 leaving key config, 14 defaults,
                                                 //     15 save the keys?, 3/7 brightness picture, 4/8 sound, 0 closed
constexpr uint32_t OPT_TOP_CUR     = 0x99CF68;   // [H] s32 top menu: 0 key config, 1 brightness, 2 sound, 3 exit
constexpr uint32_t OPT_ROW         = 0x68915C;   // [H] s32 row in key config (0..10) and sound (0..4)
constexpr uint32_t OPT_KEYS        = 0x99CF40;   // [H] u8[32] KeyDef being edited (virtual-key codes)
constexpr uint32_t OPT_KEY_CHOICE  = 0x6893C8;   // [H] u8 key config last row: 0 exit, 1 defaults; save question: 0 yes
constexpr uint32_t OPT_SPEAKER     = 0x669CFC;   // [H] s32 1 mono, 2 stereo
constexpr uint32_t OPT_SND_BITS    = 0x669F40;   // [H] s32 8 or 16
constexpr uint32_t OPT_SND_RATE    = 0x669D9C;   // [H] s32 22050 or 44100
constexpr uint32_t OPT_BGM_VOL     = 0x693471;   // [H] u8 1..100 (music buffers)
constexpr uint32_t OPT_SE_VOL      = 0x693C48;   // [H] u8 1..100 (sound effects)
constexpr uint32_t SYS_OPTIONS     = 0x08000000;

// Instant kills (an enemy's finishing grab sets the player's life to -1 directly):
// "mov word ptr [0x98A046], 0xFFFF" (66 C7 05 46 A0 98 00 FF FF); the imm16 is at +7.
// Sherry died at once in the Crawlpipe Passage (cockroaches) with unlimited health on.
constexpr uint32_t KILL_SITES[] = {0x469230, 0x4697E1, 0x475A06, 0x47C7AE, 0x47E1E5, 0x48F819};
// Player damage function (0x4DCA40): "sub si,cx; cmp eax,1" at DMG_SUB subtracts the hit from
// life; when life goes below 0 the same call starts the death (dying bit or game over), before the
// mod's refill can run. With unlimited health the mod calls a 13-byte clamp (life stays >= 1)
// written into the padding at DMG_CAVE (15 NOPs after the previous function).
constexpr uint32_t DMG_SUB  = 0x4DCACF;
constexpr uint32_t DMG_CAVE = 0x4DCA31;
// Cockroach swarm kill (cockroach routine 0x47C550, enemy 0x29): once 3+ roaches cling to the
// player a counter (0x689590) grows every frame; past 225 the next roach that latches on sets
// player routine 3 (dead) and the game-over bit 0x04000000 directly, without the damage
// function. "jbe 0x47C7CD" at ROACH_KILL_JBE skips that; the mod makes it a plain jmp.
constexpr uint32_t ROACH_KILL_JBE = 0x47C798;
// Alligator (enemy 0x23, 40A): after catching the player in its jaws (routine 0x701, 0x4693B0) it
// goes on to 0x401 = swallow (0x4691C0: player routine 5, life -1, game over). The imm16 of
// "mov dword [esi+4], 0x401" at ALLIGATOR_SWALLOW (C7 46 04 01 04 00 00) is at +3; the mod makes
// it 0x101 (back to moving) while unlimited health is on.
constexpr uint32_t ALLIGATOR_SWALLOW = 0x4693EC;
// Alligator +0x226: the gas cylinder (0x4697F0): 0 none, 1..6 biting it, 7 held in its jaws, 13 blown up
constexpr uint32_t ALLIGATOR_CYLINDER = 0x226;
// poison (spiders): u16 timer (0x1518 when bitten; REbirth colours the ECG purple while non-zero)
// and the u8 tick counter; the herb cure (0x4E8FB0) clears both
constexpr uint32_t POISON_TIMER = 0x98A108;
constexpr uint32_t POISON_TICK  = 0x98A10A;

// System_flg bits [H]
constexpr uint32_t SYS_GAME_STARTED = 0x40000000;
constexpr uint32_t SYS_LOAD_GAME    = 0x00020000;
constexpr uint32_t SYS_SAVE_GAME    = 0x00040000;
constexpr uint32_t SYS_DOOR         = 0x00004000;
constexpr uint32_t SYS_GAME_OVER    = 0x04000000;
constexpr uint32_t SYS_MOVIE        = 0x00008000;
constexpr uint32_t SYS_ENDING       = 0x00200000;

// Movies (FMV). The game's movie routine (bio2.exe 0x4CAF90) still runs under Classic REbirth,
// which only replaces the player behind it (0x401FD0 open, 0x401E40 start, 0x4417B0 playing).
constexpr uint32_t MOVIE_STATE     = 0x68058E;   // [H] u8 0 none, 1 open, 2 started, 3 playing, 4 ending, 5 cleanup
constexpr uint32_t MOVIE_INDEX     = 0x98E532;   // [H] u8 entry in the movie table
constexpr uint32_t MOVIE_SKIPPED   = 0x9814B5;   // [H] u8 1 = skipped with a button
constexpr uint32_t MOVIE_TABLE_L   = 0x532C28;   // [H] 12-byte entries: u32 path, u16 skip buttons, u8 adt, u8 ?, u8 mode
constexpr uint32_t MOVIE_TABLE_C   = 0x532D00;   // [H] Claire's table (STATUS_FLG bit 31 set)
constexpr int      MOVIE_ENTRIES   = 18;

}  // namespace addr

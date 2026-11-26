// Windows host: RE2Access.asi, loaded into bio2.exe by the Ultimate ASI Loader (dsound.dll).
// Runs the mod on its own thread: polls hotkeys, reads game memory, speaks through SAPI,
// and presses game keys (SendInput) for auto-walk and turning.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "mod.h"
#include "re2_addr.h"

namespace {

// SAPI class and interface ids, defined here so no SAPI import library is needed.
const GUID kCLSID_SpVoice = {0x96749377, 0x3391, 0x11D2, {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}};
const GUID kIID_ISpVoice = {0x6C44DF74, 0x72B9, 0x4992, {0xA1, 0xEC, 0xEF, 0x99, 0x6E, 0x04, 0x22, 0xD4}};
const GUID kCLSID_SpObjectTokenCategory = {0xA910187F, 0x0C7A, 0x45AC, {0x92, 0xCC, 0x59, 0xED, 0xAF, 0xB7, 0x7B, 0x53}};
const GUID kIID_ISpObjectTokenCategory = {0x2D3D3845, 0x39AF, 0x4850, {0xBB, 0xF9, 0x40, 0xB4, 0x97, 0x80, 0x01, 0x1D}};

HMODULE g_module = nullptr;
std::string g_dir;            // folder of the .asi (the game folder)
std::string g_ini;            // RE2Access.ini
std::string g_log_copy;       // optional second log (the mod folder's Logs), set by deploy
FILE* g_log = nullptr;
FILE* g_log2 = nullptr;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void log_line(const std::string& l) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[32];
    std::snprintf(ts, sizeof ts, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    for (FILE* f : {g_log, g_log2})
        if (f) { std::fputs(ts, f); std::fputs(l.c_str(), f); std::fputc('\n', f); std::fflush(f); }
}

int ini_int(const char* key, int def) { return GetPrivateProfileIntA("RE2Access", key, def, g_ini.c_str()); }
std::string ini_str(const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA("RE2Access", key, def, buf, sizeof buf, g_ini.c_str());
    return buf;
}
void ini_set(const char* key, const std::string& v) { WritePrivateProfileStringA("RE2Access", key, v.c_str(), g_ini.c_str()); }

// ---------------------------------------------------------------- memory
struct ProcessMemory : MemoryReader {
    bool read(uint32_t a, void* out, size_t n) override {
        SIZE_T got = 0;
        return a >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
    }
    bool write(uint32_t a, const void* in, size_t n) override {
        SIZE_T put = 0;
        if (a < 0x10000) return false;
        if (WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, in, n, &put) && put == n) return true;
        // code pages (the unlimited-health patch): unprotect, copy, protect again
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy((void*)(uintptr_t)a, in, n);
        VirtualProtect((LPVOID)(uintptr_t)a, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, n);
        return true;
    }
};


// ---------------------------------------------------------------- PC menu text
// The save/load screens draw their strings through bio2.exe's text function (0x431470),
// which Classic REbirth redirects to its own renderer with a jump. We put our function in
// that jump, note each string, and pass it on.
typedef int(__cdecl* TextFn)(int, int, const char*, int, int);
TextFn g_text_orig = nullptr;
std::mutex g_text_mutex;
std::vector<ScreenText> g_text_cur, g_text_last;
ULONGLONG g_text_last_ms = 0, g_text_cur_ms = 0;

int __cdecl text_hook(int a, int b, const char* s, int colour, int e) {
    {
        std::lock_guard<std::mutex> lock(g_text_mutex);
        ScreenText t;
        t.x = a; t.y = b; t.colour = colour;
        if (s) t.text.assign(s, strnlen(s, 260));
        // the same first string again (or a pause) means a new frame has started
        ULONGLONG now = GetTickCount64();
        if (!g_text_cur.empty() &&
            ((g_text_cur[0].x == t.x && g_text_cur[0].y == t.y && g_text_cur[0].text == t.text) ||
             now - g_text_cur_ms > 60 || g_text_cur.size() >= 128)) {
            g_text_last.swap(g_text_cur);
            g_text_cur.clear();
            g_text_last_ms = now;
        }
        if (g_text_cur.empty()) g_text_cur_ms = now;
        if (g_text_cur.size() < 128) g_text_cur.push_back(t);
    }
    return g_text_orig ? g_text_orig(a, b, s, colour, e) : 0;
}

// (Re)installs the hook if the jump at the text function does not point at us.
void ensure_text_hook() {
    uint8_t* f = (uint8_t*)(uintptr_t)addr::TEXT_DRAW_FUNC;
    uint8_t op;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), f, &op, 1, &got) || got != 1 || op != 0xE9) return;
    int32_t rel;
    if (!ReadProcessMemory(GetCurrentProcess(), f + 1, &rel, 4, &got) || got != 4) return;
    uintptr_t target = (uintptr_t)f + 5 + rel;
    if (target == (uintptr_t)&text_hook) return;
    g_text_orig = (TextFn)target;
    int32_t mine = (int32_t)((uintptr_t)&text_hook - ((uintptr_t)f + 5));
    DWORD old;
    if (VirtualProtect(f + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(f + 1, &mine, 4);
        VirtualProtect(f + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), f, 5);
        char msg[96];
        std::snprintf(msg, sizeof msg, "menu text hook installed (renderer at %08X)", (unsigned)target);
        log_line(msg);
    }
}

std::vector<ScreenText> latest_screen_text() {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    ULONGLONG now = GetTickCount64();
    if (now - g_text_last_ms > 250) {
        // a menu that draws the same single frame and then waits: use what is pending
        if (!g_text_cur.empty() && now - g_text_cur_ms < 250) return g_text_cur;
        return {};
    }
    return g_text_last;
}

// ---------------------------------------------------------------- speech (SAPI)
// Installed voices, without sphelper.h (which needs ATL with some compilers).
std::vector<ISpObjectToken*> list_voices() {
    std::vector<ISpObjectToken*> out;
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, kIID_ISpObjectTokenCategory, (void**)&cat)) || !cat)
        return out;
    IEnumSpObjectTokens* en = nullptr;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en) {
        ULONG n = 0;
        en->GetCount(&n);
        for (ULONG i = 0; i < n; i++) { ISpObjectToken* t = nullptr; if (en->Next(1, &t, nullptr) == S_OK && t) out.push_back(t); }
        en->Release();
    }
    cat->Release();
    return out;
}
std::string token_id(ISpObjectToken* t) {
    WCHAR* id = nullptr;
    std::string r;
    if (t && SUCCEEDED(t->GetId(&id)) && id) { r = utf8(id); CoTaskMemFree(id); }
    return r;
}
std::string token_name(ISpObjectToken* t) {
    WCHAR* name = nullptr;
    std::string r = "voice";
    if (t && SUCCEEDED(t->GetStringValue(nullptr, &name)) && name) { r = utf8(name); CoTaskMemFree(name); }
    return r;
}

// ---------------------------------------------------------------- audio output (0.9.23)
// The mod plays its own sound: SAPI renders speech into memory (SpeechSink below) and the
// audio description WAVs are loaded into memory too. One WASAPI stream mixes both, so speech
// can be made louder than SAPI's own maximum (a limiter keeps it from distorting), and the
// stream has its own audio session, separate from the game's. That lets the mod lower the
// game's own session (music, effects, movie sound) while it is talking ("ducking"). If the
// stream cannot be opened, speech goes straight to the speakers and WAVs through PlaySound,
// as before 0.9.23.
const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const GUID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const GUID kIID_IAudioSessionManager = {0xBFA971F1, 0x4D5E, 0x40BB, {0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4}};
const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIID_ISequentialStream = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};
const GUID kIID_IStream = {0x0000000C, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kCLSID_SpStream = {0x715D9C59, 0x4442, 0x11D2, {0x96, 0x05, 0x00, 0xC0, 0x4F, 0x8E, 0xE6, 0x28}};
const GUID kIID_ISpStream = {0x12E3CCA9, 0x7518, 0x44C5, {0xA5, 0xE7, 0xBA, 0x5A, 0x79, 0xCB, 0x92, 0x9E}};
const GUID kSPDFID_WaveFormatEx = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};
// our own audio session (speech and descriptions), and the tag on our changes to the game's
const GUID kSpeechSession = {0x6F1E2A43, 0x8B7D, 0x4C55, {0x9A, 0x31, 0x52, 0xE4, 0x0B, 0x7C, 0x2D, 0x91}};
const GUID kDuckContext = {0x2B8C5E10, 0x47A1, 0x4E0B, {0x8F, 0x62, 0x1D, 0x93, 0xC4, 0x5A, 0x7E, 0x08}};

constexpr int kRate = 22050;   // the mod's sample rate (Piper's, and what SAPI is asked for)

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct AudioOut {
    std::mutex mu;
    std::vector<float> speech;            // speech samples still to play (from speech_pos)
    size_t speech_pos = 0;
    std::vector<float> file;              // the audio description line playing
    size_t file_pos = 0;
    float speech_gain = 1, file_gain = 1;
    bool duck_on = true, duck_hold = false;   // duck_hold: the introduction (never duck it)
    float duck_level = 0.65f;
    std::atomic<bool> ok{false};          // the stream is open
    std::atomic<ULONGLONG> last_sound{0}; // when our stream last played something
    HANDLE ready = nullptr;
    // owned by the audio thread
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    ISimpleAudioVolume* game = nullptr;   // the game's own session
    UINT32 frames = 0;
    float env = 0;                        // limiter
    float mult = 1;                       // current ducking (1 = none)
    float normal = -1;                    // the game's volume when not ducked
    float last_set = -1;
    ULONGLONG last_step = 0, last_poll = 0;
    bool first_open = true;

    static DWORD WINAPI thread_main(LPVOID p) { ((AudioOut*)p)->loop(); return 0; }
    bool start() {
        ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
        if (!t) return false;
        CloseHandle(t);
        WaitForSingleObject(ready, 3000);
        return ok;
    }
    bool open() {
        IMMDeviceEnumerator* en = nullptr;
        HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
        if (FAILED(hr) || !en) return false;
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        en->Release();
        if (FAILED(hr) || !device) { device = nullptr; return false; }
        if (FAILED(device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&client)) || !client) { close(); return false; }
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 1;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = kRate * 4;
        // Windows converts our mono 22 kHz to the device's own format
        const DWORD flags = 0x80000000 /*AUTOCONVERTPCM*/ | 0x08000000 /*SRC_DEFAULT_QUALITY*/;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 600000 /*60 ms*/, 0, &wf, &kSpeechSession);
        if (FAILED(hr)) {
            char m[80]; std::snprintf(m, sizeof m, "[AUDIO] stream not opened (error %08lX)", (unsigned long)hr); log_line(m);
            close(); return false;
        }
        if (FAILED(client->GetBufferSize(&frames)) || FAILED(client->GetService(kIID_IAudioRenderClient, (void**)&render)) || !render) {
            close(); return false;
        }
        // the game's session: the process's default one (GUID_NULL)
        IAudioSessionManager* sm = nullptr;
        if (SUCCEEDED(device->Activate(kIID_IAudioSessionManager, CLSCTX_ALL, nullptr, (void**)&sm)) && sm) {
            if (FAILED(sm->GetSimpleAudioVolume(nullptr, FALSE, &game))) game = nullptr;
            sm->Release();
        }
        if (game) {
            float v = 1;
            game->GetMasterVolume(&v);
            if (first_open) {
                // A crash while ducked leaves Windows remembering the lowered volume: put it back.
                int n = ini_int("game_volume_normal", -1);
                float nv = n / 1000.0f;
                if (n > 0 && v < nv - 0.01f && std::fabs(v - nv * duck_level) < 0.03f) {
                    game->SetMasterVolume(nv, &kDuckContext);
                    log_line("[AUDIO] game volume put back to " + std::to_string(n / 10) + " percent (it was left lowered)");
                    v = nv;
                }
            }
            normal = v / std::max(mult, 0.01f);
            if (normal > 1) normal = 1;
            last_set = v;
            remember_normal();
        }
        client->Start();
        first_open = false;
        log_line(std::string("[AUDIO] mod stream open (") + std::to_string(frames) + " frames)" + (game ? ", ducking available" : ", no game session: no ducking"));
        return true;
    }
    void close() {
        if (client) client->Stop();
        release(render); release(client); release(game); release(device);
        ok = false;
    }
    void remember_normal() {
        static int saved = -2;
        int n = (int)std::lround(normal * 1000);
        if (n != saved) { saved = n; ini_set("game_volume_normal", std::to_string(n)); }
    }
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool signalled = false;
        ULONGLONG retry = 0;
        for (;;) {
            if (!client) {
                ULONGLONG now = GetTickCount64();
                if (now >= retry) {
                    ok = open();
                    retry = now + 2000;
                }
                if (!signalled) { SetEvent(ready); signalled = true; }
                if (!client) { Sleep(100); continue; }
            }
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) { log_line("[AUDIO] stream lost, reopening"); close(); continue; }
            UINT32 n = frames > pad ? frames - pad : 0;
            if (n) {
                BYTE* data = nullptr;
                if (FAILED(render->GetBuffer(n, &data))) { log_line("[AUDIO] stream lost, reopening"); close(); continue; }
                mix((float*)data, n);
                render->ReleaseBuffer(n, 0);
            }
            duck_step();
            Sleep(5);
        }
    }
    void mix(float* out, UINT32 n) {
        static const float rel = std::exp(-1.0f / (0.15f * kRate));   // 150 ms release
        const float thr = 0.9f;
        bool any = false;
        std::lock_guard<std::mutex> lock(mu);
        for (UINT32 i = 0; i < n; i++) {
            float s = 0;
            if (speech_pos < speech.size()) { s += speech[speech_pos++] * speech_gain; any = true; }
            if (file_pos < file.size()) { s += file[file_pos++] * file_gain; any = true; }
            float a = std::fabs(s);
            env = a > env ? a : a + (env - a) * rel;
            if (env > thr) s *= thr / env;
            out[i] = std::max(-1.0f, std::min(1.0f, s));
        }
        if (speech_pos >= speech.size()) { speech.clear(); speech_pos = 0; }
        else if (speech_pos > (size_t)kRate * 10) { speech.erase(speech.begin(), speech.begin() + speech_pos); speech_pos = 0; }
        if (file_pos >= file.size() && !file.empty()) { file.clear(); file_pos = 0; }
        if (any) last_sound = GetTickCount64();
    }
    // Lowers the game's session while we are making sound (down in about 80 ms, back up over
    // about half a second, 300 ms after we stop). Follows the player's own changes to the
    // game's volume (Volume Mixer) and keeps the normal level in the ini.
    void duck_step() {
        if (!game) return;
        ULONGLONG now = GetTickCount64();
        float dt = last_step ? (float)(now - last_step) : 0;
        last_step = now;
        bool want, hold;
        float level;
        { std::lock_guard<std::mutex> lock(mu); want = duck_on; hold = duck_hold; level = duck_level; }
        ULONGLONG ls = last_sound;
        want = want && !hold && ls && now - ls < 300;
        float target = want ? level : 1.0f;
        if (mult > target) mult = std::max(target, mult - dt / 80.0f);
        else if (mult < target) mult = std::min(target, mult + dt / 500.0f);
        if (now - last_poll > 500) {
            last_poll = now;
            float v;
            if (SUCCEEDED(game->GetMasterVolume(&v)) && std::fabs(v - last_set) > 0.02f) {
                normal = std::min(1.0f, v / std::max(mult, 0.01f));   // the player changed it
                last_set = v;
                remember_normal();
            }
        }
        float v = normal * mult;
        if (std::fabs(v - last_set) > 0.004f || (mult == 1 && v != last_set)) {
            game->SetMasterVolume(v, &kDuckContext);
            last_set = v;
        }
    }
    // ---- called from the mod thread
    void push_speech(const int16_t* p, size_t n) {
        std::lock_guard<std::mutex> lock(mu);
        for (size_t i = 0; i < n; i++) speech.push_back(p[i] / 32768.0f);
    }
    void clear_speech() { std::lock_guard<std::mutex> lock(mu); speech.clear(); speech_pos = 0; }
    void play(std::vector<float>&& s, float gain) {
        std::lock_guard<std::mutex> lock(mu);
        file.swap(s); file_pos = 0; file_gain = gain;
    }
    void stop_file() { std::lock_guard<std::mutex> lock(mu); file.clear(); file_pos = 0; }
    void set_speech_gain(float g) { std::lock_guard<std::mutex> lock(mu); speech_gain = g; }
    void set_ducking(bool on, float level) { std::lock_guard<std::mutex> lock(mu); duck_on = on; duck_level = level; }
    void set_hold(bool h) { std::lock_guard<std::mutex> lock(mu); duck_hold = h; }
};

// A 16-bit PCM or float WAV as mono samples at kRate.
bool load_wav(const std::string& path, std::vector<float>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const char* data = nullptr;
    uint32_t len = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        uint32_t sz;
        std::memcpy(&sz, &b[p + 4], 4);
        size_t body = p + 8;
        if (body + sz > b.size()) sz = (uint32_t)(b.size() - body);
        if (!std::memcmp(&b[p], "fmt ", 4) && sz >= 16) {
            uint16_t t16; uint32_t t32;
            std::memcpy(&t16, &b[body], 2); fmt = t16;
            std::memcpy(&t16, &b[body + 2], 2); ch = t16;
            std::memcpy(&t32, &b[body + 4], 4); rate = (int)t32;
            std::memcpy(&t16, &b[body + 14], 2); bits = t16;
            if (fmt == 0xFFFE && sz >= 26) { std::memcpy(&t16, &b[body + 24], 2); fmt = t16; }   // extensible
        } else if (!std::memcmp(&b[p], "data", 4)) {
            data = &b[body]; len = sz;
        }
        p = body + sz + (sz & 1);
    }
    if (!data || ch < 1 || rate < 1000 || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) return false;
    size_t bytes = bits / 8, n = len / (bytes * ch);
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; i++) {
        float s = 0;
        for (int c = 0; c < ch; c++) {
            const char* q = data + (i * ch + c) * bytes;
            if (fmt == 1) { int16_t v; std::memcpy(&v, q, 2); s += v / 32768.0f; }
            else { float v; std::memcpy(&v, q, 4); s += v; }
        }
        mono[i] = s / ch;
    }
    if (rate == kRate) { out.swap(mono); return true; }
    size_t m = (size_t)((double)n * kRate / rate);
    out.resize(m);
    for (size_t i = 0; i < m; i++) {
        double x = (double)i * rate / kRate;
        size_t a = (size_t)x;
        double fr = x - a;
        float s0 = mono[std::min(a, n - 1)], s1 = mono[std::min(a + 1, n - 1)];
        out[i] = (float)(s0 + (s1 - s0) * fr);
    }
    return true;
}

// SAPI writes its speech here (22 kHz, 16-bit, mono) as fast as it makes it; we queue it for
// the mixer. A minimal IStream: SAPI only writes, asks the position and asks for Stat.
struct SpeechSink : IStream {
    LONG refs = 1;
    AudioOut* out = nullptr;
    ULONGLONG pos = 0;
    int odd = -1;   // a byte left over from an odd-sized write
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pv) override {
        if (!pv) return E_POINTER;
        if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_ISequentialStream) || IsEqualGUID(riid, kIID_IStream)) {
            *pv = static_cast<IStream*>(this); AddRef(); return S_OK;
        }
        *pv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&refs); }   // static lifetime
    HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG* got) override { if (got) *got = 0; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE Write(const void* p, ULONG cb, ULONG* put) override {
        const uint8_t* b = (const uint8_t*)p;
        ULONG i = 0;
        std::vector<int16_t> s;
        s.reserve(cb / 2 + 1);
        if (odd >= 0 && cb) { s.push_back((int16_t)(odd | (b[0] << 8))); odd = -1; i = 1; }
        for (; i + 1 < cb; i += 2) s.push_back((int16_t)(b[i] | (b[i + 1] << 8)));
        if (i < cb) odd = b[i];
        if (out && !s.empty()) out->push_speech(s.data(), s.size());
        pos += cb;
        if (put) *put = cb;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* np) override {
        LONGLONG to = (LONGLONG)pos;
        if (origin == STREAM_SEEK_SET) to = move.QuadPart;
        else to += move.QuadPart;   // CUR and END (the end is where we are)
        if (to < 0) return STG_E_INVALIDFUNCTION;
        pos = (ULONGLONG)to;
        if (np) np->QuadPart = pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* st, DWORD) override {
        if (!st) return E_POINTER;
        std::memset(st, 0, sizeof *st);
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

struct Speech {
    ISpVoice* voice = nullptr;
    AudioOut* audio = nullptr;       // set when the mod's own stream is open
    ISpStream* stream = nullptr;
    SpeechSink sink;
    bool own_output = false;         // speech goes through the mod's stream
    // where the settings keys' answers go (the host's current output: SAPI or a screen reader)
    std::function<void(const std::string&)> announce;
    void tell(const std::string& t) { if (announce) announce(t); else say(t, true); }
    int rate = 2, volume = 100;      // volume 0-200 percent (above 100 needs own_output)
    void init(AudioOut* a) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(kCLSID_SpVoice, nullptr, CLSCTX_ALL, kIID_ISpVoice, (void**)&voice))) {
            voice = nullptr;
            log_line("SAPI: could not create a voice");
            return;
        }
        rate = ini_int("speech_rate", 2);
        volume = ini_int("speech_volume", 100);
        std::string vid = ini_str("speech_voice_id", "");
        if (!vid.empty()) {
            auto toks = list_voices();
            for (auto* t : toks) if (token_id(t) == vid) voice->SetVoice(t);
            for (auto* t : toks) t->Release();
        }
        if (a) {
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 1;
            wf.nSamplesPerSec = kRate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 2;
            wf.nAvgBytesPerSec = kRate * 2;
            sink.out = a;
            if (SUCCEEDED(CoCreateInstance(kCLSID_SpStream, nullptr, CLSCTX_ALL, kIID_ISpStream, (void**)&stream)) && stream &&
                SUCCEEDED(stream->SetBaseStream(&sink, kSPDFID_WaveFormatEx, &wf)) &&
                SUCCEEDED(voice->SetOutput(stream, FALSE))) {
                audio = a;
                own_output = true;
            } else {
                release(stream);
                log_line("SAPI: could not send speech to the mod's stream; speaking straight to the speakers");
            }
        }
        if (!own_output) volume = std::min(volume, 100);
        voice->SetRate(rate);
        apply_volume();
        log_line("SAPI ready, rate " + std::to_string(rate) + ", volume " + std::to_string(volume) +
                 (own_output ? " (through the mod's stream)" : " (straight to the speakers)"));
    }
    void apply_volume() {
        if (own_output) { voice->SetVolume(100); audio->set_speech_gain(volume / 100.0f); }
        else voice->SetVolume((USHORT)volume);
    }
    // cut off what is being said, including what SAPI has already made and we have queued
    void purge() {
        voice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
        if (own_output) { voice->WaitUntilDone(200); audio->clear_speech(); }
    }
    void say(const std::string& text, bool interrupt) {
        log_line(std::string("[TTS]") + (interrupt ? "(i) " : " ") + text);
        if (!voice) return;
        std::wstring w = wide(text);
        if (own_output) {
            if (interrupt) purge();
            voice->Speak(w.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
        } else {
            voice->Speak(w.c_str(), SPF_ASYNC | SPF_IS_NOT_XML | (interrupt ? SPF_PURGEBEFORESPEAK : 0), nullptr);
        }
    }
    void stop() { if (voice) purge(); }
    void change_rate(int d) {
        rate = std::max(-10, std::min(10, rate + d));
        if (voice) voice->SetRate(rate);
        ini_set("speech_rate", std::to_string(rate));
        tell("Rate " + std::to_string(rate));
    }
    void change_volume(int d) {
        int top = own_output ? 200 : 100;
        volume = std::max(0, std::min(top, volume + d));
        if (voice) apply_volume();
        ini_set("speech_volume", std::to_string(volume));
        tell("Volume " + std::to_string(volume) + " percent" +
            (!own_output && d > 0 && volume == 100 ? ". The most without the mod's own sound output, which could not start." : ""));
    }
    void next_voice() {
        if (!voice) return;
        auto toks = list_voices();
        if (toks.empty()) return;
        ISpObjectToken* cur = nullptr;
        voice->GetVoice(&cur);
        std::string curid = token_id(cur);
        if (cur) cur->Release();
        size_t idx = 0;
        for (size_t i = 0; i < toks.size(); i++) if (token_id(toks[i]) == curid) idx = i;
        ISpObjectToken* next = toks[(idx + 1) % toks.size()];
        voice->SetVoice(next);
        ini_set("speech_voice_id", token_id(next));
        std::string nm = token_name(next);
        for (auto* t : toks) t->Release();
        tell("Voice: " + nm);
    }
};

// ---------------------------------------------------------------- diagnostic picture
// F11 also saves what is on screen (the game window) as a .bmp next to the log, so a sighted
// helper (or Claude) can compare it with what the mod said.
struct WinFind { DWORD pid; HWND best; long area; };
BOOL CALLBACK find_window_cb(HWND h, LPARAM lp) {
    WinFind* c = (WinFind*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    long a = (long)(r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}
HWND find_game_window() {
    WinFind c{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(find_window_cb, (LPARAM)&c);
    return c.best;
}

// F11 picture. The game draws through DirectDraw, which a plain copy of the screen often
// misses (it comes out black), so three ways are tried in turn: the window's own content
// (PrintWindow with PW_RENDERFULLCONTENT, Windows 8.1+), the desktop duplication API (what
// screen recorders use; sees full-screen games too), and a copy of the screen.
static bool all_dark(const std::vector<uint8_t>& pix) {
    size_t lit = 0;
    for (size_t i = 0; i < pix.size(); i += 7) if (pix[i] > 16) lit++;
    return lit < pix.size() / 7 / 200;     // under half a percent of samples lit
}

static bool grab_dc(HDC src_dc, int sx, int sy, int w, int h, std::vector<uint8_t>& pix, HWND print = nullptr) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    bool ok;
    if (print) ok = PrintWindow(print, mem, 0x1 | 0x2) != 0;     // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    else ok = BitBlt(mem, 0, 0, w, h, src_dc ? src_dc : screen, sx, sy, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) / 4) * 4;
    pix.assign((size_t)stride * h, 0);
    if (ok) ok = GetDIBits(mem, bmp, 0, h, pix.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS) == h;
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    return ok;
}

// Desktop duplication: one frame of the monitor that shows the window, cropped to the window.
static bool grab_duplication(RECT win, int w, int h, std::vector<uint8_t>& pix, std::string& why) {
    HMODULE d3d = LoadLibraryA("d3d11.dll");
    if (!d3d) { why = "no d3d11"; return false; }
    auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice");
    if (!create) { why = "no D3D11CreateDevice"; return false; }
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { why = "device failed"; return false; }
    bool ok = false;
    IDXGIDevice* xdev = nullptr; IDXGIAdapter* adapter = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    RECT od = {};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xdev)) && SUCCEEDED(xdev->GetAdapter(&adapter))) {
        POINT c{(win.left + win.right) / 2, (win.top + win.bottom) / 2};
        IDXGIOutput* out = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC desc;
            out->GetDesc(&desc);
            RECT r = desc.DesktopCoordinates;
            if (!dup && c.x >= r.left && c.x < r.right && c.y >= r.top && c.y < r.bottom) {
                IDXGIOutput1* o1 = nullptr;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1))) {
                    hr = o1->DuplicateOutput(dev, &dup);
                    if (FAILED(hr)) { char b[48]; std::snprintf(b, sizeof b, "duplicate failed %08lX", (unsigned long)hr); why = b; dup = nullptr; }
                    else od = r;
                    o1->Release();
                }
            }
            out->Release();
        }
    }
    if (dup) {
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi;
        for (int tries = 0; tries < 10 && !ok; tries++) {
            hr = dup->AcquireNextFrame(200, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { why = "no frame"; break; }
            if (fi.LastPresentTime.QuadPart == 0 && tries < 9) { res->Release(); dup->ReleaseFrame(); continue; }
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                D3D11_TEXTURE2D_DESC td;
                tex->GetDesc(&td);
                td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
                ID3D11Texture2D* st = nullptr;
                if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st))) {
                    ctx->CopyResource(st, tex);
                    D3D11_MAPPED_SUBRESOURCE map;
                    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) {
                        int stride = ((w * 3 + 3) / 4) * 4;
                        pix.assign((size_t)stride * h, 0);
                        int ox = win.left - od.left, oy = win.top - od.top;
                        for (int y = 0; y < h; y++) {
                            int sy = oy + y;
                            if (sy < 0 || sy >= (int)td.Height) continue;
                            const uint8_t* src = (const uint8_t*)map.pData + (size_t)sy * map.RowPitch;
                            uint8_t* dst = pix.data() + (size_t)(h - 1 - y) * stride;     // bottom-up BMP
                            for (int x = 0; x < w; x++) {
                                int sx = ox + x;
                                if (sx < 0 || sx >= (int)td.Width) continue;
                                dst[x * 3] = src[sx * 4]; dst[x * 3 + 1] = src[sx * 4 + 1]; dst[x * 3 + 2] = src[sx * 4 + 2];
                            }
                        }
                        ctx->Unmap(st, 0);
                        ok = true;
                    }
                    st->Release();
                }
                tex->Release();
            }
            res->Release();
            dup->ReleaseFrame();
        }
        dup->Release();
    }
    if (adapter) adapter->Release();
    if (xdev) xdev->Release();
    ctx->Release(); dev->Release();
    return ok;
}

// The game's own screenshot (the PC version's Print Screen key, bio2.exe WM_KEYDOWN VK_SNAPSHOT
// -> 0x442750): it writes the frame it drew to capt0000.bmp, capt0001.bmp... in the game's
// folder. It works whatever the screen is doing (laptop lid closed, no monitor). The mod sends
// it the key and moves the picture to the Logs folder.
static std::vector<std::string> capt_files(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\capt*.bmp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName); while (FindNextFileA(f, &fd));
    FindClose(f);
    return out;
}

static void save_diag_picture_windows();

static DWORD WINAPI game_shot_thread(LPVOID) {
    HWND h = find_game_window();
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::vector<std::string> dirs = {g_dir};
    if (_stricmp(cwd, g_dir.c_str()) != 0) dirs.push_back(cwd);
    std::vector<std::vector<std::string>> before;
    for (auto& d : dirs) before.push_back(capt_files(d));
    if (h) PostMessageA(h, WM_KEYDOWN, VK_SNAPSHOT, 1);
    for (int t = 0; h && t < 30; t++) {
        Sleep(100);
        for (size_t i = 0; i < dirs.size(); i++) {
            for (auto& n : capt_files(dirs[i])) {
                if (std::find(before[i].begin(), before[i].end(), n) != before[i].end()) continue;
                Sleep(300);      // let the game finish writing it
                SYSTEMTIME st;
                GetLocalTime(&st);
                char name[64];
                std::snprintf(name, sizeof name, "\\diag_%04d-%02d-%02d_%02d-%02d-%02d.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                std::string dest = (g_log_copy.empty() ? g_dir : g_log_copy) + name;
                std::string src = dirs[i] + "\\" + n;
                if (MoveFileExA(src.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
                    log_line("DIAG picture saved (the game's own screenshot): " + dest);
                else
                    log_line("DIAG picture saved (the game's own screenshot): " + src);
                return 0;
            }
        }
    }
    log_line("DIAG picture: the game's screenshot did not appear; copying the window instead");
    save_diag_picture_windows();
    return 0;
}

void save_diag_picture() {
    CloseHandle(CreateThread(nullptr, 0, game_shot_thread, nullptr, 0, nullptr));
}

static void save_diag_picture_windows() {
    HWND h = find_game_window();
    if (!h) { log_line("DIAG picture: no game window"); return; }
    RECT r;
    GetClientRect(h, &r);
    POINT tl{0, 0};
    ClientToScreen(h, &tl);
    int w = r.right - r.left, hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0) { log_line("DIAG picture: empty window"); return; }
    RECT win{tl.x, tl.y, tl.x + w, tl.y + hh};
    std::vector<uint8_t> pix;
    std::string how, notes;
    if (grab_dc(nullptr, 0, 0, w, hh, pix, h) && !all_dark(pix)) how = "window content";
    else {
        notes += "window content dark; ";
        std::string why;
        if (grab_duplication(win, w, hh, pix, why) && !all_dark(pix)) how = "desktop duplication";
        else {
            notes += "duplication " + (why.empty() ? std::string("dark") : why) + "; ";
            HDC wdc = GetDC(h);
            bool ok = grab_dc(wdc, 0, 0, w, hh, pix);
            ReleaseDC(h, wdc);
            if (ok && !all_dark(pix)) how = "window copy";
            else if (grab_dc(nullptr, tl.x, tl.y, w, hh, pix)) how = all_dark(pix) ? "screen copy (dark: is a screen curtain on?)" : "screen copy";
        }
    }
    if (how.empty()) { log_line("DIAG picture: nothing captured (" + notes + ")"); return; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    std::snprintf(name, sizeof name, "\\diag_%04d-%02d-%02d_%02d-%02d-%02d.bmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string dir = g_log_copy.empty() ? g_dir : g_log_copy;
    std::string path = dir + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { log_line("DIAG picture: could not write " + path); return; }
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = hh; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = fh.bfOffBits + (DWORD)pix.size();
    std::fwrite(&fh, sizeof fh, 1, f);
    std::fwrite(&bi, sizeof bi, 1, f);
    std::fwrite(pix.data(), 1, pix.size(), f);
    std::fclose(f);
    log_line("DIAG picture saved (" + how + (notes.empty() ? "" : "; " + notes) + "): " + path);
}

// ---------------------------------------------------------------- game keys
// The game's own key layout: KeyDef in config.ini (32 virtual-key codes) and, in the exe, the
// pad bit each KeyDef entry produces. The mod presses the key bound to the pad bit it needs.
struct GameKeys {
    WORD vk[8] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'C', 'V', 'X', VK_ESCAPE};   // defaults
    uint32_t held = 0;
    void load(MemoryReader& m) {
        char buf[256];
        GetPrivateProfileStringA("GAME", "KeyDef", "", buf, sizeof buf, (g_dir + "\\config.ini").c_str());
        std::vector<int> kd;
        for (const char* p = buf; *p;) {
            while (*p == ' ') p++;
            if (!*p) break;
            kd.push_back((int)std::strtol(p, (char**)&p, 16));
        }
        static const uint32_t want[8] = {0x1000, 0x4000, 0x8000, 0x2000, 0x80, 0x44, 0x08, 0x40};
        for (int b = 0; b < 8; b++)
            for (int i = (int)kd.size() - 1; i >= 0; i--) {   // later entries are the main keyboard keys
                uint32_t pad = m.u32(addr::KEY_PAD_TABLE + i * 4);
                if (kd[i] && pad == want[b]) { vk[b] = (WORD)kd[i]; break; }
            }
        char msg[160];
        std::snprintf(msg, sizeof msg, "game keys: up %02X down %02X left %02X right %02X action %02X run %02X aim %02X cancel %02X",
                      vk[0], vk[1], vk[2], vk[3], vk[4], vk[5], vk[6], vk[7]);
        log_line(msg);
    }
    static void send(WORD v, bool down) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = v;
        in.ki.wScan = (WORD)MapVirtualKeyA(v, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP);
        if (v == VK_UP || v == VK_DOWN || v == VK_LEFT || v == VK_RIGHT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof in);
    }
    void apply(uint32_t want) {
        for (int b = 0; b < 8; b++) {
            uint32_t bit = 1u << b;
            if ((want & bit) && !(held & bit)) send(vk[b], true);
            if (!(want & bit) && (held & bit)) send(vk[b], false);
        }
        held = want;
    }
};

bool game_focused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------- host
// ---------------------------------------------------------------- screen readers (Prism)
// prism.dll (Prism, https://github.com/ethindp/prism, MPL-2.0; 32-bit build made by
// Mod\third_party\prism\build_prism.ps1) sits next to RE2Access.asi and is loaded at run
// time. Without it, or with no screen reader running, the mod speaks with SAPI. Only these C
// functions of prism.h (v0.18) are used; text is UTF-8.
struct ScreenReaders {
    typedef void* (__cdecl* InitFn)(void*);
    typedef size_t (__cdecl* CountFn)(void*);
    typedef uint64_t (__cdecl* IdAtFn)(void*, size_t);
    typedef void* (__cdecl* AcquireFn)(void*, uint64_t);
    typedef void (__cdecl* FreeFn)(void*);
    typedef const char* (__cdecl* NameFn)(void*);
    typedef uint64_t (__cdecl* FeaturesFn)(void*);
    typedef int (__cdecl* BackendInitFn)(void*);
    typedef int (__cdecl* OutputFn)(void*, const char*, bool);
    typedef int (__cdecl* StopFn)(void*);
    typedef const char* (__cdecl* VersionFn)();
    static constexpr uint64_t kIsScreenReader = 1ULL << 56;   // PRISM_BACKEND_IS_SCREEN_READER
    static constexpr int kOk = 0, kAlreadyInitialized = 15;   // PrismError
    struct Found { uint64_t id; std::string name; };

    HMODULE dll = nullptr;
    void* ctx = nullptr;
    InitFn init_ = nullptr; CountFn count_ = nullptr; IdAtFn id_at_ = nullptr; AcquireFn acquire_ = nullptr;
    FreeFn free_ = nullptr; NameFn name_ = nullptr; FeaturesFn features_ = nullptr; BackendInitFn binit_ = nullptr;
    OutputFn output_ = nullptr; StopFn stop_ = nullptr; VersionFn version_ = nullptr;
    void* cur = nullptr;             // the backend in use
    uint64_t cur_id = 0;
    std::string cur_name;

    bool load(const std::string& dir) {
        std::string path = dir + "\\prism.dll";
        dll = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) { log_line("[SPEECH] prism.dll not found; screen readers are not available, speech is SAPI"); return false; }
        auto get = [&](const char* n) { return (void*)GetProcAddress(dll, n); };
        init_ = (InitFn)get("prism_init"); count_ = (CountFn)get("prism_registry_count");
        id_at_ = (IdAtFn)get("prism_registry_id_at"); acquire_ = (AcquireFn)get("prism_registry_acquire");
        free_ = (FreeFn)get("prism_backend_free"); name_ = (NameFn)get("prism_backend_name");
        features_ = (FeaturesFn)get("prism_backend_get_features"); binit_ = (BackendInitFn)get("prism_backend_initialize");
        output_ = (OutputFn)get("prism_backend_output"); stop_ = (StopFn)get("prism_backend_stop");
        version_ = (VersionFn)get("prism_version_string");
        if (!init_ || !count_ || !id_at_ || !acquire_ || !free_ || !name_ || !features_ || !binit_ || !output_ || !stop_) {
            log_line("[SPEECH] prism.dll is missing functions the mod needs; speech is SAPI");
            FreeLibrary(dll); dll = nullptr;
            return false;
        }
        ctx = init_(nullptr);
        if (!ctx) { log_line("[SPEECH] Prism did not start; speech is SAPI"); return false; }
        log_line(std::string("[SPEECH] Prism ") + (version_ ? version_() : "") + " loaded");
        return true;
    }
    // the screen readers running now, in Prism's order of preference
    std::vector<Found> running() {
        std::vector<Found> v;
        if (!ctx) return v;
        size_t n = count_(ctx);
        for (size_t i = 0; i < n; i++) {
            uint64_t id = id_at_(ctx, i);
            if (cur && id == cur_id) { v.push_back({id, cur_name}); continue; }
            void* b = acquire_(ctx, id);
            if (!b) continue;
            if (features_(b) & kIsScreenReader) {
                int e = binit_(b);
                if (e == kOk || e == kAlreadyInitialized) {
                    const char* nm = name_(b);
                    v.push_back({id, nm && *nm ? nm : "Screen reader"});
                }
            }
            free_(b);
        }
        return v;
    }
    bool use(const Found& f) {
        if (cur && cur_id == f.id) return true;
        release();
        void* b = acquire_(ctx, f.id);
        if (!b) return false;
        int e = binit_(b);
        if (e != kOk && e != kAlreadyInitialized) { free_(b); return false; }
        cur = b; cur_id = f.id; cur_name = f.name;
        return true;
    }
    void release() { if (cur) free_(cur); cur = nullptr; cur_id = 0; cur_name.clear(); }
    // speech and braille through the screen reader; false if it did not take the text
    bool say(const std::string& t, bool interrupt) { return cur && output_(cur, t.c_str(), interrupt) == kOk; }
    void stop() { if (cur) stop_(cur); }
};

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

struct WinHost : Host {
    ProcessMemory memory;
    AudioOut audio;
    bool audio_ok = false;
    bool intro = false;              // the narrated introduction is on screen
    bool duck_on = true;
    int duck_pct = 65;               // the game's volume while the mod talks
    int intro_pct = 50;              // the introduction's descriptions, percent of the speech volume
    Speech speech;
    GameKeys keys;
    bool keys_loaded = false;
    uint32_t want = 0;
    MemoryReader& mem() override { return memory; }
    // Speech output (Shift F1, speech_output in RE2Access.ini): "auto" = the first screen
    // reader that is running, else SAPI; "sapi"; or a screen reader's name (NVDA, JAWS ...).
    ScreenReaders sr;
    std::string out_pref = "auto";
    bool using_sr = false;
    uint64_t next_probe_ms = 0;
    void speak(const std::string& t, bool i) override {
        if (using_sr) {
            log_line(std::string("[TTS]") + (i ? "(i) " : " ") + "[" + sr.cur_name + "] " + t);
            if (sr.say(t, i)) return;
            log_line("[SPEECH] " + sr.cur_name + " did not take the text; speaking with SAPI");
            sr.release();
            using_sr = false;
            next_probe_ms = ms() + 5000;
        }
        speech.say(t, i);
    }
    void stop_speech() override { if (using_sr) sr.stop(); else speech.stop(); }
    void init_output() {
        out_pref = lower(ini_str("speech_output", "auto"));
        if (out_pref.empty()) out_pref = "auto";
        speech.announce = [this](const std::string& t) { speak((using_sr ? "SAPI " : "") + t, true); };
        if (sr.load(g_dir)) pick();
        log_line("[SPEECH] output: " + output_name() + " (setting " + out_pref + ")");
    }
    std::string output_name() const { return using_sr ? sr.cur_name : "SAPI"; }
    // choose the output the setting asks for among what is running now
    void pick() {
        if (!sr.ctx || out_pref == "sapi") { sr.release(); using_sr = false; return; }
        auto found = sr.running();
        for (auto& f : found) {
            if (out_pref == "auto" || lower(f.name) == out_pref) {
                using_sr = sr.use(f);
                if (using_sr) return;
            }
        }
        sr.release();
        using_sr = false;
    }
    // every few seconds while speaking with SAPI under "auto" (or a named reader): a screen
    // reader started after the game is taken up
    void probe_output() {
        if (!sr.ctx || out_pref == "sapi" || using_sr || ms() < next_probe_ms) return;
        next_probe_ms = ms() + 5000;
        pick();
        if (using_sr) {
            log_line("[SPEECH] output: " + sr.cur_name);
            speak("Speech: " + sr.cur_name + ".", true);
        }
    }
    // Shift F1: automatic, each running screen reader, SAPI, round again
    void cycle_output() {
        if (!sr.ctx) { speak("Speech: SAPI. Screen reader support is not installed.", true); return; }
        std::vector<std::string> opts{"auto"};
        for (auto& f : sr.running()) opts.push_back(lower(f.name));
        opts.push_back("sapi");
        size_t i = 0;
        for (size_t k = 0; k < opts.size(); k++) if (opts[k] == out_pref) i = k;
        out_pref = opts[(i + 1) % opts.size()];
        ini_set("speech_output", out_pref);
        pick();
        log_line("[SPEECH] output: " + output_name() + " (setting " + out_pref + ")");
        speak("Speech: " + std::string(out_pref == "auto" ? "automatic, " : "") + output_name() + ".", true);
    }
    void cue(const std::string& n) override {
        log_line("[CUE] " + n);
        if (n == "wall") MessageBeep(MB_OK);
    }
    void set_buttons(uint32_t b) override { want = b; }
    // game buttons physically held (the game's own keys from config.ini)
    uint32_t player_buttons() override {
        if (!keys_loaded || !game_focused()) return 0;
        uint32_t b = 0;
        for (int i = 0; i < 8; i++)
            if (GetAsyncKeyState(keys.vk[i]) & 0x8000) b |= 1u << i;
        return b;
    }
    std::vector<ScreenText> screen_text() override { return latest_screen_text(); }
    int get_setting(const std::string& k, int def) override { return ini_int(k.c_str(), def); }
    void put_setting(const std::string& k, int v) override { ini_set(k.c_str(), std::to_string(v)); }
    std::string game_data_dir() override { return g_dir; }
    std::string mod_data_dir() override { return g_dir + "\\RE2Access"; }
    void log(const std::string& l) override { log_line(l); }
    uint64_t ms() override { return GetTickCount64(); }
    // audio description lines: one WAV at a time, at the speech volume (the introduction's
    // at intro_pct of it); through PlaySound (full volume) if the mod's stream is not open
    void play_file(const std::string& path) override {
        if (audio_ok) {
            std::vector<float> s;
            if (!load_wav(path, s)) { log_line("[AUDIO] could not play " + path); return; }
            float g = speech.volume / 100.0f * (intro ? intro_pct / 100.0f : 1.0f);
            audio.play(std::move(s), g);
            return;
        }
        std::wstring w(path.begin(), path.end());
        if (!PlaySoundW(w.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
            log_line("[AUDIO] could not play " + path);
    }
    void stop_file() override { if (audio_ok) audio.stop_file(); else PlaySoundW(nullptr, nullptr, 0); }
    void set_intro_audio(bool on) override {
        intro = on;
        audio.set_hold(on);
        log_line(std::string("[AUDIO] introduction ") + (on ? "on: descriptions at " + std::to_string(intro_pct) + " percent, no ducking" : "off"));
    }
    void start_audio() {
        duck_on = ini_int("ducking", 1) != 0;
        duck_pct = std::max(5, std::min(100, ini_int("ducking_volume", 65)));
        intro_pct = std::max(0, std::min(200, ini_int("intro_description_volume", 50)));
        audio.set_ducking(duck_on, duck_pct / 100.0f);
        audio_ok = audio.start();
        speech.init(audio_ok ? &audio : nullptr);
    }
    void toggle_ducking() {
        if (!audio_ok) { speak("Ducking is not available: the mod's own sound output could not start.", true); return; }
        duck_on = !duck_on;
        ini_set("ducking", duck_on ? "1" : "0");
        audio.set_ducking(duck_on, duck_pct / 100.0f);
        speak(duck_on ? "Ducking on. Game sound drops to " + std::to_string(duck_pct) + " percent while the mod talks." : "Ducking off.", true);
    }
    void cycle_ducking_level() {
        if (!audio_ok) { speak("Ducking is not available: the mod's own sound output could not start.", true); return; }
        static const int levels[] = {20, 35, 50, 65, 80};
        int next = levels[0];
        for (int l : levels) if (l > duck_pct) { next = l; break; }
        if (duck_pct >= 80) next = levels[0];
        duck_pct = next;
        ini_set("ducking_volume", std::to_string(duck_pct));
        audio.set_ducking(duck_on, duck_pct / 100.0f);
        speak("Game sound while the mod talks: " + std::to_string(duck_pct) + " percent" + (duck_on ? "." : ". Ducking is off; F2 turns it on."), true);
    }
    void change_intro_volume(int d) {
        intro_pct = std::max(0, std::min(200, intro_pct + d));
        ini_set("intro_description_volume", std::to_string(intro_pct));
        speak("Introduction descriptions " + std::to_string(intro_pct) + " percent", true);
    }
    void save_picture() override { save_diag_picture(); }
};

struct KeyBind { int vk; bool shift; Hotkey hk; };
const KeyBind kBinds[] = {
    {VK_OEM_MINUS, false, HK_PREV}, {VK_OEM_PLUS, false, HK_NEXT},
    {VK_OEM_4, false, HK_CAT_PREV}, {VK_OEM_6, false, HK_CAT_NEXT},
    {VK_BACK, false, HK_WHERE}, {VK_OEM_5, false, HK_GO}, {VK_OEM_5, true, HK_FACE},
    {'T', false, HK_TARGET}, {'T', true, HK_TARGET_NEXT},
    {VK_OEM_7, false, HK_ROOM}, {'H', false, HK_HEALTH}, {'I', false, HK_INVENTORY}, {'E', false, HK_ENEMIES}, {'E', true, HK_GET_AWAY},
    {VK_OEM_1, false, HK_SCAN}, {VK_OEM_COMMA, false, HK_COMPASS}, {VK_OEM_PERIOD, false, HK_EXPLORE},
    {VK_OEM_3, false, HK_REPEAT}, {VK_OEM_2, false, HK_HELP}, {VK_F10, false, HK_VERSION}, {VK_F11, false, HK_DIAG},
    {VK_F5, false, HK_UNLIMITED_HEALTH}, {VK_F6, false, HK_UNLIMITED_AMMO},
    {'M', false, HK_MAP}, {'M', true, HK_MAP_MODE}, {'Y', false, HK_TIMER},
};

DWORD WINAPI mod_thread(LPVOID) {
    Sleep(1500);
    WinHost host;
    host.start_audio();
    host.init_output();
    re2::Mod mod(host);
    host.speak(std::string("Resident Evil 2 accessibility mod ") + RE2ACCESS_VERSION + " loaded. Speech: " + host.output_name() + ".", false);
    bool was[256] = {};
    unsigned hook_check = 59;
    for (;;) {
        Sleep(16);
        if (!host.keys_loaded) {
            uint32_t probe;
            if (host.memory.read(addr::KEY_PAD_TABLE, &probe, 4)) { host.keys.load(host.memory); host.keys_loaded = true; }
        }
        if (++hook_check % 60 == 0) ensure_text_hook();
        mod.frame();
        host.probe_output();
        bool focus = game_focused();
        host.keys.apply(focus ? host.want : 0);
        if (!focus) { std::fill(std::begin(was), std::end(was), false); continue; }
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (alt) continue;
        auto pressed = [&](int vk) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool edge = down && !was[vk];
            was[vk] = down;
            return edge;
        };
        // speech settings
        // F1: next SAPI voice; Shift F1: speech output (automatic, a screen reader, SAPI).
        // Not Ctrl: Ctrl opens the game's own options menu.
        if (pressed(VK_F1)) { if (shift) host.cycle_output(); else host.speech.next_voice(); }
        if (pressed(VK_F2)) { if (shift) host.cycle_ducking_level(); else host.toggle_ducking(); }
        // during the narrated introduction, Shift F3 and F4 set its descriptions' volume
        if (pressed(VK_F3)) { if (shift) { if (host.intro && host.audio_ok) host.change_intro_volume(-10); else host.speech.change_volume(-10); } else host.speech.change_rate(-1); }
        if (pressed(VK_F4)) { if (shift) { if (host.intro && host.audio_ok) host.change_intro_volume(10); else host.speech.change_volume(10); } else host.speech.change_rate(1); }
        // mod hotkeys
        std::vector<int> seen;
        for (const auto& b : kBinds) {
            bool already = false;
            for (int v : seen) if (v == b.vk) already = true;
            bool edge = false;
            if (!already) {
                bool down = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
                edge = down && !was[b.vk];
                was[b.vk] = down;
                seen.push_back(b.vk);
                if (edge) {
                    // pick the binding that matches the Shift state
                    Hotkey hk = HK_NONE;
                    for (const auto& c : kBinds) if (c.vk == b.vk && c.shift == shift) hk = c.hk;
                    if (hk == HK_NONE) for (const auto& c : kBinds) if (c.vk == b.vk && !c.shift) hk = c.hk;
                    if (hk != HK_NONE) mod.hotkey(hk);
                    if (hk == HK_DIAG) save_diag_picture();
                }
            }
        }
    }
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        std::string p = path;
        g_dir = p.substr(0, p.find_last_of("\\/"));
        g_ini = g_dir + "\\RE2Access.ini";
        g_log = std::fopen((g_dir + "\\RE2Access.log").c_str(), "w");
        g_log_copy = ini_str("log_copy_dir", "");
        if (!g_log_copy.empty()) {
            CreateDirectoryA(g_log_copy.c_str(), nullptr);
            g_log2 = std::fopen((g_log_copy + "\\RE2Access.log").c_str(), "w");
        }
        log_line(std::string("RE2Access ") + RE2ACCESS_VERSION + " starting in " + g_dir);
        CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}

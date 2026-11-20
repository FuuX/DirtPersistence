// DirtPersistence v1.0.0 - visual dirt and mud retention for Forza Horizon 2.
// Source: https://github.com/FuuX/DirtPersistence
// Requires fh2mod.h and the FH2 mod platform.
// No modifications are made to the original EXE.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fh2mod.h"

#define MOD_VERSION "1.0.0"
#define RVA_SET_DIRT 0x13A9C40
#define RVA_SET_MUD  0x13AB9D0
#define CAR_SLOTS 2048

static const unsigned char kSetDirtBytes[] = "\xC5\xFA\x11\x89\x1C\x13\x00\x00\xC3";
static const unsigned char kSetMudBytes[]  = "\xC5\xFA\x11\x89\x20\x13\x00\x00\xC3";
static const FH2ModApi *g_api;
static FH2Mod *g_mod;
static unsigned char *g_game;
static char g_ini[MAX_PATH];
static volatile LONG g_slow = 1;
static volatile LONG g_permanent = 0;
static SRWLOCK g_lock = SRWLOCK_INIT;

typedef struct DirtState {
    void *car;
    float raw[2];
    float shown[2];
    unsigned char valid[2];
    unsigned long long last_seen;
} DirtState;
static DirtState g_car[CAR_SLOTS];
static unsigned long long g_tick;

typedef void (*SetAmountFn)(void *, float);
static SetAmountFn g_orig_set_dirt, g_orig_set_mud;

// Formats a diagnostic message and forwards it to the mod platform logger.
static void log_msg(const char *fmt, ...) {
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_api->log(g_mod, buf);
}

// Sanitizes non-finite values and constrains visual coverage to the range [0, 1].
static float clamp01(float x) {
    // Avoid a corrupt NaN propagating to the shader.
    if (!isfinite(x)) return 0.0f;
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

// Finds or allocates a vehicle tracking entry; evicts the least recently seen entry when full.
// Must be called while holding the tracking lock.
static DirtState *find_car(void *self) {
    unsigned i, victim = 0;
    unsigned long long oldest = ~(unsigned long long)0;
    for (i = 0; i < CAR_SLOTS; ++i) {
        if (g_car[i].car == self) return &g_car[i];
        if (!g_car[i].car) { victim = i; oldest = 0; break; }
        if (g_car[i].last_seen < oldest) {
            oldest = g_car[i].last_seen;
            victim = i;
        }
    }
    memset(&g_car[victim], 0, sizeof(g_car[victim]));
    g_car[victim].car = self;
    return &g_car[victim];
}

// Applies the selected retention mode independently to dirt and mud.
// Permanent mode retains peak coverage; reduced-fade mode scales decreases by 0.1.
static float transform_amount(void *self, float raw, int channel) {
    int slow = (InterlockedCompareExchange(&g_slow, 0, 0) != 0);
    int permanent = (InterlockedCompareExchange(&g_permanent, 0, 0) != 0);
    float shown;
    raw = clamp01(raw);
    AcquireSRWLockExclusive(&g_lock);
    DirtState *s = find_car(self);
    s->last_seen = ++g_tick;
    if (!s->valid[channel]) {
        s->raw[channel] = raw;
        s->shown[channel] = raw;
        s->valid[channel] = 1;
    } else if (permanent) {
        // Keep the greatest displayed coverage seen for this car and channel.
        // A drop in raw does not lower it; a bigger fresh splash can raise it.
        if (raw > s->shown[channel]) s->shown[channel] = raw;
        s->raw[channel] = raw;
    } else if (raw >= s->raw[channel]) {
        // Accumulation immediately follows incoming simulated dirt.
        s->shown[channel] = clamp01(s->shown[channel] + raw - s->raw[channel]);
        s->raw[channel] = raw;
    } else {
        // Only remove one tenth of the game's incremental reduction.
        if (slow) s->shown[channel] = clamp01(s->shown[channel] - (s->raw[channel] - raw) * 0.1f);
        else s->shown[channel] = raw;
        s->raw[channel] = raw;
    }
    shown = (permanent || slow) ? s->shown[channel] : raw;
    ReleaseSRWLockExclusive(&g_lock);
    return shown;
}

// Intercepts dirt coverage updates and forwards the adjusted value to the original function.
static void detour_set_dirt(void *self, float amount) {
    g_orig_set_dirt(self, transform_amount(self, amount, 0));
}
// Intercepts mud coverage updates and forwards the adjusted value to the original function.
static void detour_set_mud(void *self, float amount) {
    g_orig_set_mud(self, transform_amount(self, amount, 1));
}

// Verifies the expected machine-code signature before installing a hook.
static int check_bytes(unsigned rva, const unsigned char *expected, size_t size) {
    return memcmp(g_game + rva, expected, size) == 0;
}

// Waits for game code, validates both target signatures, and installs the render-side hooks.
static DWORD WINAPI hook_thread(void *unused) {
    (void)unused;
    g_game = (unsigned char *)g_api->wait_for_code(60000);
    if (!g_game) { log_msg("wait_for_code timed out"); return 0; }
    if (!check_bytes(RVA_SET_DIRT, kSetDirtBytes, sizeof(kSetDirtBytes)-1) ||
        !check_bytes(RVA_SET_MUD, kSetMudBytes, sizeof(kSetMudBytes)-1)) {
        log_msg("EXE byte mismatch; hooks NOT installed");
        return 0;
    }
    FH2Hook *h1 = g_api->hook_function(g_mod, "CarPresentation::SetDirtAmount",
        g_game + RVA_SET_DIRT, (void *)detour_set_dirt, (void **)&g_orig_set_dirt);
    FH2Hook *h2 = g_api->hook_function(g_mod, "CarPresentation::SetMudAmount",
        g_game + RVA_SET_MUD, (void *)detour_set_mud, (void **)&g_orig_set_mud);
    if (!h1 || !h2) { log_msg("hook installation failed"); return 0; }
    if (!g_api->enable_hooks(g_mod, 0)) { log_msg("enable_hooks failed"); return 0; }
    log_msg("DirtPersistence hooks installed; slow=%ld permanent=%ld", g_slow, g_permanent);
    return 0;
}

// Clears all in-memory coverage history when a retention setting changes.
static void reset_tracking(void) {
    AcquireSRWLockExclusive(&g_lock);
    memset(g_car, 0, sizeof(g_car));
    g_tick = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

// Applies setting changes, persists configuration to the INI file, and resets tracking.
static void on_change(void *context, const char *key, int value) {
    (void)context;
    if (!key) return;
    if (!strcmp(key, "SlowDirt")) {
        InterlockedExchange(&g_slow, value ? 1 : 0);
        WritePrivateProfileStringA("settings", "SlowDirt", value ? "1" : "0", g_ini);
        reset_tracking();
    } else if (!strcmp(key, "PermanentDirt")) {
        InterlockedExchange(&g_permanent, value ? 1 : 0);
        WritePrivateProfileStringA("settings", "PermanentDirt", value ? "1" : "0", g_ini);
        reset_tracking();
    }
}

// Registers mod metadata and settings, loads the INI configuration, and starts hook setup.
FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api) {
    g_api = api;
    FH2ModInfo info = {};
    info.size = sizeof(info);
    info.name = "DirtPersistence";
    info.version = MOD_VERSION;
    info.platform_version = api->platform_version;
    info.github_repo = "FuuX/DirtPersistence";
    info.check_updates = 1;
    info.description = "Extends visual dirt and mud retention on vehicles. Choose reduced fading or retain peak visible coverage during gameplay.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 0;

    HMODULE self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&fh2mod_init, &self)) return 0;
    GetModuleFileNameA(self, g_ini, MAX_PATH);
    char *dot = strrchr(g_ini, '.');
    if (dot) strcpy(dot, ".ini");
    g_slow = GetPrivateProfileIntA("settings", "SlowDirt", 1, g_ini) ? 1 : 0;
    g_permanent = GetPrivateProfileIntA("settings", "PermanentDirt", 0, g_ini) ? 1 : 0;

    FH2Setting a = {};
    a.size = sizeof(a);
    a.key = "SlowDirt";
    a.label = "Reduced Dirt and Mud Fade (10x)";
    a.description = "Reduces each simulated dirt and mud decrease to 10% of its normal value while preserving accumulation. Permanent Dirt takes priority when enabled.";
    a.value = (int)g_slow;
    a.on_change = on_change;
    api->add_setting(g_mod, &a);

    FH2Setting b = {};
    b.size = sizeof(b);
    b.key = "PermanentDirt";
    b.label = "Permanent Dirt and Mud";
    b.description = "Retains the highest visible dirt and mud coverage recorded for each tracked vehicle. Coverage may reset after vehicle changes, loading, or setting changes; it is not saved to disk.";
    b.value = (int)g_permanent;
    b.on_change = on_change;
    api->add_setting(g_mod, &b);

    HANDLE t = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    log_msg("DirtPersistence %s initialized", MOD_VERSION);
    return 0;
}

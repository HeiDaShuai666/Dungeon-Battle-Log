// town/menupanel.cpp -- THE TOWN MENU PANEL

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- TOWN MENU PANEL: state ----
bool g_tmnActive = false;
static int   g_tmnRow    = 0;
static int   g_tmnScreen = 0;           // TMN_SCREEN_* the menu was entered on
static DWORD g_tmnWatchUntil = 0;       // a clicked row's outcome deadline (0 = no watch)
static char  g_tmnWatchName[96];
static const DWORD TMN_WATCH_MS = 1500;

enum { TMN_SCREEN_NONE = 0, TMN_SCREEN_HAMLET, TMN_SCREEN_QUEST, TMN_SCREEN_PROV };

static const uint32_t TMN_OWNER_NAVI = 0x6e617669;   // 'navi' -- the estate bar + the forward slot
static const uint32_t TMN_ID_FWRD    = 0x66777264;
static const uint32_t TMN_ID_RINV    = 0x72696e76;   // 'rinv' -- the trinket (realm) inventory
static const uint32_t TMN_ID_GLOS    = 0x676c6f73;   // 'glos' -- the glossary, TogglePanel(6)
static const uint32_t TMN_ID_ALOG    = 0x616c6f67;   // 'alog' -- the activity log, TogglePanel(2)
static const uint32_t TMN_ID_TEVT    = 0x74657674;   // 'tevt' -- the town event, TogglePanel(1)
static const uint32_t TMN_ID_DDIS    = 0x64646973;   // 'ddis' -- districts, TogglePanel(3)
static const uint32_t TMN_ID_EXCH    = 0x65786368;   // 'exch' -- the heirloom exchange, TogglePanel(0)
static const uint32_t TMN_ID_STNG    = 0x73746e67;   // 'stng' -- the settings cog (the game menu)

// ---- TOWN MENU PANEL: the name table ----
struct TmnName {
    uint32_t    id;
    const char* key;
    AxStrId     fb;
    const char* fnId;
    uint32_t    vanillaSym;
};
static const TmnName kTmnNames[] = {
    { TMN_ID_RINV, "town_name_realm_inventory",        AXS_TMN_FB_TRINKETS,    "hamlet.trinket_inv",  0      },
    { TMN_ID_GLOS, "town_name_glossary",               AXS_TMN_FB_GLOSSARY,    nullptr,               SDLK_g },
    { TMN_ID_ALOG, "town_name_activity_log",           AXS_TMN_FB_ACTIVITY,    "hamlet.activity_log", 0      },
    { TMN_ID_TEVT, "menu_options_element_town_events", AXS_TMN_FB_TOWN_EVENTS, "hamlet.town_event",   0      },
    { TMN_ID_STNG, nullptr,                            AXS_TMN_SETTINGS,       "general.back",        0      },
};

static const uint32_t kTmnElsewhere[] = { TMN_ID_EXCH, TMN_ID_DDIS };

static const TmnName* tmnNameFor(uint32_t id) {
    for (const TmnName& n : kTmnNames) if (n.id == id) return &n;
    return nullptr;
}

static void tmnLabel(const TmnName* nm, const char* name, char* out, int outsz) {
    if (nm->fnId) { kmLabelWithKey(name, nm->fnId, 0, out, outsz); return; }
    const char* fmt = axs(AXS_TM_ROW_KEY_FMT);
    if (nm->vanillaSym && fmt[0]) {
        char key[64];
        KmChord c = { nm->vanillaSym, 0 };
        kmChordName(c, key, sizeof key);
        if (key[0]) { _snprintf(out, outsz, fmt, name, key); out[outsz - 1] = 0; return; }
    }
    _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;
}

// ---- TOWN MENU PANEL: which screen ----
static int tmnScreenNow(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root || circusScreenUp(base) || csTownSheetPanel(base)) return TMN_SCREEN_NONE;
    if (embProvIsOpen(root)) return TMN_SCREEN_PROV;
    if (embQsOpen(root))     return TMN_SCREEN_QUEST;
    if (tmbBareMap(root))    return TMN_SCREEN_HAMLET;
    return TMN_SCREEN_NONE;
}

// ---- TOWN MENU PANEL: the rows ----
static const int TMN_MAX_ROWS = 16;
static const int TMN_LABEL_SZ = 128;
struct TmnRow {
    uint32_t id;
    char     name[96];                  // the bare name (logs, the watch's line)
    char     label[TMN_LABEL_SZ];
};

static int tmnCollect(uintptr_t base, TmnRow* rows, int maxRows) {
    uintptr_t el[TMN_MAX_ROWS];
    int64_t   ids[TMN_MAX_ROWS];
    int found = feCollectByOwner(base, TMN_OWNER_NAVI, el, ids, nullptr, TMN_MAX_ROWS);
    int n = 0;
    // The forward button, if it is drawn this frame. Its label is the game's picker's.
    for (int i = 0; i < found; i++) {
        if ((uint32_t)ids[i] != TMN_ID_FWRD || n >= maxRows) continue;
        TmnRow& r = rows[n++];
        r.id = TMN_ID_FWRD;
        embFwdLabel(base, r.name, sizeof r.name);
        kmLabelWithKey(r.name, "hamlet.forward", 0, r.label, sizeof r.label);
        break;
    }
    for (int i = 0; i < found && n < maxRows; i++) {
        uint32_t id = (uint32_t)ids[i];
        if (id == TMN_ID_FWRD) continue;
        bool elsewhere = false;
        for (uint32_t e : kTmnElsewhere) if (e == id) elsewhere = true;
        if (elsewhere) continue;
        const TmnName* nm = tmnNameFor(id);
        if (!nm) {
            static uint32_t s_saidFor[8] = {};  // one log line per unknown id, not per keypress
            bool said = false;
            for (uint32_t s : s_saidFor) if (s == id) said = true;
            if (!said) {
                for (uint32_t& s : s_saidFor) if (!s) { s = id; break; }
                char tag[9]; idToAscii((int64_t)id, tag);
                logLine("townmenu: unknown 'navi' button 0x%08x \"%s\" left out (no name for it)", id, tag);
            }
            continue;
        }
        TmnRow& r = rows[n++];
        r.id = id;
        r.name[0] = 0;
        if (nm->key) {
            char raw[96];
            if (resolveKey(base, nm->key, raw, sizeof raw)) stripMarkup(raw, r.name, sizeof r.name);
            else logLine("townmenu: \"%s\" did not resolve -> the fallback", nm->key);
        }
        if (!r.name[0]) {
            strncpy(r.name, axs(nm->fb), sizeof r.name - 1);
            r.name[sizeof r.name - 1] = 0;
        }
        tmnLabel(nm, r.name, r.label, sizeof r.label);
    }
    return n;
}

static void tmnSpeakRow(uintptr_t base, const char* prefix) {
    TmnRow rows[TMN_MAX_ROWS];
    int n = tmnCollect(base, rows, TMN_MAX_ROWS);
    if (n <= 0) { postSpeech(axs(AXS_TMN_NONE)); return; }
    axStepCursor(&g_tmnRow, n, 0);              // re-clamp against the live bar
    char pos[48];
    pos[0] = ' ';
    _snprintf(pos + 1, sizeof pos - 1, axs(AXS_POS_N_OF_M), g_tmnRow + 1, n);
    pos[sizeof pos - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s%s.%s", prefix ? prefix : "", prefix ? " " : "",
              rows[g_tmnRow].label, pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

bool axIsTownMenu() { return g_tmnActive; }

// ---- TOWN MENU PANEL: enter / leave ----
bool tmnEnter(uintptr_t base) {
    int screen = tmnScreenNow(base);
    if (screen == TMN_SCREEN_NONE) return false;
    TmnRow rows[TMN_MAX_ROWS];
    int n = tmnCollect(base, rows, TMN_MAX_ROWS);
    if (n <= 0) {
        logLine("townmenu: screen %d has no 'navi' buttons drawn -> no menu stop", screen);
        return false;
    }
    g_tmnActive = true;
    g_resActive = false;                         // one town layer at a time
    g_ptyActive = false;
    g_rosActive = false;
    g_tmnScreen = screen;
    g_tmnRow    = 0;                             // a fresh entry lands on the top row (Embark)
    logLine("townmenu: active on screen %d, %d buttons", screen, n);
    tmnSpeakRow(base, axs(AXS_TMN_TITLE));
    return true;
}

void tmnLeave(uintptr_t base, const char* why) {
    (void)base;
    if (!g_tmnActive) return;
    g_tmnActive = false;
    logLine("townmenu: %s -> stood down to the surface underneath", why);
}

void tmnReannounce(uintptr_t base) {
    if (!g_tmnActive) return;
    logLine("townmenu: re-announcing after a modal closed");
    tmnSpeakRow(base, axs(AXS_TMN_TITLE));
}

// ---- TOWN MENU PANEL: activation ----
static void tmnArmWatch(const TmnRow& r) {
    g_tmnWatchUntil = GetTickCount() + TMN_WATCH_MS;
    strncpy(g_tmnWatchName, r.name, sizeof g_tmnWatchName - 1);
    g_tmnWatchName[sizeof g_tmnWatchName - 1] = 0;
}

static void tmnActivate(uintptr_t base, const TmnRow& r) {
    char tag[9]; idToAscii((int64_t)r.id, tag);
    logLine("townmenu: Enter on \"%s\" ('%s')", r.name, tag);
    switch (r.id) {
        case TMN_ID_FWRD:
            routeEmbarkForward(base, 0);         // E's handler: its own watch, label and refusals
            return;
        case TMN_ID_RINV:
            routeRealmInvToggle(base, 0);        // I's handler; AX_REALMINV announces the open edge
            return;
        case TMN_ID_ALOG:
            routeTownLogToggle(base, SDLK_PERIOD, 0);   // period's handler
            return;
        case TMN_ID_TEVT:
            routeTownEventToggle(base, 0);       // Shift+period's handler
            return;
    }
    // Every other button: click it, the way the mouse does.
    if (!frontEndClickElementId((int64_t)r.id)) {
        char utter[160];
        _snprintf(utter, sizeof utter, axs(AXS_EMB_FWD_UNAVAILABLE_FMT), r.name);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    tmnArmWatch(r);
}

// ---- TOWN MENU PANEL: keys ----
bool routeTownMenuKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        tmnLeave(base, "Escape");
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        TmnRow rows[TMN_MAX_ROWS];
        int n = tmnCollect(base, rows, TMN_MAX_ROWS);
        if (n <= 0) { postSpeech(axs(AXS_TMN_NONE)); return true; }
        axStepCursor(&g_tmnRow, n, 0);
        tmnActivate(base, rows[g_tmnRow]);
        return true;
    }

    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                   // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) return false;
    if (!dRow && !dCol) return true;              // a throttled held repeat: claimed, no step
    TmnRow rows[TMN_MAX_ROWS];
    int n = tmnCollect(base, rows, TMN_MAX_ROWS);
    if (n <= 0) { postSpeech(axs(AXS_TMN_NONE)); return true; }
    axStepCursor(&g_tmnRow, n, dRow + dCol);      // one strip: either axis is the step
    tmnSpeakRow(base, nullptr);
    return true;
}

// ---- TOWN MENU PANEL: Find enumerator (Ctrl+F, core/find.cpp) ----
int tmnFind(uintptr_t base, AxFindOp op, int idx, char* out, int outsz) {
    if (!g_tmnActive) return 0;
    TmnRow rows[TMN_MAX_ROWS];
    int n = tmnCollect(base, rows, TMN_MAX_ROWS);
    if (op == AXF_COUNT) return n;
    if (idx < 0 || idx >= n) return 0;
    if (op == AXF_LABEL) {
        if (!out || outsz <= 0) return 0;
        _snprintf(out, outsz, "%s.", rows[idx].label);
        out[outsz - 1] = 0;
        return 1;
    }
    if (op == AXF_GO) {
        g_tmnRow = idx;
        logLine("townmenu: find landed on row %d", idx);
        tmnSpeakRow(base, nullptr);
        return 1;
    }
    return 0;
}

// ---- TOWN MENU PANEL: per frame ----
void checkTownMenu(uintptr_t base) {
    if (g_tmnActive) {
        if (g_resActive || g_ptyActive || g_rosActive) {
            g_tmnActive = false;
            logLine("townmenu: another town layer took over -> stood down");
        } else if (tmnScreenNow(base) != g_tmnScreen) {
            g_tmnActive = false;
            logLine("townmenu: the screen changed (%d -> %d) -> stood down", g_tmnScreen, tmnScreenNow(base));
        }
    }
    if (g_tmnWatchUntil) {
        if (!resTownRoot(base)) { g_tmnWatchUntil = 0; return; }   // town went away: nothing to say
        AxContext c = currentAxContext();
        if (c != AX_TOWNMENU) {
            g_tmnWatchUntil = 0;
            logLine("townmenu: \"%s\" opened (context %d)", g_tmnWatchName, (int)c);
        } else if (GetTickCount() > g_tmnWatchUntil) {
            g_tmnWatchUntil = 0;
            logLine("townmenu: click on \"%s\" made no observable change", g_tmnWatchName);
            postSpeech(axs(AXS_NOTHING_HAPPENED));
        }
    }
}

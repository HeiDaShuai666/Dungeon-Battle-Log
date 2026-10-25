// core/find.cpp -- FIND (Ctrl+F)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "internal.h"

// ---- State ----
static const int FD_MAX_ITEMS = 8192;
static const int FD_LABEL_SZ  = 1536;
static const int FD_QUERY_MAX = 96;
                                        // (was 32 while the query could only hold ASCII)

bool g_fdActive = false;
static AxFindFn g_fdFn = nullptr;          // the surface's enumerator, captured at open
static char     g_fdWho[32];               // its table name, for the log
static int      g_fdCount = 0;             // items in the snapshot
static std::vector<char> g_fdPool;
static uint32_t g_fdLabelOff[FD_MAX_ITEMS];   // pool offset of item i as spoken
static uint32_t g_fdLowerOff[FD_MAX_ITEMS];   // pool offset of item i case-folded, for the match
static const char* fdLabel(int i) { return &g_fdPool[g_fdLabelOff[i]]; }
static const char* fdLower(int i) { return &g_fdPool[g_fdLowerOff[i]]; }
static int      g_fdSrc[FD_MAX_ITEMS];
                                           // with no label is skipped, so the two can differ)
static int      g_fdMatch[FD_MAX_ITEMS];
static int      g_fdMatches = 0;
static int      g_fdRow = 0;               // cursor over g_fdMatch
static char     g_fdQuery[FD_QUERY_MAX + 1];
static int      g_fdLen = 0;
static int      g_fdFnSayAgain = -2, g_fdFnFind = -2;   // keymap indices, resolved once

bool axIsFind() { return g_fdActive; }

// ---- SDL text input ----
typedef void (__cdecl *FdSdlVoidFn)(void);
typedef int  (__cdecl *FdSdlBoolFn)(void);              // SDL_bool is an int-sized enum
static FdSdlVoidFn g_fdSdlStart  = nullptr;              // SDL_StartTextInput (the REAL export)
static FdSdlVoidFn g_fdSdlStop   = nullptr;              // SDL_StopTextInput
static FdSdlBoolFn g_fdSdlActive = nullptr;              // SDL_IsTextInputActive
static bool        g_fdSdlTried  = false;                // resolve attempted (success or not)
static bool        g_fdTextMode  = false;                // this session types from SDL_TEXTINPUT
static bool        g_fdTextWasOn = false;                // text input was on at open: leave it on
static bool        g_fdEatText   = false;                // the last KEYDOWN was a command ...
static DWORD       g_fdEatTick   = 0;                    // ... pressed at this tick
static const DWORD FD_EAT_MS     = 100;                  // a key's own TEXTINPUT is in the same
                                                         // pump; anything later is not its text
bool g_fdKeyFolded = false;                              // set by the router per KEYDOWN (internal.h)

static void fdResolveSdl() {
    if (g_fdSdlTried) return;
    g_fdSdlTried = true;
    HMODULE sdl = GetModuleHandleA("SDL2.dll");
    if (sdl) {
        g_fdSdlStart  = (FdSdlVoidFn)GetProcAddress(sdl, "SDL_StartTextInput");
        g_fdSdlStop   = (FdSdlVoidFn)GetProcAddress(sdl, "SDL_StopTextInput");
        g_fdSdlActive = (FdSdlBoolFn)GetProcAddress(sdl, "SDL_IsTextInputActive");
    }
    logLine("find: SDL text input %s (module=%d start=%d stop=%d active=%d)",
            (g_fdSdlStart && g_fdSdlStop && g_fdSdlActive) ? "resolved" : "UNAVAILABLE -- keysym typing",
            sdl ? 1 : 0, g_fdSdlStart ? 1 : 0, g_fdSdlStop ? 1 : 0, g_fdSdlActive ? 1 : 0);
}

static void fdTextBegin() {
    fdResolveSdl();
    g_fdTextMode = false;
    g_fdEatText  = false;
    if (!g_fdSdlStart || !g_fdSdlStop || !g_fdSdlActive) return;   // fallback: keysym typing
    g_fdTextWasOn = g_fdSdlActive() != 0;
    if (!g_fdTextWasOn) g_fdSdlStart();
    g_fdTextMode = true;
    logLine("find: text input %s", g_fdTextWasOn ? "was already on (left as found)" : "started");
}

static void fdTextEnd() {
    if (g_fdTextMode && !g_fdTextWasOn && g_fdSdlStop) {
        g_fdSdlStop();
        logLine("find: text input stopped (restored)");
    }
    g_fdTextMode = false;
    g_fdEatText  = false;
}

// ---- UTF-8 helpers ----
static void fdFoldCase(char* s, size_t cap) {
    wchar_t w[FD_LABEL_SZ];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, FD_LABEL_SZ);
    if (n <= 0) {
        for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
        return;
    }
    w[FD_LABEL_SZ - 1] = 0;
    CharLowerBuffW(w, (DWORD)wcslen(w));
    char t[FD_LABEL_SZ * 2];
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, t, (int)sizeof t, nullptr, nullptr);
    if (m <= 0) return;                                   // leave the original: it still matches itself
    size_t len = strlen(t);
    if (len > cap - 1) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)t[len] & 0xC0) == 0x80) len--;   // never split a character
    }
    memcpy(s, t, len);
    s[len] = 0;
}

static void fdFilter() {
    char q[FD_QUERY_MAX + 1];
    strncpy(q, g_fdQuery, sizeof q - 1); q[sizeof q - 1] = 0;
    fdFoldCase(q, sizeof q);
    g_fdMatches = 0;
    for (int i = 0; i < g_fdCount; i++)
        if (!q[0] || strstr(fdLower(i), q)) g_fdMatch[g_fdMatches++] = i;
    g_fdRow = 0;
}

static void fdSpeak(const char* head) {
    char utter[MAILBOX_SZ];
    if (g_fdMatches <= 0 || g_fdRow < 0 || g_fdRow >= g_fdMatches) {
        _snprintf(utter, sizeof utter, "%s", head ? head : "");
    } else {
        char posln[64];
        _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), g_fdRow + 1, g_fdMatches);
        posln[sizeof posln - 1] = 0;
        _snprintf(utter, sizeof utter, "%s%s%s %s", head ? head : "", (head && head[0]) ? " " : "",
                  fdLabel(g_fdMatch[g_fdRow]), posln);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void fdSpeakQuery() {
    char head[FD_QUERY_MAX + 128];   // the query + a translated "Results: N."
    if (!g_fdLen)             _snprintf(head, sizeof head, axs(AXS_FD_CLEARED_FMT), g_fdCount);
    else if (g_fdMatches > 0) _snprintf(head, sizeof head, axs(AXS_FD_TYPED_FMT), g_fdQuery, g_fdMatches);
    else                      _snprintf(head, sizeof head, axs(AXS_FD_TYPED_NONE_FMT), g_fdQuery);
    head[sizeof head - 1] = 0;
    fdSpeak(head);
}

void fdReannounce(uintptr_t base) {
    (void)base;
    if (!g_fdActive) return;
    if (g_fdLen) { fdSpeakQuery(); return; }
    char head[160];
    _snprintf(head, sizeof head, axs(AXS_FD_OPEN_FMT), g_fdCount);
    head[sizeof head - 1] = 0;
    axAppendHint(head, sizeof head, AXS_FD_OPEN_HINT);   // "" while the F10 "Control hints" row is Off
    fdSpeak(head);
}

// ---- Open / close ----
bool g_fdComposingLabel = false;

void fdOpen(uintptr_t base, AxFindFn fn, const char* who) {
    if (g_fdActive || !fn) return;
    if (!who) who = "?";
    int n = fn(base, AXF_COUNT, 0, nullptr, 0);
    if (n > FD_MAX_ITEMS) {
        logLine("find: \"%s\" offers %d items, snapshot capped at %d", who, n, FD_MAX_ITEMS);
        n = FD_MAX_ITEMS;
    }
    if (n <= 0) {
        logLine("find: \"%s\" has nothing to search (count %d)", who, n);
        postSpeech(axs(AXS_FD_EMPTY));
        return;
    }
    g_fdCount = 0;
    g_fdPool.clear();
    g_fdComposingLabel = true;
    static char label[FD_LABEL_SZ], lower[FD_LABEL_SZ];   // composing buffers (main thread only)
    for (int i = 0; i < n; i++) {
        label[0] = 0;
        if (!fn(base, AXF_LABEL, i, label, FD_LABEL_SZ) || !label[0]) {
            logLine("find: \"%s\" item %d has no label, skipped", who, i);
            continue;
        }
        label[FD_LABEL_SZ - 1] = 0;
        strncpy(lower, label, FD_LABEL_SZ);
        lower[FD_LABEL_SZ - 1] = 0;
        fdFoldCase(lower, FD_LABEL_SZ);
        size_t ll = strlen(label) + 1, lw = strlen(lower) + 1;
        g_fdLabelOff[g_fdCount] = (uint32_t)g_fdPool.size();
        g_fdPool.insert(g_fdPool.end(), label, label + ll);
        g_fdLowerOff[g_fdCount] = (uint32_t)g_fdPool.size();
        g_fdPool.insert(g_fdPool.end(), lower, lower + lw);
        g_fdSrc[g_fdCount] = i;
        g_fdCount++;
    }
    g_fdComposingLabel = false;
    if (g_fdCount <= 0) {
        logLine("find: \"%s\" offered %d items and none had a label", who, n);
        postSpeech(axs(AXS_FD_EMPTY));
        return;
    }
    g_fdFn = fn;
    strncpy(g_fdWho, who, sizeof g_fdWho - 1); g_fdWho[sizeof g_fdWho - 1] = 0;
    g_fdQuery[0] = 0; g_fdLen = 0;
    fdFilter();
    g_fdActive = true;
    fdTextBegin();
    logLine("find: opened over \"%s\", %d items", g_fdWho, g_fdCount);
    fdReannounce(base);
}

static void fdCancel(uintptr_t base) {
    g_fdActive = false;
    fdTextEnd();
    logLine("find: cancelled (query \"%s\")", g_fdQuery);
    axModalClosed(base, "find");
    postSpeech(axs(AXS_CANCELLED), /*interrupt=*/false, SPK_EVENT);
}

static void fdCommit(uintptr_t base) {
    if (g_fdMatches <= 0) { postSpeech(axs(AXS_FD_NO_RESULTS)); return; }
    if (g_fdRow < 0) g_fdRow = 0;
    if (g_fdRow >= g_fdMatches) g_fdRow = g_fdMatches - 1;
    int pos = g_fdMatch[g_fdRow];
    int idx = g_fdSrc[pos];
    g_fdActive = false;
    fdTextEnd();
    logLine("find: Enter -> \"%s\" item %d (\"%.60s\")", g_fdWho, idx, fdLabel(pos));
    if (!g_fdFn(base, AXF_GO, idx, nullptr, 0)) {
        logLine("find: \"%s\" refused item %d -- handing back generically", g_fdWho, idx);
        if (!axModalClosed(base, "find")) postSpeech(axs(AXS_CANCELLED));
    }
}

// ---- Typing ----
static void fdType(const char* text) {
    size_t len  = strlen(text);
    size_t room = (size_t)(FD_QUERY_MAX - g_fdLen);
    size_t take = len < room ? len : room;
    while (take > 0 && take < len && ((unsigned char)text[take] & 0xC0) == 0x80) take--;
    if (!take) { fdSpeakQuery(); return; }
    memcpy(g_fdQuery + g_fdLen, text, take);
    g_fdLen += (int)take;
    g_fdQuery[g_fdLen] = 0;
    fdFilter();
    logLine("find: \"%s\" -> %d of %d", g_fdQuery, g_fdMatches, g_fdCount);
    fdSpeakQuery();
}

bool routeFindText(uintptr_t base, void* ev, uint32_t type) {
    (void)base;
    if (!g_fdActive) return false;
    if (type != SDL_EVT_TEXTINPUT || !g_fdTextMode) return true;
    if (g_fdEatText) {
        g_fdEatText = false;
        if ((DWORD)(GetTickCount() - g_fdEatTick) <= FD_EAT_MS) return true;   // a command key's text
    }
    char text[33]; text[0] = 0;
    safeReadCStr((uintptr_t)ev + SDL_TEXTINPUT_TEXT, text, sizeof text);
    if (!text[0]) return true;
    fdType(text);
    return true;
}

// ---- Keys ----
bool routeFindKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!g_fdActive) return false;                   // belt-and-braces; the row's predicate gates
    if (g_fdFnSayAgain == -2) g_fdFnSayAgain = kmFind("general.say_again");
    if (g_fdFnFind == -2)     g_fdFnFind     = kmFind("general.find");
    const uint16_t CTRL = (uint16_t)(KMOD_LCTRL | KMOD_RCTRL);
    const uint16_t ALT  = (uint16_t)(KMOD_LALT | KMOD_RALT);
    g_fdEatText = true;
    g_fdEatTick = GetTickCount();
    bool sayAgain = g_fdFnSayAgain >= 0 ? kmIsCurrentKey(g_fdFnSayAgain, sym, mod) : (sym == SDLK_COMMA);
    bool findKey  = g_fdFnFind >= 0     ? kmIsCurrentKey(g_fdFnFind, sym, mod)
                                        : (sym == SDLK_f && (mod & CTRL) != 0);
    if (g_fdTextMode && g_fdKeyFolded && !(mod & (CTRL | ALT))) sayAgain = findKey = false;
    if (sayAgain || findKey) { if (!repeat) fdReannounce(base); return true; }
    if (sym == SDLK_ESCAPE) { if (!repeat) fdCancel(base); return true; }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { if (!repeat) fdCommit(base); return true; }

    int dRow = 0, dCol = 0;
    if (axDecodeArrow(sym, mod, repeat, false, &dRow, &dCol)) {
        if (!dRow) return true;                      // throttled auto-repeat: claimed, no step
        if (g_fdMatches <= 0) { postSpeech(axs(AXS_FD_NO_RESULTS)); return true; }
        axStepCursor(&g_fdRow, g_fdMatches, dRow);   // hard stop: the end row re-reads
        fdSpeak(nullptr);
        return true;
    }
    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                      // held jump: one landing per press
        if (g_fdMatches <= 0) { postSpeech(axs(AXS_FD_NO_RESULTS)); return true; }
        axStepCursor(&g_fdRow, g_fdMatches, jump);
        fdSpeak(nullptr);
        return true;
    }
    if (mod & (CTRL | ALT)) {
        if (!repeat) g_fdEatText = false;
        return true;
    }
    if (sym == SDLK_BACKSPACE) {
        if (repeat) return true;                     // one character per press
        if (!g_fdLen) { fdReannounce(base); return true; }   // nothing to erase: say where we are
        // One whole CHARACTER: step back over UTF-8 continuation bytes onto the lead byte.
        do { g_fdLen--; } while (g_fdLen > 0 && ((unsigned char)g_fdQuery[g_fdLen] & 0xC0) == 0x80);
        g_fdQuery[g_fdLen] = 0;
        fdFilter();
        fdSpeakQuery();
        return true;
    }
    if (g_fdTextMode) {
        if (!repeat) g_fdEatText = false;
        return true;
    }
    if (sym >= 0x20 && sym <= 0x7e) {
        if (repeat) return true;                     // a held key does not type (one per press)
        char one[2] = { (char)sym, 0 };
        fdType(one);
        return true;
    }
    return true;                                     // every other key: the modal swallows it
}

// core/find.cpp -- FIND (Ctrl+F)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

// ---- State ----
static const int FD_MAX_ITEMS = 512;
static const int FD_LABEL_SZ  = 384;
static const int FD_QUERY_MAX = 32;

bool g_fdActive = false;
static AxFindFn g_fdFn = nullptr;          // the surface's enumerator, captured at open
static char     g_fdWho[32];               // its table name, for the log
static int      g_fdCount = 0;             // items in the snapshot
static char     g_fdLabel[FD_MAX_ITEMS][FD_LABEL_SZ];   // as spoken
static char     g_fdLower[FD_MAX_ITEMS][FD_LABEL_SZ];   // ASCII-lowercased, for the match
static int      g_fdSrc[FD_MAX_ITEMS];
                                           // with no label is skipped, so the two can differ)
static int      g_fdMatch[FD_MAX_ITEMS];
static int      g_fdMatches = 0;
static int      g_fdRow = 0;               // cursor over g_fdMatch
static char     g_fdQuery[FD_QUERY_MAX + 1];
static int      g_fdLen = 0;
static int      g_fdFnSayAgain = -2, g_fdFnFind = -2;   // keymap indices, resolved once

bool axIsFind() { return g_fdActive; }

static void fdLowerAscii(char* s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static void fdFilter() {
    char q[FD_QUERY_MAX + 1];
    strncpy(q, g_fdQuery, sizeof q - 1); q[sizeof q - 1] = 0;
    fdLowerAscii(q);
    g_fdMatches = 0;
    for (int i = 0; i < g_fdCount; i++)
        if (!q[0] || strstr(g_fdLower[i], q)) g_fdMatch[g_fdMatches++] = i;
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
                  g_fdLabel[g_fdMatch[g_fdRow]], posln);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void fdSpeakQuery() {
    char head[160];
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
    g_fdComposingLabel = true;
    for (int i = 0; i < n; i++) {
        char* label = g_fdLabel[g_fdCount];
        label[0] = 0;
        if (!fn(base, AXF_LABEL, i, label, FD_LABEL_SZ) || !label[0]) {
            logLine("find: \"%s\" item %d has no label, skipped", who, i);
            continue;
        }
        label[FD_LABEL_SZ - 1] = 0;
        strncpy(g_fdLower[g_fdCount], label, FD_LABEL_SZ);
        g_fdLower[g_fdCount][FD_LABEL_SZ - 1] = 0;
        fdLowerAscii(g_fdLower[g_fdCount]);
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
    logLine("find: opened over \"%s\", %d items", g_fdWho, g_fdCount);
    fdReannounce(base);
}

static void fdCancel(uintptr_t base) {
    g_fdActive = false;
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
    logLine("find: Enter -> \"%s\" item %d (\"%.60s\")", g_fdWho, idx, g_fdLabel[pos]);
    if (!g_fdFn(base, AXF_GO, idx, nullptr, 0)) {
        logLine("find: \"%s\" refused item %d -- handing back generically", g_fdWho, idx);
        if (!axModalClosed(base, "find")) postSpeech(axs(AXS_CANCELLED));
    }
}

// ---- Keys ----
bool routeFindKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!g_fdActive) return false;                   // belt-and-braces; the row's predicate gates
    if (g_fdFnSayAgain == -2) g_fdFnSayAgain = kmFind("general.say_again");
    if (g_fdFnFind == -2)     g_fdFnFind     = kmFind("general.find");
    const uint16_t CTRL = (uint16_t)(KMOD_LCTRL | KMOD_RCTRL);
    const uint16_t ALT  = (uint16_t)(KMOD_LALT | KMOD_RALT);
    bool sayAgain = g_fdFnSayAgain >= 0 ? kmIsCurrentKey(g_fdFnSayAgain, sym, mod) : (sym == SDLK_COMMA);
    bool findKey  = g_fdFnFind >= 0     ? kmIsCurrentKey(g_fdFnFind, sym, mod)
                                        : (sym == SDLK_f && (mod & CTRL) != 0);
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
    if (mod & (CTRL | ALT)) return true;             // a chord: not typing, swallowed
    if (sym == SDLK_BACKSPACE) {
        if (repeat) return true;                     // one character per press
        if (!g_fdLen) { fdReannounce(base); return true; }   // nothing to erase: say where we are
        g_fdQuery[--g_fdLen] = 0;
        fdFilter();
        fdSpeakQuery();
        return true;
    }
    if (sym >= 0x20 && sym <= 0x7e) {
        if (repeat) return true;                     // a held key does not type (one per press)
        if (g_fdLen >= FD_QUERY_MAX) { fdSpeakQuery(); return true; }   // full: re-read, no beep
        g_fdQuery[g_fdLen++] = (char)sym;
        g_fdQuery[g_fdLen] = 0;
        fdFilter();
        logLine("find: \"%s\" -> %d of %d", g_fdQuery, g_fdMatches, g_fdCount);
        fdSpeakQuery();
        return true;
    }
    return true;                                     // every other key: the modal swallows it
}

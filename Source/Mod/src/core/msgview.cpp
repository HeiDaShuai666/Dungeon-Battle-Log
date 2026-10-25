// core/msgview.cpp -- THE POPUP READER

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- The shared scratch ----
static MvRows g_mvScratch;

MvRows* mvBegin() {
    g_mvScratch.n = 0;
    return &g_mvScratch;
}

static MvRowSlot* mvAddSlot(MvRows* r, int kind, int opt) {
    if (!r || r->n >= MV_MAX_ROWS) {
        static bool s_full = false;
        if (r && !s_full) { s_full = true; logLine("msgview: a popup has more than %d rows -- the rest are cut", MV_MAX_ROWS); }
        return nullptr;
    }
    MvRowSlot* s = &r->row[r->n++];
    s->kind = kind;
    s->opt  = opt;
    s->text[0] = 0;
    return s;
}

void mvTextLine(MvRows* r, const char* line) {
    if (!line || !line[0]) return;
    MvRowSlot* s = mvAddSlot(r, MV_TEXT, -1);
    if (!s) return;
    _snprintf(s->text, MV_ROW_SZ, "%s", line);
    s->text[MV_ROW_SZ - 1] = 0;
}

const char* mvSplitLine(const char* p, char* out, int outsz) {
    if (out && outsz > 0) out[0] = 0;
    if (!p || !*p) return p;
    const char* e = p;
    while (*e && *e != '\n') {
        if (e[0] == '<' && e[1] == 'c' && e[2] == '>') {
            e += 3;
            for (int k = 0; k < 6 && *e; k++) e++;
            continue;
        }
        e++;
    }
    char seg[MV_ROW_SZ];
    int len = (int)(e - p);
    if (len >= MV_ROW_SZ) len = MV_ROW_SZ - 1;
    memcpy(seg, p, (size_t)len);
    seg[len] = 0;
    if (out && outsz > 0) stripMarkup(seg, out, outsz);   // also trims, so " " reads empty
    return *e ? e + 1 : e;
}

// A RAW game string, split on its own line breaks; blank lines dropped.
void mvText(MvRows* r, const char* raw) {
    if (!raw || !raw[0]) return;
    char line[MV_ROW_SZ];
    const char* p = raw;
    while (*p) {
        p = mvSplitLine(p, line, sizeof line);
        if (line[0]) mvTextLine(r, line);
    }
}

void mvOption(MvRows* r, int opt, const char* line) {
    MvRowSlot* s = mvAddSlot(r, MV_OPT, opt);
    if (!s) return;
    _snprintf(s->text, MV_ROW_SZ, "%s", line && line[0] ? line : axs(AXS_MV_OPTION_FALLBACK));
    s->text[MV_ROW_SZ - 1] = 0;
}

void mvCloseLabel(char* out, int outsz) {
    char raw[128], word[128];
    word[0] = 0;
    if (resolveKey(g_base, "str_overlay_loot_close", raw, sizeof raw)) stripMarkup(raw, word, sizeof word);
    _snprintf(out, outsz, axs(AXS_MV_BUTTON_FMT), word[0] ? word : axs(AXS_MV_CLOSE));
    out[outsz - 1] = 0;
}

void mvClose(MvRows* r) {
    MvRowSlot* s = mvAddSlot(r, MV_CLOSE, -1);
    if (!s) return;
    mvCloseLabel(s->text, MV_ROW_SZ);
    axAppendHint(s->text, MV_ROW_SZ, AXS_ENTER_ACTIVATE_HINT);   // "" while Control hints is Off
}

int mvFirstNonText(const MvRows* r) {
    if (!r) return -1;
    for (int i = 0; i < r->n; i++) if (r->row[i].kind != MV_TEXT) return i;
    return -1;
}

int mvRowOfOption(const MvRows* r, int opt) {
    if (!r) return -1;
    for (int i = 0; i < r->n; i++) if (r->row[i].kind == MV_OPT && r->row[i].opt == opt) return i;
    return -1;
}

// ---- The cursor ----
void mvSync(MvCursor* c, uint32_t sig, const MvRows* r) {
    if (!c) return;
    if (c->sig != sig) {
        c->sig = sig;
        c->row = 0;
    }
    int n = r ? r->n : 0;
    if (c->row >= n) c->row = n - 1;
    if (c->row < 0)  c->row = 0;
}

void mvRowLine(const MvRows* r, int row, char* out, int outsz) {
    out[0] = 0;
    if (!r || row < 0 || row >= r->n) return;
    _snprintf(out, outsz, "%s", r->row[row].text);
    out[outsz - 1] = 0;
}

void mvSpeak(const MvCursor* c, const MvRows* r, const char* prefix) {
    if (!c || !r || r->n <= 0) { postSpeech(axs(AXS_MV_EMPTY)); return; }
    char line[MV_ROW_SZ];
    mvRowLine(r, c->row, line, sizeof line);
    char posln[64];
    _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), c->row + 1, r->n);
    posln[sizeof posln - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s%s%s%s", prefix ? prefix : "", (prefix && prefix[0]) ? " " : "",
              line, posln[0] ? " " : "", posln);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- The keys ----
int mvKey(MvCursor* c, const MvRows* r, uint32_t sym, uint16_t mod, uint8_t repeat, bool lrOptions,
          int* optOut) {
    if (optOut) *optOut = -1;
    if (!c || !r) return MVK_PASS;
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return MVK_PASS;

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return MVK_DONE;                 // one activation per physical press
        if (r->n <= 0) return MVK_PASS;
        const MvRowSlot& s = r->row[c->row];
        if (s.kind == MV_OPT)   { if (optOut) *optOut = s.opt; return MVK_OPTION; }
        if (s.kind == MV_CLOSE) return MVK_CLOSE;
        return MVK_TEXT;
    }

    if (lrOptions && (sym == SDLK_LEFT || sym == SDLK_RIGHT) &&
        !(mod & (KMOD_LSHIFT | KMOD_RSHIFT))) {
        if (repeat) return MVK_DONE;
        int first = mvFirstNonText(r);
        if (first < 0) return MVK_DONE;              // no options: claimed, nothing to step to
        int dir = sym == SDLK_RIGHT ? 1 : -1;
        int at = c->row;
        if (at < first) at = first;                  // from the text: land on the first option
        else {
            int nx = at + dir;
            if (nx >= first && nx < r->n) at = nx;   // hard stop at either end of the options
        }
        c->row = at;
        mvSpeak(c, r, nullptr);
        return MVK_DONE;
    }

    int jump = 0, dRow = 0, dCol = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return MVK_DONE;                  // held jump: one landing per press
        dRow = jump;
    } else if (axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) {
        if (!dRow) return MVK_DONE;                  // throttled auto-repeat: claimed, no step
    } else {
        return MVK_PASS;
    }
    if (r->n <= 0) { postSpeech(axs(AXS_MV_EMPTY)); return MVK_DONE; }
    int from = c->row;
    axStepCursor(&c->row, r->n, dRow);               // hard stop: the end row re-reads
    logLine("msgview: row %d -> %d (of %d)", from, c->row, r->n);
    mvSpeak(c, r, nullptr);
    return MVK_DONE;
}

// ---- The close gesture: the game's own Escape ----
bool mvSendEscape(const char* who) {
    uint32_t scan = klScancodeForKey(SDLK_ESCAPE);
    if (!scan) scan = 41;                            // SDL_SCANCODE_ESCAPE
    bool ok = enqueueSynthKeyAt(SDL_EVT_KEYDOWN, scan, SDLK_ESCAPE, 0, 0) &&
              enqueueSynthKeyAt(SDL_EVT_KEYUP,   scan, SDLK_ESCAPE, 0, 1);
    logLine("msgview: %s Close row -> synthesised Escape %s", who ? who : "?", ok ? "queued" : "DROPPED (queue full)");
    return ok;
}

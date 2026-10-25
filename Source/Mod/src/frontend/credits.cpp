// frontend/credits.cpp -- THE CREDITS SCREEN (UI::Panel::Credits)

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "internal.h"

// ---- File layout ----
static const uint32_t CR_HDR_SZ      = 0x14;      // 5 x u32: count, 1, recOff, idxOff, poolOff
static const uint32_t CR_REC_SZ      = 24;        // { a, children, textOff, level, columns, 0 }
static const uint32_t CR_REC_TEXT    = 8;         // u32 offset into the string pool
static const uint32_t CR_REC_LEVEL   = 12;        // i32: 0 top heading, 1 section heading, 2 name
static const uint32_t CR_MAX_RECORDS = 65536;
static const uint32_t CR_MAX_FILE    = 16u << 20; // refuse anything absurd (shipped 287,719 bytes)
static const int      CR_ROW_MAX     = 512;       // one spoken row (the longest shipped line is ~60)

struct CrRow {
    uint32_t text;
    int      level;    // 0 / 1 = a heading, 2 = a name
    int      section;
    int      names;
};

static std::vector<char>  g_crFile;              // the whole file, kept: rows point into it
static std::vector<CrRow> g_crRows;              // every non-empty line, in display order
static bool     g_crLoadTried = false;           // one parse attempt per process
static bool     g_crLoaded    = false;           // ... and whether it produced rows

uintptr_t g_crObj      = 0;
DWORD     g_crSeenTick = 0;      // when the hook last fired
static bool g_crActive    = false;   // mirror of the open gate, for edge detection
static bool g_crAnnounced = false;   // the open line was spoken for this appearance
static int  g_crRow       = 0;       // row cursor
static bool g_crPlaced    = false;   // has a nav key landed on a row yet?
static int  g_crSpokenSec = -1;      // section of the last row spoken (-1 = none yet)

static const uintptr_t CR_STATE_OFF = 0x58;      // UI::Panel::Base lifecycle: 0 out, 1..4 up
static const DWORD     CR_GAP_MS    = 500;       // no draw for this long = it has left the screen

// ---- Loading the file ----
static bool crReadFile(std::vector<char>* out) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) { logLine("credits: GetModuleFileNameW failed"); return false; }
    for (int up = 0; up < 3; up++) {             // strip Darkest.exe, win64, _windows*
        wchar_t* s = wcsrchr(path, L'\\');
        if (!s) { logLine("credits: exe path too shallow"); return false; }
        *s = 0;
    }
    wcsncat(path, L"\\shared\\credits\\credits.cdt", MAX_PATH * 2 - wcslen(path) - 1);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logLine("credits: cannot open %ls (error %lu)", path, GetLastError());
        return false;
    }
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > CR_HDR_SZ && sz.QuadPart < CR_MAX_FILE;
    if (ok) {
        out->resize((size_t)sz.QuadPart + 1);    // +1: a guaranteed NUL after the pool
        DWORD got = 0;
        ok = ReadFile(h, out->data(), (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
        (*out)[(size_t)sz.QuadPart] = 0;
    }
    CloseHandle(h);
    logLine("credits: read %ls -> %s (%lld bytes)", path, ok ? "ok" : "FAILED", (long long)sz.QuadPart);
    return ok;
}

static bool crParse() {
    const char* d = g_crFile.data();
    size_t size = g_crFile.size() - 1;           // without the guard NUL
    uint32_t hdr[5];
    memcpy(hdr, d, sizeof hdr);
    uint32_t count = hdr[0], recOff = hdr[2], poolOff = hdr[4];
    if (count == 0 || count > CR_MAX_RECORDS || recOff < CR_HDR_SZ ||
        (uint64_t)recOff + (uint64_t)count * CR_REC_SZ > size || poolOff >= size) {
        logLine("credits: header rejected (count %u recOff 0x%x poolOff 0x%x size %zu)",
                count, recOff, poolOff, size);
        return false;
    }
    g_crRows.clear();
    g_crRows.reserve(count);
    int section = -1;
    for (uint32_t i = 0; i < count; i++) {
        const char* r = d + recOff + (size_t)i * CR_REC_SZ;
        uint32_t textOff; int32_t level;
        memcpy(&textOff, r + CR_REC_TEXT, 4);
        memcpy(&level,   r + CR_REC_LEVEL, 4);
        if ((uint64_t)poolOff + textOff >= size) {
            logLine("credits: record %u text offset 0x%x is past the file -- stopping there", i, textOff);
            break;
        }
        uint32_t at = poolOff + textOff;
        if (!d[at]) continue;                    // a spacer line
        CrRow row = { at, level, 0, 0 };
        if (level <= 1) section = (int)g_crRows.size();
        row.section = section < 0 ? (int)g_crRows.size() : section;   // a name before any heading
        if (level > 1 && section >= 0) g_crRows[section].names++;
        g_crRows.push_back(row);
    }
    int heads = 0;
    for (const CrRow& r : g_crRows) if (r.level <= 1) heads++;
    logLine("credits: parsed %u records -> %d rows (%d headings)", count, (int)g_crRows.size(), heads);
    return !g_crRows.empty();
}

static bool crEnsureLoaded() {
    if (g_crLoadTried) return g_crLoaded;
    g_crLoadTried = true;
    g_crLoaded = crReadFile(&g_crFile) && crParse();
    if (!g_crLoaded) { g_crRows.clear(); g_crFile.clear(); }
    return g_crLoaded;
}

// ---- The gate ----
static uintptr_t crObj(uintptr_t base) {
    if (!g_crObj || g_crObj <= 0x10000) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(g_crObj, &vft) || vft != base + CR_VFT_RVA) return 0;
    return g_crObj;
}
static bool crIsOpen(uintptr_t base) {
    uintptr_t obj = crObj(base);
    if (!obj) return false;
    if (!g_crSeenTick || (GetTickCount() - g_crSeenTick) >= CR_GAP_MS) return false;
    uint32_t st = 0;
    if (!safeReadU32(obj + CR_STATE_OFF, &st)) return false;
    return st >= 1 && st <= 4;                     // appearing / shown (5..7 = fading out)
}
bool axIsCredits() { return g_crActive; }

// ---- Speaking ----
static void crRowLine(int i, char* out, int sz) {
    out[0] = 0;
    if (i < 0 || i >= (int)g_crRows.size()) return;
    const CrRow& r = g_crRows[i];
    const char* text = g_crFile.data() + r.text;
    if (r.level <= 1 && r.names > 0) _snprintf(out, sz, axs(AXS_CR_HEADING_FMT), text, r.names);
    else                             _snprintf(out, sz, "%s", text);
    out[sz - 1] = 0;
}

static void crSpeakRow() {
    if (g_crRows.empty()) { postSpeech(axs(AXS_CR_EMPTY)); return; }
    if (g_crRow < 0) g_crRow = 0;
    if (g_crRow >= (int)g_crRows.size()) g_crRow = (int)g_crRows.size() - 1;
    const CrRow& r = g_crRows[g_crRow];
    char line[CR_ROW_MAX], utter[CR_ROW_MAX * 2];
    crRowLine(g_crRow, line, sizeof line);
    if (r.level > 1 && r.section != g_crSpokenSec && r.section != g_crRow &&
        r.section >= 0 && r.section < (int)g_crRows.size())
        _snprintf(utter, sizeof utter, axs(AXS_CR_IN_SECTION_FMT),
                  g_crFile.data() + g_crRows[r.section].text, line);
    else
        _snprintf(utter, sizeof utter, "%s", line);
    utter[sizeof utter - 1] = 0;
    g_crSpokenSec = r.section;
    postSpeech(utter);
}

static void crSpeakOpen(uintptr_t base) {
    char title[128];
    if (!resolveKey(base, "menu_base_element_view_credits", title, sizeof title) || !title[0])
        _snprintf(title, sizeof title, "%s", axs(AXS_PM_CREDITS));
    title[sizeof title - 1] = 0;
    char utter[CR_ROW_MAX * 2];
    if (g_crRows.empty()) {
        _snprintf(utter, sizeof utter, "%s. %s", title, axs(AXS_CR_EMPTY));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    char line[CR_ROW_MAX];
    crRowLine(0, line, sizeof line);
    _snprintf(utter, sizeof utter, "%s. %s", title, line);
    utter[sizeof utter - 1] = 0;
    axAppendHint(utter, sizeof utter, AXS_CR_OPEN_HINT);   // "" while the F10 "Control hints" row is Off
    g_crRow = 0;
    g_crPlaced = true;                          // the open line already landed on row 0
    g_crSpokenSec = g_crRows[0].section;
    postSpeech(utter);
}

void crReannounce(uintptr_t base) {
    if (!g_crActive) return;
    logLine("credits: re-announcing after a modal closed");
    crSpeakOpen(base);
}

// ---- THE CREDITS' FIND ENUMERATOR (Ctrl+F, core/find.cpp) ----
int crFind(uintptr_t base, AxFindOp op, int idx, char* out, int outsz) {
    (void)base;
    if (!g_crActive) return 0;
    int n = (int)g_crRows.size();
    if (op == AXF_COUNT) return n;
    if (idx < 0 || idx >= n) return 0;
    if (op == AXF_LABEL) {
        if (!out || outsz <= 0) return 0;
        crRowLine(idx, out, outsz);
        return out[0] ? 1 : 0;
    }
    if (op == AXF_GO) {
        g_crPlaced = true;
        g_crRow = idx;
        g_crSpokenSec = -1;                      // a jump: always say which section it landed in
        logLine("credits: find landed on row %d (of %d)", idx, n);
        crSpeakRow();
        return 1;
    }
    return 0;
}

// ---- Per-frame watch ----
void checkCredits(uintptr_t base) {
    bool open = crIsOpen(base);
    if (open && !g_crActive) {
        g_crActive    = true;
        g_crAnnounced = false;
        g_crPlaced    = false;
        g_crRow       = 0;
        g_crSpokenSec = -1;
        uint32_t st = 0; safeReadU32(g_crObj + CR_STATE_OFF, &st);
        logLine("credits: opened, obj=%p state=%u", (void*)g_crObj, st);
    } else if (!open && g_crActive) {
        g_crActive = false;
        logLine("credits: closed");
        if (!axModalClosed(base, "credits")) postSpeech(axs(AXS_CR_CLOSED));
        return;
    }
    if (g_crActive && !g_crAnnounced) {
        g_crAnnounced = true;
        crEnsureLoaded();                        // once per process; a failure is logged there
        crSpeakOpen(base);
    }
}

// ---- Keys ----
static int crHeadingBefore(int from) {
    for (int i = from - 1; i >= 0; i--) if (g_crRows[i].level <= 1) return i;
    return -1;
}
static int crHeadingAfter(int from) {
    for (int i = from + 1; i < (int)g_crRows.size(); i++) if (g_crRows[i].level <= 1) return i;
    return -1;
}

bool routeCreditsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    (void)base;
    if (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN) {
        if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
        if (axNavHoldRepeat(repeat)) return true;
        if (g_crRows.empty()) { postSpeech(axs(AXS_CR_EMPTY)); return true; }
        int to = sym == SDLK_PAGEUP ? crHeadingBefore(g_crRow) : crHeadingAfter(g_crRow);
        int from = g_crRow;
        if (to >= 0) g_crRow = to;
        g_crPlaced = true;
        logLine("credits: heading jump %d -> %d (of %d)", from, g_crRow, (int)g_crRows.size());
        crSpeakRow();
        return true;
    }
    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last row
    if (isJump) {
        if (!jump) return true;                         // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                             // throttled repeat: claimed, no step
    if (g_crRows.empty()) { postSpeech(axs(AXS_CR_EMPTY)); return true; }
    if (!g_crPlaced) {                                  // first arrow lands on row 0
        g_crPlaced = true;
        g_crRow = 0;
        if (!isJump) { crSpeakRow(); return true; }
    }
    axStepCursor(&g_crRow, (int)g_crRows.size(), dRow); // hard stop: re-read the end row
    crSpeakRow();
    return true;
}

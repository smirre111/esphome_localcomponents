// =============================================================================
// VENDORED COPY — DO NOT EDIT HERE.
//
// Source of truth: BlindsESP/main/include/ScheduleText.h
// Sync with:       BlindsESP/proto/regen_stubs.sh
// Guarded by:      ctest `scheduletext_drift_hub_vs_node`
//
// Only the hub parses this today — the node never sees the text, just the
// resulting entries. It lives in the node tree anyway so the format has ONE
// definition and one test suite, in the same place as the other shared
// contract headers, rather than a hub-only copy that could drift from the
// wire semantics it encodes.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// ScheduleText — a compact, human-editable schedule format.
//
//   06:00 daily open; 21:45 daily close; 12:00 sat position:40
//   07:30 weekend open; 08:00 mon,thu position:65
//
// One line expresses what took twenty Home Assistant entities: a time picker, an
// action select, a days select, a position number and an enable switch, per slot.
// Twenty controls to say three things is worse UX than the YAML it replaced,
// which is what using it showed.
//
// Dependency-free like BootPolicy.h / FrameCrypto.h / MotorPolicy.h, so the
// parser is tested on the host. That matters more here than for most of this
// code: a parser is the one place where a typo becomes silent data loss, and the
// whole reason the entity version was defensible was that it could not be
// mistyped. This format only beats it if bad input is rejected with a reason
// rather than half-applied.
//
// Grammar (case-insensitive, whitespace-tolerant):
//   schedule := entry (';' entry)*
//   entry    := HH:MM WS days WS action
//   days     := 'daily' | 'weekdays' | 'weekend' | daylist
//   daylist  := day (',' day)*            e.g. mon,thu  — arbitrary masks, which
//                                         the days SELECT could not express
//   day      := mon|tue|wed|thu|fri|sat|sun
//   action   := 'open' | 'close' | 'stop' | 'position' ':' 0..100
//
// An empty string is valid and means "no entries".
// ---------------------------------------------------------------------------

namespace scheduletext
{

static constexpr uint8_t kMaxEntries = 8;
static constexpr size_t  kErrorLen   = 96;
static constexpr size_t  kTextLen    = 240;

// Mirrors SchedAction in blinds.proto EXACTLY. Note the gap: STOP is 2 and
// POSITION is 3, not 2 — assuming a dense 0,1,2 was a real bug. It made
// "position" write STOP to the node, so a scheduled position move would have
// halted the blind wherever it happened to be instead.
static constexpr uint8_t kActionOpen     = 0;
static constexpr uint8_t kActionClose    = 1;
static constexpr uint8_t kActionStop     = 2;
static constexpr uint8_t kActionPosition = 3;

struct Entry
{
    uint16_t minuteOfDay;
    uint8_t  dayMask;       // bit0 = MON .. bit6 = SUN
    uint8_t  action;
    uint8_t  positionPct;
};

struct ParseResult
{
    bool    ok;
    // ZERO whenever ok is false. A parse that returned the entries seen
    // before the error would apply a schedule the user never asked for —
    // silent data loss, and strictly worse than the twenty controls this
    // format replaces. Caught by NothingIsAppliedWhenAnyEntryIsBad.
    uint8_t count;
    Entry   entries[kMaxEntries];
    char    error[kErrorLen];   // empty when ok
};

namespace detail
{

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c; }
inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Case-insensitive compare of [b,e) against a NUL-terminated literal.
inline bool token_is(const char *b, const char *e, const char *lit)
{
    for (; b != e; ++b, ++lit)
    {
        if (*lit == '\0' || lower(*b) != *lit)
            return false;
    }
    return *lit == '\0';
}

inline void copy_error(char *dst, const char *msg, const char *tok_b, const char *tok_e)
{
    size_t i = 0;
    for (; msg[i] != '\0' && i < kErrorLen - 1; ++i)
        dst[i] = msg[i];
    if (tok_b != nullptr && i < kErrorLen - 4)
    {
        dst[i++] = ' ';
        dst[i++] = '\'';
        for (const char *p = tok_b; p != tok_e && i < kErrorLen - 2; ++p)
            dst[i++] = *p;
        dst[i++] = '\'';
    }
    dst[i] = '\0';
}

inline uint8_t day_bit(const char *b, const char *e)
{
    if (token_is(b, e, "mon")) return 1 << 0;
    if (token_is(b, e, "tue")) return 1 << 1;
    if (token_is(b, e, "wed")) return 1 << 2;
    if (token_is(b, e, "thu")) return 1 << 3;
    if (token_is(b, e, "fri")) return 1 << 4;
    if (token_is(b, e, "sat")) return 1 << 5;
    if (token_is(b, e, "sun")) return 1 << 6;
    return 0;
}

}  // namespace detail

inline ParseResult parse(const char *text)
{
    using namespace detail;

    ParseResult r{};
    r.ok = true;
    r.error[0] = '\0';
    if (text == nullptr)
        return r;

    const char *p = text;
    while (*p != '\0')
    {
        while (is_space(*p) || *p == ';')
            ++p;
        if (*p == '\0')
            break;

        if (r.count >= kMaxEntries)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "more than 8 entries", nullptr, nullptr);
            return r;
        }

        // ---- time ----------------------------------------------------------
        const char *tb = p;
        while (*p != '\0' && !is_space(*p) && *p != ';')
            ++p;
        const char *te = p;
        if (te - tb < 4 || te - tb > 5)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "expected HH:MM, got", tb, te);
            return r;
        }
        const char *colon = tb;
        while (colon != te && *colon != ':')
            ++colon;
        if (colon == te)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "expected HH:MM, got", tb, te);
            return r;
        }
        int hh = 0, mm = 0;
        for (const char *q = tb; q != colon; ++q)
        {
            if (!is_digit(*q)) { r.ok = false; r.count = 0; copy_error(r.error, "bad hour in", tb, te); return r; }
            hh = hh * 10 + (*q - '0');
        }
        for (const char *q = colon + 1; q != te; ++q)
        {
            if (!is_digit(*q)) { r.ok = false; r.count = 0; copy_error(r.error, "bad minute in", tb, te); return r; }
            mm = mm * 10 + (*q - '0');
        }
        if (hh > 23 || mm > 59)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "time out of range:", tb, te);
            return r;
        }

        // ---- days ----------------------------------------------------------
        while (is_space(*p)) ++p;
        const char *db = p;
        while (*p != '\0' && !is_space(*p) && *p != ';')
            ++p;
        const char *de = p;
        if (db == de)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "missing days after", tb, te);
            return r;
        }
        uint8_t mask = 0;
        if (token_is(db, de, "daily"))         mask = 0b1111111;
        else if (token_is(db, de, "weekdays")) mask = 0b0011111;
        else if (token_is(db, de, "weekend"))  mask = 0b1100000;
        else
        {
            const char *s = db;
            while (s < de)
            {
                const char *c = s;
                while (c < de && *c != ',') ++c;
                const uint8_t bit = day_bit(s, c);
                if (bit == 0)
                {
                    r.ok = false; r.count = 0;
                    copy_error(r.error, "unknown day", s, c);
                    return r;
                }
                mask |= bit;
                s = (c < de) ? c + 1 : de;
            }
        }

        // ---- action --------------------------------------------------------
        while (is_space(*p)) ++p;
        const char *ab = p;
        while (*p != '\0' && !is_space(*p) && *p != ';')
            ++p;
        const char *ae = p;
        if (ab == ae)
        {
            r.ok = false; r.count = 0;
            copy_error(r.error, "missing action after", db, de);
            return r;
        }
        uint8_t action = kActionOpen;
        uint8_t pct    = 0;
        if (token_is(ab, ae, "open"))       action = kActionOpen;
        else if (token_is(ab, ae, "close")) action = kActionClose;
        else if (token_is(ab, ae, "stop"))  action = kActionStop;
        else
        {
            const char *c = ab;
            while (c < ae && *c != ':') ++c;
            if (!token_is(ab, c, "position"))
            {
                r.ok = false; r.count = 0;
                copy_error(r.error, "unknown action", ab, ae);
                return r;
            }
            if (c == ae || c + 1 == ae)
            {
                r.ok = false; r.count = 0;
                copy_error(r.error, "position needs a percentage, e.g. position:40 —", ab, ae);
                return r;
            }
            int v = 0;
            for (const char *q = c + 1; q != ae; ++q)
            {
                if (!is_digit(*q)) { r.ok = false; r.count = 0; copy_error(r.error, "bad percentage in", ab, ae); return r; }
                v = v * 10 + (*q - '0');
                if (v > 100) break;
            }
            if (v > 100)
            {
                r.ok = false; r.count = 0;
                copy_error(r.error, "percentage above 100 in", ab, ae);
                return r;
            }
            action = kActionPosition;
            pct    = (uint8_t) v;
        }

        Entry &e = r.entries[r.count++];
        e.minuteOfDay = (uint16_t) (hh * 60 + mm);
        e.dayMask     = mask;
        e.action      = action;
        e.positionPct = pct;
    }
    return r;
}

// Render the canonical form. Used to echo back what was actually applied, so a
// tolerated input (`6:00 D Open`) visibly becomes `06:00 daily open`.
inline void format(const Entry *entries, uint8_t count, char *out, size_t out_len)
{
    if (out == nullptr || out_len == 0)
        return;
    size_t o = 0;
    auto put = [&](const char *s) {
        while (*s != '\0' && o + 1 < out_len)
            out[o++] = *s++;
    };
    auto put2 = [&](int v) {
        if (o + 2 < out_len)
        {
            out[o++] = (char) ('0' + (v / 10) % 10);
            out[o++] = (char) ('0' + v % 10);
        }
    };

    for (uint8_t i = 0; i < count; i++)
    {
        if (i) put("; ");
        const Entry &e = entries[i];
        put2(e.minuteOfDay / 60);
        put(":");
        put2(e.minuteOfDay % 60);
        put(" ");

        if (e.dayMask == 0b1111111)      put("daily");
        else if (e.dayMask == 0b0011111) put("weekdays");
        else if (e.dayMask == 0b1100000) put("weekend");
        else
        {
            static const char *kNames[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
            bool first = true;
            for (int d = 0; d < 7; d++)
            {
                if (!(e.dayMask & (1 << d))) continue;
                if (!first) put(",");
                put(kNames[d]);
                first = false;
            }
            if (first) put("daily");   // empty mask should not render as nothing
        }

        put(" ");
        if (e.action == kActionClose)         put("close");
        else if (e.action == kActionStop)     put("stop");
        else if (e.action == kActionPosition) { put("position:");
            if (e.positionPct >= 100) put("100");
            else if (e.positionPct >= 10) put2(e.positionPct);
            else if (o + 1 < out_len) out[o++] = (char) ('0' + e.positionPct);
        }
        else put("open");
    }
    out[o] = '\0';
}

}  // namespace scheduletext

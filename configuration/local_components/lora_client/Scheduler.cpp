// VENDORED COPY — DO NOT EDIT HERE. Source: BlindsESP/main/Scheduler.cpp
// Sync with BlindsESP/proto/regen_stubs.sh; guarded by ctest
// scheduler_drift_hub_vs_node. See Scheduler.h for why the hub shares this.

#include "Scheduler.h"

namespace sched
{

namespace
{

// Break a UTC epoch down as if it were local, by shifting first. gmtime_r is
// used deliberately: it does no timezone lookup, so this stays free of tz data
// and identical on device and host.
//
// The shift is done in int64 because a negative offset near the epoch would
// otherwise underflow an unsigned type.
void local_tm(uint64_t utc_epoch, int32_t utc_offset, struct tm *out)
{
    const int64_t shifted = static_cast<int64_t>(utc_epoch) + utc_offset;
    const time_t  t       = static_cast<time_t>(shifted);
    gmtime_r(&t, out);
}

} // namespace

uint8_t local_weekday_bit(uint64_t utc_epoch, int32_t utc_offset)
{
    struct tm tmv;
    local_tm(utc_epoch, utc_offset, &tmv);
    // tm_wday is 0 = Sunday; our bit 0 is Monday.
    const int idx = (tmv.tm_wday + 6) % 7;
    return static_cast<uint8_t>(1u << idx);
}

uint16_t local_minute_of_day(uint64_t utc_epoch, int32_t utc_offset)
{
    struct tm tmv;
    local_tm(utc_epoch, utc_offset, &tmv);
    return static_cast<uint16_t>(tmv.tm_hour * 60 + tmv.tm_min);
}

uint64_t local_midnight_utc(uint64_t utc_epoch, int32_t utc_offset)
{
    struct tm tmv;
    local_tm(utc_epoch, utc_offset, &tmv);
    const int64_t secs_into_day = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
    return static_cast<uint64_t>(static_cast<int64_t>(utc_epoch) - secs_into_day);
}

uint64_t next_occurrence(const Entry *entries, int count,
                         uint64_t now_utc, int32_t utc_offset,
                         int *which_out)
{
    if (which_out != nullptr)
        *which_out = -1;
    if (entries == nullptr || count <= 0)
        return 0;
    if (count > kMaxEntries)
        count = kMaxEntries;

    uint64_t best       = 0;
    int      best_index = -1;

    // Walk forward day by day from today's local midnight. The first day is
    // included because an entry later TODAY is the common case; days beyond
    // cover entries whose weekday has not come round yet.
    const uint64_t midnight0 = local_midnight_utc(now_utc, utc_offset);

    for (int day = 0; day < kSearchDays; day++)
    {
        // Step a whole day at a time. Since the offset is constant here (the
        // hub owns DST), a local day is exactly 86400 s and this cannot drift.
        const uint64_t midnight = midnight0 + static_cast<uint64_t>(day) * 86400ULL;
        const uint8_t  wday     = local_weekday_bit(midnight, utc_offset);

        for (int i = 0; i < count; i++)
        {
            const Entry &e = entries[i];
            if (!e.valid())
                continue;
            if ((e.dayMask & wday) == 0)
                continue;

            const uint64_t fire = midnight + static_cast<uint64_t>(e.minuteOfDay) * 60ULL;
            if (fire <= now_utc)
                continue; // strictly after "now"

            // Lowest index wins a tie, so `<` (not `<=`) on an equal timestamp
            // leaves the earlier-indexed entry in place.
            if (best == 0 || fire < best)
            {
                best       = fire;
                best_index = i;
            }
        }

        // Once a day has produced a hit, no later day can beat it.
        if (best != 0)
            break;
    }

    if (which_out != nullptr)
        *which_out = best_index;
    return best;
}

uint64_t last_missed(const Entry *entries, int count,
                     uint64_t from_utc, uint64_t to_utc, int32_t utc_offset,
                     int *which_out)
{
    if (which_out != nullptr)
        *which_out = -1;
    if (entries == nullptr || count <= 0 || to_utc <= from_utc)
        return 0;
    if (count > kMaxEntries)
        count = kMaxEntries;

    uint64_t best       = 0;
    int      best_index = -1;

    // Walk the local days spanned by the window. Start one day early so an
    // entry late on the previous local day is still considered.
    const uint64_t first_midnight = local_midnight_utc(from_utc, utc_offset);
    const uint64_t span_days      = (to_utc - first_midnight) / 86400ULL + 1ULL;

    for (uint64_t day = 0; day <= span_days; day++)
    {
        const uint64_t midnight = first_midnight + day * 86400ULL;
        const uint8_t  wday     = local_weekday_bit(midnight, utc_offset);

        for (int i = 0; i < count; i++)
        {
            const Entry &e = entries[i];
            if (!e.valid())
                continue;
            if ((e.dayMask & wday) == 0)
                continue;

            const uint64_t fire = midnight + static_cast<uint64_t>(e.minuteOfDay) * 60ULL;
            if (fire <= from_utc || fire > to_utc)
                continue;

            // LATEST wins here (the opposite of next_occurrence): replaying an
            // earlier missed entry after a later one would leave the blind in
            // the wrong state. `>=` so that on a tie the highest index wins,
            // which is the entry that would have run last.
            if (best == 0 || fire >= best)
            {
                best       = fire;
                best_index = i;
            }
        }
    }

    if (which_out != nullptr)
        *which_out = best_index;
    return best;
}

} // namespace sched

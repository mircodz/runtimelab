// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "gcinternal.h"

#ifdef SERVER_GC
namespace SVR
{
#else // SERVER_GC
namespace WKS
{
#endif // SERVER_GC

// String deduplication, an opt-in experiment whose settings are the GCStringDedup* entries of gcconfig.h. This file holds all of it, the rest of the
// GC calls into it from a few places: collect.cpp (after mark and after plan), plan_phase.cpp (the decision to compact), relocate_compact.cpp (the
// relocate phase and every relocated reference), background.cpp (the final mark of a background GC), interface.cpp (the roots on a stack),
// dynamic_heap_count.cpp (DATAS) and diagnostics.cpp (heap verification).
//
// What a GC does, in order:
//   measure  gc_heap::measure_string_duplicates: a workstation GC that condemns gen2 counts the duplicates of the heap and fires an event, it changes nothing.
//   queue    string_dedup_queue_after_mark: right after marking in a blocking GC that condemns gen1, the marked strings of gen1 become candidates when the
//            GC promotes them, unless they are pinned or their header holds a hash code, a lock or a sync block. A string leaves gen1 once, so it is a
//            candidate once.
//   find     string_dedup_find_after_mark, called by the same function: hashes the candidates and looks them up in a table of the strings of gen2. A string
//            that is not in the table becomes the canonical string of its content, any other string is a duplicate of the canonical one. The time
//            (GCStringDedupBudgetUs) and the size of the table (GCStringDedupFindTableBits) are limited.
//   merge    (regions only, in a GC that compacts) string_dedup_merge_begin registers the duplicates at the start of the relocate phase,
//            string_dedup_redirect replaces every reference to one while gc_heap::relocate_address relocates it, string_dedup_merge_end drops the
//            registrations. string_dedup_compaction_pays makes a gen1 GC that would sweep compact when the duplicates that it found are worth enough.
//   report   string_dedup_queue_report, after the plan phase: removes the strings that the GC left in a younger generation from the table and fires the events.
//
// Between these, string_dedup_queue_relocate follows the entries of the queue and the strings of the table when the GC compacts, string_dedup_queue_background_mark
// removes the ones that die in a background GC, and string_dedup_queue_merge moves the queue of a heap that DATAS removes to another heap. The finder leaves a
// string alone when a stack, a register or a byref refers to it (string_dedup_note_root), because it can be under construction.

#ifdef FEATURE_EVENT_TRACE

// The GC does not know the string method table, so strings are recognized by their shape: a variable sized object with two byte
// components and the base size of System.String (object header, method table pointer, length and the terminating null character).
// No array has that base size. The characters start right after the length.
const uint32_t string_dedup_base_size = (uint32_t)(sizeof (ObjHeader) + sizeof (Object) + sizeof (uint32_t) + sizeof (uint16_t));
const size_t string_dedup_chars_offset = sizeof (Object) + sizeof (uint32_t);

static bool string_dedup_is_string (MethodTable* mt)
{
    return mt->HasComponentSize() && (mt->RawGetComponentSize() == sizeof (uint16_t)) && (mt->GetBaseSize() == string_dedup_base_size);
}

// Anything other than the bits the GC uses for itself means the identity of the string is observable (hash code, lock, sync block).
static bool string_dedup_header_has_state (CObjectHeader* o)
{
    return (o->GetHeader()->GetBits() & ~(uint32_t)(BIT_SBLK_GC_RESERVE | BIT_SBLK_FINALIZER_RUN)) != 0;
}

// The quality of this hash only affects the speed of the table, equal contents are always verified with memcmp. The seed keeps the contents
// that collide from being known in advance, it is 0 where that does not matter.
static uint64_t string_dedup_hash (const uint8_t* chars, size_t byte_count, uint64_t seed)
{
    const uint64_t multiplier = 0xFF51AFD7ED558CCDULL;

    uint64_t hash = ((uint64_t)byte_count ^ seed) * 0x9E3779B97F4A7C15ULL;
    while (byte_count >= sizeof (uint64_t))
    {
        uint64_t chunk;
        memcpy (&chunk, chars, sizeof (uint64_t));
        hash = (hash ^ chunk) * multiplier;
        hash ^= hash >> 32;
        chars += sizeof (uint64_t);
        byte_count -= sizeof (uint64_t);
    }

    if (byte_count > 0)
    {
        uint64_t chunk = 0;
        memcpy (&chunk, chars, byte_count);
        hash = (hash ^ chunk) * multiplier;
        hash ^= hash >> 32;
    }

    hash *= 0xC4CEB9FE1A85EC53ULL;
    hash ^= hash >> 33;

    return (hash != 0) ? hash : 1;
}

#endif //FEATURE_EVENT_TRACE

#if defined(FEATURE_EVENT_TRACE) && !defined(MULTIPLE_HEAPS)

// String deduplication measurement (GCStringDedupMeasure): right after marking in a full blocking GC, every live string is hashed
// and the distinct contents are counted to find out how much memory deduplicating them would free. The heap is not modified.
//
// The result is reported with the StringDedupMeasure GC event. The payload of its version 1 is these uint64 values in order:
//   GC index, measurement time in microseconds, bytes of live objects, string count, string bytes,
//   strings skipped because they are pinned, strings skipped because the object header holds a hash code, a lock or a sync block,
//   candidate strings, distinct candidate contents, candidates that did not fit in the table,
//   bytes freed by deduplicating all candidates, the same counting only strings in gen2, LOH and POH,
//   then the bytes freed for each string length bucket (see string_dedup_length_bucket).

struct string_dedup_entry
{
    uint64_t hash;        // 0 marks an empty slot
    uint8_t* sample;      // one of the strings with this content
    uint32_t old_count;   // strings with this content in gen2, LOH and POH
    uint32_t young_count; // strings with this content in gen0 and gen1
};

struct string_dedup_stats
{
    string_dedup_entry* slots;
    size_t slot_count;    // always a power of two
    size_t used_slots;
    uint64_t live_bytes;
    uint64_t string_count;
    uint64_t string_bytes;
    uint64_t pinned_count;
    uint64_t header_state_count;
    uint64_t candidate_count;
    uint64_t untracked_count;
    bool grow_failed;     // growing the table failed, so it is not tried again for every later string
};

const size_t string_dedup_initial_slots = (size_t)1 << 16;

// With 24 bytes per slot on 64-bit the table takes up to 192 MiB, plus the 96 MiB of the previous table while it grows to that size.
const size_t string_dedup_max_slots = (size_t)1 << 23;
const int string_dedup_length_buckets = 8;

// Buckets by string length in characters: 0-3, 4-7, 8-15, 16-31, 32-63, 64-127, 128-511, 512 and longer.
static int string_dedup_length_bucket (uint32_t length)
{
    static const uint32_t bucket_limits[string_dedup_length_buckets - 1] = { 4, 8, 16, 32, 64, 128, 512 };

    int bucket = 0;
    while ((bucket < (string_dedup_length_buckets - 1)) && (length >= bucket_limits[bucket]))
    {
        bucket++;
    }

    return bucket;
}

static bool string_dedup_grow (string_dedup_stats* stats)
{
    size_t new_slot_count = stats->slot_count * 2;
    string_dedup_entry* new_slots = new (nothrow) string_dedup_entry[new_slot_count];
    if (new_slots == nullptr)
    {
        stats->grow_failed = true;
        return false;
    }

    memset (new_slots, 0, new_slot_count * sizeof (string_dedup_entry));
    for (size_t i = 0; i < stats->slot_count; i++)
    {
        if (stats->slots[i].hash != 0)
        {
            size_t index = (size_t)stats->slots[i].hash & (new_slot_count - 1);
            while (new_slots[index].hash != 0)
            {
                index = (index + 1) & (new_slot_count - 1);
            }

            new_slots[index] = stats->slots[i];
        }
    }

    delete[] stats->slots;
    stats->slots = new_slots;
    stats->slot_count = new_slot_count;

    return true;
}

// Returns the entry for the strings with the content of 'str', adding it if this content is new, or nullptr if the table is full.
static string_dedup_entry* string_dedup_find_or_add (string_dedup_stats* stats, uint64_t hash, CObjectHeader* str, uint32_t length)
{
    while (true)
    {
        size_t mask = stats->slot_count - 1;
        size_t index = (size_t)hash & mask;
        while (stats->slots[index].hash != 0)
        {
            string_dedup_entry* slot = &stats->slots[index];
            if ((slot->hash == hash) &&
                (((CObjectHeader*)slot->sample)->GetNumComponents() == length) &&
                (memcmp ((uint8_t*)slot->sample + string_dedup_chars_offset, (uint8_t*)str + string_dedup_chars_offset, (size_t)length * sizeof (uint16_t)) == 0))
            {
                return slot;
            }

            index = (index + 1) & mask;
        }

        // Linear probing: keep the table at most half full while it can still grow, at most three quarters full once it cannot.
        bool at_max_size = (stats->slot_count == string_dedup_max_slots);
        size_t max_used_slots = at_max_size ? ((stats->slot_count / 4) * 3) : (stats->slot_count / 2);
        if (stats->used_slots < max_used_slots)
        {
            string_dedup_entry* slot = &stats->slots[index];
            slot->hash = hash;
            slot->sample = (uint8_t*)str;
            slot->old_count = 0;
            slot->young_count = 0;
            stats->used_slots++;

            return slot;
        }

        if (at_max_size || stats->grow_failed || !string_dedup_grow (stats))
        {
            return nullptr;
        }
    }
}

bool gc_heap::measure_string_duplicates_visit (Object* obj, void* context)
{
    string_dedup_stats* stats = (string_dedup_stats*)context;
    CObjectHeader* o = (CObjectHeader*)obj;
    if (!o->IsMarked())
    {
        return true;
    }

#ifndef USE_REGIONS
    // Segments mark every object of an in-range frozen segment because those are never collected, regions never mark them.
    // Skip them so that both measure the same live set.
    heap_segment* seg = find_segment ((uint8_t*)o, FALSE);
    if ((seg != nullptr) && heap_segment_read_only_p (seg))
    {
        return true;
    }
#endif //!USE_REGIONS

    uint64_t object_size = Align (size ((uint8_t*)o));
    stats->live_bytes += object_size;

    MethodTable* mt = o->GetMethodTable();
    if (!string_dedup_is_string (mt))
    {
        return true;
    }

    stats->string_count++;
    stats->string_bytes += object_size;

    if (o->IsPinned())
    {
        stats->pinned_count++;
        return true;
    }

    if (string_dedup_header_has_state (o))
    {
        stats->header_state_count++;
        return true;
    }

    uint32_t length = o->GetNumComponents();
    uint64_t hash = string_dedup_hash ((uint8_t*)o + string_dedup_chars_offset, (size_t)length * sizeof (uint16_t), 0);
    stats->candidate_count++;

    string_dedup_entry* entry = string_dedup_find_or_add (stats, hash, o, length);
    if (entry == nullptr)
    {
        stats->untracked_count++;
    }
    else if (object_gennum ((uint8_t*)o) >= max_generation)
    {
        entry->old_count++;
    }
    else
    {
        entry->young_count++;
    }

    return true;
}

void gc_heap::measure_string_duplicates()
{
#if !defined(BUILD_AS_STANDALONE) && !defined(FEATURE_NATIVEAOT)
    assert (string_dedup_base_size == StringObject::GetBaseSize());
    assert (string_dedup_chars_offset == StringObject::GetBufferOffset());
#endif //!BUILD_AS_STANDALONE && !FEATURE_NATIVEAOT

    if (!GCEventEnabledStringDedupMeasure_V1())
    {
        return;
    }

    string_dedup_stats stats = {};
    stats.slot_count = string_dedup_initial_slots;
    stats.slots = new (nothrow) string_dedup_entry[stats.slot_count];
    if (stats.slots == nullptr)
    {
        return;
    }

    memset (stats.slots, 0, stats.slot_count * sizeof (string_dedup_entry));

    uint64_t start_time = GetHighPrecisionTimeStamp();
    walk_heap_per_heap (measure_string_duplicates_visit, &stats, max_generation, TRUE);

    uint64_t distinct_count = 0;
    uint64_t freed_bytes = 0;
    uint64_t freed_bytes_old = 0;
    uint64_t freed_bytes_by_length[string_dedup_length_buckets] = {};
    for (size_t i = 0; i < stats.slot_count; i++)
    {
        const string_dedup_entry* entry = &stats.slots[i];
        if (entry->hash == 0)
        {
            continue;
        }

        distinct_count++;
        uint64_t total_count = (uint64_t)entry->old_count + entry->young_count;
        if (total_count < 2)
        {
            continue;
        }

        CObjectHeader* sample = (CObjectHeader*)entry->sample;
        uint64_t string_object_size = Align (size ((uint8_t*)sample));
        uint64_t freed = (total_count - 1) * string_object_size;
        freed_bytes += freed;
        freed_bytes_by_length[string_dedup_length_bucket (sample->GetNumComponents())] += freed;
        if (entry->old_count > 1)
        {
            freed_bytes_old += ((uint64_t)entry->old_count - 1) * string_object_size;
        }
    }

    uint64_t elapsed_time = GetHighPrecisionTimeStamp() - start_time;

    GCEventFireStringDedupMeasure_V1 (
        (uint64_t)settings.gc_index,
        elapsed_time,
        stats.live_bytes,
        stats.string_count,
        stats.string_bytes,
        stats.pinned_count,
        stats.header_state_count,
        stats.candidate_count,
        distinct_count,
        stats.untracked_count,
        freed_bytes,
        freed_bytes_old,
        freed_bytes_by_length[0],
        freed_bytes_by_length[1],
        freed_bytes_by_length[2],
        freed_bytes_by_length[3],
        freed_bytes_by_length[4],
        freed_bytes_by_length[5],
        freed_bytes_by_length[6],
        freed_bytes_by_length[7]
    );

    delete[] stats.slots;
}

#endif //FEATURE_EVENT_TRACE && !MULTIPLE_HEAPS

#ifdef FEATURE_EVENT_TRACE

// Candidate queue (GCStringDedupQueue): every blocking GC that condemns gen1 queues the live strings that are about to leave gen1, the
// strings that survived two GCs and are likely to stay around. A queued string is only a candidate for deduplication: nothing is hashed,
// compared or changed here. The entries do not keep their strings alive, like short weak handles: whoever frees or moves a queued string
// has to update the queue, so the queue stays valid for whoever consumes it later.
//
// The entries only point at strings of gen2. A string that the GC leaves in a younger generation, because it is in or next to a pinned
// plug, is removed after planning. Only a gen2 GC frees or moves a string of gen2, so only those look at the old entries: a blocking gen2
// GC removes the entries of the strings that died and relocates the others, and a background GC removes the entries of the strings that
// its final mark did not reach, before its sweep frees them. A gen1 GC only relocates the entries it queued itself.
//
// Every heap has its own queue that is filled from its own gen1. The entries can point into any heap, because the regions of a heap
// that goes out of service move to the other heaps, which is fine because the mark bits and the relocation information are global.
// When a heap goes out of service its entries are merged into another heap, like its finalization queue.
//
// The result is reported with the StringDedupQueue GC event, once for every heap and blocking or background GC. The payload of its
// version 3 is these uint64 values in order:
//   GC index, status (0 active, 1 inactive because the queue could not be allocated), condemned generation,
//   live strings in gen1, strings queued, strings not queued because the queue is full,
//   strings skipped because they are pinned, strings skipped because the object header holds a hash code, a lock or a sync block,
//   entries removed because their string died, entries relocated by compaction, entries in the queue after the GC,
//   time the GC spent on the queue in microseconds, heap number,
//   entries removed because the GC left their string in a younger generation, 1 for a background GC and 0 for a blocking GC.

// The native memory of the feature: the queues, the table, the pairs and the redirections of the merge and the sets of the strings that the roots lead to are
// allocated and freed through string_dedup_new and string_dedup_delete, which count the bytes that they hold. The small structures that hold the buffers
// (the state of a queue, the header of the table and of a set) are not counted. The StringDedupFind event reports the bytes that are held when it is fired
// and the most that were held since the report before it.
static size_t string_dedup_native_held;

// The most that string_dedup_native_held reached since the last report, which the threads of a server GC raise while they grow their queues. It is a pointer
// because that is what Interlocked can compare and exchange in this width on every platform.
static uint8_t* volatile string_dedup_native_peak;

template <typename T>
static T* string_dedup_new (size_t count)
{
    T* buffer = new (nothrow) T[count];
    if (buffer != nullptr)
    {
        size_t bytes = count * sizeof (T);
        size_t held = Interlocked::ExchangeAddPtr (&string_dedup_native_held, bytes) + bytes;
        for (uint8_t* peak = string_dedup_native_peak; (size_t)peak < held; )
        {
            uint8_t* seen = Interlocked::CompareExchangePointer (&string_dedup_native_peak, (uint8_t*)held, peak);
            if (seen == peak)
            {
                break;
            }

            peak = seen;
        }
    }

    return buffer;
}

// 'count' is the one that string_dedup_new was given for the buffer.
template <typename T>
static void string_dedup_delete (T* buffer, size_t count)
{
    if (buffer != nullptr)
    {
        assert (string_dedup_native_held >= (count * sizeof (T)));
        delete[] buffer;
        Interlocked::ExchangeAddPtr (&string_dedup_native_held, (size_t)0 - (count * sizeof (T)));
    }
}

// Ends the work of a GC with the strings: the peak of the next one starts at what is held now.
static void string_dedup_native_restart_peak()
{
    string_dedup_native_peak = (uint8_t*)string_dedup_native_held;
}

struct string_dedup_candidate
{
    uint8_t* obj;
    uint64_t signature; // length and first characters when queued, to check that the entry still points at the same string
};

struct string_dedup_queue_stats
{
    uint64_t gc_index;
    uint64_t status;
    uint64_t condemned_generation;
    uint64_t seen;
    uint64_t enqueued;
    uint64_t dropped;
    uint64_t skipped_pinned;
    uint64_t skipped_header;
    uint64_t died;
    uint64_t relocated;
    uint64_t elapsed_us;
    uint64_t heap_index;
    uint64_t demoted;
    uint64_t background;
    bool report_pending;
};

// Finder (GCStringDedupFind): after every blocking GC the queued candidates are hashed and looked up in a table of
// canonical strings, the first string that was seen with each content. A candidate that finds another string with its content is a
// duplicate. The heap is not modified, a later step is going to redirect the references to the duplicates to the canonical string.
//
// The table does not keep its strings alive either and is maintained like the queue, so all its entries point at strings of gen2: a gen2 GC
// or a background GC removes the entries of the strings that died, a compacting gen2 GC relocates the others and the entries of the strings
// that a GC left in a younger generation are removed after planning. The candidates are consumed right after marking, while no string has
// been touched by the plan phase (it can overwrite the first bytes of a string until the GC has compacted), so the queue is empty after every
// GC and a GC only hashes the strings it queued itself. The canonical strings that it adds are still in gen1 then. A GC that does not collect
// gen2 relocates them and removes the ones it leaves in a younger generation, which are the last entries of the table, and leaves the others.
//
// Every GC that collects gen2 visits all the entries, so the table is made for that: the strings are in an array in the order in which they
// were added, which is mostly the address order of the batch that a GC promoted, and a separate index finds them by their hash. A pass only
// reads the array, sequentially, and moving the strings leaves the index alone. A removed string leaves a hole in the array and a slot of
// the index that points at it, both are dropped when the table is rebuilt, which also happens when it has become much smaller than its size.
//
// A GC that does not collect gen2 cannot tell whether the canonical string of a duplicate is still alive, it is only removed from the table
// by the next gen2 or background GC. Whoever redirects references to a canonical string has to look at its mark bit in that GC first.
// HeapVerify assumes that the strings of the table are not changed in place, because it checks their hash.
//
// A server GC has one table for all the heaps, which belongs to the queue of heap 0 (the other queues have none). Every heap fills and checks its own queue,
// and one thread at a time touches the table while the others wait or are not interested in it: the thread that arrives last at the join after the heaps
// have filled their queues removes the strings that died from the table (a GC that condemns gen2) and consumes the queues of all the heaps in the order of the
// heaps, the thread of heap 0 relocates the table in the relocate phase and prunes it in the final mark of a background GC, and the thread that arrives
// last at the join after the plan phase checks it for the strings that the GC demoted, which needs the regions of all the heaps to have their final
// generations. Without regions a heap decides to demote its segments by itself, so the finder is for the server GC with regions only.
//
// The result is reported with the StringDedupFind GC event, once for every blocking or background GC. The payload of its version 3 is these
// uint64 values in order:
//   GC index, status (0 active, 1 inactive because the table could not be allocated), condemned generation,
//   candidates examined, candidates skipped because the object header holds a hash code, a lock or a sync block,
//   duplicates found, bytes of the duplicates, canonical strings added, candidates not added because the table is full,
//   entries removed because their string died, entries removed because the GC left their string in a younger generation,
//   entries in the table after the GC, time the GC spent on the finder in microseconds, 1 for a background GC and 0 for a blocking GC,
//   candidates that were not examined because the time that GCStringDedupBudgetUs gives the finder was spent,
//   the bytes of native memory that the feature holds when the event is fired (the pairs of the merge are still held then), the most that it held
//   since the previous GC ended its work with the strings (see string_dedup_new).

struct string_dedup_slot
{
    uint32_t entry; // 1 + the position of the string in 'strings', 0 for a slot that has never been used
    uint32_t hash;  // hash of the content
};

struct string_dedup_table
{
    uint8_t** strings;        // the canonical strings in the order in which they were added, nullptr for a removed one
    uint32_t* hashes;         // the hash of every string of 'strings', to rebuild the index
    string_dedup_slot* slots; // the index, open addressing, the slot of a removed string is skipped
    size_t count;             // used elements of 'strings' and 'hashes', including the removed strings
    size_t capacity;          // allocated elements of 'strings' and 'hashes'
    size_t max_capacity;      // the table does not grow beyond it
    size_t slot_count;        // a power of two, the slots are at most half used, at most three quarters at the largest size
    size_t max_slot_count;    // a power of two
    size_t live;              // strings that have not been removed
    uint64_t seed;
};

struct string_dedup_find_stats
{
    uint64_t gc_index;
    uint64_t status;
    uint64_t condemned_generation;
    uint64_t candidates;
    uint64_t skipped_header;
    uint64_t duplicates;
    uint64_t duplicate_bytes;
    uint64_t added;
    uint64_t not_added;
    uint64_t died;
    uint64_t demoted;
    uint64_t elapsed_us;
    uint64_t background;
    uint64_t skipped_budget;
};

const size_t string_dedup_table_initial_capacity = (size_t)1 << 11;

// The largest size of the table is set by GCStringDedupFindTableBits: the index has at most 2^bits slots and the table holds three quarters
// of that many strings. With the default of 21 bits that is 1572864 strings in 34 MiB on 64-bit (8 bytes for the string and 4 for its hash,
// plus 8 bytes for every slot of the index), 24 bits hold twelve million strings in 272 MiB. The table it is rebuilt into comes on top.
// Every GC that collects gen2 visits all the strings, so the size is also what limits the pause that the table adds.
const int string_dedup_table_min_bits = 12;
const int string_dedup_table_max_bits = 24;

// The table starts with a capacity that the smallest allowed size has room for, and the entries of the index and the size computations have to
// fit in 32 bits.
static_assert (string_dedup_table_initial_capacity <= ((((size_t)1 << string_dedup_table_min_bits) / 4) * 3), "The table must not start larger than its smallest allowed size");
static_assert (string_dedup_table_max_bits <= 30, "The table must stay within what 32 bits can count");

#ifdef USE_REGIONS

// Merge (GCStringDedupMerge, regions only): a GC that compacts replaces every reference to a duplicate by a reference to
// its canonical string, so nothing refers to the duplicate anymore and the next gen2 GC reclaims it (the duplicate itself is still planned and
// compacted like any survivor). For a duplicate it is all or nothing: a reference that is left behind makes two references that were the same
// object different ones. Only the relocate phase of a compacting GC visits every reference (the roots, the survivors, the handles and the
// cards of the older generations) and rewrites them anyway, so the merge happens there: while it consumes the candidates the finder records
// the pairs of a duplicate and its canonical string, string_dedup_merge_begin registers them at the start of the relocate phase,
// relocate_address calls string_dedup_redirect for every reference it relocates before it does anything else with it (relocate_pre_plug_info is the one
// caller that passes the address of an object that a pinned plug overwrote, which is not a reference, so it calls relocate_address_core, which does not
// merge), and string_dedup_merge_end drops the registrations.
//
// A GC that does not compact does not merge. The recorded pairs are dropped then.
//
// In a server GC the heaps share what the merge needs, which the queue of heap 0 keeps: one thread consumes the candidates of all the heaps and records the
// pairs (the strings that the roots of every heap noted are moved into one set before that), one thread registers them in the join that starts the relocate
// phase and publishes the table of the redirections, which every thread reads, the threads of all the heaps replace references and count them with atomic
// additions, and one thread drops the registrations in the join that ends the relocate phase. A server GC does not merge while a background GC marks or
// sweeps yet, it reports status 4 then.
//
// A GC that runs while a background GC marks (current_c_gc_state == c_gc_state_marking) does merge, but the references that the relocate phase
// gives a canonical string are written with plain stores, which neither the write barrier nor the write watch of the background GC see, and the
// canonical string can be dead already (the table is pruned only by the GCs of gen2 and by the final mark of a background GC), so the sweep that
// follows would free it behind those references. A string has no references to trace, so marking it for the background GC is all that the background
// GC needs to know about them: it is what the background GC could have done just before this GC, and gcmemcopy carries the mark bit of every plug that
// moves. The bit is set before anything is relocated, at the address that the string has now, and only in that state: nothing is marked while the
// background GC sweeps (its final mark pruned the table, so every entry is alive for the sweep, and a bit set behind the sweep would stay for the next
// background GC), nor before its marking starts (gcmemcopy does not carry bits then, and the background GC will find the references by itself). The
// strings are marked as their pairs are registered: if the table of the redirections cannot be allocated after that, they stay marked without a
// reference, which is floating garbage of that background GC and nothing else (its sweep clears the bits).
//
// A gen1 GC that would sweep compacts instead when the duplicates that the finder recorded in it add up to GCStringDedupCompactMinKB or more
// (string_dedup_compaction_pays, asked by decide_on_compacting after every other reason to compact): a sweeping GC of regions promotes the survivors
// where they are and never looks at their references, and a duplicate that the GC that found it did not merge is never found again, because a
// string leaves gen1 only once. A full GC is not asked to compact: what its compaction costs depends on the whole heap, not on what its duplicates are worth.
//
// A string that a stack, a register or a byref refers to is left out altogether (the finder does not even look it up): strings are written through
// such references while they are built (string.Create, spans, fixed), so the string must neither be merged into another one, because the
// written string would not be the one that the rewritten reference leads to, nor become the canonical string of its content, because the
// references that are merged into it would see it change. GCHeap::Promote notes these strings while the GC marks (string_dedup_note_root).
//
// The pair is dropped when the GC is going to leave the canonical string in a younger generation than gen2, because the finder removes it
// from its table then. The finder does not record a pair for an entry of the queue that is left from a GC that could not consume it (the table
// could not be allocated then): it is in gen2, which a GC of gen1 does not condemn, so this GC would not see all the references to it.
//
// The result is reported with the StringDedupMerge GC event, once for every blocking GC that ran the finder. The payload of its version 3 is
// these uint64 values in order:
//   GC index, status (0 the relocate phase ran, 1 the GC did not compact, 3 out of memory, 4 the GC overlapped a background GC, which a server GC
//   does not merge during yet: the duplicates were found and not registered),
//   condemned generation, duplicates that the finder recorded as pairs (all of its duplicates), candidates left out because a stack, a
//   register or a byref refers to them, pairs dropped when they were registered, pairs that were registered, registered duplicates whose
//   references were replaced, their size in bytes, references that were replaced, registered duplicates that no reference was found for (that
//   must be 0, every duplicate is marked so something refers to it), time that the merge itself spent in microseconds (the cost of looking at
//   every reference that the relocate phase relocates is in the pause only), 1 if the GC compacted and 0 if it did not, the size in bytes of the
//   duplicates that the finder recorded as pairs, 1 if the GC compacted only because string_dedup_compaction_pays asked for it, the state of the
//   background GC when the GC had marked (0 it was not marking or sweeping, 1 marking, 2 sweeping), the canonical strings that the GC marked for the
//   background GC.

struct string_dedup_pair
{
    uint8_t* duplicate;
    uint8_t* canonical;  // where the canonical string is going to be after the GC, nullptr for a pair that was dropped
    size_t bytes;        // the size of the duplicate
};

struct string_dedup_merge_stats
{
    uint64_t gc_index;
    uint64_t status;
    uint64_t condemned_generation;
    uint64_t pairs;
    uint64_t skipped_rooted;
    uint64_t dropped;
    uint64_t registered;
    uint64_t merged;
    uint64_t merged_bytes;
    uint64_t references;
    uint64_t unreferenced;
    uint64_t elapsed_us;
    uint64_t compacted;
    uint64_t pair_bytes;
    uint64_t upgraded;
    uint64_t bgc;
    uint64_t bgc_marked;
    uint64_t registered_bytes; // not in the event: merged_bytes is this, because every registered duplicate is referenced (unreferenced is 0)
};

// What relocate_address looks at while the merge is active: the registered duplicates in an open addressing table that is two thirds full at most, a slot with a
// duplicate and its canonical string in the slot that the address of the duplicate leads to (an empty slot has no duplicate), so that finding one takes one cache
// line, and a filter with 16 bits for every duplicate, which tells most other addresses apart with one load.
struct string_dedup_redirect_slot
{
    uint8_t* duplicate;
    uint8_t* canonical; // where the canonical string is after the GC, its lowest bit is set once a reference to the duplicate has been replaced (strings are aligned)
};

struct string_dedup_redirects
{
    string_dedup_redirect_slot* slots;
    size_t slot_count;
    size_t slot_capacity; // what is allocated, the buffers are kept for the next GC that merges
    uint64_t* filter;
    size_t filter_bits;   // a power of two
    size_t filter_capacity;
};

// The references that the relocate phase replaced. The threads of a server GC count them with atomic additions on one of a few counters that the address of the
// reference leads to (a counter for every 128 bytes of address space), each in a cache line of its own (128 bytes, the line of the CPUs of Apple), so that they do
// not all write to the same one.
#ifdef MULTIPLE_HEAPS
const size_t string_dedup_counter_stripes = 16;

struct string_dedup_counter
{
    size_t value;
    uint8_t padding[128 - sizeof (size_t)];
};

static string_dedup_counter string_dedup_replaced[string_dedup_counter_stripes];
#else //MULTIPLE_HEAPS
static size_t string_dedup_replaced;
#endif //MULTIPLE_HEAPS

static void string_dedup_replaced_reset()
{
#ifdef MULTIPLE_HEAPS
    memset (string_dedup_replaced, 0, sizeof (string_dedup_replaced));
#else //MULTIPLE_HEAPS
    string_dedup_replaced = 0;
#endif //MULTIPLE_HEAPS
}

static size_t string_dedup_replaced_total()
{
#ifdef MULTIPLE_HEAPS
    size_t total = 0;
    for (size_t i = 0; i < string_dedup_counter_stripes; i++)
    {
        total += string_dedup_replaced[i].value;
    }

    return total;
#else //MULTIPLE_HEAPS
    return string_dedup_replaced;
#endif //MULTIPLE_HEAPS
}

// The buffers of the pairs and of the redirections are kept for the next GC that merges, unless a GC with very many duplicates made them bigger than this.
const size_t string_dedup_pairs_keep = (size_t)1 << 16;
const size_t string_dedup_redirects_keep_slots = (size_t)1 << 18;

#endif //USE_REGIONS

// The queue of one heap with the statistics of the GC in progress. In a server GC the table of the finder and its statistics, and the state of the merge,
// are in the queue of heap 0, which is the only one that uses them.
struct string_dedup_queue_state
{
    string_dedup_candidate* entries;
    size_t count;
    size_t capacity;     // the entries that are allocated, a full queue doubles them (string_dedup_queue_grow)
    size_t max_capacity; // the most that it grows to, GCStringDedupQueueMax
    size_t first_new; // the entries from this index on were queued by the blocking GC in progress
    string_dedup_queue_stats gc;
    string_dedup_table* table;
    string_dedup_find_stats find;
#ifdef USE_REGIONS
    string_dedup_pair* pairs; // the duplicates that the GC in progress found, nullptr when the merge is off
    size_t pair_capacity;
    size_t pair_count;
    string_dedup_merge_stats merge;
    string_dedup_redirects redirects;
#endif //USE_REGIONS
};

// The candidates that a queue has room for when it is created: it doubles when it is full, up to GCStringDedupQueueMax.
#ifdef MULTIPLE_HEAPS
const size_t string_dedup_queue_base_capacity = (size_t)1 << 14;
#else //MULTIPLE_HEAPS
const size_t string_dedup_queue_base_capacity = (size_t)1 << 16;
#endif //MULTIPLE_HEAPS

// The merge works on the pairs that the finder records, so it implies the finder, and it needs regions.
static inline bool string_dedup_merge_enabled()
{
#ifdef USE_REGIONS
    return GCConfig::GetStringDedupMerge() != 0;
#else //USE_REGIONS
    return false;
#endif //USE_REGIONS
}

// A server GC needs regions: the table is checked for the strings that the GC demoted by one thread once the generations of the regions of all the heaps
// are final, which is a join after the plan phase, whereas without regions every heap can decide to demote its segments by itself.
static inline bool string_dedup_find_enabled()
{
#if defined(MULTIPLE_HEAPS) && !defined(USE_REGIONS)
    return false;
#else //MULTIPLE_HEAPS && !USE_REGIONS
    return (GCConfig::GetStringDedupFind() != 0) || string_dedup_merge_enabled();
#endif //MULTIPLE_HEAPS && !USE_REGIONS
}

// The tests that the passes over the queue and the table make for each of their entries. The passes spend their time in them, so they are
// inlined instead of going through the GC interface (GCHeap::IsPromoted and GCHeap::WhichGeneration), which costs more than the memory
// access that the test is about.
enum string_dedup_check
{
    string_dedup_check_marked,     // marked by the mark phase of a blocking GC that condemns gen2, before it plans
#ifdef BACKGROUND_GC
    string_dedup_check_bgc_marked, // marked by the final mark of a background GC
#endif //BACKGROUND_GC
    string_dedup_check_in_gen2     // in gen2 after the GC has planned the heap
};

struct string_dedup_test
{
    static inline gc_heap* any_heap()
    {
#ifdef MULTIPLE_HEAPS
        return gc_heap::g_heaps[0];
#else //MULTIPLE_HEAPS
        return (gc_heap*)0;
#endif //MULTIPLE_HEAPS
    }

    // The same as GCHeap::IsPromoted in a blocking GC that condemns gen2: whatever is outside of the range of the GC is alive.
    static inline bool alive_after_mark (uint8_t* obj)
    {
        gc_heap* hp = any_heap();

        return !((obj < hp->highest_address) && (obj >= hp->lowest_address)) || hp->is_mark_set (obj);
    }

#ifdef BACKGROUND_GC
    // The same as GCHeap::IsPromoted in a background GC.
    static inline bool alive_after_bgc_mark (uint8_t* obj)
    {
        gc_heap* hp = any_heap();

        return !((obj < hp->background_saved_highest_address) && (obj >= hp->background_saved_lowest_address)) || hp->background_marked (obj);
    }
#endif //BACKGROUND_GC

    // The same as GCHeap::WhichGeneration (obj) == max_generation.
    static inline bool in_gen2 (uint8_t* obj)
    {
#ifdef USE_REGIONS
        return (obj >= g_gc_lowest_address) && (obj < g_gc_highest_address) &&
               ((gc_heap::map_region_to_generation_skewed[get_skewed_basic_region_index_for_address (obj)] & gc_heap::RI_GEN_MASK) == max_generation);
#else //USE_REGIONS
        return g_theGCHeap->WhichGeneration ((Object*)obj) == max_generation;
#endif //USE_REGIONS
    }
};

// Whether an entry passes the check, the others are removed.
static inline bool string_dedup_entry_passes (string_dedup_check check, uint8_t* obj)
{
    switch (check)
    {
        case string_dedup_check_marked:
            return string_dedup_test::alive_after_mark (obj);
#ifdef BACKGROUND_GC
        case string_dedup_check_bgc_marked:
            return string_dedup_test::alive_after_bgc_mark (obj);
#endif //BACKGROUND_GC
        default:
            assert (check == string_dedup_check_in_gen2);
            return string_dedup_test::in_gen2 (obj);
    }
}

static uint64_t string_dedup_signature (CObjectHeader* str)
{
    uint32_t length = str->GetNumComponents();
    uint32_t first_chars = 0;
    memcpy (&first_chars, (uint8_t*)str + string_dedup_chars_offset, ((length < 2) ? length : 2) * sizeof (uint16_t));

    return ((uint64_t)length << 32) | first_chars;
}

#ifdef USE_REGIONS
// Makes room for 'count' pairs of the GC in progress. Every candidate leads to at most one pair, so the candidates of the queues that are consumed set the
// size, and the pairs of an earlier GC are not needed anymore. Returns false when memory ran out.
static bool string_dedup_pairs_reserve (string_dedup_queue_state* owner, size_t count)
{
    if (owner->pair_capacity < count)
    {
        string_dedup_delete (owner->pairs, owner->pair_capacity);
        owner->pairs = string_dedup_new<string_dedup_pair> (count);
        owner->pair_capacity = (owner->pairs != nullptr) ? count : 0;
    }

    return owner->pairs != nullptr;
}
#endif //USE_REGIONS

// The most that a queue grows to: GCStringDedupQueueMax, but not less than what it starts with and not more than 2^26 candidates (1 GB).
static size_t string_dedup_queue_max_capacity()
{
    int64_t configured = GCConfig::GetStringDedupQueueMax();
    size_t limit = (configured <= 0) ? 0 : ((configured > ((int64_t)1 << 26)) ? ((size_t)1 << 26) : (size_t)configured);

    return (limit > string_dedup_queue_base_capacity) ? limit : string_dedup_queue_base_capacity;
}

static bool string_dedup_queue_reserve (string_dedup_queue_state* queue)
{
    queue->max_capacity = string_dedup_queue_max_capacity();
    if (queue->entries == nullptr)
    {
        queue->entries = string_dedup_new<string_dedup_candidate> (string_dedup_queue_base_capacity);
        queue->capacity = (queue->entries != nullptr) ? string_dedup_queue_base_capacity : 0;
    }

    return queue->entries != nullptr;
}

// Doubles the room of a full queue, up to its maximum. Returns false when it cannot grow, the candidate that does not fit is dropped then: a string
// leaves gen1 once, so it is lost for good.
static bool string_dedup_queue_grow (string_dedup_queue_state* queue)
{
    if (queue->capacity >= queue->max_capacity)
    {
        return false;
    }

    size_t capacity = (queue->capacity < (queue->max_capacity / 2)) ? (queue->capacity * 2) : queue->max_capacity;
    string_dedup_candidate* entries = string_dedup_new<string_dedup_candidate> (capacity);
    if (entries == nullptr)
    {
        return false;
    }

    memcpy (entries, queue->entries, queue->count * sizeof (string_dedup_candidate));
    string_dedup_delete (queue->entries, queue->capacity);
    queue->entries = entries;
    queue->capacity = capacity;

    return true;
}

// A queue that grew gives its memory back once it is empty, the next GC that needs the room grows it again.
static void string_dedup_queue_shrink (string_dedup_queue_state* queue)
{
    if ((queue->count == 0) && (queue->capacity > string_dedup_queue_base_capacity))
    {
        string_dedup_delete (queue->entries, queue->capacity);
        queue->entries = nullptr;
        queue->capacity = 0;
    }
}

static void string_dedup_queue_add (CObjectHeader* str, string_dedup_queue_state* queue)
{
    string_dedup_queue_stats* stats = &queue->gc;
    stats->seen++;

    if (str->IsPinned())
    {
        stats->skipped_pinned++;
    }
    else if (string_dedup_header_has_state (str))
    {
        stats->skipped_header++;
    }
    else if ((queue->count == queue->capacity) && !string_dedup_queue_grow (queue))
    {
        stats->dropped++;
    }
    else
    {
        string_dedup_candidate* candidate = &queue->entries[queue->count++];
        candidate->obj = (uint8_t*)str;
        candidate->signature = string_dedup_signature (str);
        stats->enqueued++;
    }
}

// Queues the marked strings of a range of the small object heap, the objects are parsed by size because the heap is not planned yet.
static void string_dedup_queue_add_range (uint8_t* start, uint8_t* end, string_dedup_queue_state* queue)
{
    uint8_t* o = start;
    while (o < end)
    {
        CObjectHeader* obj = (CObjectHeader*)o;
        if (obj->IsMarked() && string_dedup_is_string (obj->GetMethodTable()))
        {
            string_dedup_queue_add (obj, queue);
        }

        o += Align (size (o));
    }
}

// Removes the entries that do not pass the check, the others keep their order. Returns the number of removed entries.
static size_t string_dedup_queue_remove_if (string_dedup_queue_state* queue, string_dedup_check check)
{
    size_t kept = 0;
    for (size_t i = 0; i < queue->count; i++)
    {
        if (string_dedup_entry_passes (check, queue->entries[i].obj))
        {
            queue->entries[kept++] = queue->entries[i];
        }
    }

    size_t removed = queue->count - kept;
    queue->count = kept;

    return removed;
}

static void string_dedup_queue_fire (const string_dedup_queue_stats* stats, size_t queue_size)
{
    if (!GCEventEnabledStringDedupQueue_V3())
    {
        return;
    }

    GCEventFireStringDedupQueue_V3 (
        stats->gc_index,
        stats->status,
        stats->condemned_generation,
        stats->seen,
        stats->enqueued,
        stats->dropped,
        stats->skipped_pinned,
        stats->skipped_header,
        stats->died,
        stats->relocated,
        (uint64_t)queue_size,
        stats->elapsed_us,
        stats->heap_index,
        stats->demoted,
        stats->background
    );
}

// The number of slots of the index for a table that holds 'capacity' strings: a power of two with room for twice as many strings, or for
// four thirds as many at the largest size.
static size_t string_dedup_table_slot_count (const string_dedup_table* table, size_t capacity)
{
    size_t slot_count = 1;
    while ((slot_count < (capacity * 2)) && (slot_count < table->max_slot_count))
    {
        slot_count *= 2;
    }

    return slot_count;
}

// Rebuilds the table with another capacity from its live strings, which drops the removed ones and keeps the order of the others. The
// capacity has to hold all of them. Returns false and leaves the table as it is when memory ran out.
static bool string_dedup_table_rebuild (string_dedup_table* table, size_t capacity)
{
    assert ((capacity >= table->live) && (capacity <= table->max_capacity));

    size_t slot_count = string_dedup_table_slot_count (table, capacity);
    uint8_t** strings = string_dedup_new<uint8_t*> (capacity);
    uint32_t* hashes = string_dedup_new<uint32_t> (capacity);
    string_dedup_slot* slots = string_dedup_new<string_dedup_slot> (slot_count);
    if ((strings == nullptr) || (hashes == nullptr) || (slots == nullptr))
    {
        string_dedup_delete (strings, capacity);
        string_dedup_delete (hashes, capacity);
        string_dedup_delete (slots, slot_count);

        return false;
    }

    memset (slots, 0, slot_count * sizeof (string_dedup_slot));
    size_t count = 0;
    for (size_t i = 0; i < table->count; i++)
    {
        if (table->strings[i] != nullptr)
        {
            uint32_t hash = table->hashes[i];
            strings[count] = table->strings[i];
            hashes[count] = hash;

            size_t index = (size_t)hash & (slot_count - 1);
            while (slots[index].entry != 0)
            {
                index = (index + 1) & (slot_count - 1);
            }

            slots[index].entry = (uint32_t)(count + 1);
            slots[index].hash = hash;
            count++;
        }
    }

    string_dedup_delete (table->strings, table->capacity);
    string_dedup_delete (table->hashes, table->capacity);
    string_dedup_delete (table->slots, table->slot_count);
    table->strings = strings;
    table->hashes = hashes;
    table->slots = slots;
    table->count = count;
    table->capacity = capacity;
    table->slot_count = slot_count;

    return true;
}

static string_dedup_table* string_dedup_table_create()
{
    string_dedup_table* table = new (nothrow) string_dedup_table();
    if (table != nullptr)
    {
        table->seed = GetHighPrecisionTimeStamp();

        int64_t bits = GCConfig::GetStringDedupFindTableBits();
        if (bits < string_dedup_table_min_bits)
        {
            bits = string_dedup_table_min_bits;
        }
        else if (bits > string_dedup_table_max_bits)
        {
            bits = string_dedup_table_max_bits;
        }

        table->max_slot_count = (size_t)1 << bits;
        table->max_capacity = (table->max_slot_count / 4) * 3;
        if (!string_dedup_table_rebuild (table, string_dedup_table_initial_capacity))
        {
            delete table;
            table = nullptr;
        }
    }

    return table;
}

// Called when a new string is going to be added to a table whose arrays are full. Every string that was ever added since the table was
// rebuilt has a slot in the index, so the arrays being full keeps the index at most half full, or three quarters at the largest size, which
// keeps probing short and makes it end at an empty slot. Many removed strings are dropped by rebuilding at the same size, otherwise the table
// has to grow. Returns false when that is not possible because the table is at its largest size or memory ran out. The strings and the
// slots have moved when it returns true.
static bool string_dedup_table_make_room (string_dedup_table* table)
{
    assert (table->count == table->capacity);

    if ((table->count - table->live) > (table->capacity / 8))
    {
        return string_dedup_table_rebuild (table, table->capacity);
    }

    if (table->capacity < table->max_capacity)
    {
        size_t capacity = table->capacity * 2;

        return string_dedup_table_rebuild (table, (capacity < table->max_capacity) ? capacity : table->max_capacity);
    }

    return false;
}

// Rebuilds a table that has been emptied by removing strings, at a much smaller size or without the holes that they left, so that the
// passes over it stay cheap.
static void string_dedup_table_shrink (string_dedup_table* table)
{
    size_t capacity = string_dedup_table_initial_capacity;
    while (capacity < (table->live * 2))
    {
        capacity *= 2;
    }

    if (capacity > table->capacity)
    {
        capacity = table->capacity;
    }

    size_t holes = table->count - table->live;
    if (((capacity * 4) <= table->capacity) || ((holes > 0) && (holes >= table->live)))
    {
        string_dedup_table_rebuild (table, capacity);
    }
}

static uint32_t string_dedup_table_hash (const string_dedup_table* table, CObjectHeader* str)
{
    uint64_t hash = string_dedup_hash ((uint8_t*)str + string_dedup_chars_offset, (size_t)str->GetNumComponents() * sizeof (uint16_t), table->seed);

    return (uint32_t)(hash ^ (hash >> 32));
}

static bool string_dedup_equal (CObjectHeader* a, CObjectHeader* b, uint32_t length)
{
    return (a->GetNumComponents() == length) &&
           (memcmp ((uint8_t*)a + string_dedup_chars_offset, (uint8_t*)b + string_dedup_chars_offset, (size_t)length * sizeof (uint16_t)) == 0);
}

// Returns the slot of the canonical string with the content of 'str', or nullptr when there is none. Then 'free_slot' is the empty slot that
// ended the search, where a new string goes. The slots of removed strings are skipped, they stay in use until the table is rebuilt.
static string_dedup_slot* string_dedup_table_find (string_dedup_table* table, uint32_t hash, CObjectHeader* str, uint32_t length, size_t* free_slot)
{
    size_t mask = table->slot_count - 1;
    size_t index = (size_t)hash & mask;
    while (table->slots[index].entry != 0)
    {
        string_dedup_slot* slot = &table->slots[index];
        if (slot->hash == hash)
        {
            uint8_t* canonical = table->strings[slot->entry - 1];
            if ((canonical != nullptr) && string_dedup_equal ((CObjectHeader*)canonical, str, length))
            {
                return slot;
            }
        }

        index = (index + 1) & mask;
    }

    *free_slot = index;

    return nullptr;
}

// Removes the strings from position 'first' on that do not pass the check. Returns the number of removed strings.
static size_t string_dedup_table_remove_if (string_dedup_table* table, string_dedup_check check, size_t first)
{
    size_t removed = 0;
    for (size_t i = first; i < table->count; i++)
    {
        uint8_t* obj = table->strings[i];
        if ((obj != nullptr) && !string_dedup_entry_passes (check, obj))
        {
            table->strings[i] = nullptr;
            removed++;
        }
    }

    table->live -= removed;

    return removed;
}

static void string_dedup_table_relocate (string_dedup_table* table, ScanContext* sc, size_t first)
{
    for (size_t i = first; i < table->count; i++)
    {
        if (table->strings[i] != nullptr)
        {
            GCHeap::Relocate ((Object**)&table->strings[i], sc);
        }
    }
}

// The strings that the finder added in the GC in progress. It appends them and a rebuild keeps the order, so they are the last ones.
static size_t string_dedup_table_added_start (const string_dedup_table* table, const string_dedup_find_stats* stats)
{
    return table->count - (size_t)stats->added;
}

#ifdef USE_REGIONS

// The slot where the search for a pointer starts in a table of 'mask + 1' slots, a power of two.
static inline size_t string_dedup_pointer_slot (uint8_t* p, size_t mask)
{
    return (size_t)(((uint64_t)(size_t)p * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

// The strings that a stack, a register or a byref refers to, noted by GCHeap::Promote while the GC marks. Every heap has its own set for the roots that it
// scans, so the threads do not share one while they mark, and a server GC moves them into the set of heap 0 before it consumes the candidates. The sets
// are empty after every GC that ran the finder, so they only hold what the GC in progress has seen. When memory runs out some of them are not in them, which
// string_dedup_roots_incomplete remembers, and the GC does not merge.
struct string_dedup_rooted_set
{
    uint8_t** slots;    // nullptr for an empty slot
    size_t slot_count;  // a power of two, 0 until the first string is added
    size_t count;
    bool incomplete;
};

static bool string_dedup_roots_incomplete;

static void string_dedup_rooted_add (string_dedup_rooted_set* set, uint8_t* o)
{
    if ((set->count + 1) * 2 > set->slot_count)
    {
        size_t slot_count = (set->slot_count == 0) ? 1024 : (set->slot_count * 2);
        uint8_t** slots = string_dedup_new<uint8_t*> (slot_count);
        if (slots == nullptr)
        {
            set->incomplete = true;

            return;
        }

        memset (slots, 0, slot_count * sizeof (uint8_t*));
        for (size_t i = 0; i < set->slot_count; i++)
        {
            if (set->slots[i] != nullptr)
            {
                size_t index = string_dedup_pointer_slot (set->slots[i], slot_count - 1);
                while (slots[index] != nullptr)
                {
                    index = (index + 1) & (slot_count - 1);
                }

                slots[index] = set->slots[i];
            }
        }

        string_dedup_delete (set->slots, set->slot_count);
        set->slots = slots;
        set->slot_count = slot_count;
    }

    size_t index = string_dedup_pointer_slot (o, set->slot_count - 1);
    while (set->slots[index] != nullptr)
    {
        if (set->slots[index] == o)
        {
            return;
        }

        index = (index + 1) & (set->slot_count - 1);
    }

    set->slots[index] = o;
    set->count++;
}

// A heap that has not noted a string has no set.
static bool string_dedup_rooted_contains (const string_dedup_rooted_set* set, uint8_t* o)
{
    if ((set == nullptr) || (set->count == 0))
    {
        return false;
    }

    for (size_t index = string_dedup_pointer_slot (o, set->slot_count - 1); set->slots[index] != nullptr; index = (index + 1) & (set->slot_count - 1))
    {
        if (set->slots[index] == o)
        {
            return true;
        }
    }

    return false;
}

// Empties the set. A set that has grown large is given back, a GC with many roots is rare.
static void string_dedup_rooted_clear (string_dedup_rooted_set* set)
{
    if (set == nullptr)
    {
        return;
    }

    if (set->slot_count > ((size_t)1 << 16))
    {
        string_dedup_delete (set->slots, set->slot_count);
        set->slots = nullptr;
        set->slot_count = 0;
    }
    else if (set->count != 0)
    {
        memset (set->slots, 0, set->slot_count * sizeof (uint8_t*));
    }

    set->count = 0;
    set->incomplete = false;
}

#ifdef MULTIPLE_HEAPS
// Moves the strings of 'from' into 'into', which both exist.
static void string_dedup_rooted_move (string_dedup_rooted_set* into, string_dedup_rooted_set* from)
{
    for (size_t i = 0; i < from->slot_count; i++)
    {
        if (from->slots[i] != nullptr)
        {
            string_dedup_rooted_add (into, from->slots[i]);
        }
    }

    into->incomplete |= from->incomplete;
    string_dedup_rooted_clear (from);
}
#endif //MULTIPLE_HEAPS

// Records that 'duplicate' has the content of the canonical string. There is room for a pair for every candidate that is consumed.
static void string_dedup_pair_add (string_dedup_queue_state* owner, uint8_t* duplicate, uint8_t* canonical, size_t bytes)
{
    if ((owner->pairs == nullptr) || (owner->merge.status == 3))
    {
        return;
    }

    assert (owner->pair_count < owner->pair_capacity);
    owner->pairs[owner->pair_count++] = string_dedup_pair { duplicate, canonical, bytes };
    owner->merge.pairs++;
    owner->merge.pair_bytes += bytes;
}

#endif //USE_REGIONS

struct string_dedup_rooted_set;

// The finder looks at the clock once for this many candidates, so it overruns the time that it has by at most as many.
const size_t string_dedup_budget_check_interval = 256;

// The time at which the finder stops examining the candidates of the GC that starts now, 0 when GCStringDedupBudgetUs sets no limit.
static uint64_t string_dedup_find_deadline()
{
    int64_t budget = GCConfig::GetStringDedupBudgetUs();

    return (budget > 0) ? (GetHighPrecisionTimeStamp() + (uint64_t)budget) : 0;
}

// The candidates of a queue do not depend on each other, but looking one up is a chain of cache misses: the slot of its hash, the pointer to the canonical
// string in 'strings', and the canonical string itself. So the finder works ahead while it examines a candidate: it prefetches the string of the candidate that is
// 'string_dedup_candidate_distance' ahead, hashes the one 'string_dedup_hash_distance' ahead and prefetches its slot, looks at the slot of the one
// 'string_dedup_pointer_distance' ahead (it has arrived by now) and prefetches the pointer to its canonical string, and reads that pointer for the one
// 'string_dedup_string_distance' ahead and prefetches the string. These are hints: the lookup of the candidate itself is the same as without them, so the
// table may change in between, which it does with every canonical string that is added. The hashes wait in a ring until their candidate is examined.
const size_t string_dedup_candidate_distance = 24;
const size_t string_dedup_hash_distance = 12;
const size_t string_dedup_pointer_distance = 8;
const size_t string_dedup_string_distance = 4;
const size_t string_dedup_ring_size = 16;
static_assert (string_dedup_string_distance < string_dedup_pointer_distance, "The stages of the lookup are in the order of the chain of cache misses");
static_assert (string_dedup_pointer_distance < string_dedup_hash_distance, "The stages of the lookup are in the order of the chain of cache misses");
static_assert (string_dedup_hash_distance < string_dedup_ring_size, "The hashes of the candidates that are ahead have to fit in the ring");
static_assert ((string_dedup_ring_size & (string_dedup_ring_size - 1)) == 0, "The ring is indexed with a mask");

static inline string_dedup_slot* string_dedup_table_first_slot (string_dedup_table* table, uint32_t hash)
{
    return &table->slots[(size_t)hash & (table->slot_count - 1)];
}

// Hashes a candidate that is ahead and prefetches the slot that its hash leads to.
static inline void string_dedup_hash_ahead (string_dedup_table* table, uint8_t* obj, uint32_t* ring, size_t position)
{
    uint32_t hash = string_dedup_table_hash (table, (CObjectHeader*)obj);
    ring[position & (string_dedup_ring_size - 1)] = hash;
    Prefetch ((void*)string_dedup_table_first_slot (table, hash));
}

// Prefetches what the first slot of 'hash' leads to when it is the slot of a canonical string with that hash: the pointer to the string, or the string.
static inline void string_dedup_prefetch_canonical (string_dedup_table* table, uint32_t hash, bool pointer_ready)
{
    string_dedup_slot* slot = string_dedup_table_first_slot (table, hash);
    if ((slot->entry != 0) && (slot->hash == hash))
    {
        if (pointer_ready)
        {
            uint8_t* canonical = table->strings[slot->entry - 1];
            if (canonical != nullptr)
            {
                Prefetch ((void*)canonical);
            }
        }
        else
        {
            Prefetch ((void*)&table->strings[slot->entry - 1]);
        }
    }
}

// The work that the finder does ahead while it examines the candidate at position 'i' of the 'count' that 'entries' has.
static inline void string_dedup_work_ahead (string_dedup_table* table, const string_dedup_candidate* entries, size_t count, uint32_t* ring, size_t i)
{
    if ((i + string_dedup_candidate_distance) < count)
    {
        Prefetch ((void*)entries[i + string_dedup_candidate_distance].obj);
    }

    if ((i + string_dedup_hash_distance) < count)
    {
        string_dedup_hash_ahead (table, entries[i + string_dedup_hash_distance].obj, ring, i + string_dedup_hash_distance);
    }

    if ((i + string_dedup_pointer_distance) < count)
    {
        string_dedup_prefetch_canonical (table, ring[(i + string_dedup_pointer_distance) & (string_dedup_ring_size - 1)], false);
    }

    if ((i + string_dedup_string_distance) < count)
    {
        string_dedup_prefetch_canonical (table, ring[(i + string_dedup_string_distance) & (string_dedup_ring_size - 1)], true);
    }
}

// Takes the candidates out of the queue 'source'. A string whose content is not in the table of the queue 'owner' becomes the canonical string for it,
// any other string is a duplicate of the canonical one. The two are the same queue unless a server GC shares the table. 'roots' are the strings that
// the roots lead to, nullptr when there are none. The candidates that are left when the clock reaches 'deadline' (0 for none) are not examined, and
// are gone with the queue: a string leaves gen1 once.
static void string_dedup_find_consume (string_dedup_queue_state* source, string_dedup_queue_state* owner, string_dedup_rooted_set* roots, uint64_t deadline)
{
    string_dedup_table* table = owner->table;
    string_dedup_find_stats* stats = &owner->find;
    const size_t count = source->count;
    uint32_t ring[string_dedup_ring_size];
    for (size_t j = 0; (j < string_dedup_hash_distance) && (j < count); j++)
    {
        string_dedup_hash_ahead (table, source->entries[j].obj, ring, j);
    }

    for (size_t i = 0; i < count; i++)
    {
        if ((deadline != 0) && ((i % string_dedup_budget_check_interval) == 0) && (GetHighPrecisionTimeStamp() >= deadline))
        {
            stats->skipped_budget += count - i;
            break;
        }

        string_dedup_work_ahead (table, source->entries, count, ring, i);

        CObjectHeader* str = (CObjectHeader*)source->entries[i].obj;
        stats->candidates++;

#ifdef USE_REGIONS
        // A string that a stack, a register or a byref refers to can be under construction. It is not merged into another string, and it does
        // not become the canonical string of its content either, because the references that are merged into it would see it change.
        if ((owner->pairs != nullptr) && string_dedup_rooted_contains (roots, (uint8_t*)str))
        {
            owner->merge.skipped_rooted++;
            continue;
        }
#endif //USE_REGIONS
        // The identity of the string may have become observable since it was queued.
        if (string_dedup_header_has_state (str))
        {
            stats->skipped_header++;
            continue;
        }

        uint32_t length = str->GetNumComponents();
        uint32_t hash = ring[i & (string_dedup_ring_size - 1)];
        size_t free_slot = 0;
        string_dedup_slot* canonical = string_dedup_table_find (table, hash, str, length, &free_slot);
        if (canonical != nullptr)
        {
            if (table->strings[canonical->entry - 1] != (uint8_t*)str)
            {
                size_t bytes = Align (size ((uint8_t*)str));
                stats->duplicates++;
                stats->duplicate_bytes += bytes;
#ifdef USE_REGIONS
                // The entries from before 'first_new' are left from a GC that could not consume them, they are in gen2 and a gen1 GC does not see all
                // the references to them.
                if (i >= source->first_new)
                {
                    string_dedup_pair_add (owner, (uint8_t*)str, table->strings[canonical->entry - 1], bytes);
                }
#endif //USE_REGIONS
            }

            continue;
        }

#ifdef USE_REGIONS
        // When the strings that the roots refer to could not all be noted (status 3) any string can be under construction: nothing becomes canonical.
        if (owner->merge.status == 3)
        {
            stats->not_added++;
            continue;
        }
#endif //USE_REGIONS

        // Making room moves the slots, then the one that is free has to be searched again.
        if (table->count == table->capacity)
        {
            if (!string_dedup_table_make_room (table))
            {
                stats->not_added++;
                continue;
            }

            string_dedup_table_find (table, hash, str, length, &free_slot);
        }

        table->strings[table->count] = (uint8_t*)str;
        table->hashes[table->count] = hash;
        table->slots[free_slot].entry = (uint32_t)(table->count + 1);
        table->slots[free_slot].hash = hash;
        table->count++;
        table->live++;
        stats->added++;
    }

    source->count = 0;
    string_dedup_queue_shrink (source);
}

static void string_dedup_find_fire (const string_dedup_find_stats* stats, size_t table_size)
{
    if (!GCEventEnabledStringDedupFind_V3())
    {
        return;
    }

    GCEventFireStringDedupFind_V3 (
        stats->gc_index,
        stats->status,
        stats->condemned_generation,
        stats->candidates,
        stats->skipped_header,
        stats->duplicates,
        stats->duplicate_bytes,
        stats->added,
        stats->not_added,
        stats->died,
        stats->demoted,
        (uint64_t)table_size,
        stats->elapsed_us,
        stats->background,
        stats->skipped_budget,
        (uint64_t)string_dedup_native_held,
        (uint64_t)(size_t)string_dedup_native_peak
    );
}

#ifdef USE_REGIONS

// The first slot of the search for a duplicate in a table of 'slot_count' slots. The strings of a size are at a regular distance, which a plain
// multiplication turns into clusters for some distances, so the high bits are folded into the low ones, which are reduced to the range by multiplying
// so the table does not have to have a power of two slots.
static inline size_t string_dedup_redirect_slot_of (uint8_t* p, size_t slot_count)
{
    uint64_t hash = (uint64_t)(size_t)p * 0x9E3779B97F4A7C15ull;
    hash ^= hash >> 32;

    return (size_t)(((hash & 0xFFFFFFFFull) * slot_count) >> 32);
}

// The bit of a duplicate in the filter, which has 'filter_bits' bits, a power of two.
static inline size_t string_dedup_redirect_filter_bit (uint8_t* p, size_t filter_bits)
{
    return (size_t)(((uint64_t)(size_t)p * 0xC2B2AE3D27D4EB4Full) >> 40) & (filter_bits - 1);
}

// Fills the map with the registered pairs, the ones that have a canonical string, which it copies, in buffers that it keeps for the next time.
// Returns false when memory runs out.
static bool string_dedup_redirects_create (string_dedup_redirects* map, const string_dedup_pair* pairs, size_t pair_count, size_t registered)
{
    size_t slot_count = registered + (registered / 2) + 16;
    size_t filter_bits = 1024;
    while (filter_bits < (registered * 16))
    {
        filter_bits *= 2;
    }

    if (map->slot_capacity < slot_count)
    {
        string_dedup_delete (map->slots, map->slot_capacity);
        map->slots = string_dedup_new<string_dedup_redirect_slot> (slot_count);
        map->slot_capacity = (map->slots != nullptr) ? slot_count : 0;
    }

    if (map->filter_capacity < filter_bits)
    {
        string_dedup_delete (map->filter, map->filter_capacity / 64);
        map->filter = string_dedup_new<uint64_t> (filter_bits / 64);
        map->filter_capacity = (map->filter != nullptr) ? filter_bits : 0;
    }

    if ((map->slot_capacity < slot_count) || (map->filter_capacity < filter_bits))
    {
        return false;
    }

    memset (map->slots, 0, slot_count * sizeof (string_dedup_redirect_slot));
    memset (map->filter, 0, filter_bits / 8);
    map->slot_count = slot_count;
    map->filter_bits = filter_bits;
    // The slot and the filter word of a duplicate are far from the ones of the one before, so the ones of a pair that is a few ahead are prefetched (see the finder).
    const size_t ahead = 8;
    for (size_t i = 0; i < pair_count; i++)
    {
        if ((i + ahead) < pair_count)
        {
            uint8_t* next = pairs[i + ahead].duplicate;
            Prefetch ((void*)&map->slots[string_dedup_redirect_slot_of (next, slot_count)]);
            Prefetch ((void*)&map->filter[string_dedup_redirect_filter_bit (next, filter_bits) / 64]);
        }

        if (pairs[i].canonical == nullptr)
        {
            continue;
        }

        size_t index = string_dedup_redirect_slot_of (pairs[i].duplicate, slot_count);
        while (map->slots[index].duplicate != nullptr)
        {
            index = (index + 1 == slot_count) ? 0 : (index + 1);
        }

        map->slots[index].duplicate = pairs[i].duplicate;
        map->slots[index].canonical = pairs[i].canonical;

        size_t bit = string_dedup_redirect_filter_bit (pairs[i].duplicate, filter_bits);
        map->filter[bit / 64] |= (uint64_t)1 << (bit % 64);
    }

    return true;
}

// The slot of a registered duplicate, nullptr for any other address.
static inline string_dedup_redirect_slot* string_dedup_redirects_find (const string_dedup_redirects* map, uint8_t* o)
{
    size_t bit = string_dedup_redirect_filter_bit (o, map->filter_bits);
    if ((map->filter[bit / 64] & ((uint64_t)1 << (bit % 64))) == 0)
    {
        return nullptr;
    }

    for (size_t index = string_dedup_redirect_slot_of (o, map->slot_count); map->slots[index].duplicate != nullptr; index = (index + 1 == map->slot_count) ? 0 : (index + 1))
    {
        if (map->slots[index].duplicate == o)
        {
            return &map->slots[index];
        }
    }

    return nullptr;
}

static void string_dedup_merge_fire (const string_dedup_merge_stats* stats)
{
    if (!GCEventEnabledStringDedupMerge_V3())
    {
        return;
    }

    GCEventFireStringDedupMerge_V3 (
        stats->gc_index,
        stats->status,
        stats->condemned_generation,
        stats->pairs,
        stats->skipped_rooted,
        stats->dropped,
        stats->registered,
        stats->merged,
        stats->merged_bytes,
        stats->references,
        stats->unreferenced,
        stats->elapsed_us,
        stats->compacted,
        stats->pair_bytes,
        stats->upgraded,
        stats->bgc,
        stats->bgc_marked
    );
}

#endif //USE_REGIONS

// Starts the statistics of the finder for a GC that condemns gen1 or gen2, and removes the strings that died from the table, which only a GC that
// condemns gen2 can tell. 'owner' is the queue that has the table.
static void string_dedup_find_start (string_dedup_queue_state* owner, uint64_t gc_index, int condemned_gen_number)
{
    owner->find = {};
    owner->find.gc_index = gc_index;
    owner->find.condemned_generation = (uint64_t)condemned_gen_number;

    if ((condemned_gen_number == max_generation) && (owner->table != nullptr))
    {
        uint64_t start_time = GetHighPrecisionTimeStamp();
        owner->find.died = string_dedup_table_remove_if (owner->table, string_dedup_check_marked, 0);
        string_dedup_table_shrink (owner->table);
        owner->find.elapsed_us = GetHighPrecisionTimeStamp() - start_time;
    }
}

// Runs right after marking in a blocking GC that condemns gen1, when the queue has been filled and the plan phase has not changed anything. It
// consumes the candidates of the queue 'source' into the table of the queue 'owner', which is the same queue unless a server GC shares the table.
// 'roots' are the strings that the roots lead to, nullptr when there are none, 'deadline' is when the finder has to stop (see string_dedup_find_deadline).
static void string_dedup_find_after_mark (string_dedup_queue_state* owner, string_dedup_queue_state* source, string_dedup_rooted_set* roots, uint64_t deadline)
{
    string_dedup_find_stats* stats = &owner->find;
    uint64_t start_time = GetHighPrecisionTimeStamp();

    if (owner->table == nullptr)
    {
        owner->table = string_dedup_table_create();
    }

    if (owner->table == nullptr)
    {
        stats->status = 1;
    }
    else
    {
        string_dedup_find_consume (source, owner, roots, deadline);

        // This also consumed the entries that an earlier GC could not, because the table could not be created then, so none is old anymore.
        source->first_new = 0;
    }

    stats->elapsed_us += GetHighPrecisionTimeStamp() - start_time;
}

// Runs after the plan phase of a GC that ran the finder or that can have left strings of the table in a younger generation.
static void string_dedup_find_report (string_dedup_queue_state* queue, bool queued, bool check_all)
{
    string_dedup_find_stats* stats = &queue->find;
    string_dedup_table* table = queue->table;

    if (table != nullptr)
    {
        uint64_t start_time = GetHighPrecisionTimeStamp();
        size_t first = check_all ? 0 : string_dedup_table_added_start (table, stats);
        stats->demoted = string_dedup_table_remove_if (table, string_dedup_check_in_gen2, first);
        stats->elapsed_us += GetHighPrecisionTimeStamp() - start_time;
    }

    if (queued || (stats->demoted != 0))
    {
        string_dedup_find_fire (stats, (table != nullptr) ? table->live : 0);
    }
}

bool gc_heap::string_dedup_queue_enabled()
{
    return GCConfig::GetStringDedupQueue() || string_dedup_find_enabled();
}

string_dedup_queue_state* gc_heap::string_dedup_owner()
{
#ifdef MULTIPLE_HEAPS
    return g_heaps[0]->string_dedup_queue;
#else //MULTIPLE_HEAPS
    return string_dedup_queue;
#endif //MULTIPLE_HEAPS
}

// Runs right after marking in a blocking GC that condemns gen1.
void gc_heap::string_dedup_queue_after_mark (int condemned_gen_number)
{
    assert (!settings.concurrent);

    if (string_dedup_queue == nullptr)
    {
        string_dedup_queue = new (nothrow) string_dedup_queue_state();
    }

    string_dedup_queue_state* queue = string_dedup_queue;
    if (queue != nullptr)
    {
        queue->gc = {};
        queue->gc.gc_index = (uint64_t)settings.gc_index;
        queue->gc.condemned_generation = (uint64_t)condemned_gen_number;
        queue->gc.heap_index = (uint64_t)heap_number;
        queue->gc.report_pending = true;

#ifndef MULTIPLE_HEAPS
        string_dedup_find_start (queue, (uint64_t)settings.gc_index, condemned_gen_number);
#ifdef USE_REGIONS
        queue->pair_count = 0;
        queue->merge = {};
        queue->merge.gc_index = (uint64_t)settings.gc_index;
        queue->merge.condemned_generation = (uint64_t)condemned_gen_number;
#ifdef BACKGROUND_GC
        // A GC that sees the background GC marking keeps seeing it (the background GC needs this GC to end before its final mark). One that sees it
        // sweeping can see it end, which changes nothing: both mean that there is nothing to mark.
        queue->merge.bgc = (current_c_gc_state == c_gc_state_marking) ? 1 : ((current_c_gc_state == c_gc_state_planning) ? 2 : 0);
#endif //BACKGROUND_GC
#endif //USE_REGIONS
#endif //!MULTIPLE_HEAPS

        if (!string_dedup_queue_reserve (queue))
        {
            queue->gc.status = 1;
        }
        else
        {
            uint64_t start_time = GetHighPrecisionTimeStamp();

            if (condemned_gen_number == max_generation)
            {
                queue->gc.died = string_dedup_queue_remove_if (queue, string_dedup_check_marked);
            }

            queue->first_new = queue->count;

            if (settings.promotion)
            {
#ifdef USE_REGIONS
                for (heap_segment* region = generation_start_segment (generation_of (1)); region != nullptr; region = heap_segment_next (region))
                {
                    string_dedup_queue_add_range (heap_segment_mem (region), heap_segment_allocated (region), queue);
                }
#else //USE_REGIONS
                string_dedup_queue_add_range (generation_allocation_start (generation_of (1)), generation_allocation_start (generation_of (0)), queue);
#endif //USE_REGIONS
            }

            queue->gc.elapsed_us = GetHighPrecisionTimeStamp() - start_time;

#ifndef MULTIPLE_HEAPS
            if (string_dedup_find_enabled())
            {
#ifdef USE_REGIONS
                string_dedup_rooted_set* roots = string_dedup_roots;
                if (string_dedup_merge_enabled() && (!string_dedup_pairs_reserve (queue, queue->count + 1) || string_dedup_roots_incomplete || ((roots != nullptr) && roots->incomplete)))
                {
                    queue->merge.status = 3;
                }
#else //USE_REGIONS
                string_dedup_rooted_set* roots = nullptr;
#endif //USE_REGIONS

                string_dedup_find_after_mark (queue, queue, roots, string_dedup_find_deadline());
            }
#endif //!MULTIPLE_HEAPS
        }
    }

#if defined(USE_REGIONS) && !defined(MULTIPLE_HEAPS)
    // The strings that the roots noted have been used by now.
    string_dedup_rooted_clear (string_dedup_roots);
    string_dedup_roots_incomplete = false;
#endif //USE_REGIONS && !MULTIPLE_HEAPS

#ifdef MULTIPLE_HEAPS
    // Planning overwrites dead objects, so no heap may start it before every heap has checked its entries, which can point into other
    // heaps. All heaps have to get here, whatever happened above.
    gc_t_join.join (this, gc_join_string_dedup_queue);
    if (gc_t_join.joined())
    {
        // The finder has one table for all the heaps, which belongs to the queue of heap 0. This thread consumes the queues of all the heaps while the other
        // threads wait, in the order of the heaps, and the marks are final. The strings that the roots of the heaps noted are in one set for the merge.
        string_dedup_queue_state* owner = string_dedup_owner();
        if (string_dedup_find_enabled() && (owner != nullptr))
        {
            string_dedup_find_start (owner, (uint64_t)settings.gc_index, condemned_gen_number);

#ifdef USE_REGIONS
            string_dedup_rooted_set* roots = g_heaps[0]->string_dedup_roots;
            if (string_dedup_merge_enabled())
            {
                owner->pair_count = 0;
                owner->merge = {};
                owner->merge.gc_index = (uint64_t)settings.gc_index;
                owner->merge.condemned_generation = (uint64_t)condemned_gen_number;
#ifdef BACKGROUND_GC
                owner->merge.bgc = (current_c_gc_state == c_gc_state_marking) ? 1 : ((current_c_gc_state == c_gc_state_planning) ? 2 : 0);
#endif //BACKGROUND_GC

                size_t candidates = 0;
                for (int i = 0; i < n_heaps; i++)
                {
                    string_dedup_queue_state* source = g_heaps[i]->string_dedup_queue;
                    candidates += (source != nullptr) ? source->count : 0;

                    string_dedup_rooted_set* from = (i == 0) ? nullptr : g_heaps[i]->string_dedup_roots;
                    if (from != nullptr)
                    {
                        if (roots == nullptr)
                        {
                            roots = new (nothrow) string_dedup_rooted_set();
                            g_heaps[0]->string_dedup_roots = roots;
                        }

                        if (roots != nullptr)
                        {
                            string_dedup_rooted_move (roots, from);
                        }
                        else
                        {
                            string_dedup_roots_incomplete = true;
                        }
                    }
                }

                if (!string_dedup_pairs_reserve (owner, candidates + 1) || string_dedup_roots_incomplete || ((roots != nullptr) && roots->incomplete))
                {
                    owner->merge.status = 3;
                }
                else if (owner->merge.bgc != 0)
                {
                    // The merge of a server GC does not mark the canonical strings for a background GC yet, so a GC that overlaps one does not merge.
                    owner->merge.status = 4;
                }
            }
#else //USE_REGIONS
            string_dedup_rooted_set* roots = nullptr;
#endif //USE_REGIONS

            // The time of the finder is for all the heaps together, the ones that come after the time is spent have all their candidates skipped.
            uint64_t deadline = string_dedup_find_deadline();
            for (int i = 0; i < n_heaps; i++)
            {
                string_dedup_queue_state* source = g_heaps[i]->string_dedup_queue;
                if (source != nullptr)
                {
                    string_dedup_find_after_mark (owner, source, roots, deadline);
                }
            }
        }

#ifdef USE_REGIONS
        // The strings that the roots noted have been used by now, whatever happened above.
        for (int i = 0; i < n_heaps; i++)
        {
            string_dedup_rooted_clear (g_heaps[i]->string_dedup_roots);
        }

        string_dedup_roots_incomplete = false;
#endif //USE_REGIONS

        gc_t_join.restart();
    }
#endif //MULTIPLE_HEAPS
}

// Runs in the relocate phase of a compacting GC, like the relocation of the finalization queue.
void gc_heap::string_dedup_queue_relocate()
{
    string_dedup_queue_state* queue = string_dedup_queue;
    if ((queue == nullptr) || !queue->gc.report_pending)
    {
        return;
    }

    ScanContext sc;
    sc.promotion = FALSE;
#ifdef MULTIPLE_HEAPS
    sc.thread_number = heap_number;
    sc.thread_count = n_heaps;
#else //MULTIPLE_HEAPS
    sc.thread_count = 1;
#endif //MULTIPLE_HEAPS

    // A gen1 GC does not move the strings of gen2, so it only relocates the entries it queued itself.
    bool gen2_p = (settings.condemned_generation == max_generation);
    uint64_t start_time = GetHighPrecisionTimeStamp();

    for (size_t i = (gen2_p ? 0 : queue->first_new); i < queue->count; i++)
    {
        uint8_t* old_address = queue->entries[i].obj;
        GCHeap::Relocate ((Object**)&queue->entries[i].obj, &sc);
        if (queue->entries[i].obj != old_address)
        {
            queue->gc.relocated++;
        }
    }

    queue->gc.elapsed_us += GetHighPrecisionTimeStamp() - start_time;

    // The table is relocated by the thread of the heap whose queue has it, which is heap 0 in a server GC. Every plan is done when the relocate phase starts.
    if (queue->table != nullptr)
    {
        start_time = GetHighPrecisionTimeStamp();
        string_dedup_table_relocate (queue->table, &sc, gen2_p ? 0 : string_dedup_table_added_start (queue->table, &queue->find));
        queue->find.elapsed_us += GetHighPrecisionTimeStamp() - start_time;
    }
}

#ifdef USE_REGIONS

// Called by GCHeap::Promote for the roots that are on a stack or in a register and for every byref, with the object that they lead to. The thread of every
// heap notes what it finds in a set of its heap, so the threads of a server GC do not share one.
void gc_heap::string_dedup_note_root (uint8_t* o, int thread_number)
{
    // A GC that does not condemn gen1 does not look for duplicates.
    if (!string_dedup_merge_enabled() || (settings.condemned_generation < 1) || !string_dedup_is_string (((CObjectHeader*)o)->GetMethodTable()))
    {
        return;
    }

#ifdef MULTIPLE_HEAPS
    if ((thread_number < 0) || (thread_number >= n_heaps))
    {
        string_dedup_roots_incomplete = true;

        return;
    }

    string_dedup_rooted_set** set = &g_heaps[thread_number]->string_dedup_roots;
#else //MULTIPLE_HEAPS
    UNREFERENCED_PARAMETER (thread_number);
    string_dedup_rooted_set** set = &string_dedup_roots;
#endif //MULTIPLE_HEAPS
    if (*set == nullptr)
    {
        *set = new (nothrow) string_dedup_rooted_set();
        if (*set == nullptr)
        {
            string_dedup_roots_incomplete = true;

            return;
        }
    }

    string_dedup_rooted_add (*set, o);
}

// Runs at the start of the relocate phase of a GC that compacts, before any reference is relocated, and registers the pairs that can be merged. In a server
// GC one thread does it for all the heaps while the others wait, once every plan is done.
// The duplicate has to be in a region that this GC condemns, because only then does it see every reference to it, which holds for the strings that
// leave gen1, and the canonical string has to end up in gen2 without being demoted, because it is removed from the table otherwise. While a
// background GC marks, every canonical string that is registered is marked for it (see the comment at the top of the merge).
void gc_heap::string_dedup_merge_begin()
{
    THREAD_FROM_HEAP;
    string_dedup_queue_state* queue = string_dedup_owner();
    if ((queue == nullptr) || (queue->pairs == nullptr) || !queue->gc.report_pending)
    {
        return;
    }

    string_dedup_merge_stats* stats = &queue->merge;
    if (stats->status != 0)
    {
        return;
    }

    if (queue->pair_count == 0)
    {
        return;
    }

    uint64_t start_time = GetHighPrecisionTimeStamp();

    // The duplicates of a content are often together, so the canonical string of the previous pair is likely to be the one of this pair.
    uint8_t* last_canonical = nullptr;
    uint8_t* last_relocated = nullptr;
    size_t registered = 0;
    for (size_t i = 0; i < queue->pair_count; i++)
    {
        string_dedup_pair* pair = &queue->pairs[i];
        assert (get_region_gen_num (pair->duplicate) <= settings.condemned_generation);
        if (pair->canonical != last_canonical)
        {
            last_canonical = pair->canonical;
            last_relocated = last_canonical;
            relocate_address (&last_relocated THREAD_NUMBER_ARG);
            if ((get_region_plan_gen_num (last_relocated) != max_generation) || is_region_demoted (last_relocated))
            {
                last_relocated = nullptr;
            }
#ifdef BACKGROUND_GC
            // Only the bit is set, by address: the header of a string in front of a pinned plug can be overwritten by the plug's saved info.
            else if ((current_c_gc_state == c_gc_state_marking) &&
                     background_mark (last_canonical, background_saved_lowest_address, background_saved_highest_address))
            {
                stats->bgc_marked++;
            }
#endif //BACKGROUND_GC
        }

        uint8_t* canonical = last_relocated;
        pair->canonical = canonical;
        if (canonical != nullptr)
        {
            registered++;
            stats->registered_bytes += pair->bytes;
        }
        else
        {
            stats->dropped++;
        }
    }

    stats->registered = registered;
    if (registered != 0)
    {
        if (!string_dedup_redirects_create (&queue->redirects, queue->pairs, queue->pair_count, registered))
        {
            stats->status = 3;
            stats->registered = 0;
            stats->registered_bytes = 0;
        }
        else
        {
            // From now on relocate_address replaces the references to the registered duplicates.
            string_dedup_replaced_reset();
            string_dedup_redirect_map = &queue->redirects;
        }
    }

    stats->elapsed_us += GetHighPrecisionTimeStamp() - start_time;
}

// Called by relocate_address for every reference that it relocates while the merge is active. Returns true when it replaced the reference.
bool gc_heap::string_dedup_redirect (uint8_t** pold_address)
{
    string_dedup_redirect_slot* slot = string_dedup_redirects_find (string_dedup_redirect_map, *pold_address);
    if (slot == nullptr)
    {
        return false;
    }

    size_t canonical = (size_t)slot->canonical;
    *pold_address = (uint8_t*)(canonical & ~(size_t)1);
    if ((canonical & 1) == 0)
    {
        // The threads of a server GC that are the first to replace a reference to the duplicate all store the same value.
        slot->canonical = (uint8_t*)(canonical | 1);
    }

#ifdef MULTIPLE_HEAPS
    Interlocked::ExchangeAddPtr (&string_dedup_replaced[((size_t)pold_address >> 7) & (string_dedup_counter_stripes - 1)].value, (size_t)1);
#else //MULTIPLE_HEAPS
    string_dedup_replaced++;
#endif //MULTIPLE_HEAPS

    return true;
}

// Runs at the end of the relocate phase of a GC that compacts: the references are replaced, so the registrations are dropped and the
// duplicates are counted. In a server GC one thread does it for all the heaps, once all of them are done relocating.
void gc_heap::string_dedup_merge_end()
{
    string_dedup_redirects* map = string_dedup_redirect_map;
    if (map == nullptr)
    {
        return;
    }

    string_dedup_redirect_map = nullptr;

    string_dedup_queue_state* queue = string_dedup_owner();
    string_dedup_merge_stats* stats = &queue->merge;
    uint64_t start_time = GetHighPrecisionTimeStamp();

    for (size_t i = 0; i < map->slot_count; i++)
    {
        string_dedup_redirect_slot* slot = &map->slots[i];
        if (slot->duplicate == nullptr)
        {
            continue;
        }

        if (((size_t)slot->canonical & 1) != 0)
        {
            stats->merged++;
        }
        else
        {
            stats->unreferenced++;
        }
    }

    // A duplicate without a reference is a failure that the reader reports, so the size of the registered duplicates is what was merged.
    stats->merged_bytes = stats->registered_bytes;
    stats->references = string_dedup_replaced_total();
    stats->elapsed_us += GetHighPrecisionTimeStamp() - start_time;

    // The buffers stay for the next GC that merges, unless this GC had so many duplicates that they are big.
    if (map->slot_capacity > string_dedup_redirects_keep_slots)
    {
        string_dedup_delete (map->slots, map->slot_capacity);
        map->slots = nullptr;
        map->slot_capacity = 0;
        map->slot_count = 0;
        string_dedup_delete (map->filter, map->filter_capacity / 64);
        map->filter = nullptr;
        map->filter_capacity = 0;
        map->filter_bits = 0;
    }
}

// Called by decide_on_compacting for a GC that is going to sweep, after every other reason to compact. A sweeping GC merges nothing, so a gen1 GC
// compacts when the duplicates that the finder recorded in it are worth GCStringDedupCompactMinKB or more. When this returns true it has recorded
// that the GC compacts for the merge (the report clears that if the GC did not compact after all).
bool gc_heap::string_dedup_compaction_pays()
{
    int64_t min_kb = GCConfig::GetStringDedupCompactMinKB();
    string_dedup_queue_state* queue = string_dedup_owner();
    if ((min_kb <= 0) || !string_dedup_merge_enabled() || (queue == nullptr) || (queue->pairs == nullptr) || !queue->gc.report_pending)
    {
        return false;
    }

    string_dedup_merge_stats* stats = &queue->merge;
    if ((settings.condemned_generation != 1) || (stats->status != 0) || ((int64_t)(stats->pair_bytes >> 10) < min_kb))
    {
        return false;
    }

    stats->upgraded = 1;

    return true;
}

// Runs after the plan phase of a GC that ran the finder: reports whether the GC compacted and fires the StringDedupMerge event. 'owner' is the queue that has
// the state of the merge.
void gc_heap::string_dedup_merge_report (string_dedup_queue_state* owner)
{
    string_dedup_merge_stats* merge = &owner->merge;
    merge->compacted = settings.compaction ? 1 : 0;
    // A veto that came after string_dedup_compaction_pays asked for a compaction leaves a GC that did not compact.
    merge->upgraded &= merge->compacted;
    if (!settings.compaction && (merge->status == 0))
    {
        merge->status = 1;
    }

    string_dedup_merge_fire (merge);

#if defined(BACKGROUND_GC) && defined(_DEBUG)
    // A canonical string that was marked for the background GC has its mark bit where the compaction put it.
    if ((merge->bgc == 1) && ((merge->status == 0) || (merge->status == 3)))
    {
        for (size_t i = 0; i < owner->pair_count; i++)
        {
            assert ((owner->pairs[i].canonical == nullptr) || background_object_marked (owner->pairs[i].canonical, FALSE));
        }
    }
#endif //BACKGROUND_GC && _DEBUG

    owner->pair_count = 0;

    // The pairs are given back too when this GC had so many duplicates that they are big.
    if (owner->pair_capacity > string_dedup_pairs_keep)
    {
        string_dedup_delete (owner->pairs, owner->pair_capacity);
        owner->pairs = nullptr;
        owner->pair_capacity = 0;
    }
}

#endif //USE_REGIONS

// Runs after the plan phase of every non-concurrent GC.
void gc_heap::string_dedup_queue_report()
{
#ifdef MULTIPLE_HEAPS
    // The strings that a heap queued or the table has can be in the regions of another heap, and those have their final generations only when that heap
    // is done with the plan phase, so the heaps wait for each other here, and the table, which they share, is checked by one thread. Every heap has to join
    // when the GC ran the finder, whatever its queue looks like.
    if ((settings.condemned_generation >= 1) && string_dedup_find_enabled())
    {
        gc_t_join.join (this, gc_join_string_dedup_report);
        if (gc_t_join.joined())
        {
            string_dedup_queue_state* owner = string_dedup_owner();
            if (owner != nullptr)
            {
                bool owner_queued = owner->gc.report_pending;
                string_dedup_find_report (owner, owner_queued, (settings.condemned_generation == max_generation));
#ifdef USE_REGIONS
                if (owner_queued && string_dedup_merge_enabled())
                {
                    string_dedup_merge_report (owner);
                }
#endif //USE_REGIONS
            }

            string_dedup_native_restart_peak();
            gc_t_join.restart();
        }
    }
#endif //MULTIPLE_HEAPS

    string_dedup_queue_state* queue = string_dedup_queue;
    if (queue == nullptr)
    {
        return;
    }

    // The GC leaves the strings that are in or next to a pinned plug where they are, so a string can end up in a younger generation than
    // it was in. Gen1 and gen0 GCs do not look at entries of gen2, so such entries cannot be kept. A gen2 GC can do this to any entry
    // and a gen1 GC to the ones it queued. Without regions a GC of any generation can demote a whole segment when the heap expands.
    bool queued = queue->gc.report_pending;
    bool check_all = (settings.condemned_generation == max_generation);
#ifndef USE_REGIONS
    check_all |= settings.demotion;
#endif //!USE_REGIONS
    if (!queued && !check_all)
    {
        return;
    }

    string_dedup_queue_stats* stats = &queue->gc;
    if (!queued)
    {
        *stats = {};
        stats->gc_index = (uint64_t)settings.gc_index;
        stats->condemned_generation = (uint64_t)settings.condemned_generation;
        stats->heap_index = (uint64_t)heap_number;
#ifndef MULTIPLE_HEAPS
        queue->find = {};
        queue->find.gc_index = stats->gc_index;
        queue->find.condemned_generation = stats->condemned_generation;
#endif //!MULTIPLE_HEAPS
    }

    stats->report_pending = false;

    uint64_t start_time = GetHighPrecisionTimeStamp();
    size_t first = check_all ? 0 : queue->first_new;
    size_t kept = first;
    for (size_t i = first; i < queue->count; i++)
    {
        if (string_dedup_test::in_gen2 (queue->entries[i].obj))
        {
            queue->entries[kept++] = queue->entries[i];
        }
    }

    stats->demoted = queue->count - kept;
    queue->count = kept;
    stats->elapsed_us += GetHighPrecisionTimeStamp() - start_time;

#ifndef MULTIPLE_HEAPS
    if (string_dedup_find_enabled())
    {
        string_dedup_find_report (queue, queued, check_all);
    }

#ifdef USE_REGIONS
    if (queued && string_dedup_merge_enabled())
    {
        string_dedup_merge_report (queue);
    }
#endif //USE_REGIONS

    if (string_dedup_find_enabled())
    {
        string_dedup_native_restart_peak();
    }
#endif //!MULTIPLE_HEAPS

    if (queued || (stats->demoted != 0))
    {
        string_dedup_queue_fire (stats, queue->count);
    }
}

#ifdef BACKGROUND_GC
// Runs in the final mark of a background GC, when the mark array tells which strings are alive. The sweep that follows frees the other
// strings of gen2 without passing through the other hooks, so their entries are removed here. Every heap does this before the heaps join
// at the end of the final mark, so no heap can sweep before all entries are checked. The EE is suspended, which keeps blocking GCs from
// using the queue at the same time.
void gc_heap::string_dedup_queue_background_mark()
{
    string_dedup_queue_state* queue = string_dedup_queue;
    if (queue == nullptr)
    {
        return;
    }

    assert (settings.concurrent);
    string_dedup_queue_stats stats = {};
    stats.gc_index = (uint64_t)settings.gc_index;
    stats.condemned_generation = (uint64_t)max_generation;
    stats.heap_index = (uint64_t)heap_number;
    stats.background = 1;

    uint64_t start_time = GetHighPrecisionTimeStamp();
    stats.died = string_dedup_queue_remove_if (queue, string_dedup_check_bgc_marked);
    stats.elapsed_us = GetHighPrecisionTimeStamp() - start_time;

    string_dedup_queue_fire (&stats, queue->count);

    // The table is pruned by the thread of the heap whose queue has it, which is heap 0 in a server GC, before the other threads get to the join.
    if (queue->table != nullptr)
    {
        string_dedup_find_stats find = {};
        find.gc_index = stats.gc_index;
        find.condemned_generation = stats.condemned_generation;
        find.background = 1;

        start_time = GetHighPrecisionTimeStamp();
        find.died = string_dedup_table_remove_if (queue->table, string_dedup_check_bgc_marked, 0);
        string_dedup_table_shrink (queue->table);
        find.elapsed_us = GetHighPrecisionTimeStamp() - start_time;

        string_dedup_find_fire (&find, queue->table->live);
        string_dedup_native_restart_peak();
    }
}
#endif //BACKGROUND_GC

#ifdef DYNAMIC_HEAP_COUNT
// Moves the entries of a heap that goes out of service to this heap. The entries that do not fit are dropped, they are only candidates.
void gc_heap::string_dedup_queue_merge (gc_heap* from_hp)
{
    string_dedup_queue_state* from = from_hp->string_dedup_queue;
    if ((from == nullptr) || (from->count == 0))
    {
        return;
    }

    if (string_dedup_queue == nullptr)
    {
        string_dedup_queue = new (nothrow) string_dedup_queue_state();
    }

    string_dedup_queue_state* to = string_dedup_queue;
    if ((to != nullptr) && string_dedup_queue_reserve (to))
    {
        while (((to->capacity - to->count) < from->count) && string_dedup_queue_grow (to))
        {
        }

        size_t room = to->capacity - to->count;
        size_t moved = (from->count < room) ? from->count : room;
        memcpy (&to->entries[to->count], from->entries, moved * sizeof (string_dedup_candidate));
        to->count += moved;
    }

    from->count = 0;
    string_dedup_queue_shrink (from);
}
#endif //DYNAMIC_HEAP_COUNT

#ifdef VERIFY_HEAP
// The strings that GC stress allocates are chopped in place, which changes their length and content.
static bool string_dedup_verify_content_p()
{
#if defined(STRESS_HEAP) && !defined(FEATURE_NATIVEAOT)
    return !GCStress<cfg_any>::IsEnabled();
#else
    return true;
#endif //STRESS_HEAP && !FEATURE_NATIVEAOT
}

// Every string of the table must be a string of gen2 that still has the content it was added for, and the index must have exactly one slot for
// every string that was added since the table was rebuilt, the removed ones included.
static void string_dedup_table_verify (string_dedup_table* table)
{
    bool verify_content_p = string_dedup_verify_content_p();
    if ((table->count > table->capacity) || (table->capacity > table->max_capacity) || (table->live > table->count) ||
        ((table->slot_count & (table->slot_count - 1)) != 0) || (table->slot_count > table->max_slot_count) ||
        ((table->capacity * 4) > (table->slot_count * 3)))
    {
        FATAL_GC_ERROR();
    }

    size_t live = 0;
    for (size_t i = 0; i < table->count; i++)
    {
        uint8_t* o = table->strings[i];
        if (o == nullptr)
        {
            continue;
        }

        live++;
        if ((o < g_gc_lowest_address) || (o >= g_gc_highest_address) || (((size_t)o & (sizeof (void*) - 1)) != 0))
        {
            FATAL_GC_ERROR();
        }

        CObjectHeader* str = (CObjectHeader*)o;
        if (!string_dedup_is_string (str->GetMethodTable()) || (g_theGCHeap->WhichGeneration ((Object*)o) != max_generation))
        {
            FATAL_GC_ERROR();
        }

        size_t free_slot = 0;
        if (verify_content_p)
        {
            string_dedup_slot* slot = string_dedup_table_find (table, table->hashes[i], str, str->GetNumComponents(), &free_slot);
            if ((table->hashes[i] != string_dedup_table_hash (table, str)) || (slot == nullptr) || (slot->entry != (i + 1)))
            {
                FATAL_GC_ERROR();
            }
        }
    }

    size_t used_slots = 0;
    size_t live_slots = 0;
    for (size_t i = 0; i < table->slot_count; i++)
    {
        uint32_t entry = table->slots[i].entry;
        if (entry == 0)
        {
            continue;
        }

        used_slots++;
        if ((entry > table->count) || (table->slots[i].hash != table->hashes[entry - 1]))
        {
            FATAL_GC_ERROR();
        }

        if (table->strings[entry - 1] != nullptr)
        {
            live_slots++;
        }
    }

    if ((live != table->live) || (used_slots != table->count) || (live_slots != live))
    {
        FATAL_GC_ERROR();
    }
}

// The bytes that the buffers of a queue, of its table and of the merge, and of a set of the strings that the roots lead to, add up to.
static size_t string_dedup_native_bytes_of (const string_dedup_queue_state* queue, const string_dedup_rooted_set* roots)
{
    size_t bytes = 0;
    if (queue != nullptr)
    {
        bytes += queue->capacity * sizeof (string_dedup_candidate);
        if (queue->table != nullptr)
        {
            bytes += (queue->table->capacity * (sizeof (uint8_t*) + sizeof (uint32_t))) + (queue->table->slot_count * sizeof (string_dedup_slot));
        }

#ifdef USE_REGIONS
        bytes += queue->pair_capacity * sizeof (string_dedup_pair);
        bytes += (queue->redirects.slot_capacity * sizeof (string_dedup_redirect_slot)) + ((queue->redirects.filter_capacity / 64) * sizeof (uint64_t));
#endif //USE_REGIONS
    }

#ifdef USE_REGIONS
    if (roots != nullptr)
    {
        bytes += roots->slot_count * sizeof (uint8_t*);
    }
#else //USE_REGIONS
    UNREFERENCED_PARAMETER (roots);
#endif //USE_REGIONS

    return bytes;
}

// Every entry must still point at the string it was queued for, which fails when an entry was not relocated or kept after its string died,
// and the string must be in gen2, which fails when an entry was not removed after the GC demoted its string. The bytes that string_dedup_new and
// string_dedup_delete count must be the ones of the buffers that all the heaps have.
void gc_heap::string_dedup_queue_verify()
{
#ifdef MULTIPLE_HEAPS
    if (heap_number == 0)
    {
        size_t bytes = 0;
        for (int i = 0; i < n_max_heaps; i++)
        {
#ifdef USE_REGIONS
            bytes += string_dedup_native_bytes_of (g_heaps[i]->string_dedup_queue, g_heaps[i]->string_dedup_roots);
#else //USE_REGIONS
            bytes += string_dedup_native_bytes_of (g_heaps[i]->string_dedup_queue, nullptr);
#endif //USE_REGIONS
        }

        if (bytes != string_dedup_native_held)
        {
            FATAL_GC_ERROR();
        }
    }
#else //MULTIPLE_HEAPS
#ifdef USE_REGIONS
    size_t bytes = string_dedup_native_bytes_of (string_dedup_queue, string_dedup_roots);
#else //USE_REGIONS
    size_t bytes = string_dedup_native_bytes_of (string_dedup_queue, nullptr);
#endif //USE_REGIONS
    if (bytes != string_dedup_native_held)
    {
        FATAL_GC_ERROR();
    }
#endif //MULTIPLE_HEAPS

    string_dedup_queue_state* queue = string_dedup_queue;
    if (queue == nullptr)
    {
        return;
    }

    bool verify_content_p = string_dedup_verify_content_p();
    for (size_t i = 0; i < queue->count; i++)
    {
        uint8_t* o = queue->entries[i].obj;
        if ((o < g_gc_lowest_address) || (o >= g_gc_highest_address) || (((size_t)o & (sizeof (void*) - 1)) != 0))
        {
            FATAL_GC_ERROR();
        }

        CObjectHeader* str = (CObjectHeader*)o;
        if (!string_dedup_is_string (str->GetMethodTable()) || (verify_content_p && (string_dedup_signature (str) != queue->entries[i].signature)))
        {
            FATAL_GC_ERROR();
        }

        if (g_theGCHeap->WhichGeneration ((Object*)o) != max_generation)
        {
            FATAL_GC_ERROR();
        }
    }

    // The table belongs to the queue of one heap, which is heap 0 in a server GC.
    if (queue->table != nullptr)
    {
        string_dedup_table_verify (queue->table);
    }

#ifdef USE_REGIONS
    // The merge registers duplicates only while the relocate phase runs, and the strings that the roots note are consumed right after marking.
    if ((string_dedup_redirect_map != nullptr) || ((string_dedup_roots != nullptr) && (string_dedup_roots->count != 0)))
    {
        FATAL_GC_ERROR();
    }
#endif //USE_REGIONS
}
#endif //VERIFY_HEAP

#endif //FEATURE_EVENT_TRACE

} // namespace WKS/SVR

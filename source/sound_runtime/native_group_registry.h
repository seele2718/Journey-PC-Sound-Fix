/* Native group-use identities owned by the serialized control domain.
 * Generic AddGroup activation/finalization can run outside worker/update
 * serialization. Admission therefore uses the native worker unpause boundary;
 * finalizers retire tickets atomically rather than mutating this table.
 * System startup seeds the table before releasing the native semaphore.
 *
 * CONTROL THREAD ONLY. Mixer/END callbacks use native_channel_binding.h, not
 * this table. No FMOD calls, allocation, userdata replacement or DSP ownership
 * occur here. A new activation is a new identity even at the same address.
 * Keeping a pointer to a pooled group is not evidence that its old use lives.
 *
 * Tokens are globally increasing within this registry, including after table
 * slot reuse. Exhaustion rejects new routes rather than wrapping. Do not reset
 * the registry until every old DSP callback and borrower is gone.
 */
#ifndef JOURNEY_NATIVE_GROUP_REGISTRY_H
#define JOURNEY_NATIVE_GROUP_REGISTRY_H
#include <stdint.h>
#include <stddef.h>
#ifndef JOURNEY_GROUP_CAPACITY
#define JOURNEY_GROUP_CAPACITY 2048u
#endif
enum { JOURNEY_GROUP_SYSTEM=1, JOURNEY_GROUP_CUE=2, JOURNEY_GROUP_STREAM=3 };
typedef struct JourneyGroupIdentity {
    uintptr_t group, owner;
    uint32_t generation;
    uint64_t admission_ticket; /* optional native cue-use lifetime, owner domain */
    unsigned kind, state; /* 0 vacant, 1 live; no accumulated dead entries */
} JourneyGroupIdentity;
typedef struct JourneyGroupRegistry {
    JourneyGroupIdentity entries[JOURNEY_GROUP_CAPACITY];
    uint32_t serial;
    unsigned live, peak, rejected;
} JourneyGroupRegistry;

static inline size_t journey_group_hash(uintptr_t group) {
    group ^= group >> 17;
    group *= (uintptr_t)0xed5ad4bbu;
    group ^= group >> 11;
    return group % JOURNEY_GROUP_CAPACITY;
}
/* Bounded open addressing, no per-source or per-callback voice-table scan. */
static inline JourneyGroupIdentity *journey_group_find(JourneyGroupRegistry *r,
                                               uintptr_t group, int insert) {
    size_t index=journey_group_hash(group);
    for(size_t n=0;n<JOURNEY_GROUP_CAPACITY;n++) {
        JourneyGroupIdentity *e=&r->entries[index];
        if(e->state==1 && e->group==group)return e;
        if(!e->state)return insert?e:NULL;
        if(++index==JOURNEY_GROUP_CAPACITY)index=0;
    }
    return NULL;
}
/* Control-thread deletion keeps probe chains intact without leaving thousands
 * of tombstones after level turnover. No client retains an entry pointer;
 * identities are the copied global generation, not physical table positions. */
static inline void journey_group_erase(JourneyGroupRegistry *r,JourneyGroupIdentity *e) {
    size_t hole=(size_t)(e-r->entries),scan=(hole+1)%JOURNEY_GROUP_CAPACITY;
    e->state=0;e->generation=0;--r->live;
    for(size_t n=0;n<JOURNEY_GROUP_CAPACITY-1;n++) {
        JourneyGroupIdentity *candidate=&r->entries[scan];
        if(!candidate->state)break;
        size_t home=journey_group_hash(candidate->group);
        size_t to_hole=(hole+JOURNEY_GROUP_CAPACITY-home)%JOURNEY_GROUP_CAPACITY;
        size_t to_scan=(scan+JOURNEY_GROUP_CAPACITY-home)%JOURNEY_GROUP_CAPACITY;
        if(to_hole<to_scan) {
            r->entries[hole]=*candidate;
            candidate->state=0;candidate->generation=0;hole=scan;
        }
        scan=(scan+1)%JOURNEY_GROUP_CAPACITY;
    }
}
static inline uint32_t journey_group_generation(JourneyGroupRegistry *r,uintptr_t group) {
    JourneyGroupIdentity *e=group?journey_group_find(r,group,0):NULL;
    return e?e->generation:0;
}
static inline int journey_group_retire(JourneyGroupRegistry *r,uintptr_t group,
                                uintptr_t owner,uint32_t expected) {
    JourneyGroupIdentity *e=group?journey_group_find(r,group,0):NULL;
    if(!e || !expected || e->owner!=owner || e->generation!=expected)return 0;
    journey_group_erase(r,e);
    return 1;
}
/* Called at a VERIFIED new-use boundary, never on ordinary parent updates.
 * Failed native activation also invalidates its previous identity. It must
 * not let a later source borrow an old route with a stale parent/owner.
 * result is the unchanged native FMOD result; zero means success.
 */
static inline uint32_t journey_group_activate(JourneyGroupRegistry *r,uintptr_t group,
                                      uintptr_t owner,unsigned kind,int result) {
    if(!group || !owner || kind<JOURNEY_GROUP_SYSTEM || kind>JOURNEY_GROUP_STREAM) {
        ++r->rejected;return 0;
    }
    JourneyGroupIdentity *e=journey_group_find(r,group,1);
    if(result || !e || r->serial==UINT32_MAX) {
        if(e && e->state)journey_group_erase(r,e);
        ++r->rejected;return 0;
    }
    if(!e->state && ++r->live>r->peak)r->peak=r->live;
    e->group=group;e->owner=owner;e->kind=kind;e->generation=++r->serial;e->state=1;
    e->admission_ticket=0;
    return e->generation;
}
#endif

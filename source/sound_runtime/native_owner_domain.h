/* Native control-domain ownership of cue and group identities.
 *
 * Hash/route mutations occur only inside the existing worker/update semaphore
 * domain. Finalizers only CAS-clear their cue-use ticket, invalidating every
 * dependent cue/Stream identity without touching the hash or graph. Stale
 * entries are physically removed later by the serialized control owner.
 * Group generations remain globally unique in this registry. No native
 * userdata writes, allocations, FMOD calls, new queues or new locks.
 */
#ifndef JOURNEY_NATIVE_OWNER_DOMAIN_H
#define JOURNEY_NATIVE_OWNER_DOMAIN_H
#include "native_group_registry.h"
#include "native_cue_admission.h"
typedef struct JourneyOwnerDomain {
    JourneyGroupRegistry groups;
    JourneyCueAdmissions cues;
    void *manager; /* initialize once before admission; acquire-read by callbacks */
} JourneyOwnerDomain;
static inline int journey_domain_initialize(JourneyOwnerDomain *d,void *manager) {
    if(!manager)return 0;
    void *previous=__atomic_load_n(&d->manager,__ATOMIC_ACQUIRE);
    if(previous)return previous==manager;
    __atomic_store_n(&d->manager,manager,__ATOMIC_RELEASE);return 1;
}
static inline int journey_domain_entry_live(JourneyOwnerDomain *d,const JourneyGroupIdentity *e) {
    return e && e->state && (e->kind==JOURNEY_GROUP_SYSTEM ||
        (e->admission_ticket && journey_cue_live(&d->cues,e->admission_ticket)));
}
static inline uint32_t journey_domain_generation(JourneyOwnerDomain *d,uintptr_t group) {
    JourneyGroupIdentity *e=group?journey_group_find(&d->groups,group,0):NULL;
    return journey_domain_entry_live(d,e)?e->generation:0;
}
static inline void journey_domain_prune(JourneyOwnerDomain *d) {
    /* Erasure can move later entries back. Recheck the current slot rather
     * than skipping a moved stale entry. Each erase decreases live count. */
    for(unsigned i=0;i<JOURNEY_GROUP_CAPACITY;)
        if(d->groups.entries[i].state && !journey_domain_entry_live(d,&d->groups.entries[i]))
            journey_group_erase(&d->groups,&d->groups.entries[i]);
        else ++i;
}
static inline uint64_t journey_domain_cue_ticket(JourneyOwnerDomain *d,void *cue) {
    JourneyNativeCueUse use;
    void *manager=__atomic_load_n(&d->manager,__ATOMIC_ACQUIRE);
    return journey_cue_native_snapshot(cue,manager,&use)?journey_cue_capture(&d->cues,use.handle):0;
}
static inline uint64_t journey_domain_admit(JourneyOwnerDomain *d,void *cue) {
    JourneyNativeCueUse use;
    void *manager=__atomic_load_n(&d->manager,__ATOMIC_ACQUIRE);
    if(!journey_cue_native_snapshot(cue,manager,&use))return 0;
    uint64_t ticket=journey_cue_admit(&d->cues,use.handle);
    if(!ticket)return 0;
    JourneyGroupIdentity *e=journey_group_find(&d->groups,(uintptr_t)use.group,0);
    if(e && e->owner==(uintptr_t)cue && e->kind==JOURNEY_GROUP_CUE &&
       e->admission_ticket==ticket)return ticket;
    /* Native pooled cue group has a single owner/kind throughout its lifetime.
     * Refuse conflicting identities; never hijack another live native group. */
    if(journey_domain_entry_live(d,e))goto failure;
    if(d->groups.live==JOURNEY_GROUP_CAPACITY)journey_domain_prune(d);
    if(!journey_group_activate(&d->groups,(uintptr_t)use.group,(uintptr_t)cue,JOURNEY_GROUP_CUE,0))goto failure;
    e=journey_group_find(&d->groups,(uintptr_t)use.group,0);
    e->admission_ticket=ticket;return ticket;
failure:
    journey_cue_retire(&d->cues,ticket);return 0;
}
static inline uint32_t journey_domain_stream(JourneyOwnerDomain *d,uintptr_t group,
                                            uintptr_t stream,uintptr_t cue_group) {
    JourneyGroupIdentity *cue=journey_group_find(&d->groups,cue_group,0);
    if(!group || !stream || !journey_domain_entry_live(d,cue) || cue->kind!=JOURNEY_GROUP_CUE)return 0;
    uint64_t ticket=cue->admission_ticket;
    JourneyGroupIdentity *e=journey_group_find(&d->groups,group,0);
    /* Publication is a new protected Stream use, even if an FMOD group address
     * has been recycled for another Stream. Give it a new generation; never
     * overwrite a cue or System identity. Call only at native publication. */
    if(journey_domain_entry_live(d,e) && e->kind!=JOURNEY_GROUP_STREAM)return 0;
    if(d->groups.live==JOURNEY_GROUP_CAPACITY)journey_domain_prune(d);
    uint32_t generation=journey_group_activate(&d->groups,group,stream,JOURNEY_GROUP_STREAM,0);
    if(generation)journey_group_find(&d->groups,group,0)->admission_ticket=ticket;
    return generation;
}
/* Off-domain finalizer boundary: no table access, no reclamation. Native cue
 * is still protected and has not advanced its handle at the guarded stop hook. */
static inline int journey_domain_finalize(JourneyOwnerDomain *d,void *cue,void *group) {
    JourneyNativeCueUse use;
    void *manager=__atomic_load_n(&d->manager,__ATOMIC_ACQUIRE);
    if(!journey_cue_native_snapshot(cue,manager,&use) || use.group!=group)return 0;
    return journey_cue_retire(&d->cues,journey_cue_capture(&d->cues,use.handle));
}
#endif

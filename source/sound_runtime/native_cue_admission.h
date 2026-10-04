/* Native cue-use identities and atomic retirement tickets.
 *
 * The supported executable has 1,024 pooled cues. Native handle at cue+418
 * selects a pool slot modulo1,024 and advances by0x4000 on finalization.
 * The adapter must also verify manager->pool[slot] == cue. A handle is not
 * a pointer; failed activation is NOT a new admitted use.
 *
 * Only the existing worker/update serialization domain admits uses. Native
 * Start holds an activity reference through queued/active grain processing.
 * Off-domain finalizers capture an exact ticket BEFORE native stop/recycle,
 * then clear only that ticket. They do not edit a group hash, allocate, call
 * FMOD, reclaim DSP storage, lock, or wait. Control-side graph reclamation is
 * separate and still requires native DSP destruction acknowledgment.
 *
 * Tickets combine the native handle and a nonwrapping patch serial. Delayed
 * retirement of an already captured ticket cannot retire a newer use, even
 * across native handle wrap. This does not authorize capturing from arbitrary
 * stale cue pointers after native reuse. Initialization/reset requires all
 * prior callbacks to have stopped. Special/preloaded Streams use their own
 * publication boundaries; this helper does not admit them from callbacks.
 */
#ifndef JOURNEY_NATIVE_CUE_ADMISSION_H
#define JOURNEY_NATIVE_CUE_ADMISSION_H
#include <stdint.h>
#include <string.h>
enum { JOURNEY_NATIVE_CUE_SLOTS=1024 };
typedef struct JourneyCueAdmissions {
    uint64_t tickets[JOURNEY_NATIVE_CUE_SLOTS];
    uint32_t serial; /* serialized control owner only */
} JourneyCueAdmissions;
_Static_assert(__atomic_always_lock_free(8,0),"cue retirement requires lock-free64");
_Static_assert(sizeof(void *)==8,"native cue layout is Win64");

typedef struct JourneyNativeCueUse {
    void *cue,*group;
    uint32_t handle;
} JourneyNativeCueUse;
/* Read ONLY a native-hook-supplied cue with native lifetime protection. This
 * is not a general safe pointer probe and must not inspect arbitrary userdata.
 * Native resolver1402b6650: class bits0x1800, pool+2f0, uint32 count+308,
 * full-handle comparison. Manager identity comes from initialized audio state.
 * No guessed hierarchy traversal; cue+38 is self, NOT its parent.
 */
static inline int journey_cue_native_snapshot(void *cue,void *manager,
                                             JourneyNativeCueUse *out) {
    void *actual_manager=NULL,*pool=NULL,*slot=NULL,*group=NULL;
    uint32_t count=0,handle=0,flags=0;
    memset(out,0,sizeof(*out));
    if(!cue || !manager)return 0;
    memcpy(&actual_manager,(unsigned char *)cue+0x30,8);
    if(actual_manager!=manager)return 0;
    memcpy(&handle,(unsigned char *)cue+0x418,4);
    if((handle&0x3c00u)!=0x1800u)return 0;
    memcpy(&count,(unsigned char *)manager+0x308,4);
    memcpy(&pool,(unsigned char *)manager+0x2f0,8);
    if(count!=JOURNEY_NATIVE_CUE_SLOTS || !pool)return 0;
    memcpy(&slot,(unsigned char *)pool+8*(handle%count),8);
    if(slot!=cue)return 0;
    memcpy(&flags,cue,4);
    if(flags&0x10u)return 0; /* native available/reset state */
    memcpy(&group,(unsigned char *)cue+0xc0,8);
    if(!group)return 0;
    out->cue=cue;out->group=group;out->handle=handle;return 1;
}

static inline uint64_t journey_cue_capture(const JourneyCueAdmissions *a,
                                           uint32_t native_handle) {
    uint64_t ticket=__atomic_load_n(&a->tickets[native_handle%JOURNEY_NATIVE_CUE_SLOTS],
                                  __ATOMIC_ACQUIRE);
    return (uint32_t)(ticket>>32)==native_handle ? ticket : 0;
}
/* Called with a verified native pool identity and held native activity ref.
 * Repeated observations of the same admitted use are idempotent. A still-live
 * different generation is NOT displaced: flag it for integration diagnosis.
 */
static inline uint64_t journey_cue_admit(JourneyCueAdmissions *a,
                                         uint32_t native_handle) {
    unsigned slot=native_handle%JOURNEY_NATIVE_CUE_SLOTS;
    uint64_t old=__atomic_load_n(&a->tickets[slot],__ATOMIC_ACQUIRE);
    if(old)return (uint32_t)(old>>32)==native_handle ? old : 0;
    if(a->serial==UINT32_MAX)return 0;
    uint64_t ticket=((uint64_t)native_handle<<32)|++a->serial;
    __atomic_store_n(&a->tickets[slot],ticket,__ATOMIC_RELEASE);
    return ticket;
}
static inline int journey_cue_live(const JourneyCueAdmissions *a,uint64_t ticket) {
    if(!(uint32_t)ticket)return 0;
    unsigned slot=(uint32_t)(ticket>>32)%JOURNEY_NATIVE_CUE_SLOTS;
    return __atomic_load_n(&a->tickets[slot],__ATOMIC_ACQUIRE)==ticket;
}
static inline int journey_cue_retire(JourneyCueAdmissions *a,uint64_t ticket) {
    if(!(uint32_t)ticket)return 0;
    unsigned slot=(uint32_t)(ticket>>32)%JOURNEY_NATIVE_CUE_SLOTS;
    return __atomic_compare_exchange_n(&a->tickets[slot],&ticket,0,0,
                                      __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
#endif

/* Generation-safe ownership of native FMOD Channel handles.
 *
 * Exact shipped FMOD 1.10.08 public Channel handle:
 * bit0=1; bits1..16=serial; bits17..28=native channel slot; bits29..31=System.
 * 1800cdd00 validates the full handle against native slot+0x50. Reject native
 * wildcard serial 0xffff and pointer-style groups. NEVER dereference a handle.
 *
 * A direct native-slot index avoids scanning voices or adding a hash table.
 * Each binding is one atomic {full native handle, patch token}; each patch
 * token identifies a slot AND its monotonically increasing reuse generation.
 * END only clears this binding and marks its generation retired. No graph API,
 * native userdata overwrite, allocation, mutex, or waiting in callbacks.
 *
 * Owner constructs/binds BEFORE unpause. It may reclaim only AFTER the native
 * DSP destruction callback acknowledges every owner of that generation.
 * Missing END/cancel leaks capacity safely; it does not free live storage.
 * Generation exhaustion returns failure rather than wrapping. A fresh System
 * gets a fresh registry only after all old callbacks and DSP owners are gone.
 * Native serial wrap is not an infinite-history ID: the game/DLL callback
 * ordering must preclude arbitrarily delayed events across a full native wrap.
 */
#ifndef JOURNEY_NATIVE_CHANNEL_BINDING_H
#define JOURNEY_NATIVE_CHANNEL_BINDING_H
#include <stdint.h>
#include <stddef.h>

enum { JOURNEY_NATIVE_CHANNEL_SLOTS=4096, JOURNEY_BINDING_VOICE_SLOTS=1024 };
typedef struct JourneyChannelBindings {
    uint64_t native[JOURNEY_NATIVE_CHANNEL_SLOTS];
    uint64_t state[JOURNEY_BINDING_VOICE_SLOTS]; /* token<<1 | retired */
    uint32_t generations[JOURNEY_BINDING_VOICE_SLOTS]; /* single owner-thread only */
} JourneyChannelBindings;

static inline int journey_binding_native_slot(uintptr_t channel) {
    unsigned serial=(unsigned)(channel>>1)&0xffffU;
    if(channel>UINT32_MAX || !(channel&1U) || !serial || serial==0xffffU)return -1;
    return (int)((channel>>17)&0xfffU);
}
static inline uint32_t journey_binding_bind_channel(JourneyChannelBindings *b,uintptr_t channel,unsigned slot) {
    int native=journey_binding_native_slot(channel);
    if(native<0 || slot>=JOURNEY_BINDING_VOICE_SLOTS ||
       __atomic_load_n(&b->state[slot],__ATOMIC_ACQUIRE)!=0 ||
       b->generations[slot]>=0x3fffffU)return 0;
    uint32_t token=(++b->generations[slot]<<10)|slot;
    uint64_t vacant=0,state=(uint64_t)token<<1;
    /* Single owner allocates; callback only changes active->retired. */
    __atomic_store_n(&b->state[slot],state,__ATOMIC_RELEASE);
    uint64_t entry=((uint64_t)(uint32_t)channel<<32)|token;
    if(!__atomic_compare_exchange_n(&b->native[native],&vacant,entry,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        __atomic_store_n(&b->state[slot],0,__ATOMIC_RELEASE);
        return 0; /* existing live owner cannot be displaced */
    }
    return token;
}
static inline int journey_binding_retire_exact(JourneyChannelBindings *b,uintptr_t channel,uint32_t token) {
    int native=journey_binding_native_slot(channel);
    if(native<0 || !token)return 0;
    uint64_t entry=((uint64_t)(uint32_t)channel<<32)|token;
    if(!__atomic_compare_exchange_n(&b->native[native],&entry,0,0,
                                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    unsigned slot=token&1023U;
    uint64_t state=(uint64_t)token<<1;
    return __atomic_compare_exchange_n(&b->state[slot],&state,state|1U,0,
                                       __ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
static inline uint32_t journey_binding_channel_end(JourneyChannelBindings *b,uintptr_t channel) {
    int native=journey_binding_native_slot(channel);
    if(native<0)return 0;
    uint64_t entry=__atomic_load_n(&b->native[native],__ATOMIC_ACQUIRE);
    if((uint32_t)(entry>>32)!=(uint32_t)channel)return 0;
    uint32_t token=(uint32_t)entry;
    return journey_binding_retire_exact(b,channel,token)?token:0;
}
static inline int journey_binding_is_retired(const JourneyChannelBindings *b,uint32_t token) {
    return token && __atomic_load_n(&b->state[token&1023U],__ATOMIC_ACQUIRE)==(((uint64_t)token<<1)|1U);
}
static inline int journey_binding_reclaim_after_dsp_release(JourneyChannelBindings *b,uint32_t token) {
    if(!token)return 0;
    uint64_t ended=((uint64_t)token<<1)|1U;
    return __atomic_compare_exchange_n(&b->state[token&1023U],&ended,0,0,
                                       __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
#endif

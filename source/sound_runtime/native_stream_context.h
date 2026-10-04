#ifndef JOURNEY_NATIVE_STREAM_CONTEXT_H
#define JOURNEY_NATIVE_STREAM_CONTEXT_H
#include <stdint.h>
/* Caller-owned snapshot, valid only until synchronous factory returns.
 * Copy scalar routing before the pool can reuse the origin Stream object.
 * Not a retained pointer to an origin cache entry or native descriptor. */
typedef struct JourneyStreamContext {
    void *cue;
    void *cue_group;
    uint32_t cue_generation;
    float dry,wet;
    unsigned valid;
} JourneyStreamContext;
_Static_assert(sizeof(JourneyStreamContext)<=64,"assembly reserves 64 context bytes");
#endif

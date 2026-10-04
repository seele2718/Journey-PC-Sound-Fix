/* Exact same-value suppression for patch-owned FMOD control targets. */

#ifndef JOURNEY_REVERB_CONTROL_CACHE_H
#define JOURNEY_REVERB_CONTROL_CACHE_H

#include <stdint.h>

#include "pc_reverb_patch_runtime.h"

typedef struct JourneyControlVector {
    float x;
    float y;
    float z;
} JourneyControlVector;

typedef struct JourneyPublishedGroupState {
    float volume;
    JourneyControlVector position;
    JourneyControlVector velocity;
    float minimum;
    float maximum;
    float level;
    float doppler;
    unsigned int mode;
    int mute;
    int paused;
    uint16_t valid;
} JourneyPublishedGroupState;

void journey_group_publication_invalidate(JourneyPublishedGroupState *state);

int journey_publish_group_controls(
    const JourneyFmodApi *api,
    void *target,
    JourneyPublishedGroupState *published,
    int paused,
    int mute,
    float volume);

int journey_mirror_spatial_state_cached(
    const JourneyFmodApi *api,
    void *source_group,
    void *wet_group,
    JourneyPublishedGroupState *published,
    unsigned int mode_3d_mask);

#endif

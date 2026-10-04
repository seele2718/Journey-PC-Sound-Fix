/* Mirror Journey's non-spatial parent state onto one parallel wet cue. */

#ifndef JOURNEY_REVERB_GROUP_MIRROR_H
#define JOURNEY_REVERB_GROUP_MIRROR_H

typedef int (*JourneyGroupGetFloat)(void *group, float *value);
typedef int (*JourneyGroupGetBool)(void *group, int *value);
typedef int (*JourneyGroupGetParent)(void *group, void **parent);
typedef int (*JourneyGroupSetFloat)(void *group, float value);
typedef int (*JourneyGroupSetBool)(void *group, int value);

typedef struct {
    JourneyGroupGetFloat get_volume;
    JourneyGroupGetBool get_mute;
    JourneyGroupGetParent get_parent;
    JourneyGroupSetFloat set_volume;
    JourneyGroupSetBool set_mute;
    JourneyGroupGetBool get_paused;
    JourneyGroupSetBool set_paused;
} JourneyGroupMirrorApi;

typedef struct {
    float volume;
    int mute;
    int traversed_groups;
    int status;
    int paused;
} JourneyGroupMirrorResult;

/*
 * ``system_master`` is deliberately excluded: the shared renderer output is
 * already beneath it.  All groups from ``source_cue`` through Journey's own
 * master are included. Pause must be mirrored onto this input group: the
 * parallel TAIL edge otherwise continues pulling a paused source. The shared
 * renderer is NOT paused, so already accumulated room tails can continue.
 * Pitch remains on the original Channel; do not multiply it again.
 * On a read error or malformed/deep chain, the wet cue is failed closed
 * (muted at zero volume) rather than leaking unscaled reverb.
 */
JourneyGroupMirrorResult journey_mirror_group_chain(
    const JourneyGroupMirrorApi *api,
    void *source_cue,
    void *system_master,
    void *wet_cue);

/* Read the exact same authored parent state without publishing it.  The
 * optimized runtime uses this to publish pause, mute, and the final
 * Ducker-scaled volume once, instead of first writing the unscaled volume and
 * immediately overwriting it. */
JourneyGroupMirrorResult journey_read_group_chain(
    const JourneyGroupMirrorApi *api,
    void *source_cue,
    void *system_master);

#endif

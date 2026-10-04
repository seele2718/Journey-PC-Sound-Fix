#include "reverb_group_mirror.h"

enum { JOURNEY_GROUP_CHAIN_LIMIT = 9 };

static void fail_closed(const JourneyGroupMirrorApi *api, void *wet_cue)
{
    (void)api->set_paused(wet_cue, 1);
    (void)api->set_mute(wet_cue, 1);
    (void)api->set_volume(wet_cue, 0.0f);
}

JourneyGroupMirrorResult journey_read_group_chain(
    const JourneyGroupMirrorApi *api,
    void *source_cue,
    void *system_master)
{
    JourneyGroupMirrorResult result;
    void *group = source_cue;
    result.volume = 1.0f;
    result.mute = 0;
    result.traversed_groups = 0;
    result.status = 0;
    result.paused = 0;
    if (api == 0 || source_cue == 0 || system_master == 0) {
        result.status = -1;
        return result;
    }
    while (group != system_master) {
        float volume;
        int mute;
        int paused;
        void *parent = 0;
        if (group == 0 || result.traversed_groups == JOURNEY_GROUP_CHAIN_LIMIT ||
            api->get_volume(group, &volume) != 0 ||
            api->get_mute(group, &mute) != 0 ||
            api->get_paused(group, &paused) != 0 ||
            api->get_parent(group, &parent) != 0) {
            result.status = -2;
            return result;
        }
        result.volume *= volume;
        result.mute |= mute != 0;
        result.paused |= paused != 0;
        ++result.traversed_groups;
        group = parent;
    }
    return result;
}

JourneyGroupMirrorResult journey_mirror_group_chain(
    const JourneyGroupMirrorApi *api,
    void *source_cue,
    void *system_master,
    void *wet_cue)
{
    JourneyGroupMirrorResult result;
    void *group = source_cue;
    result.volume = 1.0f;
    result.mute = 0;
    result.traversed_groups = 0;
    result.status = 0;
    result.paused = 0;
    if (api == 0 || source_cue == 0 || system_master == 0 || wet_cue == 0) {
        if (api != 0 && wet_cue != 0) fail_closed(api, wet_cue);
        result.status = -1;
        return result;
    }
    while (group != system_master) {
        float volume;
        int mute;
        int paused;
        void *parent = 0;
        if (group == 0 || result.traversed_groups == JOURNEY_GROUP_CHAIN_LIMIT ||
            api->get_volume(group, &volume) != 0 ||
            api->get_mute(group, &mute) != 0 ||
            api->get_paused(group, &paused) != 0 ||
            api->get_parent(group, &parent) != 0) {
            fail_closed(api, wet_cue);
            result.status = -2;
            return result;
        }
        result.volume *= volume;
        result.mute |= mute != 0;
        result.paused |= paused != 0;
        ++result.traversed_groups;
        group = parent;
    }

    /* Publish input pause before gain. Shared renderer tails stay independent. */
    if (api->set_paused(wet_cue, result.paused) != 0) goto write_error;
    if (api->set_mute(wet_cue, result.mute) != 0) goto write_error;
    if (api->set_volume(wet_cue, result.volume) != 0) goto write_error;
    return result;

write_error:
    fail_closed(api, wet_cue);
    result.status = -3;
    return result;
}

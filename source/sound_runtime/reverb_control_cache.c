#include "reverb_control_cache.h"

#include <stddef.h>

enum {
    JOURNEY_PUBLISHED_VOLUME = 1U << 0,
    JOURNEY_PUBLISHED_MUTE = 1U << 1,
    JOURNEY_PUBLISHED_PAUSED = 1U << 2,
    JOURNEY_PUBLISHED_MODE = 1U << 3,
    JOURNEY_PUBLISHED_ATTRIBUTES = 1U << 4,
    JOURNEY_PUBLISHED_DISTANCE = 1U << 5,
    JOURNEY_PUBLISHED_LEVEL = 1U << 6,
    JOURNEY_PUBLISHED_DOPPLER = 1U << 7
};

static int bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    size_t index;
    for (index = 0U; index < size; ++index)
        if (a[index] != b[index]) return 0;
    return 1;
}

void journey_group_publication_invalidate(JourneyPublishedGroupState *state)
{
    if (state != NULL) state->valid = 0U;
}

int journey_publish_group_controls(
    const JourneyFmodApi *api,
    void *target,
    JourneyPublishedGroupState *published,
    int paused,
    int mute,
    float volume)
{
    if (api == NULL || target == NULL || published == NULL) return 0;
    if ((published->valid & JOURNEY_PUBLISHED_PAUSED) == 0U ||
        published->paused != paused) {
        if (api->group_set_paused(target, paused) != 0) return 0;
        published->paused = paused;
        published->valid |= JOURNEY_PUBLISHED_PAUSED;
    }
    if ((published->valid & JOURNEY_PUBLISHED_MUTE) == 0U ||
        published->mute != mute) {
        if (api->group_set_mute(target, mute) != 0) return 0;
        published->mute = mute;
        published->valid |= JOURNEY_PUBLISHED_MUTE;
    }
    if ((published->valid & JOURNEY_PUBLISHED_VOLUME) == 0U ||
        !bytes_equal(&published->volume, &volume, sizeof(volume))) {
        if (api->group_set_volume(target, volume) != 0) return 0;
        published->volume = volume;
        published->valid |= JOURNEY_PUBLISHED_VOLUME;
    }
    return 1;
}

int journey_mirror_spatial_state_cached(
    const JourneyFmodApi *api,
    void *source_group,
    void *wet_group,
    JourneyPublishedGroupState *published,
    unsigned int mode_3d_mask)
{
    unsigned int mode = 0U;
    JourneyControlVector position;
    JourneyControlVector velocity;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float level = 0.0f;
    float doppler = 0.0f;
    const int had_mode = published != NULL &&
        (published->valid & JOURNEY_PUBLISHED_MODE) != 0U;
    int force_spatial;

    if (api == NULL || source_group == NULL || wet_group == NULL ||
        published == NULL ||
        api->group_get_mode(source_group, &mode) != 0)
        return 0;
    force_spatial = !had_mode || published->mode != mode;
    if (force_spatial) {
        if (api->group_set_mode(wet_group, mode) != 0) return 0;
        published->mode = mode;
        published->valid |= JOURNEY_PUBLISHED_MODE;
    }
    if ((mode & mode_3d_mask) == 0U)
        return 1;
    if (api->group_get_3d_attributes(
            source_group, &position, &velocity) != 0 ||
        api->group_get_3d_min_max_distance(
            source_group, &minimum, &maximum) != 0 ||
        api->group_get_3d_level(source_group, &level) != 0 ||
        api->group_get_3d_doppler_level(source_group, &doppler) != 0)
        return 0;
    if (force_spatial ||
        (published->valid & JOURNEY_PUBLISHED_ATTRIBUTES) == 0U ||
        !bytes_equal(&published->position, &position, sizeof(position)) ||
        !bytes_equal(&published->velocity, &velocity, sizeof(velocity))) {
        if (api->group_set_3d_attributes(
                wet_group, &position, &velocity) != 0)
            return 0;
        published->position = position;
        published->velocity = velocity;
        published->valid |= JOURNEY_PUBLISHED_ATTRIBUTES;
    }
    if (force_spatial ||
        (published->valid & JOURNEY_PUBLISHED_DISTANCE) == 0U ||
        !bytes_equal(&published->minimum, &minimum, sizeof(minimum)) ||
        !bytes_equal(&published->maximum, &maximum, sizeof(maximum))) {
        if (api->group_set_3d_min_max_distance(
                wet_group, minimum, maximum) != 0)
            return 0;
        published->minimum = minimum;
        published->maximum = maximum;
        published->valid |= JOURNEY_PUBLISHED_DISTANCE;
    }
    if (force_spatial ||
        (published->valid & JOURNEY_PUBLISHED_LEVEL) == 0U ||
        !bytes_equal(&published->level, &level, sizeof(level))) {
        if (api->group_set_3d_level(wet_group, level) != 0)
            return 0;
        published->level = level;
        published->valid |= JOURNEY_PUBLISHED_LEVEL;
    }
    if (force_spatial ||
        (published->valid & JOURNEY_PUBLISHED_DOPPLER) == 0U ||
        !bytes_equal(&published->doppler, &doppler, sizeof(doppler))) {
        if (api->group_set_3d_doppler_level(wet_group, doppler) != 0)
            return 0;
        published->doppler = doppler;
        published->valid |= JOURNEY_PUBLISHED_DOPPLER;
    }
    return 1;
}

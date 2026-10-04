#include "branch_spatial_mirror.h"

#include <stddef.h>
#include <string.h>

enum { JOURNEY_FMOD_3D = 0x10U };

typedef struct JourneyBranchVector {
    float x;
    float y;
    float z;
} JourneyBranchVector;

static int publish_spatial(
    const JourneyFmodApi *api,
    JourneyBranchSpatialLink *link)
{
    return journey_mirror_spatial_state_cached(
        api, link->parent_group, link->child_group,
        &link->published, JOURNEY_FMOD_3D);
}

void journey_branch_spatial_reset(JourneyBranchSpatialMirror *mirror)
{
    if (mirror != NULL) memset(mirror, 0, sizeof(*mirror));
}

int journey_branch_spatial_register(
    JourneyBranchSpatialMirror *mirror,
    const JourneyFmodApi *api,
    void *child_handler,
    void *parent_handler,
    void *child_group,
    void *parent_group)
{
    uint32_t index;
    JourneyBranchSpatialLink *link = NULL;
    if (mirror == NULL || api == NULL || child_handler == NULL ||
        parent_handler == NULL || child_group == NULL || parent_group == NULL)
        return 0;
    for (index = 0U; index < mirror->high_water; ++index) {
        if (mirror->links[index].active &&
            mirror->links[index].child_handler == child_handler) {
            link = &mirror->links[index];
            break;
        }
        if (link == NULL && !mirror->links[index].active)
            link = &mirror->links[index];
    }
    if (link == NULL) {
        if (mirror->high_water == JOURNEY_MAX_BRANCH_SPATIAL_LINKS)
            return 0;
        link = &mirror->links[mirror->high_water++];
    }
    if (!link->active) ++mirror->active_count;
    memset(link, 0, sizeof(*link));
    link->child_handler = child_handler;
    link->parent_handler = parent_handler;
    link->child_group = child_group;
    link->parent_group = parent_group;
    link->active = 1U;
    return publish_spatial(api, link);
}

void journey_branch_spatial_unregister(
    JourneyBranchSpatialMirror *mirror,
    void *handler)
{
    uint32_t index;
    if (mirror == NULL || handler == NULL) return;
    for (index = 0U; index < mirror->high_water; ++index) {
        JourneyBranchSpatialLink *link = &mirror->links[index];
        /* Control-side retirement can remove either end of a logical link.
         * Match both parent and child identities so a surviving child slot
         * cannot retain the retired parent's FMOD group. */
        if (link->active && (link->child_handler == handler ||
                             link->parent_handler == handler)) {
            memset(link, 0, sizeof(*link));
            if (mirror->active_count != 0U) --mirror->active_count;
        }
    }
    while (mirror->high_water != 0U &&
           !mirror->links[mirror->high_water - 1U].active)
        --mirror->high_water;
}

uint32_t journey_branch_spatial_update(
    JourneyBranchSpatialMirror *mirror,
    const JourneyFmodApi *api)
{
    uint32_t index;
    uint32_t failures = 0U;
    if (mirror == NULL || api == NULL) return 1U;
    for (index = 0U; index < mirror->high_water; ++index) {
        JourneyBranchSpatialLink *link = &mirror->links[index];
        if (!link->active) continue;
        if (!publish_spatial(api, link)) {
            journey_group_publication_invalidate(&link->published);
            ++failures;
        }
    }
    return failures;
}

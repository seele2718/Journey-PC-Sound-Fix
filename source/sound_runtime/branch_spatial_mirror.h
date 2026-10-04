/* Preserve console Branch Child spatial ownership on Journey PC.
 *
 * Console Branch replaces the current graph on one handler. PC instead
 * creates a child handler on its authored category bus, retaining only a
 * logical parent pointer. These slots keep category/gain routing untouched
 * while publishing the logical parent's live spatial controls to the child.
 */

#ifndef JOURNEY_BRANCH_SPATIAL_MIRROR_H
#define JOURNEY_BRANCH_SPATIAL_MIRROR_H

#include <stdint.h>

#include "pc_reverb_patch_runtime.h"
#include "reverb_control_cache.h"

enum { JOURNEY_MAX_BRANCH_SPATIAL_LINKS = 1024 };

typedef struct JourneyBranchSpatialLink {
    void *child_handler;
    void *parent_handler;
    void *child_group;
    void *parent_group;
    JourneyPublishedGroupState published;
    uint8_t active;
} JourneyBranchSpatialLink;

typedef struct JourneyBranchSpatialMirror {
    JourneyBranchSpatialLink links[JOURNEY_MAX_BRANCH_SPATIAL_LINKS];
    uint32_t high_water;
    uint32_t active_count;
} JourneyBranchSpatialMirror;

void journey_branch_spatial_reset(JourneyBranchSpatialMirror *mirror);

/* Registers and immediately publishes one Branch child. The child remains on
 * its original category bus; only FMOD mode/position/range/doppler/3D level
 * are copied. Returns zero if the first publication failed or no slot exists.
 */
int journey_branch_spatial_register(
    JourneyBranchSpatialMirror *mirror,
    const JourneyFmodApi *api,
    void *child_handler,
    void *parent_handler,
    void *child_group,
    void *parent_group);

void journey_branch_spatial_unregister(
    JourneyBranchSpatialMirror *mirror,
    void *handler);

/* Refreshes every live link. Returns the number of failed publications. */
uint32_t journey_branch_spatial_update(
    JourneyBranchSpatialMirror *mirror,
    const JourneyFmodApi *api);

#endif

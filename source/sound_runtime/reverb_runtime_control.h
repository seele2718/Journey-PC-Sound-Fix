/* Game-thread control bridge for Journey PC's existing ReverbBarn state.
 *
 * This file contains no FMOD calls.  It reads the already-parsed 54-node
 * preset tree and publishes complete twelve-float parameter blocks to the two
 * lock-free Slapper mailboxes.  The mixer therefore never touches MSVC tree
 * nodes or std::string objects.
 */

#ifndef JOURNEY_REVERB_RUNTIME_CONTROL_H
#define JOURNEY_REVERB_RUNTIME_CONTROL_H

#include <stdint.h>

#include "slapper_control_mailbox.h"

enum {
    JOURNEY_REVERB_NAME_CAPACITY = 32,
    JOURNEY_REVERB_TREE_STACK_CAPACITY = 64,
};

typedef struct {
    SlapperPresetMailbox sfx;
    SlapperPresetMailbox music;
    volatile uint32_t sfx_transition_gain_bits;
    char last_sfx_name[JOURNEY_REVERB_NAME_CAPACITY];
    char last_music_name[JOURNEY_REVERB_NAME_CAPACITY];
    uint8_t have_sfx;
    uint8_t have_music;
} JourneyReverbControl;

typedef struct {
    int sfx_name_changed;
    int music_name_changed;
    int sfx_preset_published;
    int music_preset_published;
    int sfx_lookup_resolved;
    int music_lookup_resolved;
    int sfx_supported;
    int music_supported;
    float sfx_transition_gain;
} JourneyReverbPublishResult;

void journey_reverb_control_reset(JourneyReverbControl *control);

/*
 * ``manager`` is the PC ReverbBarn object after 0x140124B80 has completed.
 * The function is bounded, allocation-free, and intended for the game thread.
 */
JourneyReverbPublishResult journey_reverb_publish_from_manager(
    JourneyReverbControl *control,
    const void *manager);

#endif

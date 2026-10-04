/* Portable console-equivalent Ducker state/lifecycle adapter.
 *
 * The integer envelope rules mirror PS4 FUN_003EE0F0/FUN_003EFD20.  This
 * module deliberately knows nothing about FMOD: the Journey runtime wrapper
 * owns dry/wet application and publishes the 32 effective category scalars.
 */

#ifndef JOURNEY_DUCKER_ADAPTER_H
#define JOURNEY_DUCKER_ADAPTER_H

#include <stdint.h>

enum {
    JOURNEY_DUCKER_CATEGORIES = 32,
    JOURNEY_DUCKER_STATES = 32,
    JOURNEY_DUCKER_MAX_HANDLERS = 4096,
    JOURNEY_DUCKER_MAX_ASSOCIATIONS = 1024,
    JOURNEY_DUCKER_Q16_ONE = 0x10000,
};

typedef struct JourneyDuckerState {
    uint32_t target_mask;
    uint32_t target_q16;
    uint32_t attack_step_q16;
    uint32_t release_step_q16;
    uint32_t current_q16;
    uint8_t watched_category;
    uint8_t configured;
    uint8_t retiring;
} JourneyDuckerState;

typedef struct JourneyDuckerHandler {
    void *handler;
    uint8_t category;
    uint8_t active;
} JourneyDuckerHandler;

typedef struct JourneyDuckerAssociation {
    void *cue;
    uint8_t local_index;
    uint8_t identity;
    uint8_t active;
} JourneyDuckerAssociation;

typedef struct JourneyDuckerAdapter {
    JourneyDuckerState states[JOURNEY_DUCKER_STATES];
    JourneyDuckerHandler handlers[JOURNEY_DUCKER_MAX_HANDLERS];
    JourneyDuckerAssociation associations[JOURNEY_DUCKER_MAX_ASSOCIATIONS];
    volatile uint32_t effective_q16[JOURNEY_DUCKER_CATEGORIES];
    uint32_t live[JOURNEY_DUCKER_CATEGORIES];
    uint32_t flagged[JOURNEY_DUCKER_CATEGORIES];
    uint32_t configured_mask;
    uint32_t healthy;
} JourneyDuckerAdapter;

void journey_ducker_reset(JourneyDuckerAdapter *adapter);
int journey_ducker_counts_cue_flags(uint16_t cue_flags);
int journey_ducker_register_handler(
    JourneyDuckerAdapter *adapter, void *handler, uint8_t category);
void journey_ducker_unregister_handler(
    JourneyDuckerAdapter *adapter, void *handler);

/* Descriptor is the authored 20-byte opcode-10 row.  Positive configuration
 * stores cue/local-index -> native-ID so the later negative operation can
 * resolve the process-global state. */
int journey_ducker_configure(
    JourneyDuckerAdapter *adapter, void *cue, const void *descriptor);
int journey_ducker_retire_local(
    JourneyDuckerAdapter *adapter, void *cue, uint32_t scaled_local_index);
void journey_ducker_forget_local(
    JourneyDuckerAdapter *adapter, void *cue, uint32_t local_index);
void journey_ducker_forget_cue(
    JourneyDuckerAdapter *adapter, void *cue);

void journey_ducker_step(JourneyDuckerAdapter *adapter);
uint64_t journey_ducker_advance_samples(
    JourneyDuckerAdapter *adapter, uint64_t elapsed_samples,
    uint32_t sample_rate, uint64_t remainder_60);
uint32_t journey_ducker_effective_q16(
    const JourneyDuckerAdapter *adapter, uint8_t category);
float journey_ducker_scale_sample(float sample, uint32_t gain_q16);

#endif

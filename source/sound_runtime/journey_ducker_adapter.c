#include "journey_ducker_adapter.h"

#include <stddef.h>
#include <string.h>

static uint32_t read_u32(const void *base, size_t offset)
{
    uint32_t value = 0U;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static int16_t read_i16(const void *base, size_t offset)
{
    int16_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint8_t read_u8(const void *base, size_t offset)
{
    uint8_t value = 0U;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint32_t native_step(uint32_t target, uint32_t seconds_q16)
{
    uint64_t updates = ((uint64_t)seconds_q16 * 60U) >> 16;
    uint32_t step;
    if (updates == 0U) updates = 1U;
    step = (JOURNEY_DUCKER_Q16_ONE - target) / (uint32_t)updates;
    return step == 0U ? 1U : step;
}

void journey_ducker_reset(JourneyDuckerAdapter *adapter)
{
    unsigned category;
    if (adapter == NULL) return;
    memset(adapter, 0, sizeof(*adapter));
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category)
        __atomic_store_n(&adapter->effective_q16[category],
                         JOURNEY_DUCKER_Q16_ONE, __ATOMIC_RELAXED);
    adapter->healthy = 1U;
}

int journey_ducker_counts_cue_flags(uint16_t cue_flags)
{
    return (cue_flags & 0x40U) == 0U;
}

int journey_ducker_register_handler(
    JourneyDuckerAdapter *adapter, void *handler, uint8_t category)
{
    unsigned index;
    if (adapter == NULL || !adapter->healthy || handler == NULL ||
        category >= JOURNEY_DUCKER_CATEGORIES)
        return 0;
    for (index = 0; index < JOURNEY_DUCKER_MAX_HANDLERS; ++index) {
        JourneyDuckerHandler *slot = &adapter->handlers[index];
        if (slot->active && slot->handler == handler) {
            slot->category = category;
            return 1;
        }
    }
    for (index = 0; index < JOURNEY_DUCKER_MAX_HANDLERS; ++index) {
        JourneyDuckerHandler *slot = &adapter->handlers[index];
        if (!slot->active) {
            slot->handler = handler;
            slot->category = category;
            slot->active = 1U;
            return 1;
        }
    }
    adapter->healthy = 0U;
    return 0;
}

void journey_ducker_forget_cue(JourneyDuckerAdapter *adapter, void *cue)
{
    unsigned index;
    if (adapter == NULL || cue == NULL) return;
    for (index = 0; index < JOURNEY_DUCKER_MAX_ASSOCIATIONS; ++index) {
        JourneyDuckerAssociation *slot = &adapter->associations[index];
        if (slot->active && slot->cue == cue)
            memset(slot, 0, sizeof(*slot));
    }
}

void journey_ducker_unregister_handler(
    JourneyDuckerAdapter *adapter, void *handler)
{
    unsigned index;
    if (adapter == NULL || handler == NULL) return;
    for (index = 0; index < JOURNEY_DUCKER_MAX_HANDLERS; ++index) {
        JourneyDuckerHandler *slot = &adapter->handlers[index];
        if (slot->active && slot->handler == handler) {
            memset(slot, 0, sizeof(*slot));
            break;
        }
    }
    journey_ducker_forget_cue(adapter, handler);
}

static int remember_association(
    JourneyDuckerAdapter *adapter, void *cue, uint8_t local_index,
    uint8_t identity)
{
    unsigned index;
    for (index = 0; index < JOURNEY_DUCKER_MAX_ASSOCIATIONS; ++index) {
        JourneyDuckerAssociation *slot = &adapter->associations[index];
        if (slot->active && slot->cue == cue &&
            slot->local_index == local_index) {
            slot->identity = identity;
            return 1;
        }
    }
    for (index = 0; index < JOURNEY_DUCKER_MAX_ASSOCIATIONS; ++index) {
        JourneyDuckerAssociation *slot = &adapter->associations[index];
        if (!slot->active) {
            slot->cue = cue;
            slot->local_index = local_index;
            slot->identity = identity;
            slot->active = 1U;
            return 1;
        }
    }
    adapter->healthy = 0U;
    return 0;
}

int journey_ducker_configure(
    JourneyDuckerAdapter *adapter, void *cue, const void *descriptor)
{
    const uint8_t identity = descriptor == NULL
        ? 0xffU : read_u8(descriptor, 0U);
    const int16_t signed_watched = descriptor == NULL
        ? -1 : read_i16(descriptor, 2U);
    uint32_t target;
    JourneyDuckerState *state;
    if (adapter == NULL || !adapter->healthy || cue == NULL ||
        descriptor == NULL || identity >= JOURNEY_DUCKER_STATES ||
        signed_watched < 0 || signed_watched >= 32)
        return 0;
    target = read_u32(descriptor, 8U);
    if (target > JOURNEY_DUCKER_Q16_ONE) target = JOURNEY_DUCKER_Q16_ONE;
    if (!remember_association(adapter, cue, (uint8_t)signed_watched,
                              identity))
        return 0;
    state = &adapter->states[identity];
    if (!state->configured)
        state->current_q16 = JOURNEY_DUCKER_Q16_ONE;
    state->watched_category = (uint8_t)signed_watched;
    state->target_mask = read_u32(descriptor, 4U);
    state->target_q16 = target;
    state->attack_step_q16 = native_step(target, read_u32(descriptor, 12U));
    state->release_step_q16 = native_step(target, read_u32(descriptor, 16U));
    state->configured = 1U;
    state->retiring = 0U;
    adapter->configured_mask |= 1U << identity;
    return 1;
}

static JourneyDuckerAssociation *find_association(
    JourneyDuckerAdapter *adapter, void *cue, uint32_t local_index)
{
    unsigned index;
    if (local_index >= 32U) return NULL;
    for (index = 0; index < JOURNEY_DUCKER_MAX_ASSOCIATIONS; ++index) {
        JourneyDuckerAssociation *slot = &adapter->associations[index];
        if (slot->active && slot->cue == cue &&
            slot->local_index == (uint8_t)local_index)
            return slot;
    }
    return NULL;
}

int journey_ducker_retire_local(
    JourneyDuckerAdapter *adapter, void *cue, uint32_t scaled_local_index)
{
    JourneyDuckerAssociation *association;
    JourneyDuckerState *state;
    uint32_t local_index;
    if (adapter == NULL || !adapter->healthy || cue == NULL ||
        scaled_local_index % 3U != 0U)
        return 0;
    local_index = scaled_local_index / 3U;
    association = find_association(adapter, cue, local_index);
    if (association == NULL) return 0;
    state = &adapter->states[association->identity];
    if (!state->configured) return 0;
    state->retiring = 1U;
    memset(association, 0, sizeof(*association));
    return 1;
}

void journey_ducker_forget_local(
    JourneyDuckerAdapter *adapter, void *cue, uint32_t local_index)
{
    JourneyDuckerAssociation *association;
    if (adapter == NULL || cue == NULL) return;
    association = find_association(adapter, cue, local_index);
    if (association != NULL) memset(association, 0, sizeof(*association));
}

void journey_ducker_step(JourneyDuckerAdapter *adapter)
{
    uint32_t effective[JOURNEY_DUCKER_CATEGORIES];
    unsigned category;
    unsigned index;
    if (adapter == NULL || !adapter->healthy) return;
    memset(adapter->live, 0, sizeof(adapter->live));
    memset(adapter->flagged, 0, sizeof(adapter->flagged));
    for (index = 0; index < JOURNEY_DUCKER_MAX_HANDLERS; ++index) {
        JourneyDuckerHandler *handler = &adapter->handlers[index];
        uint32_t flags;
        if (!handler->active) continue;
        ++adapter->live[handler->category];
        memcpy(&flags, handler->handler, sizeof(flags));
        if ((flags & 0xEU) != 0U) ++adapter->flagged[handler->category];
    }
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category)
        effective[category] = JOURNEY_DUCKER_Q16_ONE;
    for (index = 0; index < JOURNEY_DUCKER_STATES; ++index) {
        JourneyDuckerState *state = &adapter->states[index];
        uint32_t goal;
        if (!state->configured) continue;
        goal = (!state->retiring &&
                adapter->flagged[state->watched_category] <
                    adapter->live[state->watched_category])
            ? state->target_q16 : JOURNEY_DUCKER_Q16_ONE;
        if (state->current_q16 > goal) {
            const uint32_t distance = state->current_q16 - goal;
            state->current_q16 -= distance < state->attack_step_q16
                ? distance : state->attack_step_q16;
        } else if (state->current_q16 < goal) {
            const uint32_t distance = goal - state->current_q16;
            state->current_q16 += distance < state->release_step_q16
                ? distance : state->release_step_q16;
        }
        for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category) {
            if ((state->target_mask & (1U << category)) != 0U &&
                state->current_q16 < effective[category])
                effective[category] = state->current_q16;
        }
        if (state->retiring && state->current_q16 == JOURNEY_DUCKER_Q16_ONE) {
            memset(state, 0, sizeof(*state));
            adapter->configured_mask &= ~(1U << index);
        }
    }
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category)
        __atomic_store_n(&adapter->effective_q16[category], effective[category],
                         __ATOMIC_RELEASE);
}

/*
 * Advance an unchanged end-of-gap handler snapshot by several 60 Hz ticks.
 *
 * A synchronous catch-up sees the same final handler state for every missed
 * tick. The native integer envelope is linear between endpoints, so one census
 * of the 4096 handler slots and a clamped N-step move reproduce repeated steps
 * without rereading the unchanged state on each tick.
 */
static void journey_ducker_advance_ticks(
    JourneyDuckerAdapter *adapter, uint64_t ticks)
{
    uint32_t effective[JOURNEY_DUCKER_CATEGORIES];
    unsigned category;
    unsigned index;
    if (adapter == NULL || !adapter->healthy || ticks == 0U) return;

    memset(adapter->live, 0, sizeof(adapter->live));
    memset(adapter->flagged, 0, sizeof(adapter->flagged));
    for (index = 0; index < JOURNEY_DUCKER_MAX_HANDLERS; ++index) {
        JourneyDuckerHandler *handler = &adapter->handlers[index];
        uint32_t flags;
        if (!handler->active) continue;
        ++adapter->live[handler->category];
        memcpy(&flags, handler->handler, sizeof(flags));
        if ((flags & 0xEU) != 0U) ++adapter->flagged[handler->category];
    }
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category)
        effective[category] = JOURNEY_DUCKER_Q16_ONE;
    for (index = 0; index < JOURNEY_DUCKER_STATES; ++index) {
        JourneyDuckerState *state = &adapter->states[index];
        uint32_t goal;
        uint32_t distance;
        uint32_t step;
        uint64_t steps_needed;
        if (!state->configured) continue;
        goal = (!state->retiring &&
                adapter->flagged[state->watched_category] <
                    adapter->live[state->watched_category])
            ? state->target_q16 : JOURNEY_DUCKER_Q16_ONE;
        if (state->current_q16 != goal) {
            if (state->current_q16 > goal) {
                distance = state->current_q16 - goal;
                step = state->attack_step_q16;
            } else {
                distance = goal - state->current_q16;
                step = state->release_step_q16;
            }
            steps_needed = ((uint64_t)distance + step - 1U) / step;
            if (ticks >= steps_needed) {
                state->current_q16 = goal;
            } else {
                const uint32_t delta = (uint32_t)(ticks * step);
                if (state->current_q16 > goal)
                    state->current_q16 -= delta;
                else
                    state->current_q16 += delta;
            }
        }
        for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category) {
            if ((state->target_mask & (1U << category)) != 0U &&
                state->current_q16 < effective[category])
                effective[category] = state->current_q16;
        }
        if (state->retiring && state->current_q16 == JOURNEY_DUCKER_Q16_ONE) {
            memset(state, 0, sizeof(*state));
            adapter->configured_mask &= ~(1U << index);
        }
    }
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category)
        __atomic_store_n(&adapter->effective_q16[category], effective[category],
                         __ATOMIC_RELEASE);
}

uint64_t journey_ducker_advance_samples(
    JourneyDuckerAdapter *adapter, uint64_t elapsed_samples,
    uint32_t sample_rate, uint64_t remainder_60)
{
    uint64_t scaled;
    if (adapter == NULL || sample_rate == 0U) return remainder_60;
    scaled = elapsed_samples * 60U + remainder_60;
    journey_ducker_advance_ticks(adapter, scaled / sample_rate);
    return scaled % sample_rate;
}

uint32_t journey_ducker_effective_q16(
    const JourneyDuckerAdapter *adapter, uint8_t category)
{
    if (adapter == NULL || category >= JOURNEY_DUCKER_CATEGORIES)
        return JOURNEY_DUCKER_Q16_ONE;
    return __atomic_load_n(&adapter->effective_q16[category],
                           __ATOMIC_ACQUIRE);
}

float journey_ducker_scale_sample(float sample, uint32_t gain_q16)
{
    return sample * ((float)gain_q16 * (1.0f / 65536.0f));
}

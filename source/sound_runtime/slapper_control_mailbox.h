/*
 * Lock-free publication of Journey's twelve-float ReverbBarn preset.
 *
 * The game thread is the sole writer and the FMOD mixer thread is the sole
 * reader.  Every payload word is atomic, so this remains well-defined C even
 * while a room change is being published.  The odd/even sequence prevents a
 * reader from accepting a mixture of two presets.  The mixer only calls
 * slapper_set_preset after consuming a new, complete even generation.
 */

#ifndef JOURNEY_SLAPPER_CONTROL_MAILBOX_H
#define JOURNEY_SLAPPER_CONTROL_MAILBOX_H

#include <stdint.h>
#include <string.h>

enum { SLAPPER_PRESET_FIELD_COUNT = 12 };

typedef struct {
    volatile uint32_t sequence;
    volatile uint32_t words[SLAPPER_PRESET_FIELD_COUNT];
} SlapperPresetMailbox;

static __attribute__((unused)) void slapper_mailbox_publish(
    SlapperPresetMailbox *mailbox,
    const float fields[SLAPPER_PRESET_FIELD_COUNT])
{
    int index;
    (void)__atomic_fetch_add(&mailbox->sequence, 1U, __ATOMIC_ACQ_REL);
    for (index = 0; index < SLAPPER_PRESET_FIELD_COUNT; ++index) {
        uint32_t bits;
        memcpy(&bits, &fields[index], sizeof(bits));
        __atomic_store_n(&mailbox->words[index], bits, __ATOMIC_RELAXED);
    }
    (void)__atomic_fetch_add(&mailbox->sequence, 1U, __ATOMIC_RELEASE);
}

/*
 * Returns one only for a complete generation newer than *last_sequence.
 * A bounded retry is preferable on the real-time thread: if the game thread
 * happens to be publishing, the old renderer targets remain valid for this
 * 256-frame block and the mixer can try again on the next block.
 */
static __attribute__((unused)) int slapper_mailbox_consume(
    const SlapperPresetMailbox *mailbox,
    uint32_t *last_sequence,
    float fields[SLAPPER_PRESET_FIELD_COUNT])
{
    int attempt;
    for (attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = __atomic_load_n(
            &mailbox->sequence, __ATOMIC_ACQUIRE);
        uint32_t bits[SLAPPER_PRESET_FIELD_COUNT];
        uint32_t after;
        int index;
        if (before == 0U || (before & 1U) != 0U || before == *last_sequence)
            return 0;
        for (index = 0; index < SLAPPER_PRESET_FIELD_COUNT; ++index)
            bits[index] = __atomic_load_n(
                &mailbox->words[index], __ATOMIC_RELAXED);
        after = __atomic_load_n(&mailbox->sequence, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1U) == 0U) {
            for (index = 0; index < SLAPPER_PRESET_FIELD_COUNT; ++index)
                memcpy(&fields[index], &bits[index], sizeof(bits[index]));
            *last_sequence = after;
            return 1;
        }
    }
    return 0;
}

#endif

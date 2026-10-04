#include "reverb_runtime_control.h"

#include <stddef.h>
#include <string.h>

enum {
    MANAGER_SFX_NAME = 0x20,
    MANAGER_MUSIC_NAME = 0x78,
    MANAGER_SFX_REMAINING = 0xD8,
    MANAGER_PRESET_TREE_HEADER = 0x8C0,

    NODE_LEFT = 0x00,
    NODE_ROOT_FROM_HEADER = 0x08,
    NODE_RIGHT = 0x10,
    NODE_IS_NIL = 0x19,
    NODE_KEY = 0x20,
    NODE_KEY_LENGTH = 0x30,
    NODE_KEY_CAPACITY = 0x38,
    NODE_EFFECT_MODE = 0x40,
    NODE_FIELDS = 0x44,
};

static const void *read_pointer(const void *base, size_t offset)
{
    const void *value;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static size_t read_size(const void *base, size_t offset)
{
    size_t value;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static float read_float(const void *base, size_t offset)
{
    float value;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static unsigned char fold_ascii(unsigned char value)
{
    if (value >= (unsigned char)'a' && value <= (unsigned char)'z')
        return (unsigned char)(value - ((unsigned char)'a' - (unsigned char)'A'));
    return value;
}

static size_t bounded_name_length(const char *name)
{
    size_t length = 0;
    while (length < JOURNEY_REVERB_NAME_CAPACITY && name[length] != '\0')
        ++length;
    return length;
}

static int names_equal_exact(const char *left, const char *right)
{
    size_t index;
    for (index = 0; index < JOURNEY_REVERB_NAME_CAPACITY; ++index) {
        if (left[index] != right[index]) return 0;
        if (left[index] == '\0') return 1;
    }
    return 1;
}

static int names_equal_native(
    const char *requested,
    const char *candidate,
    size_t candidate_length)
{
    size_t requested_length = bounded_name_length(requested);
    size_t index;
    if (requested_length == JOURNEY_REVERB_NAME_CAPACITY ||
        requested_length != candidate_length)
        return 0;
    for (index = 0; index < requested_length; ++index) {
        if (fold_ascii((unsigned char)requested[index]) !=
            fold_ascii((unsigned char)candidate[index]))
            return 0;
    }
    return 1;
}

static const char *node_key(const void *node, size_t capacity)
{
    if (capacity <= 15U)
        return (const char *)node + NODE_KEY;
    return (const char *)read_pointer(node, NODE_KEY);
}

static const void *find_preset_node(const void *manager, const char *name)
{
    const void *header = read_pointer(manager, MANAGER_PRESET_TREE_HEADER);
    const void *stack[JOURNEY_REVERB_TREE_STACK_CAPACITY];
    size_t stack_count = 0;
    const void *root;
    if (header == NULL) return NULL;
    root = read_pointer(header, NODE_ROOT_FROM_HEADER);
    if (root == NULL || *((const uint8_t *)root + NODE_IS_NIL) != 0U)
        return NULL;
    stack[stack_count++] = root;
    while (stack_count != 0U) {
        const void *node = stack[--stack_count];
        const void *left;
        const void *right;
        size_t length;
        size_t capacity;
        const char *key;
        if (node == NULL || *((const uint8_t *)node + NODE_IS_NIL) != 0U)
            continue;
        length = read_size(node, NODE_KEY_LENGTH);
        capacity = read_size(node, NODE_KEY_CAPACITY);
        key = node_key(node, capacity);
        if (key != NULL && names_equal_native(name, key, length)) return node;
        left = read_pointer(node, NODE_LEFT);
        right = read_pointer(node, NODE_RIGHT);
        if (left != NULL && *((const uint8_t *)left + NODE_IS_NIL) == 0U) {
            if (stack_count == JOURNEY_REVERB_TREE_STACK_CAPACITY) return NULL;
            stack[stack_count++] = left;
        }
        if (right != NULL && *((const uint8_t *)right + NODE_IS_NIL) == 0U) {
            if (stack_count == JOURNEY_REVERB_TREE_STACK_CAPACITY) return NULL;
            stack[stack_count++] = right;
        }
    }
    return NULL;
}

static int preset_uses_supported_long_delay(const void *node)
{
    /* The shortest native early tap is delay + timeFactor*0.471 at 24 kHz. */
    const float delay = read_float(node, NODE_FIELDS + 3U * sizeof(float));
    const float time_factor = read_float(node, NODE_FIELDS + 4U * sizeof(float));
    const float shortest_samples = (delay + time_factor * 0.471f) * 24000.0f;
    (void)NODE_EFFECT_MODE;
    return shortest_samples >= 128.0f;
}

static int publish_named_preset(
    JourneyReverbControl *control,
    const void *manager,
    const char *name,
    int music,
    int *resolved,
    int *supported)
{
    const void *node = find_preset_node(manager, name);
    float fields[SLAPPER_PRESET_FIELD_COUNT];
    int index;
    *resolved = node != NULL;
    *supported = node != NULL && preset_uses_supported_long_delay(node);
    if (!*supported) return 0;
    for (index = 0; index < SLAPPER_PRESET_FIELD_COUNT; ++index)
        fields[index] = read_float(node, NODE_FIELDS + (size_t)index * sizeof(float));
    slapper_mailbox_publish(music ? &control->music : &control->sfx, fields);
    return 1;
}

static void copy_name(char destination[JOURNEY_REVERB_NAME_CAPACITY],
                      const char *source)
{
    size_t index;
    for (index = 0; index < JOURNEY_REVERB_NAME_CAPACITY; ++index) {
        destination[index] = source[index];
        if (source[index] == '\0') {
            for (++index; index < JOURNEY_REVERB_NAME_CAPACITY; ++index)
                destination[index] = '\0';
            return;
        }
    }
}

void journey_reverb_control_reset(JourneyReverbControl *control)
{
    memset(control, 0, sizeof(*control));
    {
        float unity = 1.0f;
        uint32_t bits;
        memcpy(&bits, &unity, sizeof(bits));
        __atomic_store_n(&control->sfx_transition_gain_bits, bits,
                         __ATOMIC_RELAXED);
    }
}

JourneyReverbPublishResult journey_reverb_publish_from_manager(
    JourneyReverbControl *control,
    const void *manager)
{
    JourneyReverbPublishResult result;
    const char *sfx_name = (const char *)manager + MANAGER_SFX_NAME;
    const char *music_name = (const char *)manager + MANAGER_MUSIC_NAME;
    float remaining = read_float(manager, MANAGER_SFX_REMAINING);
    float gain = (remaining >= 0.5f ? remaining - 0.5f : 0.5f - remaining) * 2.0f;
    uint32_t gain_bits;
    memset(&result, 0, sizeof(result));
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    memcpy(&gain_bits, &gain, sizeof(gain_bits));
    __atomic_store_n(&control->sfx_transition_gain_bits, gain_bits,
                     __ATOMIC_RELEASE);
    result.sfx_transition_gain = gain;

    result.sfx_name_changed = !control->have_sfx ||
        !names_equal_exact(control->last_sfx_name, sfx_name);
    if (result.sfx_name_changed) {
        result.sfx_preset_published = publish_named_preset(
            control, manager, sfx_name, 0,
            &result.sfx_lookup_resolved, &result.sfx_supported);
        copy_name(control->last_sfx_name, sfx_name);
        control->have_sfx = 1U;
    }

    result.music_name_changed = !control->have_music ||
        !names_equal_exact(control->last_music_name, music_name);
    if (result.music_name_changed) {
        result.music_preset_published = publish_named_preset(
            control, manager, music_name, 1,
            &result.music_lookup_resolved, &result.music_supported);
        copy_name(control->last_music_name, music_name);
        control->have_music = 1U;
    }
    return result;
}

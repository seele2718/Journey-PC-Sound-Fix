/*
 * Runtime core for Journey PC's static reverb repair.
 *
 * This owns no Journey object memory.  Voice, cue, and Stream associations are
 * kept in one externally allocated state block, so the final PE patch does not
 * depend on undocumented padding in the game's active-voice records.
 */

#include "pc_reverb_patch_runtime.h"
#include "branch_spatial_mirror.h"
#include "journey_ducker_adapter.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "reverb_native_master_adapter.h"

#define SLAPPER_PORTABLE_LIBRARY
#include "slapper_portable_reference.c"
#include "reverb_group_mirror.h"
#include "reverb_runtime_control.h"
#include "slapper_native_stereo.h"
#include "reverb_control_cache.h"

enum {
    /* Fixed-capacity tables live in the one-time heap state allocation.
     * They bound memory use; these are not authored voice limits. */
    JOURNEY_MAX_PATCH_VOICES = 1024,
    JOURNEY_MAX_PATCH_CUES = 1024,
    JOURNEY_MAX_STREAM_ROUTES = 4096,
    FMOD_PLUGIN_SDK_VERSION_110 = 110,
    FMOD_CHANNELCONTROL_DSP_HEAD = -1,
    FMOD_CHANNELCONTROL_DSP_TAIL = -3,
    FMOD_SPEAKERMODE_MONO = 2,
    FMOD_SPEAKERMODE_STEREO = 3,
    FMOD_SPEAKERMODE_QUAD = 4,
    /* FMOD 1.10.08 public speaker/channel encodings used by Journey. */
    FMOD_CHANNELMASK_STEREO = 0x3,
    FMOD_CHANNELMASK_MONO = 0x1,
    FMOD_CHANNELMASK_QUAD = 0x33,
    FMOD_DSPCONNECTION_TYPE_STANDARD = 0,
    FMOD_3D = 0x10,
    FMOD_ERR_INVALID_PARAM = 31,
    /* Authored Ducker categories 4,6,8,10,13,20,26,30. */
    JOURNEY_DUCKER_TARGET_MASK = 0x44102550U,
};

extern void *journey_ducker_category_group(uint32_t category);

typedef struct JourneyFmodVector {
    float x;
    float y;
    float z;
} JourneyFmodVector;

typedef void JourneyFmodDspState;

typedef JourneyFmodResult (__cdecl *JourneyDspReadCallback)(
    JourneyFmodDspState *, float *, float *, unsigned int, int, int *);
typedef JourneyFmodResult (__cdecl *JourneyDspShouldProcessCallback)(
    JourneyFmodDspState *, int, unsigned int, unsigned int, int, int);

typedef struct JourneyFmodDspDescription110 {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers;
    int numoutputbuffers;
    void *create;
    void *release;
    void *reset;
    JourneyDspReadCallback read;
    void *process;
    void *setposition;
    int numparameters;
    void *paramdesc;
    void *setparameterfloat;
    void *setparameterint;
    void *setparameterbool;
    void *setparameterdata;
    void *getparameterfloat;
    void *getparameterint;
    void *getparameterbool;
    void *getparameterdata;
    JourneyDspShouldProcessCallback shouldiprocess;
    void *userdata;
    void *sys_register;
    void *sys_deregister;
    void *sys_mix;
} JourneyFmodDspDescription110;

typedef struct JourneyFmodDspStatePrefix {
    void *instance;
} JourneyFmodDspStatePrefix;

typedef struct JourneyRenderer {
    Slapper slapper;
    const SlapperPresetMailbox *mailbox;
    const volatile uint32_t *transition_gain_bits;
    uint32_t applied_sequence;
    uint32_t meter_index;
    int apply_transition_gain;
    /* Mixer-thread scratch belongs to the persistent renderer, not its stack. */
    double callback_mono[HOST_FRAMES];
    double callback_quad[EARLY_LANES][HOST_FRAMES];
} JourneyRenderer;

typedef struct JourneyBranchConfig {
    uint64_t native_cue_ticket;
    int source_channels;
    uint32_t binding_token;
    float wet_gain;
    unsigned retire_request;
    unsigned retire_ack;
    unsigned release_ack;
} JourneyBranchConfig;

typedef struct JourneyDuckerGain {
    volatile uint32_t gain_q16;
    void *dsp;
    uint8_t category;
} JourneyDuckerGain;

typedef struct JourneyPatchVoice {
    unsigned retirement_pending;
    void *source_channel;
    void *route_group;
    void *tap_dsp;
    int cue_index;
    JourneyBranchConfig branch;
    uint8_t active;
} JourneyPatchVoice;

typedef struct JourneyPatchCue {
    uint64_t shared_chain_token;
    uint64_t native_cue_ticket;
    uint32_t source_generation;
    unsigned owned_dsps;
    unsigned releasing;
    void *journey_cue;
    void *source_group;
    void *wet_group;
    void *wet_head;
    void *spatial_fold_dsp;
    /* Spatial processors only: ancestor gains are read once per private wet
     * cue and applied by journey_publish_group_controls, not multiplied again
     * at each shared spatial node. These nodes do not replace the dry graph. */
    struct {
        void *source;
        void *group;
        void *fold;
        int original_volume_ramp;
        uint8_t ramp_restored;
        JourneyPublishedGroupState published;
    } spatial_ancestors[8];
    uint8_t spatial_ancestor_count;
    uint8_t spatial_chain_connected;
    void *observing_fold; /* Atomic DSP identity: lifetime/onset = final input. */
    uint32_t initial_mix_complete;
    int original_volume_ramp;
    uint8_t volume_ramp_restored;
    JourneyPublishedGroupState published;
    uint32_t live_voices;
    uint8_t music;
    uint8_t category;
    uint8_t active;
} JourneyPatchCue;

typedef struct JourneyStreamRoute {
    void *stream_group;
    uint32_t cue_generation,stream_generation;
    void *stream;
    void *source_group;
    float dry;
    float wet;
    int source_channels;
    uint8_t active;
} JourneyStreamRoute;


#include "native_owner_domain.h"
#include "native_channel_binding.h"
enum { JOURNEY_SHARED_CAPACITY=JOURNEY_MAX_PATCH_CUES,
       JOURNEY_SHARED_ANCESTORS=8, JOURNEY_SHARED_CHAIN_DEPTH=9 };
typedef struct JourneySharedChain {
    unsigned state,refs,count,music; /* 0 free, 1 live/building, 2 retiring */
    uint32_t generation,stamps[JOURNEY_SHARED_ANCESTORS];
    void *sources[JOURNEY_SHARED_ANCESTORS],*head;
    uint64_t updated_epoch;
    JourneyPatchCue dsp_owner; /* independent callback storage, not a cue slot */
} JourneySharedChain;

typedef struct JourneyControlCueUse {
    void *cue;
    uint64_t ticket;
} JourneyControlCueUse;

typedef struct JourneyReverbPatchState {
    JourneyControlCueUse control_uses[JOURNEY_NATIVE_CUE_SLOTS];
    JourneyOwnerDomain native_owners;
    JourneyChannelBindings channel_bindings;
    JourneySharedChain shared_chains[JOURNEY_SHARED_CAPACITY];
    JourneyFmodApi api;
    void *system;
    const float *resampler_table;
    void *system_master;
    void *master_dsp;
    JourneyMaster48Adapter master;
    void *silent_sink;
    void *sfx_renderer_group;
    void *music_renderer_group;
    void *sfx_renderer_dsp;
    void *music_renderer_dsp;
    void *sfx_renderer_head;
    void *music_renderer_head;
    JourneyRenderer sfx_renderer;
    JourneyRenderer music_renderer;
    JourneyReverbControl control;
    JourneyBranchSpatialMirror branch_spatial;
    JourneyDuckerAdapter ducker;
    JourneyDuckerGain ducker_gains[JOURNEY_DUCKER_CATEGORIES];
    uint64_t ducker_last_parent_clock;
    uint64_t ducker_sample_remainder_60;
    uint32_t ducker_sample_rate;
    uint8_t ducker_have_clock;
    JourneyPatchVoice voices[JOURNEY_MAX_PATCH_VOICES];
    JourneyPatchCue cues[JOURNEY_MAX_PATCH_CUES];
    JourneyStreamRoute streams[JOURNEY_MAX_STREAM_ROUTES];
    JourneyReverbPatchStats stats;
} JourneyReverbPatchState;

/* All three bounded tables share the existing state lifetime. */
#define channel_bindings (g_patch->channel_bindings)
#define g_native_owners (g_patch->native_owners)
#define journey_shared_chains (g_patch->shared_chains)
static JourneyReverbPatchState *g_patch;
/* Native ownership helpers. Call-site hooks are installed by the builder.
 * g_native_owners.manager must be initialized before source admission.
 */
#include "native_owner_domain.h"
#include "native_owner_startup.h"

int journey_event_owner_startup(void *wrapper,const uint32_t category_types[33]) {
    return g_patch && g_patch->stats.initialized &&
        journey_domain_startup(&g_native_owners,wrapper,g_patch->system,
                              g_patch->system_master,category_types);
}
static void journey_control_admit(void *cue,uint64_t ticket);
static void journey_control_reap(void);
void journey_event_cue_admit_native(void *cue) {
    if(!g_patch)return;
    uint64_t ticket=journey_domain_admit(&g_native_owners,cue);
    if(ticket)journey_control_admit(cue,ticket);
}
void journey_event_cue_retire(void *cue,void *group) {
    if(!g_patch)return;
    (void)journey_domain_finalize(&g_native_owners,cue,group);
}
uint32_t journey_event_group_generation(void *group) {
    if(!g_patch)return 0;
    return journey_domain_generation(&g_native_owners,(uintptr_t)group);
}
static uint64_t journey_event_cue_ticket(void *cue) {
    if(!g_patch)return 0;
    return journey_domain_cue_ticket(&g_native_owners,cue);
}
static int journey_event_cue_ticket_live(uint64_t ticket) {
    if(!g_patch)return 0;
    return journey_cue_live(&g_native_owners.cues,ticket);
}
static uint32_t journey_event_stream_owner(void *stream,void *group,void *cue_group) {
    if(!g_patch)return 0;
    return journey_domain_stream(&g_native_owners,(uintptr_t)group,
        (uintptr_t)stream,(uintptr_t)cue_group);
}


/*
 * PS4 LR1 declares its four native lanes as a raw four-channel bus and feeds
 * them through one centered output panner. The selected stereo path uses the
 * recovered focus-180 normalized mode-1 matrix.
 */



static const float k_desert_fields[SLAPPER_PRESET_FIELD_COUNT] = {
    1.0f, 0.105f, 0.01f, 0.336f, 0.014f, 0.098f,
    0.0f, 1.0f, 3.427f, 0.049f, 0.797f, 1.0f
};

static const float k_journey_music_fields[SLAPPER_PRESET_FIELD_COUNT] = {
    1.0f, 0.67f, 0.05f, 0.017f, 0.035f, 0.75f,
    0.02f, 0.32f, 3.2f, 0.65f, 0.76f, 0.65f
};

static void *read_pointer_at(const void *base, size_t offset)
{
    void *value = NULL;
    if (base != NULL)
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static float read_float_at(const void *base, size_t offset)
{
    float value = 0.0f;
    if (base != NULL)
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static SlapperPreset preset_from_fields(
    const float fields[SLAPPER_PRESET_FIELD_COUNT])
{
    SlapperPreset preset;
    preset.effect_level = fields[0];
    preset.effect_low_pass_cutoff = fields[1];
    preset.effect_high_pass_cutoff = fields[2];
    preset.early_delay = fields[3];
    preset.early_time_factor = fields[4];
    preset.early_level = fields[5];
    preset.slapback = fields[6];
    preset.early_hf_ratio = fields[7];
    preset.decay_time = fields[8];
    preset.decay_level = fields[9];
    preset.decay_hf_ratio = fields[10];
    preset.diffusion = fields[11];
    return preset;
}

static float load_atomic_float(const volatile uint32_t *source)
{
    const uint32_t bits = __atomic_load_n(source, __ATOMIC_ACQUIRE);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static JourneyFmodResult __cdecl ducker_gain_read(
    JourneyFmodDspState *state,
    float *input,
    float *output,
    unsigned int length,
    int input_channels,
    int *output_channels)
{
    JourneyFmodDspStatePrefix *prefix = (JourneyFmodDspStatePrefix *)state;
    JourneyDuckerGain *gain = NULL;
    uint32_t gain_q16;
    float scalar;
    uint64_t samples;
    uint64_t index;
    if (g_patch == NULL || prefix == NULL || output == NULL ||
        output_channels == NULL || input_channels <= 0 ||
        g_patch->api.dsp_get_user_data(prefix->instance,
                                       (void **)&gain) != 0 ||
        gain == NULL)
        return FMOD_ERR_INVALID_PARAM;
    *output_channels = input_channels;
    gain_q16 = __atomic_load_n(&gain->gain_q16, __ATOMIC_ACQUIRE);
    scalar = journey_ducker_scale_sample(1.0f, gain_q16);
    samples = (uint64_t)length * (uint64_t)(unsigned int)input_channels;
    if (input == NULL) {
        memset(output, 0, (size_t)samples * sizeof(float));
        return 0;
    }
    for (index = 0; index < samples; ++index)
        output[index] = input[index] * scalar;
    return 0;
}

static JourneyFmodResult __cdecl always_process(
    JourneyFmodDspState *state,
    int inputs_idle,
    unsigned int length,
    unsigned int input_mask,
    int input_channels,
    int speaker_mode)
{
    (void)state;
    (void)inputs_idle;
    (void)length;
    (void)input_mask;
    (void)input_channels;
    (void)speaker_mode;
    return 0;
}

static JourneyFmodResult __cdecl renderer_read(
    JourneyFmodDspState *state,
    float *input,
    float *output,
    unsigned int length,
    int input_channels,
    int *output_channels)
{
    JourneyFmodDspStatePrefix *prefix = (JourneyFmodDspStatePrefix *)state;
    JourneyRenderer *renderer = NULL;
    unsigned int offset;
    if (g_patch == NULL || prefix == NULL || output == NULL ||
        output_channels == NULL || length == 0U ||
        length % HOST_FRAMES != 0U || input_channels <= 0 ||
        g_patch->api.dsp_get_user_data(prefix->instance,
                                       (void **)&renderer) != 0 ||
        renderer == NULL) {
        return FMOD_ERR_INVALID_PARAM;
    }
    *output_channels = 2;
    for (offset = 0; offset < length; offset += HOST_FRAMES) {
        double *mono = renderer->callback_mono;
        double (*quad)[HOST_FRAMES] = renderer->callback_quad;
        float fields[SLAPPER_PRESET_FIELD_COUNT];
        float transition_gain = 1.0f;
        unsigned int frame;
        if (slapper_mailbox_consume(renderer->mailbox,
                                    &renderer->applied_sequence,
                                    fields)) {
            const SlapperPreset preset = preset_from_fields(fields);
            slapper_set_preset(&renderer->slapper, &preset);
        }
        if (renderer->apply_transition_gain)
            transition_gain = load_atomic_float(
                renderer->transition_gain_bits);
        for (frame = 0; frame < HOST_FRAMES; ++frame) {
            const unsigned int source_frame = offset + frame;
            if (input == NULL) {
                mono[frame] = 0.0;
            } else if (input_channels == 1) {
                mono[frame] = input[source_frame];
            } else {
                double sum = 0.0;
                int channel;
                for (channel = 0; channel < input_channels; ++channel)
                    sum += input[source_frame * (unsigned int)input_channels +
                                 (unsigned int)channel];
                mono[frame] = sum / sqrt((double)input_channels);
            }
        }
        slapper_process(&renderer->slapper, mono, quad);
        for (frame = 0; frame < HOST_FRAMES; ++frame) {
            const double lanes[4] = {quad[0][frame], quad[1][frame],
                                     quad[2][frame], quad[3][frame]};
            journey_lr1_stereo_sample(lanes, transition_gain,
                                      output + (offset + frame) * 2U);
        }
    }
    return 0;
}

static JourneyFmodResult __cdecl branch_created(JourneyFmodDspState *state)
{
    JourneyFmodDspStatePrefix *prefix=(JourneyFmodDspStatePrefix *)state;
    JourneyBranchConfig *config=NULL;
    if(g_patch->api.dsp_get_user_data(prefix->instance,(void **)&config)!=0 || !config)return FMOD_ERR_INVALID_PARAM;
    ((void **)state)[1]=config; /* FMOD_DSP_STATE::plugindata, owned by this DSP */
    __atomic_store_n(&config->release_ack,0,__ATOMIC_RELEASE);
    return 0;
}
static JourneyFmodResult __cdecl branch_released(JourneyFmodDspState *state)
{
    JourneyBranchConfig *config=((void **)state)[1];
    if(!config)return FMOD_ERR_INVALID_PARAM;
    __atomic_store_n(&config->release_ack,1,__ATOMIC_RELEASE);
    return 0;
}
static JourneyFmodResult __cdecl branch_read(
    JourneyFmodDspState *state,
    float *input,
    float *output,
    unsigned int length,
    int input_channels,
    int *output_channels)
{
    JourneyFmodDspStatePrefix *prefix = (JourneyFmodDspStatePrefix *)state;
    JourneyBranchConfig *config = state ? ((void **)state)[1] : NULL;
    unsigned int frame;
    if (g_patch == NULL || prefix == NULL || output == NULL ||
        output_channels == NULL || input_channels <= 0 ||
        config == NULL) {
        return FMOD_ERR_INVALID_PARAM;
    }
    *output_channels = 1;
    if(!journey_event_cue_ticket_live(config->native_cue_ticket) ||
       __atomic_load_n(&config->retire_request,__ATOMIC_ACQUIRE) ||
       journey_binding_is_retired(&channel_bindings,config->binding_token)) {
        memset(output,0,(size_t)length*sizeof(*output));
        __atomic_store_n(&config->retire_ack,1,__ATOMIC_RELEASE);
        return 0;
    }
    if (input == NULL) {
        memset(output, 0, (size_t)length * sizeof(*output));
        return 0;
    }
    for (frame = 0; frame < length; ++frame) {
        const float *source = input +
            frame * (unsigned int)input_channels;
        float value;
        if (config->source_channels == 1) {
            if (input_channels == 1) {
                value = source[0];
            } else {
                float energy = 0.0f;
                float sum = 0.0f;
                int channel;
                for (channel = 0; channel < input_channels; ++channel) {
                    energy += source[channel] * source[channel];
                    sum += source[channel];
                }
                value = (sum < 0.0f ? -1.0f : 1.0f) *
                    (float)sqrt((double)energy);
            }
        } else if (input_channels == 1) {
            value = source[0];
        } else {
            /* PS4 stereo sampler params[0x51]=0, [0x52]=0xff:
             * 0x431190 -> 0x431250 sums both source lanes without normalization. */
            value = source[0] + source[1];
        }
        output[frame] = value * config->wet_gain;
    }
    return 0;
}

/* Every wet cue contains one mono sum. FMOD applies its existing distance and
 * Doppler processing, then equal-power speaker panning. Recover that scalar
 * BEFORE different cues mix; collapsing the combined scene cannot undo pan.
 * This is an energy identity, not a gain fitted to recorded audio. */
static JourneyFmodResult __cdecl wet_cue_scalar_read(
    JourneyFmodDspState *state, float *input, float *output,
    unsigned int length, int input_channels, int *output_channels)
{
    unsigned int frame;
    (void)state;
    if (!output || !output_channels || input_channels < 1)
        return FMOD_ERR_INVALID_PARAM;
    *output_channels = 1;
    for (frame = 0; frame < length; ++frame) {
        double energy = 0.0, sum = 0.0;
        int channel;
        for (channel = 0; input && channel < input_channels; ++channel) {
            const double value = input[frame * (unsigned int)input_channels + channel];
            energy += value * value;
            sum += value;
        }
        output[frame] = (float)((sum < 0.0 ? -1.0 : 1.0) * sqrt(energy));
    }
    {
        JourneyFmodDspStatePrefix *prefix = (JourneyFmodDspStatePrefix *)state;
        JourneyPatchCue *cue = state ? ((void **)state)[1] : NULL;
        if (prefix && cue
                && prefix->instance == __atomic_load_n(
                    &cue->observing_fold, __ATOMIC_ACQUIRE)
                ) {
            __atomic_store_n(&cue->initial_mix_complete, 1U, __ATOMIC_RELEASE);
        }
    }
    return 0;
}

static JourneyFmodResult __cdecl master_standard_read(
    JourneyFmodDspState *state, float *input, float *output,
    unsigned int length, int input_channels, int *output_channels)
{
    JourneyFmodDspStatePrefix *prefix = (JourneyFmodDspStatePrefix *)state;
    JourneyMaster48Adapter *master = NULL;
    if (!g_patch || !prefix ||
        g_patch->api.dsp_get_user_data(prefix->instance, (void **)&master) != 0 ||
        master != &g_patch->master)
        return FMOD_ERR_INVALID_PARAM;
    return journey_master48_adapter_process(master, input, output, length,
        input_channels, output_channels) ? 0 : FMOD_ERR_INVALID_PARAM;
}

static JourneyFmodResult __cdecl cue_created(JourneyFmodDspState *state)
{
    JourneyFmodDspStatePrefix *prefix=(JourneyFmodDspStatePrefix *)state;
    JourneyPatchCue *cue=NULL;
    if(g_patch->api.dsp_get_user_data(prefix->instance,(void **)&cue)!=0 || !cue)return FMOD_ERR_INVALID_PARAM;
    ((void **)state)[1]=cue;
    __atomic_add_fetch(&cue->owned_dsps,1,__ATOMIC_ACQ_REL);
    return 0;
}
static JourneyFmodResult __cdecl cue_released(JourneyFmodDspState *state)
{
    JourneyPatchCue *cue=((void **)state)[1];
    if(!cue)return FMOD_ERR_INVALID_PARAM;
    __atomic_sub_fetch(&cue->owned_dsps,1,__ATOMIC_ACQ_REL);
    return 0;
}
static void initialize_description(
    JourneyFmodDspDescription110 *description,
    const char *name,
    JourneyDspReadCallback callback,
    void *userdata,
    int always_active)
{
    size_t length = 0;
    while (name[length] != '\0') ++length;
    memset(description, 0, sizeof(*description));
    if (length > sizeof(description->name) - 1U)
        length = sizeof(description->name) - 1U;
    description->pluginsdkversion = FMOD_PLUGIN_SDK_VERSION_110;
    memcpy(description->name, name, length);
    description->version = 0x00010000U;
    description->numinputbuffers = 1;
    description->numoutputbuffers = 1;
    description->read = callback;
    if(callback==wet_cue_scalar_read) {
        description->create=cue_created;description->release=cue_released;
    }
    description->shouldiprocess = always_active ? always_process : NULL;
    description->userdata = userdata;
}

static int initialize_ducker_path(
    JourneyReverbPatchState *patch, uint32_t sample_rate)
{
    unsigned category;
    journey_ducker_reset(&patch->ducker);
    patch->ducker_sample_rate = sample_rate;
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category) {
        JourneyDuckerGain *gain = &patch->ducker_gains[category];
        JourneyFmodDspDescription110 description;
        void *group;
        if ((JOURNEY_DUCKER_TARGET_MASK & (1U << category)) == 0U)
            continue;
        gain->category = (uint8_t)category;
        __atomic_store_n(&gain->gain_q16, JOURNEY_DUCKER_Q16_ONE,
                         __ATOMIC_RELAXED);
        group = journey_ducker_category_group(category);
        initialize_description(&description, "Journey Native Ducker",
                               ducker_gain_read, gain, 0);
        if (group == NULL ||
            patch->api.system_create_dsp(
                patch->system, &description, &gain->dsp) != 0 ||
            patch->api.group_add_dsp(
                group, FMOD_CHANNELCONTROL_DSP_TAIL, gain->dsp) != 0 ||
            patch->api.dsp_set_active(gain->dsp, 1) != 0) {
            /* Any already-attached nodes remain unity pass-throughs.  Opcode
             * wrappers see ducker_initialized==0 and retain stock PC fades. */
            patch->ducker.healthy = 0U;
            ++patch->stats.ducker_failures;
            return 0;
        }
    }
    patch->stats.ducker_initialized = 1U;
    return 1;
}

static int api_is_complete(const JourneyFmodApi *api)
{
    return api != NULL &&
        api->publish_new_wet_group != NULL &&
        api->publish_new_wet_groups != NULL &&
        api->group_get_volume_ramp != NULL && api->group_set_volume_ramp != NULL &&
        api->system_update != NULL &&
        api->system_get_master_group != NULL &&
        api->system_get_dsp_buffer_size != NULL &&
        api->system_get_software_format != NULL &&
        api->system_create_group != NULL &&
        api->system_create_dsp != NULL && api->group_add_group != NULL &&
        api->group_add_dsp != NULL && api->group_get_dsp != NULL &&
        api->group_get_dsp_clock != NULL &&
        api->group_get_num_channels != NULL &&
        api->group_get_volume != NULL && api->group_get_mute != NULL &&
        api->group_get_paused != NULL && api->group_set_paused != NULL &&
        api->group_get_parent != NULL && api->group_get_mode != NULL &&
        api->group_get_3d_attributes != NULL &&
        api->group_get_3d_min_max_distance != NULL &&
        api->group_get_3d_level != NULL &&
        api->group_get_3d_doppler_level != NULL &&
        api->group_set_volume != NULL && api->group_set_mute != NULL &&
        api->group_set_mode != NULL &&
        api->group_set_3d_attributes != NULL &&
        api->group_set_3d_min_max_distance != NULL &&
        api->group_set_3d_level != NULL &&
        api->group_set_3d_doppler_level != NULL &&
        api->group_release != NULL &&
        api->channel_set_group != NULL && api->channel_get_dsp != NULL &&
        api->channel_get_current_sound != NULL &&
        api->sound_get_format != NULL && api->dsp_get_user_data != NULL &&
        api->dsp_set_channel_format != NULL &&
        api->dsp_add_input != NULL && api->dsp_disconnect_all != NULL &&
        api->dsp_set_active != NULL && api->dsp_release != NULL;
}

static void invalidate_publication_state(JourneyPatchCue *cue)
{
    unsigned i;
    journey_group_publication_invalidate(&cue->published);
    for (i = 0U; i < 7U; ++i)
        journey_group_publication_invalidate(
            &cue->spatial_ancestors[i].published);
}


/* Shared spatial ancestry. Control-domain mutations only; FMOD DSP
 * release callbacks decrement owned_dsps. A chain never borrows another cue's
 * callback storage. Native group generations, order and music/SFX destination
 * are part of identity; source dry/wet, scalar gain and Ducker remain private.
 *
 * One slot per patch-cue capacity. Retiring chains can temporarily consume
 * slots: admission fails safely instead of recycling callback-owned memory.
 * This is a bounded policy, not a claim of unlimited native hierarchy depth.
 * No timers, delayed-free guesses, callback graph edits or process aborts.
 */

static uint64_t journey_shared_epoch=1;
static unsigned journey_shared_builds,journey_shared_frees,journey_shared_reuses;

static void journey_shared_failure(void) { ++g_patch->stats.mirror_failures; }
static JourneySharedChain *journey_shared_resolve(uint64_t token) {
    unsigned i=(unsigned)token;
    if(!token || i>=JOURNEY_SHARED_CAPACITY)return NULL;
    JourneySharedChain *n=&journey_shared_chains[i];
    return n->state==1 && n->generation==(uint32_t)(token>>32)?n:NULL;
}
/* On an API failure retain the handle for a later control update. Never lose
 * the only reference to callback-owned storage or claim it has been released. */
static int journey_shared_cleanup(JourneySharedChain *n) {
    int complete=1;
    for(unsigned i=0;i<n->count;i++) {
        void *d=n->dsp_owner.spatial_ancestors[i].fold;
        void *g=n->dsp_owner.spatial_ancestors[i].group;
        if(d) {
            if(g_patch->api.dsp_disconnect_all(d,1,1) || g_patch->api.dsp_release(d)) {
                journey_shared_failure();complete=0;continue;
            }
            n->dsp_owner.spatial_ancestors[i].fold=NULL;
        }
        if(g) {
            if(g_patch->api.group_release(g)) {journey_shared_failure();complete=0;}
            else n->dsp_owner.spatial_ancestors[i].group=NULL;
        }
    }
    return complete;
}
static void journey_shared_reap(void) {
    for(unsigned i=0;i<JOURNEY_SHARED_CAPACITY;i++) {
        JourneySharedChain *n=&journey_shared_chains[i];
        if(n->state==2 && journey_shared_cleanup(n) &&
           !__atomic_load_n(&n->dsp_owner.owned_dsps,__ATOMIC_ACQUIRE)) {
            uint32_t generation=n->generation;
            memset(n,0,sizeof(*n));n->generation=generation;++journey_shared_frees;
        }
    }
}
static void journey_shared_retire(JourneySharedChain *n) {
    if(n->refs || n->state!=1){journey_shared_failure();return;}
    n->state=2;
    __atomic_store_n(&n->dsp_owner.observing_fold,NULL,__ATOMIC_RELEASE);
    (void)journey_shared_cleanup(n);
}
static int release_spatial_ancestors(JourneyPatchCue *cue) {
    if(cue->shared_chain_token) {
        JourneySharedChain *n=journey_shared_resolve(cue->shared_chain_token);
        if(!n || !n->refs){journey_shared_failure();return 0;}
        if(cue->spatial_fold_dsp && g_patch->api.dsp_disconnect_all(cue->spatial_fold_dsp,0,1)) {
            journey_shared_failure();return 0;
        }
        cue->shared_chain_token=0;
        if(!--n->refs)journey_shared_retire(n);
    }
    cue->spatial_chain_connected=0;
    __atomic_store_n(&cue->observing_fold,NULL,__ATOMIC_RELEASE);
    return 1;
}
static int collect_spatial_ancestors(JourneyPatchCue *cue,void **sources,unsigned *count) {
    void *group=cue->source_group,*seen[JOURNEY_SHARED_CHAIN_DEPTH];
    unsigned traversed=0;*count=0;
    while(group!=g_patch->system_master) {
        void *parent=NULL;unsigned mode=0;
        if(!group || traversed==JOURNEY_SHARED_CHAIN_DEPTH)return 0;
        for(unsigned i=0;i<traversed;i++)if(seen[i]==group)return 0;
        seen[traversed]=group;
        if(g_patch->api.group_get_mode(group,&mode) || g_patch->api.group_get_parent(group,&parent))return 0;
        if(traversed && (mode&FMOD_3D))sources[(*count)++]=group;
        traversed++;group=parent;
    }
    return traversed!=0;
}
static int journey_shared_matches(JourneySharedChain *n,void **sources,uint32_t *stamps,unsigned count,unsigned music) {
    if(n->state!=1 || n->count!=count || n->music!=music)return 0;
    for(unsigned i=0;i<count;i++)if(n->sources[i]!=sources[i] || n->stamps[i]!=stamps[i])return 0;
    return 1;
}
static int journey_shared_update(JourneySharedChain *n,int force) {
    if(!force && n->updated_epoch==journey_shared_epoch)return 1;
    for(unsigned i=0;i<n->count;i++) {
        if(journey_event_group_generation(n->sources[i])!=n->stamps[i])return 0;
        if(!n->dsp_owner.spatial_ancestors[i].ramp_restored &&
           __atomic_load_n(&n->dsp_owner.initial_mix_complete,__ATOMIC_ACQUIRE)) {
            if(g_patch->api.group_set_volume_ramp(n->dsp_owner.spatial_ancestors[i].group,
                n->dsp_owner.spatial_ancestors[i].original_volume_ramp))return 0;
            n->dsp_owner.spatial_ancestors[i].ramp_restored=1;
        }
        if(!journey_mirror_spatial_state_cached(&g_patch->api,n->sources[i],
            n->dsp_owner.spatial_ancestors[i].group,&n->dsp_owner.spatial_ancestors[i].published,FMOD_3D))return 0;
    }
    n->updated_epoch=journey_shared_epoch;return 1;
}
static JourneySharedChain *journey_shared_create(void **sources,uint32_t *stamps,unsigned count,unsigned music) {
    JourneySharedChain *n=NULL;
    /* Reaping belongs to the existing update pass, not each source admission. */
    for(unsigned i=0;i<JOURNEY_SHARED_CAPACITY;i++)
        if(!journey_shared_chains[i].state && journey_shared_chains[i].generation!=UINT32_MAX){n=&journey_shared_chains[i];break;}
    if(!n)return NULL;
    if(n->generation)++journey_shared_reuses;
    n->generation++;n->state=1;n->count=count;n->music=music;++journey_shared_builds;
    memcpy(n->sources,sources,count*sizeof(void *));memcpy(n->stamps,stamps,count*sizeof(uint32_t));
    void *previous=NULL,*publication[JOURNEY_SHARED_ANCESTORS];
    for(unsigned i=0;i<count;i++) {
        JourneyFmodDspDescription110 d;void *head=NULL,*tail=NULL;
        initialize_description(&d,"Journey Shared Spatial Fold",wet_cue_scalar_read,&n->dsp_owner,0);
        if(g_patch->api.system_create_group(g_patch->system,"Journey Shared Spatial Ancestor",&n->dsp_owner.spatial_ancestors[i].group))goto fail;
        void *g=n->dsp_owner.spatial_ancestors[i].group;publication[i]=g;
        if(g_patch->api.group_get_volume_ramp(g,&n->dsp_owner.spatial_ancestors[i].original_volume_ramp) ||
           g_patch->api.group_set_volume_ramp(g,0) || g_patch->api.group_set_volume(g,1) ||
           g_patch->api.group_set_mute(g,0) || g_patch->api.group_add_group(g_patch->silent_sink,g,1,NULL) ||
           g_patch->api.group_get_dsp(g,FMOD_CHANNELCONTROL_DSP_HEAD,&head) ||
           g_patch->api.group_get_dsp(g,FMOD_CHANNELCONTROL_DSP_TAIL,&tail) ||
           g_patch->api.dsp_set_channel_format(head,FMOD_CHANNELMASK_MONO,1,FMOD_SPEAKERMODE_MONO) ||
           g_patch->api.dsp_set_channel_format(tail,FMOD_CHANNELMASK_QUAD,4,FMOD_SPEAKERMODE_QUAD) ||
           !journey_mirror_spatial_state_cached(&g_patch->api,sources[i],g,&n->dsp_owner.spatial_ancestors[i].published,FMOD_3D) ||
           g_patch->api.system_create_dsp(g_patch->system,&d,&n->dsp_owner.spatial_ancestors[i].fold))goto fail;
        void *fold=n->dsp_owner.spatial_ancestors[i].fold;
        if(g_patch->api.dsp_add_input(fold,tail,NULL,FMOD_DSPCONNECTION_TYPE_STANDARD) ||
           g_patch->api.dsp_set_active(fold,1))goto fail;
        if(previous) {if(g_patch->api.dsp_add_input(head,previous,NULL,FMOD_DSPCONNECTION_TYPE_STANDARD))goto fail;}
        else n->head=head;
        previous=fold;
    }
    if(g_patch->api.publish_new_wet_groups(g_patch->system,publication,count))goto fail;
    __atomic_store_n(&n->dsp_owner.observing_fold,previous,__ATOMIC_RELEASE);
    if(g_patch->api.dsp_add_input(music?g_patch->music_renderer_head:g_patch->sfx_renderer_head,
        previous,NULL,FMOD_DSPCONNECTION_TYPE_STANDARD))goto fail;
    n->updated_epoch=journey_shared_epoch;return n;
fail:
    journey_shared_retire(n);return NULL;
}
static int mirror_spatial_ancestors(JourneyPatchCue *cue,int newly_created) {
    void *sources[JOURNEY_SHARED_ANCESTORS];uint32_t stamps[JOURNEY_SHARED_ANCESTORS];unsigned count=0;
    if(cue->releasing)return 1;
    if(!collect_spatial_ancestors(cue,sources,&count))return 0;
    for(unsigned i=0;i<count;i++){stamps[i]=journey_event_group_generation(sources[i]);if(!stamps[i])return 0;}
    JourneySharedChain *n=journey_shared_resolve(cue->shared_chain_token);
    if(n && journey_shared_matches(n,sources,stamps,count,cue->music))return journey_shared_update(n,newly_created);
    if(!count && !cue->shared_chain_token && cue->spatial_chain_connected && !newly_created)return 1;
    if(cue->shared_chain_token && !release_spatial_ancestors(cue))return 0;
    n=NULL;
    if(cue->spatial_fold_dsp && g_patch->api.dsp_disconnect_all(cue->spatial_fold_dsp,0,1))return 0;
    if(newly_created && g_patch->api.publish_new_wet_groups(g_patch->system,&cue->wet_group,1))return 0;
    void *head=cue->music?g_patch->music_renderer_head:g_patch->sfx_renderer_head;
    if(count) {
        for(unsigned i=0;i<JOURNEY_SHARED_CAPACITY;i++)
            if(journey_shared_matches(&journey_shared_chains[i],sources,stamps,count,cue->music)){n=&journey_shared_chains[i];break;}
        if(!n)n=journey_shared_create(sources,stamps,count,cue->music);
        if(!n)return 0;
        if(!journey_shared_update(n,newly_created)){if(!n->refs)journey_shared_retire(n);return 0;}
        head=n->head;
    }
    if(g_patch->api.dsp_add_input(head,cue->spatial_fold_dsp,NULL,FMOD_DSPCONNECTION_TYPE_STANDARD)) {
        if(n && !n->refs)journey_shared_retire(n);
        return 0;
    }
    if(n){n->refs++;cue->shared_chain_token=((uint64_t)n->generation<<32)|(unsigned)(n-journey_shared_chains);}
    cue->spatial_chain_connected=1;
    __atomic_store_n(&cue->observing_fold,cue->spatial_fold_dsp,__ATOMIC_RELEASE);
    return 1;
}


static int create_renderer(
    JourneyReverbPatchState *patch,
    const char *group_name,
    const char *dsp_name,
    JourneyRenderer *renderer,
    const float default_fields[SLAPPER_PRESET_FIELD_COUNT],
    uint32_t seed,
    void **group_out,
    void **dsp_out,
    void **head_out)
{
    JourneyFmodDspDescription110 description;
    const SlapperPreset preset = preset_from_fields(default_fields);
    void *group = NULL;
    void *dsp = NULL;
    void *head = NULL;
    if (!slapper_init(&renderer->slapper, &preset,
                      patch->resampler_table, seed))
        return 0;
    initialize_description(&description, dsp_name, renderer_read,
                           renderer, 1);
    if (patch->api.system_create_group(patch->system, group_name,
                                       &group) != 0 ||
        patch->api.group_add_group(patch->system_master, group, 1,
                                   NULL) != 0 ||
        patch->api.system_create_dsp(patch->system, &description,
                                     &dsp) != 0 ||
        patch->api.dsp_set_channel_format(
            dsp, FMOD_CHANNELMASK_STEREO, 2, FMOD_SPEAKERMODE_STEREO) != 0 ||
        patch->api.group_add_dsp(group, 0, dsp) != 0 ||
        patch->api.group_get_dsp(group, FMOD_CHANNELCONTROL_DSP_HEAD,
                                 &head) != 0) {
        if (dsp != NULL) (void)patch->api.dsp_release(dsp);
        if (group != NULL) (void)patch->api.group_release(group);
        slapper_destroy(&renderer->slapper);
        return 0;
    }
    /* PS4 0x3b2430/0x3adfe0: source side ports enter an AUX_SINK_MONO_MONO.
     * Keep the wet path mono through its spatial mixer, before LR1 stereo. */
    if (patch->api.dsp_set_channel_format(head, FMOD_CHANNELMASK_MONO,
                                         1, FMOD_SPEAKERMODE_MONO) != 0) {
        (void)patch->api.dsp_release(dsp);
        (void)patch->api.group_release(group);
        slapper_destroy(&renderer->slapper);
        return 0;
    }
    *group_out = group;
    *dsp_out = dsp;
    *head_out = head;
    return 1;
}

int journey_reverb_patch_initialize_with_api(
    void *system,
    const JourneyFmodApi *api,
    const float resampler_table[1024])
{
    JourneyReverbPatchState *patch;
    unsigned int buffer_length = 0;
    int buffer_count = 0;
    int sample_rate = 0;
    int speaker_mode = 0;
    int raw_speakers = 0;
    if (g_patch != NULL) return g_patch->stats.initialized != 0U;
    if (system == NULL || resampler_table == NULL || !api_is_complete(api))
        return 0;
    patch = (JourneyReverbPatchState *)malloc(sizeof(*patch));
    if (patch == NULL) return 0;
    memset(patch, 0, sizeof(*patch));
    patch->api = *api;
    patch->system = system;
    patch->resampler_table = resampler_table;
    if (patch->api.system_get_dsp_buffer_size(
            system, &buffer_length, &buffer_count) != 0 ||
        patch->api.system_get_software_format(
            system, &sample_rate, &speaker_mode, &raw_speakers) != 0 ||
        sample_rate != 48000 || buffer_length == 0U ||
        buffer_length % HOST_FRAMES != 0U ||
        patch->api.system_get_master_group(
            system, &patch->system_master) != 0 ||
        patch->system_master == NULL) {
        free(patch);
        return 0;
    }
    (void)buffer_count;
    (void)speaker_mode;
    (void)raw_speakers;
    /* Fixed native stereo preset: reject another output domain before adding
     * any graph nodes. The existing preflight already enforces48k/256 cadence. */
    if (speaker_mode != FMOD_SPEAKERMODE_STEREO && speaker_mode != FMOD_SPEAKERMODE_MONO) {
        free(patch);
        return 0;
    }
    journey_reverb_control_reset(&patch->control);
    slapper_mailbox_publish(&patch->control.sfx, k_desert_fields);
    slapper_mailbox_publish(&patch->control.music, k_journey_music_fields);

    patch->sfx_renderer.mailbox = &patch->control.sfx;
    patch->sfx_renderer.transition_gain_bits =
        &patch->control.sfx_transition_gain_bits;
    patch->sfx_renderer.meter_index = 0U;
    patch->sfx_renderer.apply_transition_gain = 1;
    patch->music_renderer.mailbox = &patch->control.music;
    patch->music_renderer.transition_gain_bits = NULL;
    patch->music_renderer.meter_index = 1U;
    patch->music_renderer.apply_transition_gain = 0;

    /* Publish the state before FMOD can invoke either custom callback. */
    g_patch = patch;
    if (patch->api.system_create_group(system, "Journey Reverb Sink",
                                       &patch->silent_sink) != 0 ||
        patch->api.group_add_group(patch->system_master,
                                   patch->silent_sink, 1, NULL) != 0 ||
        patch->api.group_set_volume(patch->silent_sink, 0.0f) != 0 ||
        !create_renderer(patch, "Journey SFX Reverb",
                         "Journey SFX Slapper", &patch->sfx_renderer,
                         k_desert_fields, 0U,
                         &patch->sfx_renderer_group,
                         &patch->sfx_renderer_dsp,
                         &patch->sfx_renderer_head) ||
        !create_renderer(patch, "Journey Music Reverb",
                         "Journey Music Slapper", &patch->music_renderer,
                         k_journey_music_fields, 64U,
                         &patch->music_renderer_group,
                         &patch->music_renderer_dsp,
                         &patch->music_renderer_head)) {
        /* Initialization runs once.  Leave any partial graph silent and
         * disabled; no voice hook will attach unless initialized is set. */
        if (patch->sfx_renderer_group != NULL)
            (void)patch->api.group_set_volume(
                patch->sfx_renderer_group, 0.0f);
        if (patch->music_renderer_group != NULL)
            (void)patch->api.group_set_volume(
                patch->music_renderer_group, 0.0f);
        return 0;
    }
    {
        JourneyFmodDspDescription110 description;
        JourneyFmodResult result;
        journey_master48_adapter_initialize(&patch->master);
        initialize_description(&description, "Journey Master Standard",
            master_standard_read, &patch->master, 1);
        result = patch->api.system_create_dsp(system, &description, &patch->master_dsp);
        if (!result) result = patch->api.dsp_set_channel_format(patch->master_dsp,
            FMOD_CHANNELMASK_STEREO, 2, FMOD_SPEAKERMODE_STEREO);
        /* Index0 is the output side of the root fader. All existing engine
         * dry/category paths and mirrored wet gains meet here exactly once. */
        if (!result) result = patch->api.group_add_dsp(
            patch->system_master, 0, patch->master_dsp);
        if (result) {
            if (patch->master_dsp) (void)patch->api.dsp_release(patch->master_dsp);
            patch->master_dsp = NULL;
            (void)patch->api.group_set_volume(patch->sfx_renderer_group, 0.0f);
            (void)patch->api.group_set_volume(patch->music_renderer_group, 0.0f);
            return 0;
        }
    }
    /* Ducker failure is isolated: the room-response repair remains initialized and
     * opcode wrappers fall back to the untouched stock fade helper. */
    (void)initialize_ducker_path(patch, (uint32_t)sample_rate);
    patch->stats.initialized = 1U;
    return 1;
}

void journey_reverb_patch_publish(const void *reverb_barn)
{
    if (g_patch != NULL && g_patch->stats.initialized != 0U &&
        reverb_barn != NULL)
    {
        (void)journey_reverb_publish_from_manager(&g_patch->control, reverb_barn);
    }
}

void journey_branch_spatial_register_cue(void *child_cue)
{
    void *parent_cue;
    void *child_group;
    void *parent_group;
    if (g_patch == NULL || g_patch->stats.initialized == 0U ||
        child_cue == NULL)
        return;
    parent_cue = read_pointer_at(child_cue, 0x28U);
    if(!parent_cue || !journey_event_cue_ticket(parent_cue))return;
    child_group = read_pointer_at(child_cue, 0xC0U);
    parent_group = read_pointer_at(parent_cue, 0xC0U);
    void *actual_parent=NULL;
    if(!child_group || !parent_group ||
       g_patch->api.group_get_parent(child_group,&actual_parent)) {
        ++g_patch->stats.branch_spatial_failures;return;
    }
    if(actual_parent==parent_group)return; /* native Start Child */
    if (journey_branch_spatial_register(
            &g_patch->branch_spatial, &g_patch->api,
            child_cue, parent_cue, child_group, parent_group)) {
        ++g_patch->stats.branch_spatial_registers;
    } else {
        ++g_patch->stats.branch_spatial_failures;
    }
    g_patch->stats.branch_spatial_active =
        g_patch->branch_spatial.active_count;
}

void journey_branch_spatial_unregister_cue(void *cue)
{
    if (g_patch == NULL || cue == NULL) return;
    journey_branch_spatial_unregister(&g_patch->branch_spatial, cue);
    g_patch->stats.branch_spatial_active =
        g_patch->branch_spatial.active_count;
}

static uint8_t cue_category(const void *cue)
{
    const void *wrapper = read_pointer_at(cue, 0xF0U);
    const void *descriptor = read_pointer_at(wrapper, sizeof(void *));
    return descriptor == NULL ? 0U : *((const uint8_t *)descriptor + 5U);
}

static int cue_is_music(const void *cue)
{
    const uint8_t category = cue_category(cue);
    return category == 1U || category == 22U;
}

void journey_ducker_patch_register_cue(void *cue)
{
    const void *wrapper;
    const void *descriptor;
    uint16_t flags = 0U;
    uint8_t category;
    if (g_patch == NULL || g_patch->stats.ducker_initialized == 0U ||
        cue == NULL)
        return;
    wrapper = read_pointer_at(cue, 0xF0U);
    descriptor = read_pointer_at(wrapper, sizeof(void *));
    if (descriptor == NULL) return;
    memcpy(&flags, (const uint8_t *)descriptor + 0x0AU, sizeof(flags));
    if (!journey_ducker_counts_cue_flags(flags))
        return; /* native handler type 6 */
    category = *((const uint8_t *)descriptor + 5U);
    if (!journey_ducker_register_handler(&g_patch->ducker, cue, category))
        ++g_patch->stats.ducker_failures;
}

void journey_ducker_patch_unregister_cue(void *cue)
{
    if (g_patch == NULL || cue == NULL) return;
    journey_ducker_unregister_handler(&g_patch->ducker, cue);
}

int journey_ducker_patch_configure(void *cue, const void *descriptor)
{
    int handled;
    uint32_t target_mask = 0U;
    if (g_patch == NULL || g_patch->stats.ducker_initialized == 0U ||
        !g_patch->ducker.healthy || descriptor == NULL)
        return 0;
    memcpy(&target_mask, (const uint8_t *)descriptor + 4U,
           sizeof(target_mask));
    if ((target_mask & ~JOURNEY_DUCKER_TARGET_MASK) != 0U)
        return 0;
    handled = journey_ducker_configure(&g_patch->ducker, cue, descriptor);
    if (handled) ++g_patch->stats.ducker_configures;
    else ++g_patch->stats.ducker_failures;
    return handled;
}

int journey_ducker_patch_retire(void *cue, uint32_t scaled_local_index)
{
    int handled;
    if (g_patch == NULL || g_patch->stats.ducker_initialized == 0U ||
        !g_patch->ducker.healthy)
        return 0;
    handled = journey_ducker_retire_local(
        &g_patch->ducker, cue, scaled_local_index);
    if (handled) ++g_patch->stats.ducker_retires;
    else ++g_patch->stats.ducker_failures;
    return handled;
}

int journey_ducker_patch_forget(void *cue, uint32_t local_index)
{
    if (g_patch == NULL || g_patch->stats.ducker_initialized == 0U ||
        !g_patch->ducker.healthy)
        return 0;
    (void)cue;(void)local_index; /* deferred whole-use cleanup */
    return 1;
}



/* The following control_drop/reap/admit helpers execute under the existing
 * worker/update semaphore; the separate source-END callback below does not.
 * Never dereference a stored cue on retirement: its native pool slot can
 * already be reset. Unregister functions use that address only as a key. */
static void journey_control_drop(JourneyControlCueUse *use) {
    if(!use->ticket)return;
    journey_branch_spatial_unregister_cue(use->cue);
    journey_ducker_patch_unregister_cue(use->cue);
    memset(use,0,sizeof(*use));
}
static void journey_control_reap(void) {
    for(unsigned i=0;i<JOURNEY_NATIVE_CUE_SLOTS;i++) {
        JourneyControlCueUse *use=&g_patch->control_uses[i];
        if(use->ticket && !journey_event_cue_ticket_live(use->ticket))
            journey_control_drop(use);
    }
}
static void journey_control_admit(void *cue,uint64_t ticket) {
    unsigned index=(uint32_t)(ticket>>32)%JOURNEY_NATIVE_CUE_SLOTS;
    JourneyControlCueUse *use=&g_patch->control_uses[index];
    if(use->ticket==ticket)return;
    journey_control_drop(use); /* old same-address use, before new registration */
    use->cue=cue;use->ticket=ticket;
    journey_ducker_patch_register_cue(cue);
    journey_branch_spatial_register_cue(cue);
}

/* Called by the tested Win64 adapter, before original native END. */
void journey_event_source_end(void *channel)
{
    if(!g_patch)return;
    (void)journey_binding_channel_end(&channel_bindings,(uintptr_t)channel);
}
static int free_voice_index(void)
{
    int index;
    for (index = 0; index < JOURNEY_MAX_PATCH_VOICES; ++index)
        if (!g_patch->voices[index].active) return index;
    return -1;
}

static int free_cue_index(void)
{
    int index;
    for (index = 0; index < JOURNEY_MAX_PATCH_CUES; ++index)
        if (!g_patch->cues[index].active) return index;
    return -1;
}

static int find_cue_index(void *cue, void *source_group)
{
    int index;
    uint64_t ticket=journey_event_cue_ticket(cue);
    uint32_t generation=journey_event_group_generation(source_group);
    if(!ticket || !generation)return -1;
    for (index = 0; index < JOURNEY_MAX_PATCH_CUES; ++index) {
        JourneyPatchCue *slot = &g_patch->cues[index];
        if (slot->active && !slot->releasing && slot->journey_cue == cue &&
            slot->source_group == source_group && slot->native_cue_ticket==ticket &&
            slot->source_generation==generation)
            return index;
    }
    return -1;
}

#include "native_cue_release.inc"

static int obtain_cue_index(void *cue, void *source_group, int music,
                            int *was_created)
{
    JourneyPatchCue *slot;
    JourneyGroupMirrorApi mirror_api;
    JourneyGroupMirrorResult mirror_result;
    void *tail = NULL;
    void *renderer_head;
    JourneyFmodDspDescription110 fold_description;
    int index = find_cue_index(cue, source_group);
    *was_created = 0;
    if (index >= 0) return index;
    uint64_t ticket=journey_event_cue_ticket(cue);
    uint32_t generation=journey_event_group_generation(source_group);
    if(!ticket || !generation)return -1;
    index = free_cue_index();
    if (index < 0) return -1;
    slot = &g_patch->cues[index];
    memset(slot, 0, sizeof(*slot));
    slot->journey_cue = cue;
    slot->source_group = source_group;
    slot->native_cue_ticket=ticket;slot->source_generation=generation;
    slot->music = music != 0;
    slot->category = cue_category(cue);
    renderer_head = music
        ? g_patch->music_renderer_head : g_patch->sfx_renderer_head;
    if (g_patch->api.system_create_group(
            g_patch->system,
            music ? "Journey Wet Music Cue" : "Journey Wet SFX Cue",
            &slot->wet_group) != 0 ||
        /* A new FMOD group otherwise ramps from its default unity gain for
         * 64 samples, even when configured fully out of range before mixing.
         * Initialize at the copied state; the control loop restores the
         * group's original ramp after the first mix acknowledgment. */
        g_patch->api.group_get_volume_ramp(slot->wet_group,
                                          &slot->original_volume_ramp) != 0 ||
        g_patch->api.group_set_volume_ramp(slot->wet_group, 0) != 0 ||
        g_patch->api.group_add_group(g_patch->silent_sink,
                                     slot->wet_group, 1, NULL) != 0 ||
        g_patch->api.group_get_dsp(slot->wet_group,
                                   FMOD_CHANNELCONTROL_DSP_HEAD,
                                   &slot->wet_head) != 0 ||
        g_patch->api.group_get_dsp(slot->wet_group,
                                   FMOD_CHANNELCONTROL_DSP_TAIL,
                                   &tail) != 0
        ) {
        if (slot->wet_group != NULL)
            (void)g_patch->api.group_release(slot->wet_group);
        memset(slot, 0, sizeof(*slot));
        return -1;
    }
    initialize_description(&fold_description, "Journey Wet Cue Scalar",
                           wet_cue_scalar_read, slot, 0);
    /* Keep each input mono, retain FMOD's spatial processor, and recover
     * the distance-attenuated scalar from its equal-power speaker lanes. */
    if (g_patch->api.dsp_set_channel_format(slot->wet_head,
            FMOD_CHANNELMASK_MONO, 1, FMOD_SPEAKERMODE_MONO) != 0 ||
        g_patch->api.dsp_set_channel_format(tail,
            FMOD_CHANNELMASK_QUAD, 4, FMOD_SPEAKERMODE_QUAD) != 0 ||
        g_patch->api.system_create_dsp(g_patch->system, &fold_description,
            &slot->spatial_fold_dsp) != 0 ||
        g_patch->api.dsp_add_input(slot->spatial_fold_dsp, tail, NULL,
            FMOD_DSPCONNECTION_TYPE_STANDARD) != 0 ||
        g_patch->api.dsp_add_input(renderer_head, slot->spatial_fold_dsp, NULL,
            FMOD_DSPCONNECTION_TYPE_STANDARD) != 0 ||
        g_patch->api.dsp_set_active(slot->spatial_fold_dsp, 1) != 0) {
        slot->active=1;
        ++g_patch->stats.active_cues;
        release_cue_slot(index);
        return -1;
    }
    mirror_api.get_volume = g_patch->api.group_get_volume;
    mirror_api.get_mute = g_patch->api.group_get_mute;
    mirror_api.get_parent = g_patch->api.group_get_parent;
    mirror_api.set_volume = g_patch->api.group_set_volume;
    mirror_api.set_mute = g_patch->api.group_set_mute;
    mirror_api.get_paused = g_patch->api.group_get_paused;
    mirror_api.set_paused = g_patch->api.group_set_paused;
    mirror_result = journey_read_group_chain(
        &mirror_api, source_group, g_patch->system_master);
    if (mirror_result.status != 0 ||
        !journey_publish_group_controls(
            &g_patch->api, slot->wet_group, &slot->published,
            mirror_result.paused, mirror_result.mute,
            mirror_result.volume *
                ((float)journey_ducker_effective_q16(
                    &g_patch->ducker, slot->category) *
                 (1.0f / 65536.0f)))
        || !journey_mirror_spatial_state_cached(
            &g_patch->api, source_group, slot->wet_group,
            &slot->published, FMOD_3D)
        || !mirror_spatial_ancestors(slot, 1)) {
        slot->active=1;
        ++g_patch->stats.active_cues;
        release_cue_slot(index);
        return -1;
    }
    slot->active = 1U;
    *was_created = 1;
    ++g_patch->stats.active_cues;
    return index;
}

static int source_channel_count(void *channel)
{
    void *sound = NULL;
    int channels = 0;
    if (g_patch->api.channel_get_current_sound(channel, &sound) != 0 ||
        sound == NULL ||
        g_patch->api.sound_get_format(sound, NULL, NULL,
                                      &channels, NULL) != 0 ||
        channels <= 0)
        return 0;
    return channels;
}

static void release_voice_slot(int index);
static void attach_route(void *cue, void *channel, float dry, float wet,
                         int channels)
{
    JourneyPatchVoice *voice;
    JourneyFmodDspDescription110 description;
    void *source_group;
    void *channel_tail = NULL;
    int voice_index;
    int cue_index = -1;
    int cue_created = 0;
    if (g_patch == NULL || g_patch->stats.initialized == 0U ||
        cue == NULL || channel == NULL || channels <= 0 ||
        !(dry == dry) || !(wet == wet))
        return;
    /* The canonical direct route requires no extra objects. */
    if (dry == 1.0f && wet == 0.0f) return;
    source_group = NULL;
    if (g_patch->api.channel_get_group(channel, &source_group) != 0) goto failure;
    voice_index = free_voice_index();
    if (source_group == NULL || voice_index < 0 ||
        !journey_event_cue_ticket(cue) || !journey_event_group_generation(source_group)) goto failure;
    voice = &g_patch->voices[voice_index];
    memset(voice, 0, sizeof(*voice));
    voice->cue_index = -1;
    voice->branch.source_channels = channels;
    voice->branch.wet_gain = wet;
    voice->branch.native_cue_ticket=journey_event_cue_ticket(cue);
    voice->branch.release_ack = 1;
    voice->source_channel = channel;
    voice->branch.binding_token=journey_binding_bind_channel(&channel_bindings,(uintptr_t)channel,(unsigned)voice_index);
    if(!voice->branch.binding_token)goto failure;
    if (g_patch->api.system_create_group(g_patch->system,
                                         "Journey Dry Voice",
                                         &voice->route_group) != 0 ||
        g_patch->api.group_add_group(source_group, voice->route_group,
                                     1, NULL) != 0 ||
        g_patch->api.group_set_volume(voice->route_group, dry) != 0) {
        goto voice_failure;
    }
    if (wet != 0.0f) {
        cue_index = obtain_cue_index(cue, source_group,
                                     cue_is_music(cue), &cue_created);
        if (cue_index < 0) goto voice_failure;
        initialize_description(&description, "Journey Wet Voice Tap",
                               branch_read, &voice->branch, 0);
        description.create=branch_created;
        description.release=branch_released;
        if (g_patch->api.system_create_dsp(g_patch->system, &description,
                                           &voice->tap_dsp) != 0 ||
            g_patch->api.channel_get_dsp(channel,
                                         FMOD_CHANNELCONTROL_DSP_TAIL,
                                         &channel_tail) != 0 ||
            g_patch->api.dsp_add_input(voice->tap_dsp, channel_tail,
                                       NULL,
                                       FMOD_DSPCONNECTION_TYPE_STANDARD) != 0 ||
            g_patch->api.dsp_add_input(g_patch->cues[cue_index].wet_head,
                                       voice->tap_dsp, NULL,
                                       FMOD_DSPCONNECTION_TYPE_STANDARD) != 0 ||
            g_patch->api.dsp_set_active(voice->tap_dsp, 1) != 0) {
            goto voice_failure;
        }
    }
    if (g_patch->api.channel_set_group(channel, voice->route_group) != 0)
        goto voice_failure;
    voice->cue_index = cue_index;
    voice->source_channel = channel;
    voice->active = 1U;
    if (cue_index >= 0) ++g_patch->cues[cue_index].live_voices;
    ++g_patch->stats.active_voices;
    return;

voice_failure:
    (void)journey_binding_retire_exact(&channel_bindings,(uintptr_t)channel,voice->branch.binding_token);
    voice->cue_index=cue_index;
    voice->active=1;
    voice->retirement_pending=1;
    ++g_patch->stats.active_voices;
    if(cue_index>=0)++g_patch->cues[cue_index].live_voices;
    __atomic_store_n(&voice->branch.retire_request,1,__ATOMIC_RELEASE);
    release_voice_slot(voice_index);
failure:
    ++g_patch->stats.attach_failures;
}

void journey_reverb_patch_attach_tone(
    void *cue,
    void *channel,
    const void *selected_tone_descriptor)
{
    const float dry = read_float_at(selected_tone_descriptor, 0x18U);
    const float wet = read_float_at(selected_tone_descriptor, 0x1CU);
    attach_route(cue, channel, dry, wet, 1);
}

static void stream_route_values(const void *stream_descriptor,
                                float *dry, float *wet)
{
    /*
     * These are the fixed console voice-routing fields.  Do not follow the
     * PC Stream extension pointer computed from flags at +0x12: extension
     * +0x14/+0x1c are new/prior Stream fade durations, not dry/wet sends.
     * Every canonical Journey PC SFX Stream has both 0x4000 and 0x8000, so
     * the complete fixed LFE/dry/wet triplet is present.
     */
    *dry = read_float_at(stream_descriptor, 0x18U);
    *wet = read_float_at(stream_descriptor, 0x1CU);
}

#include "native_stream_routes.inc"

#include "native_voice_release.inc"

static void update_ducker_clock(void)
{
    uint64_t dsp_clock = 0U;
    uint64_t parent_clock = 0U;
    uint64_t scaled;
    unsigned category;
    if (g_patch->stats.ducker_initialized == 0U) return;
    if (g_patch->api.group_get_dsp_clock(
            g_patch->system_master, &dsp_clock, &parent_clock) != 0) {
        ++g_patch->stats.ducker_failures;
        return;
    }
    (void)dsp_clock;
    if (!g_patch->ducker_have_clock ||
        parent_clock < g_patch->ducker_last_parent_clock) {
        g_patch->ducker_have_clock = 1U;
        g_patch->ducker_last_parent_clock = parent_clock;
        g_patch->ducker_sample_remainder_60 = 0U;
        return;
    }
    scaled = parent_clock - g_patch->ducker_last_parent_clock;
    g_patch->ducker_last_parent_clock = parent_clock;
    {
        const uint64_t before = g_patch->ducker_sample_remainder_60;
        const uint64_t total = scaled * 60U + before;
        g_patch->stats.ducker_steps +=
            (uint32_t)(total / g_patch->ducker_sample_rate);
        g_patch->ducker_sample_remainder_60 = journey_ducker_advance_samples(
            &g_patch->ducker, scaled, g_patch->ducker_sample_rate, before);
    }
    for (category = 0; category < JOURNEY_DUCKER_CATEGORIES; ++category) {
        JourneyDuckerGain *gain = &g_patch->ducker_gains[category];
        if ((JOURNEY_DUCKER_TARGET_MASK & (1U << category)) == 0U)
            continue;
        __atomic_store_n(
            &gain->gain_q16,
            journey_ducker_effective_q16(&g_patch->ducker,
                                          (uint8_t)category),
            __ATOMIC_RELEASE);
    }
}

JourneyFmodResult journey_reverb_patch_system_update(void *system)
{
    JourneyGroupMirrorApi mirror_api;
    int index;
    if (g_patch == NULL || g_patch->stats.initialized == 0U ||
        system == NULL)
        return FMOD_ERR_INVALID_PARAM;
    journey_control_reap();
    ++journey_shared_epoch;journey_shared_reap();
    update_ducker_clock();
    g_patch->stats.branch_spatial_failures +=
        journey_branch_spatial_update(
            &g_patch->branch_spatial, &g_patch->api);
    unsigned char playing_cues[JOURNEY_MAX_PATCH_CUES]={0};
    for (index = 0; index < JOURNEY_MAX_PATCH_VOICES; ++index) {
        JourneyPatchVoice *voice = &g_patch->voices[index];
        int count = 0;
        if (!voice->active) continue;
        (void)count;
        if(!journey_event_cue_ticket_live(voice->branch.native_cue_ticket) ||
           (voice->cue_index>=0 && g_patch->cues[voice->cue_index].source_generation!=
            journey_event_group_generation(g_patch->cues[voice->cue_index].source_group))) {
            (void)journey_binding_retire_exact(&channel_bindings,(uintptr_t)voice->source_channel,voice->branch.binding_token);
            __atomic_store_n(&voice->branch.retire_request,1,__ATOMIC_RELEASE);
        }
        if(__atomic_load_n(&voice->branch.retire_request,__ATOMIC_ACQUIRE) ||
           journey_binding_is_retired(&channel_bindings,voice->branch.binding_token)) {
            __atomic_store_n(&voice->branch.retire_request,1,__ATOMIC_RELEASE);
            voice->retirement_pending=1;
            release_voice_slot(index);
        } else if(voice->cue_index>=0 && voice->cue_index<JOURNEY_MAX_PATCH_CUES)
            playing_cues[voice->cue_index]=1;
    }
    for (index = 0; index < JOURNEY_MAX_PATCH_CUES; ++index) {
        JourneyPatchCue *cue = &g_patch->cues[index];
        if (cue->active && cue->live_voices == 0U)
            release_cue_slot(index);
    }
    mirror_api.get_volume = g_patch->api.group_get_volume;
    mirror_api.get_mute = g_patch->api.group_get_mute;
    mirror_api.get_parent = g_patch->api.group_get_parent;
    mirror_api.set_volume = g_patch->api.group_set_volume;
    mirror_api.set_mute = g_patch->api.group_set_mute;
    mirror_api.get_paused = g_patch->api.group_get_paused;
    mirror_api.set_paused = g_patch->api.group_set_paused;
    for (index = 0; index < JOURNEY_MAX_PATCH_CUES; ++index) {
        JourneyPatchCue *cue = &g_patch->cues[index];
        JourneyGroupMirrorResult result;
        if (!cue->active) continue;
        if(!playing_cues[index] || !journey_event_cue_ticket_live(cue->native_cue_ticket) ||
           journey_event_group_generation(cue->source_group)!=cue->source_generation)continue;
        if (!cue->volume_ramp_restored && __atomic_load_n(
                &cue->initial_mix_complete, __ATOMIC_ACQUIRE)) {
            const int restore = g_patch->api.group_set_volume_ramp(
                cue->wet_group, cue->original_volume_ramp);
            if (restore == 0) cue->volume_ramp_restored = 1U;
            else ++g_patch->stats.mirror_failures;
        }
        result = journey_read_group_chain(
            &mirror_api, cue->source_group, g_patch->system_master);
        if (result.status == 0) {
            const float ducker = journey_ducker_scale_sample(
                1.0f, journey_ducker_effective_q16(
                    &g_patch->ducker, cue->category));
            if (!journey_publish_group_controls(
                    &g_patch->api, cue->wet_group, &cue->published,
                    result.paused, result.mute,
                    result.volume * ducker))
                result.status = -4;
        }
        if (result.status != 0 ||
            !journey_mirror_spatial_state_cached(
                &g_patch->api, cue->source_group, cue->wet_group,
                &cue->published, FMOD_3D)
            || !mirror_spatial_ancestors(cue, 0)
            ) {
            /* Never leave a failed/partially updated ancestor chain audible.
             * Retry normally on a subsequent control update. */
            (void)g_patch->api.group_set_mute(cue->wet_group, 1);
            (void)g_patch->api.group_set_volume(cue->wet_group, 0.0f);
            invalidate_publication_state(cue);
            ++g_patch->stats.mirror_failures;
        }
    }
    for (index = 0; index < JOURNEY_MAX_STREAM_ROUTES; ++index) {
        JourneyStreamRoute *route = &g_patch->streams[index];
        float ignored;
        if (!route->active) continue;
        if (!event_stream_route_live(route) || route->source_group == NULL ||
            g_patch->api.group_get_volume(route->source_group,
                                          &ignored) != 0) {
            memset(route, 0, sizeof(*route));
            if (g_patch->stats.active_stream_routes != 0U)
                --g_patch->stats.active_stream_routes;
        }
    }
    journey_domain_prune(&g_native_owners);
    {
        const JourneyFmodResult result = g_patch->api.system_update(system);
        return result;
    }
}

JourneyReverbPatchStats journey_reverb_patch_stats(void)
{
    JourneyReverbPatchStats result;
    memset(&result, 0, sizeof(result));
    if (g_patch != NULL) {
        int ducker_index;
        result = g_patch->stats;
        result.ducker_configured_mask = g_patch->ducker.configured_mask;
        for (ducker_index = 0; ducker_index < 32; ++ducker_index) {
            const JourneyDuckerState *state =
                &g_patch->ducker.states[ducker_index];
            result.ducker_effective_q16[ducker_index] =
                journey_ducker_effective_q16(
                    &g_patch->ducker, (uint8_t)ducker_index);
            result.ducker_live[ducker_index] =
                g_patch->ducker.live[ducker_index];
            result.ducker_flagged[ducker_index] =
                g_patch->ducker.flagged[ducker_index];
            result.ducker_state_current_q16[ducker_index] =
                state->current_q16;
            result.ducker_state_target_q16[ducker_index] =
                state->target_q16;
            result.ducker_state_target_mask[ducker_index] =
                state->target_mask;
            result.ducker_state_meta[ducker_index] =
                (uint32_t)state->watched_category |
                ((uint32_t)state->configured << 8) |
                ((uint32_t)state->retiring << 9);
        }
        /* These three fields are written by FMOD's mixer thread.  Reload
         * them atomically instead of relying on the aggregate structure copy
         * performed by the game thread above. */
        result.renderer_callbacks = __atomic_load_n(
            &g_patch->stats.renderer_callbacks, __ATOMIC_RELAXED);
        result.renderer_native_blocks = __atomic_load_n(
            &g_patch->stats.renderer_native_blocks, __ATOMIC_RELAXED);
        result.branch_callbacks = __atomic_load_n(
            &g_patch->stats.branch_callbacks, __ATOMIC_RELAXED);
        result.renderer_input_channel_mask = __atomic_load_n(
            &g_patch->stats.renderer_input_channel_mask, __ATOMIC_RELAXED);
        result.branch_input_channel_mask = __atomic_load_n(
            &g_patch->stats.branch_input_channel_mask, __ATOMIC_RELAXED);
        result.renderer_input_capture_state = __atomic_load_n(
            &g_patch->stats.renderer_input_capture_state, __ATOMIC_ACQUIRE);
        if (result.renderer_input_capture_state == 2U) {
            int index;
            for (index = 0; index < 4; ++index)
                result.renderer_first_input_bits[index] = __atomic_load_n(
                    &g_patch->stats.renderer_first_input_bits[index],
                    __ATOMIC_RELAXED);
        }
    }
    return result;
}

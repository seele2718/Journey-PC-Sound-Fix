/* Runtime core for the static Journey PC console-reverb repair.
 *
 * The core accepts an FMOD C-API table populated through GetProcAddress.
 * This uses Journey's shipped fmod64.dll without an added import directory
 * or an additional runtime DLL.
 */

#ifndef JOURNEY_PC_REVERB_PATCH_RUNTIME_H
#define JOURNEY_PC_REVERB_PATCH_RUNTIME_H

#include <stdint.h>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#define __cdecl
#endif

typedef int JourneyFmodResult;

typedef struct JourneyFmodApi {
    /* Exact-DLL-gated bootstrap callback; only unpublished wet groups. */
    JourneyFmodResult (__cdecl *publish_new_wet_group)(void *system, void *group);
    JourneyFmodResult (__cdecl *publish_new_wet_groups)(void *system, void **groups, unsigned count);

    JourneyFmodResult (__cdecl *system_update)(void *system);
    JourneyFmodResult (__cdecl *system_get_master_group)(void *system, void **group);
    JourneyFmodResult (__cdecl *system_get_dsp_buffer_size)(void *system, unsigned int *length, int *count);
    JourneyFmodResult (__cdecl *system_get_software_format)(void *system, int *rate, int *speaker_mode, int *raw_speakers);
    JourneyFmodResult (__cdecl *system_create_group)(void *system, const char *name, void **group);
    JourneyFmodResult (__cdecl *system_create_dsp)(void *system, const void *description, void **dsp);

    JourneyFmodResult (__cdecl *group_add_group)(void *parent, void *child, int propagate_clock, void **connection);
    JourneyFmodResult (__cdecl *group_add_dsp)(void *group, int index, void *dsp);
    JourneyFmodResult (__cdecl *group_get_dsp)(void *group, int index, void **dsp);
    JourneyFmodResult (__cdecl *group_get_dsp_clock)(void *, uint64_t *, uint64_t *);
    JourneyFmodResult (__cdecl *group_get_num_channels)(void *group, int *count);
    JourneyFmodResult (__cdecl *group_get_volume_ramp)(void *group, int *enabled);
    JourneyFmodResult (__cdecl *group_set_volume_ramp)(void *group, int enabled);
    JourneyFmodResult (__cdecl *group_get_volume)(void *group, float *value);
    JourneyFmodResult (__cdecl *group_get_mute)(void *group, int *value);
    JourneyFmodResult (__cdecl *group_get_paused)(void *group, int *value);
    JourneyFmodResult (__cdecl *group_get_parent)(void *group, void **parent);
    JourneyFmodResult (__cdecl *group_get_mode)(void *group, unsigned int *mode);
    JourneyFmodResult (__cdecl *group_get_3d_attributes)(void *group, void *position, void *velocity);
    JourneyFmodResult (__cdecl *group_get_3d_min_max_distance)(void *group, float *minimum, float *maximum);
    JourneyFmodResult (__cdecl *group_get_3d_level)(void *group, float *level);
    JourneyFmodResult (__cdecl *group_get_3d_doppler_level)(void *group, float *level);
    JourneyFmodResult (__cdecl *group_set_volume)(void *group, float value);
    JourneyFmodResult (__cdecl *group_set_mute)(void *group, int value);
    JourneyFmodResult (__cdecl *group_set_paused)(void *group, int value);
    JourneyFmodResult (__cdecl *group_set_mode)(void *group, unsigned int mode);
    JourneyFmodResult (__cdecl *group_set_3d_attributes)(void *group, const void *position, const void *velocity);
    JourneyFmodResult (__cdecl *group_set_3d_min_max_distance)(void *group, float minimum, float maximum);
    JourneyFmodResult (__cdecl *group_set_3d_level)(void *group, float level);
    JourneyFmodResult (__cdecl *group_set_3d_doppler_level)(void *group, float level);
    JourneyFmodResult (__cdecl *group_release)(void *group);

    JourneyFmodResult (__cdecl *channel_get_group)(void *channel, void **group);
    JourneyFmodResult (__cdecl *channel_set_group)(void *channel, void *group);
    JourneyFmodResult (__cdecl *channel_get_dsp)(void *channel, int index, void **dsp);
    JourneyFmodResult (__cdecl *channel_get_current_sound)(void *channel, void **sound);
    JourneyFmodResult (__cdecl *sound_get_format)(void *sound, int *type, int *format, int *channels, int *bits);

    JourneyFmodResult (__cdecl *dsp_get_user_data)(void *dsp, void **userdata);
    JourneyFmodResult (__cdecl *dsp_set_channel_format)(void *dsp, unsigned int mask, int channels, int speaker_mode);
    JourneyFmodResult (__cdecl *dsp_add_input)(void *dsp, void *input, void **connection, int type);
    JourneyFmodResult (__cdecl *dsp_disconnect_all)(void *dsp, int inputs, int outputs);
    JourneyFmodResult (__cdecl *dsp_set_active)(void *dsp, int active);
    JourneyFmodResult (__cdecl *dsp_release)(void *dsp);
} JourneyFmodApi;

/* Initialize once after FMOD and native groups are ready, before Journey's
 * startup semaphore release. */
int journey_reverb_patch_initialize_with_api(
    void *system,
    const JourneyFmodApi *api,
    const float resampler_table[1024]);

/* Publish ReverbBarn's selected SFX/music presets and transition gain. */
void journey_reverb_patch_publish(const void *reverb_barn);

/* Attach one newly created embedded Tone Channel. */
void journey_reverb_patch_attach_tone(
    void *cue,
    void *channel,
    const void *selected_tone_descriptor);

/* Register and attach the main Channel returned in one Stream object. */
void journey_reverb_patch_register_stream(
    void *cue,
    void *stream,
    const void *stream_descriptor);

/* Attach a direct transition Channel using its originating Stream route. */
void journey_reverb_patch_attach_stream_channel(
    void *cue,
    void *channel,
    const void *origin_stream);

/* Register a successor Stream and attach its main Channel by inheritance. */
void journey_reverb_patch_register_stream_successor(
    void *cue,
    void *stream,
    const void *origin_stream);

/* Reclaim retired routes; refresh spatial, gain, pause/mute and category
 * controls; then call the original FMOD System::update. */
JourneyFmodResult journey_reverb_patch_system_update(void *system);

/* Branch bookkeeping used by protected control-side admission/retirement.
 * Console Branch keeps one spatial handler; PC creates a child and needs the
 * parent's live 3D state mirrored. These are not destructor-entry hooks. */
void journey_branch_spatial_register_cue(void *child_cue);
void journey_branch_spatial_unregister_cue(void *cue);

/* Ducker bookkeeping uses protected control-side admission/retirement.
 * Configure/retire/forget are called by the localized native opcode wrappers. */
void journey_ducker_patch_register_cue(void *cue);
void journey_ducker_patch_unregister_cue(void *cue);
int journey_ducker_patch_configure(void *cue, const void *descriptor);
int journey_ducker_patch_retire(void *cue, uint32_t scaled_local_index);
int journey_ducker_patch_forget(void *cue, uint32_t local_index);

/* In-memory runtime state/counters. Initialization and health fields also gate
 * live routing; retaining this structure does not enable file logging. */
typedef struct JourneyReverbPatchStats {
    uint32_t initialized;
    uint32_t active_voices;
    uint32_t active_cues;
    uint32_t active_stream_routes;
    uint32_t attach_failures;
    uint32_t mirror_failures;
    uint32_t renderer_callbacks;
    uint32_t renderer_native_blocks;
    uint32_t branch_callbacks;
    uint32_t renderer_input_channel_mask;
    uint32_t branch_input_channel_mask;
    uint32_t branch_spatial_active;
    uint32_t branch_spatial_registers;
    uint32_t branch_spatial_failures;
    uint32_t renderer_input_capture_state;
    uint32_t renderer_first_input_bits[4];
    uint32_t ducker_initialized;
    uint32_t ducker_configures;
    uint32_t ducker_retires;
    uint32_t ducker_steps;
    uint32_t ducker_failures;
    /* Ducker state summary. State meta packs category in bits 0..7,
     * configured in bit 8, and retiring in bit 9. */
    uint32_t ducker_configured_mask;
    uint32_t ducker_effective_q16[32];
    uint32_t ducker_live[32];
    uint32_t ducker_flagged[32];
    uint32_t ducker_state_current_q16[32];
    uint32_t ducker_state_target_q16[32];
    uint32_t ducker_state_target_mask[32];
    uint32_t ducker_state_meta[32];
} JourneyReverbPatchStats;

JourneyReverbPatchStats journey_reverb_patch_stats(void);

#endif

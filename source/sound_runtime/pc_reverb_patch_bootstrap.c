/* Resolve Journey's shipped FMOD 1.10 C API without adding PE imports.
 *
 * GetModuleHandleA and GetProcAddress are reached through tiny assembly
 * thunks into Journey's existing IAT. Initialization failure emits no dialog
 * or file log; the update wrapper still forwards the original mixer update.
 */

#include "pc_reverb_patch_runtime.h"

#include <stddef.h>
#include <string.h>

typedef void *JourneyModule;
typedef void *JourneyProc;

extern JourneyModule __cdecl GetModuleHandleA(const char *name);
extern JourneyProc __cdecl GetProcAddress(JourneyModule module,
                                          const char *name);
extern const float journey_reverb_resampler_table[1024];
extern JourneyFmodResult journey_original_system_update(void *system);

static JourneyProc resolve(JourneyModule module, const char *name);

static int g_reverb_initialized;
#include "reverb_fmod_spatial_publication.inc"


static JourneyProc resolve(JourneyModule module, const char *name)
{
    if (module == NULL) return NULL;
    return GetProcAddress(module, name);
}

#define RESOLVE(table, member, module, export_name) do {                    \
    JourneyProc proc__ = resolve((module), (export_name));                   \
    if (proc__ == NULL) return 0;                                            \
    *(JourneyProc *)(void *)&(table).member = proc__;                        \
} while (0)

int journey_reverb_patch_initialize(void *system)
{
    JourneyFmodApi api;
    JourneyModule fmod;
    fmod = GetModuleHandleA("fmod64.dll");
    if (system == NULL || fmod == NULL) return 0;
    if (!spatial_publication_initialize(fmod)) return 0;
    api.publish_new_wet_group = publish_new_wet_group;
    api.publish_new_wet_groups = publish_new_wet_groups;

    RESOLVE(api, group_get_volume_ramp, fmod, "FMOD_ChannelGroup_GetVolumeRamp");
    RESOLVE(api, group_set_volume_ramp, fmod, "FMOD_ChannelGroup_SetVolumeRamp");
    RESOLVE(api, system_update, fmod, "FMOD_System_Update");
    RESOLVE(api, system_get_master_group, fmod,
            "FMOD_System_GetMasterChannelGroup");
    RESOLVE(api, system_get_dsp_buffer_size, fmod,
            "FMOD_System_GetDSPBufferSize");
    RESOLVE(api, system_get_software_format, fmod,
            "FMOD_System_GetSoftwareFormat");
    RESOLVE(api, system_create_group, fmod,
            "FMOD_System_CreateChannelGroup");
    RESOLVE(api, system_create_dsp, fmod, "FMOD_System_CreateDSP");

    RESOLVE(api, group_add_group, fmod, "FMOD_ChannelGroup_AddGroup");
    RESOLVE(api, group_add_dsp, fmod, "FMOD_ChannelGroup_AddDSP");
    RESOLVE(api, group_get_dsp, fmod, "FMOD_ChannelGroup_GetDSP");
    RESOLVE(api, group_get_dsp_clock, fmod,
            "FMOD_ChannelGroup_GetDSPClock");
    RESOLVE(api, group_get_num_channels, fmod,
            "FMOD_ChannelGroup_GetNumChannels");
    RESOLVE(api, group_get_volume, fmod, "FMOD_ChannelGroup_GetVolume");
    RESOLVE(api, group_get_mute, fmod, "FMOD_ChannelGroup_GetMute");
    RESOLVE(api, group_get_paused, fmod, "FMOD_ChannelGroup_GetPaused");
    RESOLVE(api, group_get_parent, fmod,
            "FMOD_ChannelGroup_GetParentGroup");
    RESOLVE(api, group_get_mode, fmod, "FMOD_ChannelGroup_GetMode");
    RESOLVE(api, group_get_3d_attributes, fmod,
            "FMOD_ChannelGroup_Get3DAttributes");
    RESOLVE(api, group_get_3d_min_max_distance, fmod,
            "FMOD_ChannelGroup_Get3DMinMaxDistance");
    RESOLVE(api, group_get_3d_level, fmod,
            "FMOD_ChannelGroup_Get3DLevel");
    RESOLVE(api, group_get_3d_doppler_level, fmod,
            "FMOD_ChannelGroup_Get3DDopplerLevel");
    RESOLVE(api, group_set_volume, fmod, "FMOD_ChannelGroup_SetVolume");
    RESOLVE(api, group_set_mute, fmod, "FMOD_ChannelGroup_SetMute");
    RESOLVE(api, group_set_paused, fmod, "FMOD_ChannelGroup_SetPaused");
    RESOLVE(api, group_set_mode, fmod, "FMOD_ChannelGroup_SetMode");
    RESOLVE(api, group_set_3d_attributes, fmod,
            "FMOD_ChannelGroup_Set3DAttributes");
    RESOLVE(api, group_set_3d_min_max_distance, fmod,
            "FMOD_ChannelGroup_Set3DMinMaxDistance");
    RESOLVE(api, group_set_3d_level, fmod,
            "FMOD_ChannelGroup_Set3DLevel");
    RESOLVE(api, group_set_3d_doppler_level, fmod,
            "FMOD_ChannelGroup_Set3DDopplerLevel");
    RESOLVE(api, group_release, fmod, "FMOD_ChannelGroup_Release");

    RESOLVE(api, channel_get_group, fmod, "FMOD_Channel_GetChannelGroup");
    RESOLVE(api, channel_set_group, fmod, "FMOD_Channel_SetChannelGroup");
    RESOLVE(api, channel_get_dsp, fmod, "FMOD_Channel_GetDSP");
    RESOLVE(api, channel_get_current_sound, fmod,
            "FMOD_Channel_GetCurrentSound");
    RESOLVE(api, sound_get_format, fmod, "FMOD_Sound_GetFormat");

    RESOLVE(api, dsp_get_user_data, fmod, "FMOD_DSP_GetUserData");
    RESOLVE(api, dsp_set_channel_format, fmod,
            "FMOD_DSP_SetChannelFormat");
    RESOLVE(api, dsp_add_input, fmod, "FMOD_DSP_AddInput");
    RESOLVE(api, dsp_disconnect_all, fmod, "FMOD_DSP_DisconnectAll");
    RESOLVE(api, dsp_set_active, fmod, "FMOD_DSP_SetActive");
    RESOLVE(api, dsp_release, fmod, "FMOD_DSP_Release");

    g_reverb_initialized = journey_reverb_patch_initialize_with_api(
        system, &api, journey_reverb_resampler_table);
    return g_reverb_initialized;
}

/* The hook must never disable Journey's ordinary mixer update merely because
 * the repair could not initialize.  The hard-coded IAT bridge is itself
 * admitted only after the patcher verifies that local import identity. */
JourneyFmodResult journey_reverb_patch_update_or_original(void *system)
{
    if (g_reverb_initialized)
        return journey_reverb_patch_system_update(system);
    return journey_original_system_update(system);
}

#undef RESOLVE

/* Reclaim one patch-owned voice route on the serialized control owner.
 * Source END retires identity only. Disconnect/release the tap, wait for its
 * destruction acknowledgment, then release the route group and binding.
 * Failed API operations retain outstanding handles for retry rather than
 * clearing live callback state or deliberately terminating the process.
 */
static void release_voice_slot(int index)
{
    JourneyPatchVoice *voice=&g_patch->voices[index];
    if(!voice->active)return;
    if(voice->tap_dsp) {
        if(g_patch->api.dsp_disconnect_all(voice->tap_dsp,1,1) ||
           g_patch->api.dsp_release(voice->tap_dsp)) {
            ++g_patch->stats.mirror_failures;goto done;
        }
        voice->tap_dsp=NULL;
    }
    if(!__atomic_load_n(&voice->branch.release_ack,__ATOMIC_ACQUIRE))goto done;
    if(voice->route_group) {
        if(g_patch->api.group_release(voice->route_group)) {
            ++g_patch->stats.mirror_failures;goto done;
        }
        voice->route_group=NULL;
    }
    if(!journey_binding_reclaim_after_dsp_release(&channel_bindings,voice->branch.binding_token)) {
        ++g_patch->stats.mirror_failures;goto done;
    }
    int cue_index=voice->cue_index;
    if(cue_index>=0 && cue_index<JOURNEY_MAX_PATCH_CUES &&
       g_patch->cues[cue_index].active && g_patch->cues[cue_index].live_voices)
        --g_patch->cues[cue_index].live_voices;
    memset(voice,0,sizeof(*voice));
    if(g_patch->stats.active_voices)--g_patch->stats.active_voices;
done:
    return;
}

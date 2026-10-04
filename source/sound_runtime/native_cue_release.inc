/* Reclaim a cue's patch-owned wet route on the serialized control owner.
 * Detach its shared-chain borrow, then release its private DSP and wet group.
 * Successful steps clear their handles; a failed step retains the outstanding
 * ownership for retry. The slot stays active/releasing until its DSP destruction
 * acknowledgments arrive, preventing reuse while callbacks can still reach it.
 * This is resource cleanup, not the cue's authored fade-out or stop schedule.
 */
static void release_cue_slot(int index)
{
    JourneyPatchCue *cue=&g_patch->cues[index];
    if(!cue->active)return;
    cue->releasing=1;
    if(!release_spatial_ancestors(cue))goto done;
    if(cue->spatial_fold_dsp) {
        if(g_patch->api.dsp_disconnect_all(cue->spatial_fold_dsp,1,1) ||
           g_patch->api.dsp_release(cue->spatial_fold_dsp)) {
            ++g_patch->stats.mirror_failures;goto done;
        }
        cue->spatial_fold_dsp=NULL;
    }
    if(cue->wet_group) {
        if(g_patch->api.group_release(cue->wet_group)) {
            ++g_patch->stats.mirror_failures;goto done;
        }
        cue->wet_group=NULL;
    }
    if(__atomic_load_n(&cue->owned_dsps,__ATOMIC_ACQUIRE))goto done;
    memset(cue,0,sizeof(*cue));
    if(g_patch->stats.active_cues)--g_patch->stats.active_cues;
done:
    return;
}

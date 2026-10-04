/* Stream publication, included after attach_route/stream_route_values.
 * Requires native owner generations and JourneyStreamRoute fields:
 * stream_group, cue_generation, stream_generation. Control-thread only.
 * Native call-site wrappers provide the cue lifetime and caller context. */
#include "native_stream_context.h"
extern uint32_t journey_event_group_generation(void *group);
extern int journey_event_install_predecessor_end(void *channel);

static int event_stream_route_live(const JourneyStreamRoute *r) {
    return r->active && r->cue_generation && r->stream_generation &&
        journey_event_group_generation(r->source_group)==r->cue_generation &&
        journey_event_group_generation(r->stream_group)==r->stream_generation;
}
static int find_stream_index(void *stream) {
    for(int i=0;i<JOURNEY_MAX_STREAM_ROUTES;i++)
        if(g_patch->streams[i].stream==stream && event_stream_route_live(&g_patch->streams[i]))return i;
    return -1;
}
void journey_event_prepare_stream(void *cue,const void *origin,unsigned successor,
                                  JourneyStreamContext *out) {
    memset(out,0,sizeof(*out));
    if(!g_patch || !g_patch->stats.initialized || !cue || !origin)return;
    out->cue=cue;out->cue_group=read_pointer_at(cue,0xc0U);
    out->cue_generation=journey_event_group_generation(out->cue_group);
    if(!out->cue_generation)goto failure;
    if(successor) {
        int i=find_stream_index((void *)origin);
        if(i<0 || g_patch->streams[i].source_group!=out->cue_group)goto failure;
        out->dry=g_patch->streams[i].dry;out->wet=g_patch->streams[i].wet;
    } else stream_route_values(origin,&out->dry,&out->wet);
    out->valid=1;return;
failure:
    ++g_patch->stats.attach_failures;
}
static int event_store_stream(void *stream,void *group,uint32_t generation,
                              const JourneyStreamContext *context,int channels) {
    int free_index=-1;
    for(int i=0;i<JOURNEY_MAX_STREAM_ROUTES;i++) {
        JourneyStreamRoute *r=&g_patch->streams[i];
        if(r->active && !event_stream_route_live(r)) {
            r->active=0;--g_patch->stats.active_stream_routes;
        }
        if(r->active && r->stream==stream){free_index=i;break;}
        if(!r->active && free_index<0)free_index=i;
    }
    if(free_index<0)return 0;
    JourneyStreamRoute *r=&g_patch->streams[free_index];
    if(!r->active)++g_patch->stats.active_stream_routes;
    r->stream=stream;r->source_group=context->cue_group;r->stream_group=group;
    r->cue_generation=context->cue_generation;r->stream_generation=generation;
    r->dry=context->dry;r->wet=context->wet;r->source_channels=channels;r->active=1;
    return 1;
}
void journey_event_publish_stream(void *stream,const JourneyStreamContext *context) {
    if(!g_patch || !g_patch->stats.initialized || !stream || !context || !context->valid)return;
    if(journey_event_group_generation(context->cue_group)!=context->cue_generation)goto failure;
    void *group=read_pointer_at(stream,0x48U),*channel=read_pointer_at(stream,0x50U);
    uint32_t generation;
    generation=journey_event_stream_owner(stream,group,context->cue_group);
    if(!generation || !channel)goto failure;
    int channels=source_channel_count(channel);
    if(channels<=0 || !event_store_stream(stream,group,generation,context,channels))goto failure;
    attach_route(context->cue,channel,context->dry,context->wet,channels);
    return;
failure:
    ++g_patch->stats.attach_failures;
}
/* Context-building helper entry points. Native call-site wrappers must use
 * the pre-factory snapshot so descriptor state is captured before reuse. */
void journey_reverb_patch_register_stream(void *cue,void *stream,const void *descriptor) {
    JourneyStreamContext context;
    journey_event_prepare_stream(cue,descriptor,0,&context);
    journey_event_publish_stream(stream,&context);
}
void journey_reverb_patch_register_stream_successor(void *cue,void *stream,const void *origin) {
    JourneyStreamContext context;
    journey_event_prepare_stream(cue,origin,1,&context);
    journey_event_publish_stream(stream,&context);
}
void journey_reverb_patch_attach_stream_channel(void *cue,void *channel,const void *origin) {
    JourneyStreamContext context;
    journey_event_prepare_stream(cue,origin,1,&context);
    int channels=channel?source_channel_count(channel):0;
    if(context.valid && channels>0)
        attach_route(cue,channel,context.dry,context.wet,channels);
    /* Native predecessor attachment uses the publication boundary below. */
}
void journey_event_publish_predecessor(void *channel,const JourneyStreamContext *context) {
    if(!g_patch || !g_patch->stats.initialized || !channel || !context || !context->valid)return;
    if(journey_event_group_generation(context->cue_group)!=context->cue_generation)goto failure;
    int channels=source_channel_count(channel);
    /* This one native path has no callback. Install END-only retirement before
     * attachment/unpause, never replace a Tone or Stream's native callback.
     * Failure leaves native dry playback untouched and creates no wet route. */
    if(channels<=0 || journey_event_install_predecessor_end(channel)!=0)goto failure;
    attach_route(context->cue,channel,context->dry,context->wet,channels);
    return;
failure:
    ++g_patch->stats.attach_failures;
}

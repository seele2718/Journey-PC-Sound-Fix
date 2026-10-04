/* RT-safe fixed48 Master block adapter. Caller validates48kHz stereo before
 * installing. Native processing cadence is256 frames; no heap or file I/O.
 */
#ifndef JOURNEY_MASTER_FIXED48_ADAPTER_H
#define JOURNEY_MASTER_FIXED48_ADAPTER_H
#include "reverb_native_master_fixed48.h"
typedef struct {
    JourneyNativeMaster48 master;
    float input[256][2],output[256][8];
    uint64_t frames;
    uint32_t calls,failures;
} JourneyMaster48Adapter;
static void journey_master48_adapter_initialize(JourneyMaster48Adapter *a) {
    memset(a,0,sizeof(*a));journey_native_master48_initialize(&a->master);
}
static int journey_master48_adapter_process(JourneyMaster48Adapter *a,
    const float *in,float *out,unsigned frames,int input_channels,int *output_channels) {
    if(!a || !out || !output_channels || frames%256 ||
       (input_channels!=0 && input_channels!=2) || (input_channels && !in)) {
        if(a)__atomic_add_fetch(&a->failures,1U,__ATOMIC_RELAXED);
        return 0;
    }
    *output_channels=2;
    for(unsigned offset=0;offset<frames;offset+=256) {
        if(input_channels==2)memcpy(a->input,in+offset*2,sizeof(a->input));
        else memset(a->input,0,sizeof(a->input));
        journey_native_master48_process(&a->master,a->input,a->output);
        for(unsigned i=0;i<256;i++) {
            out[(offset+i)*2]=a->output[i][0];
            out[(offset+i)*2+1]=a->output[i][1];
        }
    }
    __atomic_add_fetch(&a->calls,1U,__ATOMIC_RELAXED);
    __atomic_add_fetch(&a->frames,frames,__ATOMIC_RELAXED);
    return 1;
}
#endif

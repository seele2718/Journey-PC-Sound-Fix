/* Fixed native Master Standard state and processing:
 * 48 kHz,256-frame stereo, unity upstream master gain, authored MasterBuss.
 * Only numeric filter/dynamics/panner state is retained; no native pointers.
 * This header implements the fixed preset, not arbitrary Master settings.
 *
 * The initializer below stores exact 32-bit words into a 0x4000-byte state
 * image. Most words are IEEE-754 float bits; a few are integer flags. Repeated
 * values are parallel native lanes. Major regions consumed by process() are:
 *   0x00c0..0x06bf  four-band/two-half filter state
 *   0x06d0..        linked RMS compressor parameters and history
 *   0x18e0..        49-frame look-ahead limiter parameters and history
 *   0x1ac0/0x1b00   final per-slot gain and gain delta
 * Keeping these as bits, instead of rounded decimal literals, preserves the
 * verified binary32 initial state exactly.
 */
#ifndef JOURNEY_NATIVE_MASTER_FIXED48_H
#define JOURNEY_NATIVE_MASTER_FIXED48_H
#define SV
#include "reverb_native_master_kernels.h"
#undef SV
typedef struct {
    float st[0x4000/4];
    float work[256][8],envelope[256][8],delayed[256][8],history[49][8];
} JourneyNativeMaster48;
static float *m48_sp(JourneyNativeMaster48 *m,unsigned offset) {return m->st+offset/4;}
static void journey_native_master48_initialize(JourneyNativeMaster48 *m) {
    /* offset is a byte offset into st; bits is copied verbatim so both float
     * parameters and integer control words retain their native encoding. */
    static const struct {uint16_t offset;uint32_t bits;} words[]={
        {0x00c0,0xbfff0aebU},
        {0x00c4,0xbfff0aebU},
        {0x00c8,0xbfff0aebU},
        {0x00cc,0xbfff0aebU},
        {0x00d0,0x3f7e17a0U},
        {0x00d4,0x3f7e17a0U},
        {0x00d8,0x3f7e17a0U},
        {0x00dc,0x3f7e17a0U},
        {0x00e0,0x3f7f0b5dU},
        {0x00e4,0x3f7f0b5dU},
        {0x00e8,0x3f7f0b5dU},
        {0x00ec,0x3f7f0b5dU},
        {0x00f0,0xbfff0b5dU},
        {0x00f4,0xbfff0b5dU},
        {0x00f8,0xbfff0b5dU},
        {0x00fc,0xbfff0b5dU},
        {0x0100,0x3f7f0b5dU},
        {0x0104,0x3f7f0b5dU},
        {0x0108,0x3f7f0b5dU},
        {0x010c,0x3f7f0b5dU},
        {0x0150,0x00000002U},
        {0x0154,0x00000002U},
        {0x0158,0x00000002U},
        {0x015c,0x00000002U},
        {0x0180,0xbfff0aebU},
        {0x0184,0xbfff0aebU},
        {0x0188,0xbfff0aebU},
        {0x018c,0xbffd281cU},
        {0x0190,0x3f7e17a0U},
        {0x0194,0x3f7e17a0U},
        {0x0198,0x3f7e17a0U},
        {0x019c,0x3f7a6036U},
        {0x01a0,0x3f7f0b5dU},
        {0x01a4,0x3f7f0b5dU},
        {0x01a8,0x3f7f0b5dU},
        {0x01ac,0x387fe85fU},
        {0x01b0,0xbfff0b5dU},
        {0x01b4,0xbfff0b5dU},
        {0x01b8,0xbfff0b5dU},
        {0x01bc,0x38ffe85fU},
        {0x01c0,0x3f7f0b5dU},
        {0x01c4,0x3f7f0b5dU},
        {0x01c8,0x3f7f0b5dU},
        {0x01cc,0x387fe85fU},
        {0x0210,0x00000002U},
        {0x0214,0x00000002U},
        {0x0218,0x00000002U},
        {0x021c,0x00000001U},
        {0x0260,0x3f800000U},
        {0x0264,0x3f800000U},
        {0x0268,0x3f800000U},
        {0x026c,0x3f800000U},
        {0x0320,0x3f800000U},
        {0x0324,0x3f800000U},
        {0x0328,0x3f800000U},
        {0x032c,0x3f800000U},
        {0x03e0,0x3f800000U},
        {0x03e4,0x3f800000U},
        {0x03e8,0x3f800000U},
        {0x03ec,0x3f800000U},
        {0x04a0,0x3f800000U},
        {0x04a4,0x3f800000U},
        {0x04a8,0x3f800000U},
        {0x04ac,0x3f800000U},
        {0x0560,0x3f800000U},
        {0x0564,0x3f800000U},
        {0x0568,0x3f800000U},
        {0x056c,0x3f800000U},
        {0x0620,0x3f800000U},
        {0x0624,0x3f800000U},
        {0x0628,0x3f800000U},
        {0x062c,0x3f800000U},
        {0x06d0,0xc0c00000U},
        {0x06d4,0xc0c00000U},
        {0x06d8,0xc0c00000U},
        {0x06dc,0xc0c00000U},
        {0x06e0,0xc0c00000U},
        {0x06e4,0xc0c00000U},
        {0x06e8,0xc0c00000U},
        {0x06ec,0xc0c00000U},
        {0x06f0,0xbeaaaaaaU},
        {0x06f4,0xbeaaaaaaU},
        {0x06f8,0xbeaaaaaaU},
        {0x06fc,0xbeaaaaaaU},
        {0x0700,0xbeaaaaaaU},
        {0x0704,0xbeaaaaaaU},
        {0x0708,0xbeaaaaaaU},
        {0x070c,0xbeaaaaaaU},
        {0x0710,0x3f800000U},
        {0x0714,0x3f800000U},
        {0x0718,0x3f800000U},
        {0x071c,0x3f800000U},
        {0x0720,0x3f800000U},
        {0x0724,0x3f800000U},
        {0x0728,0x3f800000U},
        {0x072c,0x3f800000U},
        {0x0750,0x3eb4aaddU},
        {0x0754,0x3eb4aaddU},
        {0x0758,0x3eb4aaddU},
        {0x075c,0x3eb4aaddU},
        {0x0760,0x3eb4aaddU},
        {0x0764,0x3eb4aaddU},
        {0x0768,0x3eb4aaddU},
        {0x076c,0x3eb4aaddU},
        {0x0770,0x3f7f779cU},
        {0x0774,0x3f7f779cU},
        {0x0778,0x3f7f779cU},
        {0x077c,0x3f7f779cU},
        {0x0780,0x3f7f779cU},
        {0x0784,0x3f7f779cU},
        {0x0788,0x3f7f779cU},
        {0x078c,0x3f7f779cU},
        {0x0790,0x3f7ab8caU},
        {0x0794,0x3f7ab8caU},
        {0x0798,0x3f7ab8caU},
        {0x079c,0x3f7ab8caU},
        {0x07a0,0x3f7ab8caU},
        {0x07a4,0x3f7ab8caU},
        {0x07a8,0x3f7ab8caU},
        {0x07ac,0x3f7ab8caU},
        {0x18e0,0xbf000000U},
        {0x18e4,0xbf000000U},
        {0x18e8,0xbf000000U},
        {0x18ec,0xbf000000U},
        {0x18f0,0xbf000000U},
        {0x18f4,0xbf000000U},
        {0x18f8,0xbf000000U},
        {0x18fc,0xbf000000U},
        {0x1900,0xbf800000U},
        {0x1904,0xbf800000U},
        {0x1908,0xbf800000U},
        {0x190c,0xbf800000U},
        {0x1910,0xbf800000U},
        {0x1914,0xbf800000U},
        {0x1918,0xbf800000U},
        {0x191c,0xbf800000U},
        {0x1920,0x3f7c7cd0U},
        {0x1924,0x3f7c7cd0U},
        {0x1928,0x3f7c7cd0U},
        {0x192c,0x3f7c7cd0U},
        {0x1930,0x3f7c7cd0U},
        {0x1934,0x3f7c7cd0U},
        {0x1938,0x3f7c7cd0U},
        {0x193c,0x3f7c7cd0U},
        {0x1940,0x3f7b0c6bU},
        {0x1944,0x3f7b0c6bU},
        {0x1948,0x3f7b0c6bU},
        {0x194c,0x3f7b0c6bU},
        {0x1950,0x3f7b0c6bU},
        {0x1954,0x3f7b0c6bU},
        {0x1958,0x3f7b0c6bU},
        {0x195c,0x3f7b0c6bU},
        {0x1980,0x3f7b0c6bU},
        {0x1984,0x3f7b0c6bU},
        {0x1988,0x3f7b0c6bU},
        {0x198c,0x3f7b0c6bU},
        {0x1990,0x3f7b0c6bU},
        {0x1994,0x3f7b0c6bU},
        {0x1998,0x3f7b0c6bU},
        {0x199c,0x3f7b0c6bU},
        {0x1a60,0x3dd70a3dU},
        {0x1a64,0x3dd70a3dU},
        {0x1a68,0x3dd70a3dU},
        {0x1a6c,0x3dd70a3dU},
        {0x1a70,0x3f7fe668U},
        {0x1a74,0x3f7fe668U},
        {0x1a78,0x3f7fe668U},
        {0x1a7c,0x3f7fe668U},
        {0x1a80,0x3f7ff99aU},
        {0x1a84,0x3f7ff99aU},
        {0x1a88,0x3f7ff99aU},
        {0x1a8c,0x3f7ff99aU},
        {0x1ac0,0x3f800000U},
        {0x1ac4,0x3f800000U},
        {0x1ac8,0x3f800000U},
        {0x1acc,0x3f800000U},
        {0x1ad0,0x3f800000U},
        {0x1ad4,0x3f800000U},
        {0x1ad8,0x3f800000U},
        {0x1adc,0x3f800000U},
        {0x1ae0,0x3f21866cU},
        {0x1ae4,0x3f21866cU},
        {0x1ae8,0x3f21866cU},
        {0x1aec,0x3f21866cU},
        {0x1af0,0x3f21866cU},
        {0x1af4,0x3f21866cU},
        {0x1af8,0x3f21866cU},
        {0x1afc,0x3ecbd4b5U},
        {0x1b00,0xbabcf328U},
        {0x1b04,0xbabcf328U},
        {0x1b08,0xbabcf328U},
        {0x1b0c,0xbabcf328U},
        {0x1b10,0xbabcf328U},
        {0x1b14,0xbabcf328U},
        {0x1b18,0xbabcf328U},
        {0x1b1c,0xbb1a15a6U},
        {0x1b24,0x80000001U},
    };
    memset(m,0,sizeof(*m));
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);i++)
        memcpy(m48_sp(m,words[i].offset),&words[i].bits,4);
}
static void journey_native_master48_process(JourneyNativeMaster48 *m,const float input[256][2],float output[256][8]) {
    memset(m->work,0,sizeof(m->work));
    for(unsigned i=0;i<256;i++){m->work[i][1]=input[i][0];m->work[i][6]=input[i][1];}
    for(unsigned band=0;band<4;band++)for(unsigned half=0;half<2;half++)
        mk_filter(m48_sp(m,0xc0+band*0x180+half*0xc0),&m->work[0][half*4],&m->work[0][half*4],256,1);
    /* 42a150: linked RMS compressor, no lookahead, flags 5. */
    float *d1=m48_sp(m,0x6d0);
    for(unsigned half=0;half<2;half++) {
        unsigned c=half*4;
        mk_square(&m->work[0][c],&m->envelope[0][c],256,1,d1+0xc0/4+c,d1+0xc0/4+c,d1+0x100/4+c,0,0);
    }
    for(unsigned i=0;i<256;i++) {
        float linked=0;
        for(unsigned c=0;c<8;c++) linked=linked+m->envelope[i][c];
        for(unsigned c=0;c<8;c++)m->envelope[i][c]=linked;
    }
    for(unsigned half=0;half<2;half++) {
        unsigned c=half*4;
        mk_curve(&m->envelope[0][c],&m->envelope[0][c],256,1,d1+c,d1+0x20/4+c,1);
        mk_envelope(&m->envelope[0][c],&m->envelope[0][c],256,1,d1+0xa0/4+c,d1+0x80/4+c,d1+0xe0/4+c,d1+0x120/4+c,d1+0x140/4+c);
        mk_apply(&m->work[0][c],&m->envelope[0][c],&m->work[0][c],d1+0x40/4+c,d1+0x60/4+c,256,1);
    }
    /* 42bd90 spectral stage is disabled by state+8c==0. */
    /* 42ad70: 49-frame lookahead adaptive limiter. */
    float *d3=m48_sp(m,0x18e0);float zero[4]={0},maxv[4],minv[4];
    memcpy(m->delayed,m->history,sizeof(m->history));
    memcpy(m->delayed[49],m->work,(256-49)*8*4);
    memcpy(m->history,m->work[256-49],sizeof(m->history));
    for(unsigned half=0;half<2;half++) {
        unsigned c=half*4;
        mk_square(&m->work[0][c],&m->envelope[0][c],256,1,d3+0xa0/4+c,d3+0xa0/4+c,d3+0x100/4+c,maxv,minv);
        for(unsigned j=0;j<4;j++) {
            float last=d3[0x120/4+c+j], threshold=d3[0x180/4+j];
            d3[0x80/4+c+j]=(threshold<(last-maxv[j]) || maxv[j]<threshold)?d3[0x190/4+j]:d3[0x1a0/4+j];
            d3[0x120/4+c+j]=maxv[j];
        }
        mk_envelope(&m->work[0][c],&m->envelope[0][c],256,1,zero,d3+0x80/4+c,d3+0xc0/4+c,0,0);
        mk_curve(&m->envelope[0][c],&m->envelope[0][c],256,1,d3+c,d3+0x20/4+c,0);
        mk_envelope(&m->envelope[0][c],&m->envelope[0][c],256,1,d3+0x80/4+c,d3+0x60/4+c,d3+0xe0/4+c,d3+0x140/4+c,d3+0x160/4+c);
        mk_apply(&m->delayed[0][c],&m->envelope[0][c],&m->work[0][c],d3+0x40/4+c,zero,256,1);
    }
    /* 42b760 disabled. 4317c0/4360f0 planar output gain; 417e70
     * writes the stereo logical slots to physical channels zero and one. */
    memset(output,0,256*8*4);
    for(unsigned ch=0;ch<2;ch++) {
        unsigned slot=ch?6:1;float current=m48_sp(m,0x1ac0)[slot],delta=m48_sp(m,0x1b00)[slot];
        float gains[16];for(unsigned k=0;k<16;k++)gains[k]=current+delta*(float)k;
        float step=delta*16.0f;
        for(unsigned i=0;i<256;i+=16)for(unsigned k=0;k<16;k++) {
            output[i+k][ch]=m->work[i+k][slot]*gains[k];gains[k]=step+gains[k];
        }
        m48_sp(m,0x1ac0)[slot]=mk_f(mk_u(gains[0])&0x7fffffffU)>=mk_f(0x32a501ac)?gains[0]:0;
        m48_sp(m,0x1b00)[slot]=0;
    }
    /* Native physical output 417e70: min(+1), then max(-1). */
    for(unsigned i=0;i<256;i++)for(unsigned ch=0;ch<2;ch++) {
        if(output[i][ch]>1)output[i][ch]=1;
        if(output[i][ch]<-1)output[i][ch]=-1;
    }
}
#endif

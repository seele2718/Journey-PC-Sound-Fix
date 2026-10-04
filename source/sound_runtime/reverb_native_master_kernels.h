/* Scalar binary32 translations of the native Master DSP primitives.
 *
 * The hexadecimal constants passed through mk_f() are exact IEEE-754 bits,
 * not addresses. Exact bits and expression order are retained because this
 * dynamics path is sensitive to binary32 rounding; none of the coefficients
 * were fitted from a recording.
 *
 * State layout used by mk_filter, in float indices relative to s:
 *   +0x00/+0x04  feedback a1/a2
 *   +0x08..+0x10 feed-forward b0/b1/b2
 *   +0x14..+0x20 x[n-2], x[n-1], y[n-2], y[n-1]
 *   +0x24        per-lane enabled word
 * mk_square/mk_envelope are attack-release followers; mk_curve applies the
 * native log2/exp2 approximation; mk_apply applies the resulting gain.
 *
 * Reverse-engineering provenance: PS4 ELF dfbeed4c...3501b8, unchanged image
 * f7e17da8...d5f94d; native RVAs 0x435e90, 0x4355f0, 0x4349d0,
 * 0x4351e0, and 0x4353f0. The translations were checked against unchanged
 * native execution.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
/* Native vcvtps2dq uses the current rounding mode. SSE2 scalar equivalent
 * avoids a new CRT import in the static PE. Hosts retain the verified fallback. */
static int mk_round_int(float x) {
#if defined(__x86_64__)
    int result;
    __asm__("cvtss2si %1, %0" : "=r"(result) : "x"(x));
    return result;
#else
    return (int)nearbyintf(x);
#endif
}
/* C union representation access preserves the native binary32 bits without
 * per-sample CRT memcpy calls under the static payload's -fno-builtin flag. */
_Static_assert(sizeof(float)==4 && sizeof(uint32_t)==4, "native binary32 storage");
static float mk_f(uint32_t b) { union { uint32_t u; float f; } v={.u=b}; return v.f; }
static uint32_t mk_u(float x) { union { float f; uint32_t u; } v={.f=x}; return v.u; }

static uintptr_t SV mk_filter(float *s,const float *in,float *out,unsigned n,unsigned shift) {
    unsigned enabled=0; for(unsigned c=0;c<4;c++) enabled+=mk_u(s[0x24+c]);
    if(!enabled && in==out) return 0;
    unsigned stride=4u<<shift;
    for(unsigned c=0;c<4;c++) {
        float x2=s[0x14+c], x1=s[0x18+c], y2=s[0x1c+c], y1=s[0x20+c];
        for(unsigned i=0;i<n;i++) {
            float x=in[i*stride+c];
            /* 435f47..435f65: b2*x2 + (b1*x1 + b0*x), not the
             * reassociated expression printed by Ghidra. */
            float y=s[0x10+c]*x2+(s[0x0c+c]*x1+s[0x08+c]*x);
            if((i&3)==0) { y=y-s[c]*y1; y=y-s[4+c]*y2; }
            else { y=y-s[4+c]*y2; y=y-s[c]*y1; }
            out[i*stride+c]=y; x2=x1; x1=x; y2=y1; y1=y;
        }
        s[0x14+c]=x2;s[0x18+c]=x1;s[0x1c+c]=y2;s[0x20+c]=y1;
    }
    return (uintptr_t)n*stride*4;
}

static void mk_follow(const float *in,float *out,unsigned n,unsigned shift,
                      const float *a,const float *b,float *state,float *maxv,float *minv,int square) {
    unsigned stride=4u<<shift;
    for(unsigned c=0;c<4;c++) {
        float old=state[c],hi=0,lo=10000000000.0f;
        for(unsigned i=0;i<n;i++) {
            float x=in[i*stride+c]; x=square?x*x:mk_f(mk_u(x)&0x7fffffffU);
            float diff=old-x; float next=x+(old<x?a[c]:b[c])*diff;
            old=next; out[i*stride+c]=next;
            if(hi<next)hi=next;
            if(next<lo)lo=next;
        }
        state[c]=old;if(maxv)maxv[c]=hi;if(minv)minv[c]=lo;
    }
}
static void SV mk_square(const float *in,float *out,unsigned n,unsigned shift,
                         const float *a,const float *b,float *state,float *maxv,float *minv) {
    mk_follow(in,out,n,shift,a,b,state,maxv,minv,1);
}
static void SV mk_envelope(const float *in,float *out,unsigned n,unsigned shift,
                           const float *a,const float *b,float *state,float *maxv,float *minv) {
    mk_follow(in,out,n,shift,a,b,state,maxv,minv,0);
}
static void SV mk_curve(const float *in,float *out,unsigned n,unsigned shift,
                       const float *threshold,const float *slope,unsigned power) {
    unsigned stride=4u<<shift;
    for(unsigned c=0;c<4;c++) {
        float factor=mk_f(power?0x3e2a152d:0x3eaa152d)*slope[c];
        for(unsigned i=0;i<n;i++) {
            /* Split binary32 into exponent and a normalized [1,2) mantissa.
             * The polynomial then approximates log2; the following polynomial
             * reconstructs exp2 after applying the authored curve. */
            uint32_t raw=mk_u(in[i*stride+c]);
            float exponent=(float)((int)((raw&0x7f800000U)>>23)-127);
            float mantissa=mk_f((raw&0x007fffffU)|0x3f800000U);
            float poly=mantissa*mk_f(0x3e515a4b)+mk_f(0xbf8649e9);
            poly=mantissa*poly+mk_f(0x401221a2);
            poly=(mantissa+mk_f(0xbf800000))*poly;
            float level=-threshold[c]+(exponent+poly)*mk_f(0x4040a8c1);
            if((factor>0)!=(level<0)) level=0;
            float exp2=factor*level;
            if(exp2<mk_f(0xc2fdffff))exp2=mk_f(0xc2fdffff);
            int integral=mk_round_int(exp2+mk_f(0xbf000000));
            float fraction=exp2-(float)integral;
            float value=fraction*mk_f(0x3d9fcb52)+mk_f(0x3e677e26);
            value=fraction*value+mk_f(0x3f322226);
            value=fraction*value+mk_f(0x3f7ffb19);
            out[i*stride+c]=mk_f((uint32_t)(integral+127)<<23)*value;
        }
    }
}
static void SV mk_apply(const float *in,const float *envelope,float *out,
                       const float *gain,const float *bias,unsigned n,unsigned shift) {
    unsigned stride=4u<<shift;
    for(unsigned i=0;i<n;i++) for(unsigned c=0;c<4;c++)
        out[i*stride+c]=((bias?bias[c]:0)+(gain?gain[c]:1)*envelope[i*stride+c])*in[i*stride+c];
}

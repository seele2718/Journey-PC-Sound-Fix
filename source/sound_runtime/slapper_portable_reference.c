/*
 * Portable scalar transcription of Journey PS4's used LR1/Slapper path.
 *
 * The runtime includes this scalar DSP core with the native stereo adapter.
 * It retains double-precision state where declared, with explicit binary32
 * operations for native rounding-sensitive stages. The scalar block interface
 * consumes 256 mono frames at 48 kHz and emits four wet lanes at 48 kHz.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the stateful DSP entry points available to the runtime adapter. */
#define SLAPPER_API __attribute__((used, noinline))

#define HOST_FRAMES 256
#define INTERNAL_FRAMES 128
#define EARLY_LANES 4
#define LATE_LINES 16
#define EARLY_CAPACITY 65536
#define LATE_CAPACITY 4096
#define PI 3.14159265358979323846264338327950288

typedef struct {
    double effect_level;
    double effect_low_pass_cutoff;
    double effect_high_pass_cutoff;
    double early_delay;
    double early_time_factor;
    double early_level;
    double slapback;
    double early_hf_ratio;
    double decay_time;
    double decay_level;
    double decay_hf_ratio;
    double diffusion;
} SlapperPreset;

typedef struct { double b0, b1, b2, a1, a2; } BiquadCoefficients;
typedef struct {
    BiquadCoefficients c;
    double x1, x2, y1, y2;
} Biquad;

typedef struct {
    double delay;
    int integer_delay;
    double coefficient;
    double previous_input;
    double previous_output;
} FractionalTap;

typedef struct {
    FractionalTap active;
    FractionalTap pending;
    int has_pending;
} CrossfadedTap;

typedef struct {
    double *samples;
    int capacity;
    int write_index;
    uint64_t written;
    int ready;
} DelayRing;

typedef struct {
    float center;
    float output_scale;
    float phase_increment;
    float smoothing;
    float random_shape;
    uint32_t seed_a;
    uint32_t seed_b;
    float phase;
    float slope;
    float integrated;
    float smoothed;
} Modulator;

typedef struct {
    double phase;
    double history[EARLY_LANES][4];
    int channels;
    double ratio;
} RateAdapter;

/* Native 427b50/427d60/4280b0/428620, including ring-fill and first-read gates.
 * Limits and parent parameters come from executed 428790 initialization.
 * This covers the positive-delay, no-direct path used by all authored rooms.
 */
typedef struct {
 FractionalTap path[2];
 float current,target,queued,progress,sine,cosine;
 float minimum,maximum,threshold,smoothing;
 unsigned flags,active;
} NativeTap;

typedef struct {
    SlapperPreset preset;
    const float *resampler_table;
    RateAdapter input_adapter;
    RateAdapter output_adapter;

    DelayRing early_ring;
    NativeTap early_taps[EARLY_LANES];
    Biquad early_filters[EARLY_LANES];
    double early_gain_current[EARLY_LANES];
    double early_gain_target[EARLY_LANES];
    double early_gain_delta[EARLY_LANES];
    double early_cutoff_current;
    double early_cutoff_target;
    double slapback_current;
    double slapback_target;

    DelayRing late_rings[LATE_LINES];
    NativeTap late_taps[LATE_LINES];
    Modulator modulators[LATE_LINES];
    Biquad high_filters[LATE_LINES];
    Biquad low_filters[LATE_LINES];
    uint32_t seed_counter;
    double diffusion_current;
    double diffusion_target;
    double hf_current;
    double hf_target;
    double lf_current;
    double lf_target;
    double late_gain_current;
    double late_gain_target;

    double final_low_current;
    double final_high_current;
    double final_low_target;
    double final_high_target;
    Biquad final_low[EARLY_LANES];
    Biquad final_high[EARLY_LANES];
    int final_filters_ready;

    /* Persistent callback scratch: keep the audio thread's stack bounded. */
    double scratch_input_planar[EARLY_LANES][HOST_FRAMES];
    double scratch_internal_planar[EARLY_LANES][HOST_FRAMES];
    double scratch_early_unscaled[EARLY_LANES][INTERNAL_FRAMES];
    double scratch_combined[EARLY_LANES][HOST_FRAMES];
    double scratch_late_delayed[LATE_LINES][INTERNAL_FRAMES];
    double scratch_feedback[LATE_LINES][INTERNAL_FRAMES];
} Slapper;

/*
 * Keep the native wrapper independent of this private structure layout.
 * The patch builder can size the external renderer allocation from the same
 * audited core that owns the state definition, without duplicating a magic
 * byte count in assembly or in the PE patcher.
 */
SLAPPER_API uint64_t slapper_state_size(void)
{
    return (uint64_t)sizeof(Slapper);
}

static const double k_base_lengths[LATE_LINES] = {
    193.0, 813.0, 821.0, 829.0, 839.0, 853.0, 857.0, 859.0,
    863.0, 877.0, 881.0, 883.0, 887.0, 971.0, 1021.0, 1033.0
};

static const uint32_t k_shape_bits[22] = {
    0x3F80365D, 0x3F803023, 0x3F801F08, 0x3F802585,
    0x3F805040, 0x3F803811, 0x3F81600F, 0x3F80E2A8,
    0x3F806927, 0x3F818FB4, 0x3F8559DE, 0x3F8A33D7,
    0x3F92C804, 0x3F9E9CED, 0x3FA78655, 0x3FC4B7B3,
    0x3FC44274, 0x3FE4FC2F, 0x400A9036, 0x4022AFD5,
    0x4038F951, 0x40866FC1
};

static const uint32_t k_seed_table[128] = {
    0x72AE2CD6,0x3D6C4AE1,0x678418BE,0x48230029,
    0x01EB26E9,0x41BB5AF1,0x6DF11649,0x5F906952,
    0x00990F3E,0x390C7E87,0x153C12DB,0x2EA60BB3,
    0x54DE1547,0x4DB74D06,0x491C440D,0x305E0124,
    0x26A6428B,0x66BB6443,0x4DC8074D,0x2D1239B3,
    0x1E1F3B25,0x12384509,0x767D7A5A,0x5D03701F,
    0x323B4E45,0x7FF57F96,0x6BFC63CB,0x1AD46E5D,
    0x073256AE,0x0BDB301C,0x030A6B89,0x260D2213,
    0x5CFD6B36,0x58784B40,0x22EE2350,0x759A0120,
    0x0DDC5F49,0x797D3A9E,0x3BF65F32,0x1A493E12,
    0x1CD01366,0x2E404944,0x4DF25E14,0x314F4CAD,
    0x542215A1,0x2C3B6032,0x7EB74230,0x66C4366B,
    0x73DA121F,0x798B12E1,0x409D5991,0x08223EF6,
    0x7049139D,0x57727BB9,0x09023699,0x26CA58B0,
    0x408013E9,0x3CD56899,0x16C5187E,0x4A80692C,
    0x3CD65C67,0x60BF5753,0x48CC23C9,0x33EA5DB2,
    0x0D66368E,0x54DC422D,0x047E6AD6,0x2F140FBF,
    0x288F6C69,0x2FFF3C61,0x2C494657,0x75EF7983,
    0x61721916,0x489C5E9D,0x261E7DD1,0x22CD3A61,
    0x0677494A,0x7F4F0384,0x71F0401D,0x32E66B72,
    0x0FC96BCB,0x1953542C,0x50396BE8,0x18D74402,
    0x5DD511F4,0x2B0C249E,0x78742833,0x5F1E0E12,
    0x07CF0035,0x127E2059,0x5FA44CD4,0x5A9F6AD4,
    0x3A2D0E90,0x01D346CF,0x0ECC1AF4,0x6D226732,
    0x252A591D,0x19D937E6,0x0975458F,0x57D36048,
    0x7B444087,0x14815078,0x442B49F7,0x1DC037E5,
    0x7FBE3A8D,0x7F6116D4,0x2B001850,0x765F590E,
    0x251F7282,0x0633773B,0x38070C15,0x50050C7B,
    0x3BB139CE,0x4D545064,0x19DA3492,0x62701D18,
    0x3004486A,0x5C464FF8,0x6A156D69,0x513E4C85,
    0x59684D67,0x182F1F16,0x73D9470E,0x5E731796,
    0x58764F68,0x4E575ED0,0x0A4A3F4A,0x2CF74AD4
};

static double f32_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return (double)value;
}

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static BiquadCoefficients low_high_coefficients(
    int high_pass, double cutoff, double q, double sample_rate)
{
    BiquadCoefficients c;
    double omega = 2.0 * PI * cutoff / sample_rate;
    double cosine, sine, alpha, a0;
    if (omega < 0.003) omega = 0.003;
    if (omega > 2.9415922) omega = 2.9415922;
    cosine = cos(omega);
    sine = sin(omega);
    alpha = sine / (2.0 * q);
    a0 = 1.0 + alpha;
    c.a1 = -2.0 * cosine / a0;
    c.a2 = (1.0 - alpha) / a0;
    if (high_pass) {
        c.b0 = 0.5 * (1.0 + cosine) / a0;
        c.b1 = -(1.0 + cosine) / a0;
    } else {
        c.b0 = 0.5 * (1.0 - cosine) / a0;
        c.b1 = (1.0 - cosine) / a0;
    }
    c.b2 = c.b0;
    return c;
}

static double biquad_sample(Biquad *state, double sample)
{
    BiquadCoefficients *c = &state->c;
    double value = c->b0 * sample + c->b1 * state->x1 +
        c->b2 * state->x2 - c->a1 * state->y1 - c->a2 * state->y2;
    state->x2 = state->x1;
    state->x1 = sample;
    state->y2 = state->y1;
    state->y1 = value;
    return value;
}

static void fractional_values(double delay, int *integer_delay, double *coefficient)
{
    /* All recovered Slapper delays are non-negative. */
    double integer_part = (double)(uint32_t)delay;
    double fraction = delay - integer_part;
    double x;
    *integer_delay = (int)integer_part;
    if (fraction < 0.625) {
        --*integer_delay;
        x = fraction;
    } else {
        x = fraction - 1.0;
    }
    *coefficient = x * (x * (x * -0.125 + 0.25) - 0.5);
}

static __attribute__((unused)) FractionalTap make_tap(double delay)
{
    FractionalTap tap;
    memset(&tap, 0, sizeof(tap));
    tap.delay = delay;
    fractional_values(delay, &tap.integer_delay, &tap.coefficient);
    return tap;
}

static __attribute__((unused)) void retarget_tap(CrossfadedTap *tap, double delay)
{
    if (fabs(delay - tap->active.delay) <= 1.0e-12) return;
    tap->pending = tap->active;
    tap->pending.delay = delay;
    fractional_values(delay, &tap->pending.integer_delay,
                      &tap->pending.coefficient);
    tap->has_pending = 1;
}

static double fractional_sample(FractionalTap *tap, double sample)
{
    double output = tap->previous_input + tap->coefficient *
        (sample - tap->previous_output);
    tap->previous_input = sample;
    tap->previous_output = output;
    return output;
}

static int ring_init(DelayRing *ring, int capacity)
{
    size_t bytes = (size_t)capacity * sizeof(double);
    ring->samples = (double *)malloc(bytes);
    if (ring->samples != NULL) memset(ring->samples, 0, bytes);
    ring->capacity = capacity;
    ring->write_index = 0;ring->written=0;ring->ready=0;
    return ring->samples != NULL;
}

static double ring_read(const DelayRing *ring, int integer_delay, int frame)
{
    int index = ring->write_index - integer_delay + frame;
    index %= ring->capacity;
    if (index < 0) index += ring->capacity;
    return ring->samples[index];
}

static void ring_write(DelayRing *ring, double sample)
{
    if((ring->written%128)==0 && ring->written>=(uint64_t)ring->capacity)ring->ready=1;
    ring->written++;
    ring->samples[ring->write_index] = sample;
    ring->write_index = (ring->write_index + 1) % ring->capacity;
}

static __attribute__((unused)) void tap_render_block(CrossfadedTap *tap, const DelayRing *ring,
                             double output[INTERNAL_FRAMES])
{
    int frame;
    double old_weight = 1.0;
    double new_weight = 0.0;
    const double step = 1.0 / INTERNAL_FRAMES;
    for (frame = 0; frame < INTERNAL_FRAMES; ++frame) {
        double old_value = fractional_sample(
            &tap->active,
            ring_read(ring, tap->active.integer_delay, frame));
        if (tap->has_pending) {
            double new_value = fractional_sample(
                &tap->pending,
                ring_read(ring, tap->pending.integer_delay, frame));
            output[frame] = old_weight * old_value + new_weight * new_value;
            old_weight -= step;
            new_weight += step;
        } else {
            output[frame] = old_value;
        }
    }
    if (tap->has_pending) {
        tap->active = tap->pending;
        tap->has_pending = 0;
    }
}

static void nt_fraction(FractionalTap *p,float delay) {
 float integral=(float)(uint32_t)delay,frac=delay-integral;
 if(frac<0.625f){integral-=1.f;frac+=1.f;}frac-=1.f;
 p->delay=delay;p->integer_delay=(int)integral;
 p->coefficient=frac*(frac*(frac*-0.125f+0.25f)-0.5f);
}
static void nt_init(NativeTap *t,float delay,float minimum,float maximum,float threshold,float smoothing) {
 memset(t,0,sizeof(*t));t->minimum=minimum;t->maximum=maximum;t->threshold=threshold;t->smoothing=smoothing;
 t->current=t->target=(delay<minimum?minimum:(delay>maximum?maximum:delay));t->queued=-1.f;t->cosine=1.f;t->flags=2;
 nt_fraction(&t->path[0],t->current);nt_fraction(&t->path[1],t->current);
}
static void nt_target(NativeTap *t,float delay) {
 delay=(delay<t->minimum?t->minimum:(delay>t->maximum?t->maximum:delay));
 if((t->flags&2)&&delay==t->current)return;
 if(t->flags&8){t->queued=delay;return;}
 t->target=delay;
 if(fabsf(delay-t->current)>t->threshold) {
  nt_fraction(&t->path[0],t->current);nt_fraction(&t->path[1],delay);
  t->flags=(t->flags&2)|8;t->progress=0;t->sine=0;t->cosine=1;t->active=0;
 }else t->flags=(t->flags&2)|16;
}
static void nt_render(NativeTap *t,const DelayRing *ring,double *output) {
 if(!ring->ready){memset(output,0,128*sizeof(double));return;}
 unsigned old=t->active&1,newer=old^1,wait=0;
 float ow=1,nw=0,oe=0,ne=1;
 if(t->flags&8) {
  ow=t->cosine;nw=t->sine;
  /* Native initialized 250 ms equal-power fade at 128/24000 cadence. */
  ne=nw*0.9994385838508606f+ow*0.03350405395030975f;
  oe=ow*0.9994385838508606f-nw*0.03350405395030975f;
  t->sine=ne;t->cosine=oe;t->progress+=0.02133333310484886f;
 }else if(t->flags&16) {
  float current=t->smoothing*t->target+(1.f-t->smoothing)*t->current;
  if(fabsf(t->target-current)<=0.0010000000474974513f){current=t->target;t->flags&=~16u;}
  nt_fraction(&t->path[newer],current);t->current=current;t->active^=1;wait=8;
 }
 if(t->flags&2){ow=0;t->flags&=~2u;}
 float step=(t->flags&8)?1.f/128.f:1.f/120.f,od=(oe-ow)*step,nd=(ne-nw)*step;
 for(unsigned i=0;i<128;i++) {
  FractionalTap *a=&t->path[old],*b=&t->path[newer];
  float av=(float)ring_read(ring,a->integer_delay,i),bv=(float)ring_read(ring,b->integer_delay,i);
  float ao=(float)a->previous_input+(float)a->coefficient*(av-(float)a->previous_output);
  float bo=(float)b->previous_input+(float)b->coefficient*(bv-(float)b->previous_output);
  a->previous_input=av;a->previous_output=ao;b->previous_input=bv;b->previous_output=bo;
  output[i]=ow*ao+nw*bo;
  if(i>=wait){ow+=od;nw+=nd;}
 }
 if((t->flags&8)&&t->progress>=1.f) {
  t->current=t->target;t->path[0]=t->path[1];t->flags=0;
  if(t->queued>=0){float next=t->queued;nt_target(t,next);t->queued=-1;}
 }
}


/*
 * Native mode-5 modulation is binary32 throughout.  Keep this separate from
 * the double-precision reference core so the production correction is explicit.
 */
static float native_shape_for_rate(float rate, float strength)
{
    float threshold = 0.01f;
    float previous = 0.0f;
    int index;
    for (index = 0; index < 22; ++index) {
        float current;
        memcpy(&current, &k_shape_bits[index], sizeof(current));
        if (threshold >= rate) {
            float interpolated = current -
                ((threshold - rate) * (current - previous) /
                 (threshold + threshold / -1.5f));
            return 1.0f - (1.0f - interpolated) * strength;
        }
        threshold = threshold * 1.5f;
        previous = current;
    }
    return 1.0f;
}

static void geometry(double diffusion, double centers[LATE_LINES])
{
    /* Native's geometry table uses word 0x3e947ae2, one ULP above 0.29f. */
    float scale = 0.71f + 0.290000021457672119f * (float)diffusion;
    float spread = (float)diffusion * 5.0f;
    int line;
    for (line = 0; line < LATE_LINES; ++line)
    {
        float scaled = scale * (float)k_base_lengths[line];
        float minimum = scaled - spread;
        float maximum = spread + scaled;
        centers[line] = (double)((minimum + maximum) * 0.5f);
        spread = spread * 0.895510911941528320f;
    }
}

static void next_seeds(uint32_t *counter, uint32_t *a, uint32_t *b)
{
    uint32_t first, second;
    *counter += 1;
    first = k_seed_table[*counter & 0x7f];
    *counter += 1;
    second = k_seed_table[*counter & 0x7f];
    *a = first ^ second;
    *b = *a + second;
}

static Modulator make_modulator(double center, double diffusion,
                                double phase_degrees,
                                float phase_rate,
                                uint32_t *counter)
{
    Modulator mod;
    const float native_pi = 3.141590118408203125f;
    const float native_two_pi = 0x1.921fb6p+2f;
    const float native_degrees_to_radians = 0.017453290522098541f;
    const float block_rate = 187.5f;
    const float strength = 1.0f;
    float increment;
    memset(&mod, 0, sizeof(mod));
    mod.center = (float)center;
    mod.output_scale = ((float)diffusion * 4.0f) * native_pi;
    increment = (phase_rate * native_two_pi) * (1.0f / block_rate);
    mod.phase_increment = increment + increment;
    mod.smoothing = (float)exp((double)(-4.12f * strength));
    mod.random_shape = native_shape_for_rate(phase_rate, strength);
    mod.phase = (float)phase_degrees * native_degrees_to_radians;
    next_seeds(counter, &mod.seed_a, &mod.seed_b);
    return mod;
}

static void update_modulator_diffusion(Modulator *mod, double center,
                                       double diffusion)
{
    mod->center = (float)center;
    mod->output_scale = ((float)diffusion * 4.0f) *
        3.141590118408203125f;
}

static double modulator_step(Modulator *mod)
{
    if (mod->phase >= 0x1.921fb6p+2f) {
        float scaled_shape = mod->random_shape * 0x1p-31f;
        float target = scaled_shape * (float)(int32_t)mod->seed_b;
        float difference = target - mod->integrated;
        float incremented = difference * mod->phase_increment;
        mod->slope = incremented * 0x1.45f306p-3f;
        mod->seed_a ^= mod->seed_b;
        mod->seed_b = mod->seed_a + mod->seed_b;
    }
    mod->integrated = mod->slope + mod->integrated;
    {
        float difference = mod->integrated - mod->smoothed;
        float smoothing = difference * mod->smoothing;
        mod->smoothed = mod->smoothed + smoothing;
    }
    if (mod->smoothed > 1.0f) mod->smoothed = 1.0f;
    if (mod->smoothed < -1.0f) mod->smoothed = -1.0f;
    if (mod->phase >= 0x1.921fb6p+2f)
        mod->phase = mod->phase + -0x1.921fb6p+2f;
    mod->phase = mod->phase + mod->phase_increment;
    {
        float scaled = mod->smoothed * mod->output_scale;
        float value = scaled + mod->center;
        return (double)value;
    }
}

static double threshold_smooth(double current, double target)
{
    if (fabs(target - current) <= 0.01) return target;
    return 0.9 * current + 0.1 * target;
}

static double rt60_gain(double seconds, double center)
{
    if (seconds >= 10.0) return 1.0;
    return pow(10.0, -3.0 * center / 24000.0 / seconds);
}

static BiquadCoefficients high_damping(double gain, double ratio)
{
    BiquadCoefficients c = {0};
    double shaped = pow(gain, 2.0 - 2.0 / ratio);
    double one_minus = 1.0 - shaped;
    double a1 = 0.0;
    if (fabs(one_minus) >= 1.0e-15) {
        double cosine = cos(PI * 10000.0 / 24000.0);
        double linear = 2.0 * (cosine - shaped);
        double disc = linear * linear - 4.0 * one_minus * one_minus;
        double root;
        if (disc < 0.0) disc = 0.0;
        root = sqrt(disc);
        a1 = (root - linear) / (2.0 * one_minus);
        if (fabs(a1) > 1.0)
            a1 = (-linear - root) / (2.0 * one_minus);
    }
    c.b0 = gain * (1.0 + a1);
    c.a1 = a1;
    return c;
}

static BiquadCoefficients low_damping(double gain, double ratio)
{
    BiquadCoefficients c = {0};
    double theta = PI * 500.0 / 24000.0;
    double decay = exp(-theta);
    double cosine = cos(theta);
    double linear = -2.0 * cosine;
    double shaped = pow(gain, 2.0 / ratio - 2.0);
    double inner = decay * decay - 2.0 * cosine * decay + 1.0;
    double disc = linear * linear - 4.0 * (1.0 - shaped * inner);
    double root, b1;
    if (disc < 0.0) disc = 0.0;
    root = sqrt(disc);
    b1 = (root - linear) * 0.5;
    if (fabs(b1) > 1.0) b1 = (-linear - root) * 0.5;
    c.b0 = -1.0;
    c.b1 = b1;
    c.a1 = -decay;
    return c;
}

static void hadamard16(double values[LATE_LINES])
{
    int width, base, offset;
    for (width = 1; width < LATE_LINES; width *= 2) {
        for (base = 0; base < LATE_LINES; base += width * 2) {
            for (offset = 0; offset < width; ++offset) {
                double left = values[base + offset];
                double right = values[base + offset + width];
                values[base + offset] = left + right;
                values[base + offset + width] = left - right;
            }
        }
    }
    for (base = 0; base < LATE_LINES; ++base) values[base] *= 0.25;
}

static double resample_one(const double *source, double position,
                           const float *table)
{
    int integer = (int)position;
    int phase = (int)((position - (double)integer) * 256.0);
    const float *c;
    if (phase < 0) phase = 0;
    if (phase > 255) phase = 255;
    c = table + phase * 4;
    return source[integer - 3] * c[0] + source[integer - 2] * c[1] +
        source[integer - 1] * c[2] + source[integer] * c[3];
}

static void rate_process(RateAdapter *adapter, const float *table,
                         const double input[EARLY_LANES][HOST_FRAMES],
                         int input_frames, double output[EARLY_LANES][HOST_FRAMES],
                         int output_frames)
{
    int lane, frame;
    for (lane = 0; lane < adapter->channels; ++lane) {
        double source[4 + HOST_FRAMES];
        memcpy(source, adapter->history[lane], 4 * sizeof(double));
        memcpy(source + 4, input[lane], (size_t)input_frames * sizeof(double));
        for (frame = 0; frame < output_frames; ++frame) {
            double position = 4.0 + adapter->phase + frame * adapter->ratio;
            output[lane][frame] = resample_one(source, position, table);
        }
        memcpy(adapter->history[lane], source + input_frames, 4 * sizeof(double));
    }
    {
        double next = adapter->phase + output_frames * adapter->ratio;
        /* The phase accumulator is non-negative and below a few hundred. */
        adapter->phase = next - (double)(uint32_t)next;
    }
}

static void set_early_targets(Slapper *s, const SlapperPreset *preset)
{
    static const double factors[EARLY_LANES] = {0.471,0.591,0.919,0.989};
    static const double signs[EARLY_LANES] = {0.92,-0.62,-0.62,0.92};
    double common = preset->effect_level * preset->early_level * 4.0;
    int lane;
    for (lane = 0; lane < EARLY_LANES; ++lane) {
        double delay = (preset->early_delay +
                        preset->early_time_factor * factors[lane]) * 24000.0;
        nt_target(&s->early_taps[lane], (float)delay);
        s->early_gain_target[lane] = common * signs[lane];
        s->early_gain_delta[lane] =
            (s->early_gain_target[lane] - s->early_gain_current[lane]) /
            INTERNAL_FRAMES;
    }
    s->early_cutoff_target = preset->early_hf_ratio *
        preset->early_hf_ratio * 20000.0;
    s->slapback_target = fabs(preset->slapback) <= 0.001 ? 0.0 : preset->slapback;
    if (s->slapback_target < -2.0) s->slapback_target = -2.0;
    if (s->slapback_target > 2.0) s->slapback_target = 2.0;
}

SLAPPER_API int slapper_init(Slapper *s, const SlapperPreset *preset,
                             const float *table, uint32_t seed_counter)
{
    double centers[LATE_LINES];
    int lane, line;
    memset(s, 0, sizeof(*s));
    s->preset = *preset;
    s->resampler_table = table;
    s->input_adapter.channels = 1;
    s->input_adapter.ratio = 2.0;
    s->output_adapter.channels = 4;
    s->output_adapter.ratio = 0.5;
    if (!ring_init(&s->early_ring, 24064)) return 0;
    for (line = 0; line < LATE_LINES; ++line) {
        if (!ring_init(&s->late_rings[line], ((int)(k_base_lengths[line]+5.0)+127)&~127)) return 0;
    }
    for (lane = 0; lane < EARLY_LANES; ++lane) {
        static const double factors[EARLY_LANES] = {0.471,0.591,0.919,0.989};
        double delay = (preset->early_delay +
                        preset->early_time_factor * factors[lane]) * 24000.0;
        nt_init(&s->early_taps[lane], (float)delay,129.27999877929688f,24000.f,240.f,0.019999999552965164f);
        s->early_filters[lane].c = low_high_coefficients(
            0, preset->early_hf_ratio * preset->early_hf_ratio * 20000.0,
            0.33, 24000.0);
    }
    s->early_cutoff_current = 12001.01;
    s->diffusion_current = preset->diffusion;
    s->diffusion_target = preset->diffusion;
    s->hf_current = f32_from_bits(0x3f7d70a4);
    s->hf_target = preset->decay_hf_ratio;
    s->lf_current = f32_from_bits(0x3f7d70a4);
    s->lf_target = 1.0;
    s->late_gain_target = clamp_double(
        preset->effect_level * preset->decay_level * 4.0, 0.0, 10.0);
    s->late_gain_current = 10.0; /* native 43dbf0, FDN bank+0x2c */
    s->seed_counter = seed_counter;
    geometry(preset->diffusion, centers);
    {
        float phase_rate = 5.0f;
        for (line = 0; line < LATE_LINES; ++line) {
            double gain = rt60_gain(preset->decay_time, centers[line]);
            nt_init(&s->late_taps[line],(float)(k_base_lengths[line]+5),129.f,(float)(k_base_lengths[line]+5),(float)(k_base_lengths[line]+5<1000?k_base_lengths[line]+5:1000),1.f);
            s->modulators[line] = make_modulator(
                centers[line], preset->diffusion, line * 22.5,
                phase_rate, &s->seed_counter);
            s->high_filters[line].c = high_damping(gain, 1.0);
            s->low_filters[line].c = low_damping(gain, 1.0);
            phase_rate = phase_rate * 0.811377823352813721f;
        }
    }
    s->final_low_current = 23998.0;
    s->final_high_current = 2.0;
    s->final_low_target = preset->effect_low_pass_cutoff * 20000.0;
    s->final_high_target = preset->effect_high_pass_cutoff * 20000.0;
    {
        BiquadCoefficients low = low_high_coefficients(
            0, s->final_low_current, 0.5, 48000.0);
        BiquadCoefficients high = low_high_coefficients(
            1, s->final_high_current, 0.5, 48000.0);
        for (lane = 0; lane < EARLY_LANES; ++lane) {
            s->final_low[lane].c = low;
            s->final_high[lane].c = high;
        }
        s->final_filters_ready = 1;
    }
    set_early_targets(s, preset);
    return 1;
}

SLAPPER_API void slapper_set_preset(Slapper *s, const SlapperPreset *preset)
{
    s->preset = *preset;
    set_early_targets(s, preset);
    s->hf_target = preset->decay_hf_ratio;
    s->diffusion_target = preset->diffusion;
    s->late_gain_target = clamp_double(
        preset->effect_level * preset->decay_level * 4.0, 0.0, 10.0);
    s->final_low_target = preset->effect_low_pass_cutoff * 20000.0;
    s->final_high_target = preset->effect_high_pass_cutoff * 20000.0;
}

SLAPPER_API void slapper_destroy(Slapper *s)
{
    int line;
    free(s->early_ring.samples);
    for (line = 0; line < LATE_LINES; ++line) free(s->late_rings[line].samples);
    memset(s, 0, sizeof(*s));
}

SLAPPER_API void slapper_process(Slapper *s, const double mono[HOST_FRAMES],
                                 double quad[EARLY_LANES][HOST_FRAMES])
{
    double (*input_planar)[HOST_FRAMES] = s->scratch_input_planar;
    double (*internal_planar)[HOST_FRAMES] = s->scratch_internal_planar;
    double (*early_unscaled)[INTERNAL_FRAMES] = s->scratch_early_unscaled;
    double (*combined)[HOST_FRAMES] = s->scratch_combined;
    double (*late_delayed)[INTERNAL_FRAMES] = s->scratch_late_delayed;
    double (*feedback)[INTERNAL_FRAMES] = s->scratch_feedback;
    double centers[LATE_LINES];
    double gains[LATE_LINES];
    BiquadCoefficients early_coeff;
    int frame, lane, line;

    memset(input_planar, 0, sizeof(s->scratch_input_planar));
    memset(internal_planar, 0, sizeof(s->scratch_internal_planar));
    memset(combined, 0, sizeof(s->scratch_combined));
    memcpy(input_planar[0], mono, HOST_FRAMES * sizeof(double));
    rate_process(&s->input_adapter, s->resampler_table, input_planar,
                 HOST_FRAMES, internal_planar, INTERNAL_FRAMES);

    early_coeff = low_high_coefficients(0, s->early_cutoff_current,
                                        0.33, 24000.0);
    if (fabs(s->early_cutoff_target - s->early_cutoff_current) > 1.0)
        s->early_cutoff_current = 0.99 * s->early_cutoff_current +
            0.01 * s->early_cutoff_target;
    for (lane = 0; lane < EARLY_LANES; ++lane) {
        double tap_block[INTERNAL_FRAMES];
        s->early_filters[lane].c = early_coeff;
        nt_render(&s->early_taps[lane], &s->early_ring, tap_block);
        for (frame = 0; frame < INTERNAL_FRAMES; ++frame)
            early_unscaled[lane][frame] = biquad_sample(
                &s->early_filters[lane], tap_block[frame]);
    }
    for (frame = 0; frame < INTERNAL_FRAMES; ++frame)
        ring_write(&s->early_ring, internal_planar[0][frame] +
                   s->slapback_current * early_unscaled[3][frame]);
    /* Native 429060 updates parent+0x50 after the current ring write. */
    s->slapback_current = (float)((float)s->slapback_target * 0.019999999552965164f +
        (float)s->slapback_current * (1.f-0.019999999552965164f));
    if (fabs((float)s->slapback_target - (float)s->slapback_current) <= 0.0010000000474974513f)
        s->slapback_current = (float)s->slapback_target;


    if (s->diffusion_target != s->diffusion_current) {
        s->diffusion_current = s->diffusion_target;
        geometry(s->diffusion_current, centers);
        for (line = 0; line < LATE_LINES; ++line) {
            update_modulator_diffusion(
                &s->modulators[line], centers[line], s->diffusion_current);
        }
    } else {
        geometry(s->diffusion_current, centers);
    }
    s->hf_current = threshold_smooth(s->hf_current, s->hf_target);
    s->lf_current = threshold_smooth(s->lf_current, s->lf_target);
    s->late_gain_current = 0.99 * s->late_gain_current +
        0.01 * s->late_gain_target;
    for (line = 0; line < LATE_LINES; ++line) {
        double tap_block[INTERNAL_FRAMES];
        gains[line] = rt60_gain(s->preset.decay_time, centers[line]);
        nt_target(&s->late_taps[line], (float)modulator_step(&s->modulators[line]));
        nt_render(&s->late_taps[line], &s->late_rings[line], tap_block);
        s->high_filters[line].c = high_damping(gains[line], s->hf_current);
        s->low_filters[line].c = low_damping(gains[line], s->lf_current);
        for (frame = 0; frame < INTERNAL_FRAMES; ++frame) {
            double high = biquad_sample(&s->high_filters[line], tap_block[frame]);
            late_delayed[line][frame] = biquad_sample(
                &s->low_filters[line], high);
        }
    }
    for (frame = 0; frame < INTERNAL_FRAMES; ++frame) {
        double transformed[LATE_LINES];
        for (line = 0; line < LATE_LINES; ++line)
            transformed[line] = late_delayed[line][frame];
        hadamard16(transformed);
        for (line = 0; line < LATE_LINES; ++line)
            feedback[line][frame] = transformed[line];
        /* 43dbf0 selects input_count-1; 43e0a0 injects early lane3. */
        feedback[0][frame] -= early_unscaled[3][frame];
        feedback[2][frame] += early_unscaled[3][frame];
        /*
         * PS4 0x43e0a0 sends the Hadamard result back to the delay rings, but
         * its audible extraction call at 0x435130 reads the damped pre-matrix
         * state+0x18 lanes 12..15.  This is the selected native audible extraction path.
         */
        combined[0][frame] = early_unscaled[0][frame] *
            s->early_gain_current[0] + late_delayed[12][frame] *
            s->late_gain_current;
        combined[1][frame] = early_unscaled[1][frame] *
            s->early_gain_current[1] + late_delayed[13][frame] *
            s->late_gain_current;
        combined[2][frame] = early_unscaled[2][frame] *
            s->early_gain_current[2] + late_delayed[14][frame] *
            s->late_gain_current * 1.27;
        combined[3][frame] = early_unscaled[3][frame] *
            s->early_gain_current[3] + late_delayed[15][frame] *
            s->late_gain_current * 1.27;
        for (lane = 0; lane < EARLY_LANES; ++lane)
            s->early_gain_current[lane] += s->early_gain_delta[lane];
    }
    for (lane = 0; lane < EARLY_LANES; ++lane) {
        s->early_gain_current[lane] = s->early_gain_target[lane];
        s->early_gain_delta[lane] = 0.0;
    }
    for (line = 0; line < LATE_LINES; ++line)
        for (frame = 0; frame < INTERNAL_FRAMES; ++frame)
            ring_write(&s->late_rings[line], feedback[line][frame]);

    rate_process(&s->output_adapter, s->resampler_table, combined,
                 INTERNAL_FRAMES, quad, HOST_FRAMES);

    if (fabs(s->final_low_current - s->final_low_target) > 1.0 ||
        fabs(s->final_high_current - s->final_high_target) > 1.0) {
        BiquadCoefficients low = low_high_coefficients(
            0, s->final_low_current, 0.5, 48000.0);
        BiquadCoefficients high = low_high_coefficients(
            1, s->final_high_current, 0.5, 48000.0);
        for (lane = 0; lane < EARLY_LANES; ++lane) {
            s->final_low[lane].c = low;
            s->final_high[lane].c = high;
        }
        s->final_filters_ready = 1;
        s->final_low_current = 0.99 * s->final_low_current +
            0.01 * s->final_low_target;
        s->final_high_current = 0.99 * s->final_high_current +
            0.01 * s->final_high_target;
    }
    for (lane = 0; lane < EARLY_LANES; ++lane) {
        for (frame = 0; frame < HOST_FRAMES; ++frame) {
            double low = biquad_sample(&s->final_low[lane], quad[lane][frame]);
            quad[lane][frame] = biquad_sample(&s->final_high[lane], low);
        }
    }
}

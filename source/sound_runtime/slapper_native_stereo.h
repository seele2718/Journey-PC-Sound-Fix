/* PS4 LR1 raw-four -> native mode1, focus180, gain1/LFE0, flags0x443.
 * Includes the native 1/4 whole-matrix normalization. Reference-build stereo
 * selection: 393000 -> 3999c0 -> 3bc480 -> 4139e0 -> 416430.
 * These are local LR1 coefficients, not an emulator/master gain correction.
 */
#ifndef JOURNEY_SLAPPER_NATIVE_STEREO_H
#define JOURNEY_SLAPPER_NATIVE_STEREO_H
static const float journey_lr1_stereo_matrix[4][2] = {
    {0x1.b06f64p-3f, 0x1.098dc6p-3f},
    {0x1.fd4cdcp-3f, 0x1.878890p-6f},
    {0x1.878890p-6f, 0x1.fd4cdcp-3f},
    {0x1.098dc6p-3f, 0x1.b06f64p-3f}
};
static void journey_lr1_stereo_sample(const double lanes[4], float gain,
                                      float output[2])
{
    int side, lane;
    for (side = 0; side < 2; ++side) {
        float sum = 0.0f;
        for (lane = 0; lane < 4; ++lane)
            sum += (float)lanes[lane] * journey_lr1_stereo_matrix[lane][side];
        output[side] = sum * gain;
    }
}
#endif

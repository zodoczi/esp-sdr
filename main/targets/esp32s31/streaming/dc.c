#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stream.h"
#include "burst_gain_table.h"
#include "soc/soc.h"
#include <math.h>
#include <string.h>
extern void phy_pbus_force_test(unsigned, unsigned, unsigned);

/* Calibrate before publishing a new epoch. The diagnostic bus carries
 * I[9:2],Q[9:2]. Measure both DAC-to-I/Q response vectors on each setup;
 * do not assume the mapping of a different diagnostic capture path.
 * Gain-table originals remain pristine; no NVS calibration is written. */
float receiver_dc_before[2], receiver_dc_after[2], receiver_dc_jacobian[4];
unsigned receiver_dc_steps;
static int clamp_code(int value) { return value < 0 ? 0 : value > 511 ? 511 : value; }
static void dc_apply(int i, int q) {
    /* Store the DAC codes where the front end reloads them. PBUS-only writes
     * can be overwritten by gain/state transitions during reception. */
    if(!burst_gain_dc_set(receiver_config.gain, i, q))return;
    phy_pbus_force_test(2, 2, i);
    phy_pbus_force_test(3, 2, q);
    /* Like sensor-firmware: latch an identical shadow entry, then the real
     * entry, keeping forced gain enabled throughout. Never freeze all PBUS
     * receiver controls just to hold the two DC DACs. */
    unsigned reg=REG_READ(0x2010702cu)&0x00ffffffu;
    REG_WRITE(0x2010702cu,reg|(79u<<24));
    REG_WRITE(0x2010702cu,reg|(receiver_config.gain<<24));
    vTaskDelay(pdMS_TO_TICKS(10));
}
static float dc_cost(const float m[2]) { return m[0] * m[0] + m[1] * m[1]; }
void receiver_dc_calibrate(bool (*dc_measure)(float mean[2])) {
    receiver_dc_steps = 0;
    memset(receiver_dc_jacobian, 0, sizeof(receiver_dc_jacobian));
    memset(receiver_dc_before, 0, sizeof(receiver_dc_before));
    memset(receiver_dc_after, 0, sizeof(receiver_dc_after));
    unsigned codes[2];
    if(receiver_gain_max>=79 || !burst_gain_dc_codes(receiver_config.gain,codes))return;
    if (!dc_measure(receiver_dc_before))
        return;
    memcpy(receiver_dc_after, receiver_dc_before, sizeof(receiver_dc_after));
    if (dc_cost(receiver_dc_before) < .25f)
        return;
    int base[2] = {codes[0], codes[1]};
    int best[2] = {base[0], base[1]};
    float best_cost = dc_cost(receiver_dc_before);
    /* Identify the complete 2x2 response. PBUS and gain-RAM field names
     * are not an I/Q mapping contract for the live diagnostic source. */
    int di = base[0] > 495 ? -16 : 16, dq = base[1] > 495 ? -16 : 16;
    float mi[2], mq[2], observed[2];
    dc_apply(base[0] + di, base[1]);
    if (!dc_measure(mi)) {
        dc_apply(base[0], base[1]);
        return;
    }
    dc_apply(base[0], base[1] + dq);
    if (!dc_measure(mq)) {
        dc_apply(base[0], base[1]);
        return;
    }
    float a = (mi[0] - receiver_dc_before[0]) / di, b = (mq[0] - receiver_dc_before[0]) / dq;
    float c = (mi[1] - receiver_dc_before[1]) / di, d = (mq[1] - receiver_dc_before[1]) / dq;
    receiver_dc_jacobian[0] = a;
    receiver_dc_jacobian[1] = b;
    receiver_dc_jacobian[2] = c;
    receiver_dc_jacobian[3] = d;
    float determinant = a * d - b * c;
    if (fabsf(determinant) < .002f || fabsf(a) > 4 || fabsf(b) > 4 || fabsf(c) > 4 ||
        fabsf(d) > 4) {
        dc_apply(base[0], base[1]);
        return;
    }
    for (unsigned step = 0; step < 5 && best_cost > .25f; step++) {
        float delta[2] = {(-d * receiver_dc_after[0] + b * receiver_dc_after[1]) / determinant,
                          (c * receiver_dc_after[0] - a * receiver_dc_after[1]) / determinant};
        bool improved = false;
        for (unsigned trial = 0; trial < 3; trial++) {
            int next[2];
            for (unsigned axis = 0; axis < 2; axis++) {
                float change = fmaxf(-32.f, fminf(32.f, delta[axis])) / (1u << trial);
                next[axis] = clamp_code(best[axis] + (int)roundf(change));
                if (next[axis] < base[axis] - 96)
                    next[axis] = base[axis] - 96;
                if (next[axis] > base[axis] + 96)
                    next[axis] = base[axis] + 96;
            }
            if (next[0] == best[0] && next[1] == best[1])
                break;
            dc_apply(next[0], next[1]);
            receiver_dc_steps++;
            if (dc_measure(observed) && dc_cost(observed) < best_cost) {
                best_cost = dc_cost(observed);
                best[0] = next[0];
                best[1] = next[1];
                memcpy(receiver_dc_after, observed, sizeof(observed));
                improved = true;
                break;
            }
        }
        if (!improved)
            break;
    }
    /* The last trial may be worse; always latch the best measured codes. */
    dc_apply(best[0], best[1]);
}

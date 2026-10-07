#include "stream.h"
#include "burst_gain_table.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static unsigned codes[4][2], writes, observations, fail_at;
static float offset[2], response[4];
receiver_config_t receiver_config={.gain=40};
unsigned receiver_gain_max=71;
static unsigned gain_reg, latch_writes, table[160][3], originals[160][3];
extern void __wrap_phy_write_gain_mem(uint32_t,uint32_t,uint32_t,uint32_t);
void __real_phy_write_gain_mem(uint32_t a,uint32_t b,uint32_t c,uint32_t index) {
    assert(index<160);table[index][0]=a;table[index][1]=b;table[index][2]=c;
}
unsigned test_reg_read(unsigned address) {assert(address==0x2010702cu);return gain_reg;}
void test_reg_write(unsigned address,unsigned value) {
    assert(address==0x2010702cu);
    assert((value&0x00ffffffu)==(gain_reg&0x00ffffffu));
    assert(value&(1u<<23));
    assert((value>>24)==79 || (value>>24)==receiver_config.gain);
    assert(!memcmp(table[79],table[receiver_config.gain],sizeof(table[0])));
    gain_reg=value;latch_writes++;
}
void phy_pbus_force_test(unsigned block, unsigned bank, unsigned value) {
    assert(block < 4 && bank >= 1 && bank <= 2 && value <= 511);
    codes[block][bank - 1] = value;
    writes++;
}
void vTaskDelay(unsigned ticks) { (void)ticks; }
static bool measure(float mean[2]) {
    if (++observations == fail_at)
        return false;
    float i = (int)codes[2][1] - 256, q = (int)codes[3][1] - 256;
    mean[0] = offset[0] + response[0] * i + response[1] * q;
    mean[1] = offset[1] + response[2] * i + response[3] * q;
    return true;
}
static void setup(float i, float q, const float matrix[4]) {
    burst_gain_mirror(-1);
    for(unsigned j=0;j<160;j++) {
        unsigned a=(256u<<13)|(252u<<22)|j;
        unsigned b=128u|(254u<<8)|(j<<17),c=0x12345678u;
        __wrap_phy_write_gain_mem(a,b,c,j);
    }
    memcpy(originals,table,sizeof(table));
    burst_gain_mirror(receiver_config.gain);
    gain_reg=(receiver_config.gain<<24)|(1u<<23)|0x123456u;latch_writes=0;
    for (unsigned b = 0; b < 4; b++)
        for (unsigned n = 0; n < 2; n++)
            codes[b][n] = 256;
    offset[0] = i;
    offset[1] = q;
    memcpy(response, matrix, sizeof(response));
    writes = observations = fail_at = 0;
}
int main(void) {
    const float plants[][4] = {
        {.2f, 0, 0, .3f}, {0, .4f, -.3f, 0}, {.2f, .1f, -.1f, .2f}, {-.2f, 0, 0, -.4f}};
    for (unsigned p = 0; p < 4; p++) {
        setup(9, -7, plants[p]);
        receiver_dc_calibrate(measure);
        float m[2];
        assert(measure(m));
        assert(hypotf(m[0], m[1]) < .51f);
        assert(receiver_dc_steps <= 15);
        assert(latch_writes>0 && !(latch_writes&1));
        assert(gain_reg>>24==receiver_config.gain);
        // Both tables carry the same correction; RF DC and I/Q calibration
        // fields remain bit-identical to the native entry.
        unsigned index=receiver_config.gain;
        assert(!memcmp(table[index],table[index+80],sizeof(table[0])));
        assert(((table[index][0]^originals[index][0])&~((511u<<13)|(1u<<31)))==0);
        assert(((table[index][1]^originals[index][1])&~255u)==0);
        assert(table[index][2]==originals[index][2]);
        unsigned initial[2];assert(burst_gain_dc_codes(index,initial));
        assert(initial[0]==256 && initial[1]==256);
        assert(!burst_gain_dc_set(index,512,0));
        burst_gain_mirror(-1);
        assert(!memcmp(table[index],originals[index],sizeof(table[0])));
        assert(!memcmp(table[index+80],originals[index+80],sizeof(table[0])));
        // The actuator must not change the RF/baseband gain controls.
        for (unsigned b = 0; b < 2; b++)
            for (unsigned n = 0; n < 2; n++)
                assert(codes[b][n] == 256);
    }
    const float flat[] = {.001f, 0, 0, .001f};
    setup(10, -10, flat);
    receiver_dc_calibrate(measure);
    assert(codes[2][1] == 256 && codes[3][1] == 256);
    setup(0, 0, plants[0]);
    receiver_dc_calibrate(measure);
    assert(writes == 0);
    setup(10, -10, plants[0]);
    fail_at = 1;
    receiver_dc_calibrate(measure);
    assert(writes == 0);
    setup(10, -10, plants[0]);
    fail_at = 2;
    receiver_dc_calibrate(measure);
    assert(codes[2][1] == 256 && codes[3][1] == 256);
    setup(200, -200, plants[0]);
    receiver_dc_calibrate(measure);
    assert(codes[2][1] >= 160 && codes[2][1] <= 352);
    assert(codes[3][1] >= 160 && codes[3][1] <= 352);
    setup(10, -10, plants[0]);receiver_gain_max=79;
    receiver_dc_calibrate(measure);assert(writes==0 && latch_writes==0);
    puts("Analog calibration: cross-coupled/inverted plants, bounds, and failure rollback passed");
}

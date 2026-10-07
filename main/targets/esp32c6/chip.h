#pragma once
#include "rx_tuning.h"
#include "rx_lo.h"
#define SRAM_OWNER_REG 0x60095004u
#define IQ_WORDS 16380u
/* C6 stock librftest adctrig uses SRAM bank 2 and source 15. */
#define BURST_ID "C6SDR"
#define IQ_BUFFER ((uint32_t *)0x40840000)
SOC_RESERVE_MEMORY_REGION(0x40820000,0x40860000,c6_rf_dump);
extern void chip_v7_set_chan(unsigned,unsigned);
extern void phy_set_freq(unsigned,int);
static void c6_set_chan(unsigned mhz,unsigned mode) {
 /* Calibrate on a real Wi-Fi channel, then program the PLL directly. The
  * channel API converts MHz through mhz2ieee and loses off-grid requests. */
 bool channel=(mhz>=2412 && mhz<=2472 && (mhz-2412)%5==0)||mhz==2484;
 rx_lo_plan_t plan=rx_lo_plan(mhz);
 rx_lo_select(false);
 chip_v7_set_chan(channel?mhz:2412,mode);
 if(!channel)phy_set_freq(plan.mhz,plan.offset_khz);
}
#define phy_chip_set_chan c6_set_chan
#define phy_stop_tx_tone ram_stop_tx_tone
#define phy_pbus_workmode pbus_workmode
#define phy_pbus_xpd_tx_off rom_pbus_xpd_tx_off
#define phy_pbus_xpd_rx_on ram_pbus_xpd_rx_on
#define phy_set_rxclk_en set_rxclk_en
#define phy_chip_i2c_readReg rom_chip_i2c_readReg
#define phy_i2c_writeReg rom_chip_i2c_writeReg
static bool frequency_valid(unsigned f) {
 return rx_frequency_valid(f);
}
static bool stock_capture(unsigned n,unsigned divider) {
 if(divider)return false; /* Only the 80 MS/s source-15 path is verified. */
 const uint32_t owner=REG_READ(SRAM_OWNER_REG);
 /* Clock force settings from C6 rftest_init/phy_set_clk_conf. */
 REG_WRITE(0x600a9804,0xffffffffu);REG_WRITE(0x600a9814,0x7ffffu);REG_WRITE(0x600a980c,0xffffffffu);
 REG_WRITE(0x600a9004,0);
 REG_WRITE(0x600a9014,(REG_READ(0x600a9014)&~0x01ffffffu)|(1u<<6)|(2u<<12)|(3u<<18));
 REG_WRITE(0x600a9008,(REG_READ(0x600a9008)&~0x00078000u)|(15u<<15));
 REG_CLR_BIT(0x600a20b4,1);
 REG_WRITE(SRAM_OWNER_REG,(owner&~0xf00u)|0x400u);
 __asm__ volatile("fence rw,rw" ::: "memory");(void)REG_READ(SRAM_OWNER_REG);
 REG_WRITE(0x600a9004,n|(1u<<18));REG_WRITE(0x600a9004,n|(1u<<31));
 REG_WRITE(0x600a9004,n|(1u<<31)|(1u<<19));REG_WRITE(0x600a9004,n|(1u<<31));
 const int64_t deadline=esp_timer_get_time()+20000;
 bool done;do {done=(REG_READ(0x600a9004)&(1u<<18))!=0;}while(!done && esp_timer_get_time()<deadline);
 REG_WRITE(0x600a9004,0);REG_WRITE(SRAM_OWNER_REG,owner);
 __asm__ volatile("fence rw,rw" ::: "memory");(void)REG_READ(SRAM_OWNER_REG);
 return done;
}

"""Exercise the S3 production command parser with hardware calls stubbed."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class ReceiveCommands(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_rx_commands_and_removed_transmit_commands(self):
        source=(Path(__file__).resolve().parents[1]/'main/targets/esp32s3/receiver.c').read_text()
        handler=source[source.index('static void handle_command('):source.index('void app_main(')]
        stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#define BURST_SERIAL_UART 1
#define IQ_WORDS 16380u
#define CONFIG_IDF_TARGET_ESP32S3 1
#include "rx_bandwidth.h"
#include "rx_tuning.h"
#define S3_FREQ_MIN RX_FREQ_MIN
#define S3_FREQ_MAX RX_FREQ_MAX
#define CONFIG_ESP_SDR_UART_ENABLED 1
static unsigned frequency_mhz, captures, last_format;
static int rx_filter;
static bool rx_ready;
static char response[128];
static int burst_serial_port(void) { return 1; }
static unsigned burst_serial_baud(void) { return 2000000; }
static void reply(const char *s) { snprintf(response,sizeof(response),"%s",s); }
static bool gain_command(const char *s) { return false; }
static bool ring_command(const char *s){return false;}
static bool burst_version_command(const char *s) { return false; }
static bool burst_gpio_command(const char *s) { return false; }
static bool limits_command(const char *s) { return false; }
static bool capture(unsigned n,unsigned divider,unsigned format) { ++captures; last_format=format; return true; }
static void vTaskDelay(int ticks) {}
static unsigned rom_chip_i2c_readReg(unsigned a,unsigned b,unsigned c) { return 4; }
static void prepare_rx(void) { rx_ready=true; }
'''
        check=r'''
static void command(const char *s) { char line[128]; snprintf(line,sizeof(line),"%s",s); handle_command(line); }
int main(void) {
 command("CAPS"); assert(strstr(response,"DUALSERIAL")); assert(strstr(response,"HWAGC"));
 assert(!strstr(response,"REPLAY")); assert(!strstr(response,"CW"));
 command("TRANSPORT?"); assert(!strcmp(response,"TRANSPORT UART 2000000\n"));
 command("CAP16 16380 0"); assert(captures==1 && last_format==16);
 command("CAP20 16380 6"); assert(captures==2 && last_format==20);
 command("RXRUN 16380 0 2 16"); assert(captures==4 && !strcmp(response,"END\n"));
 const char *removed[]={"TX 256 40000000 0","TX16 256 40000000 0","TX20 256 40000000 0",
 "TXRUN 256 40000000 1 16","LOOP16 256 40000000 1 0","LOOP20 256 40000000 1 0",
 "REPLAY16 256 40000000 0","REPLAY20 256 40000000 0","CW START","CW KEEP","CW STOP"};
 for(unsigned i=0;i<sizeof(removed)/sizeof(removed[0]);i++) { command(removed[i]); assert(!strcmp(response,"ERR command\n")); }
 assert(captures==4);
 command("FREQ 2442"); assert(frequency_mhz==2442 && rx_ready);
 command("LPF 16"); assert(rx_filter==16); command("LPF AUTO"); assert(rx_filter==-1);
 command("BANDWIDTH 33");assert(rx_filter==16);command("BANDWIDTH 69");assert(rx_filter==0);
 command("BANDWIDTH 70");assert(!strcmp(response,"ERR command\n"));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'commands.c';path.write_text(stub+handler+check)
            binary=Path(tmp)/'commands'
            subprocess.run(['cc','-std=c11','-I'+str(Path(__file__).resolve().parents[1]/'main/common'),str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

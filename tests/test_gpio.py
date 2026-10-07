"""Compile the production GPIO service against mocked IDF GPIO/transport APIs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    'ESP32': (40, None, (1, 3), set(range(6, 12)) | {16, 17} | set(range(34, 40))),
    'ESP32C2': (21, None, (20, 19), set(range(11, 18))),
    'ESP32C3': (22, (18, 19), (21, 20), set(range(11, 18))),
    'ESP32C5': (29, (13, 14), (11, 12), set(range(15, 23))),
    'ESP32C6': (31, (12, 13), (16, 17), set(range(24, 31))),
    'ESP32C61': (30, (12, 13), (11, 10), set(range(14, 22))),
    'ESP32H2': (28, (26, 27), (24, 23), set(range(15, 22))),
    'ESP32S2': (47, (19, 20), (43, 44), set(range(26, 33)) | {46}),
    'ESP32S3': (49, (19, 20), (43, 44), set(range(26, 38)) | set(range(22, 26))),
    'ESP32S31': (62, (33, 34), (58, 59), set(range(26, 33)) | {41}),
}

@unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
class GpioService(unittest.TestCase):
    def test_gpio_service_on_all_targets(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            headers = {
                'driver/gpio.h': '''#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define GPIO_MODE_DISABLE 0
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
int gpio_config(const gpio_config_t *);
int gpio_set_level(unsigned, unsigned);
int gpio_set_direction(unsigned, int);
''',
                'esp_check.h': '#include <assert.h>\n#define ESP_ERROR_CHECK(x) assert((x)==0)\n',
                'esp_private/esp_gpio_reserve.h': '''#include <stdint.h>
#include <stdbool.h>
bool esp_gpio_is_reserved(uint64_t);
uint64_t esp_gpio_reserve(uint64_t);
''',
                'soc/usb_pins.h': '#define USBPHY_DM_NUM 19\n#define USBPHY_DP_NUM 20\n',
            }
            for name, text in headers.items():
                p = tmp/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
            harness = tmp/'harness.c'
            harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "burst_gpio.h"
#include "burst_serial.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
static uint64_t reserved = UINT64_C(1)<<5, configured;
static unsigned levels[64], modes[64], calls, order;
static int fail;
static char response[512];
bool esp_gpio_is_reserved(uint64_t mask) { return !!(reserved & mask); }
uint64_t esp_gpio_reserve(uint64_t mask) { uint64_t old=reserved;reserved|=mask;return old; }
int gpio_config(const gpio_config_t *c) {
 assert(c->mode==GPIO_MODE_DISABLE && !c->pull_up_en && !c->pull_down_en && !c->intr_type);
 assert(!(configured & c->pin_bit_mask));configured|=c->pin_bit_mask;++calls;return 0;
}
int gpio_set_level(unsigned pin,unsigned level) { ++calls;order=1;if(fail)return -1;levels[pin]=level;return 0; }
int gpio_set_direction(unsigned pin,int mode) { ++calls;if(mode==GPIO_MODE_OUTPUT)assert(order==1);order=0;modes[pin]=mode;return 0; }
bool burst_serial_send(const void *data,size_t size) { assert(size<sizeof(response));memcpy(response,data,size);response[size]=0;return true; }
int main(void) {
 burst_gpio_init();assert(configured==EXPECTED_MASK);
 assert(reserved==(EXPECTED_MASK|(UINT64_C(1)<<5)));
 unsigned initialized=calls;
 assert(burst_gpio_command("GPIO?"));assert(!strncmp(response,"GPIO",4));
 assert(calls==initialized); /* Discovery has no GPIO writes. */
 for(unsigned pin=0;pin<SOC_GPIO_PIN_COUNT;pin++) {
  char item[16],cmd[32],ack[32];snprintf(item,sizeof(item)," %u:Z",pin);
  assert(!!strstr(response,item)==!!(EXPECTED_MASK&(UINT64_C(1)<<pin)));
  if(!(EXPECTED_MASK&(UINT64_C(1)<<pin)))continue;
  for(const char *s="10Z";*s;s++) {
   snprintf(cmd,sizeof(cmd),"GPIO %u %c",pin,*s);
   assert(burst_gpio_command(cmd));snprintf(ack,sizeof(ack),"OK GPIO %u %c\n",pin,*s);
   assert(!strcmp(response,ack));
   assert(modes[pin]==(*s=='Z'?GPIO_MODE_DISABLE:GPIO_MODE_OUTPUT));
   if(*s!='Z')assert(levels[pin]==(unsigned)(*s=='1'));
  }
  assert(burst_gpio_command("GPIO?"));
 }
 const char *bad[]={"GPIO", "GPIO 5 1", "GPIO 64 1", "GPIO 999999999999999999999 0",
  "GPIO -1 0", "GPIO -0 1", "GPIO +0 1", "GPIO 00 1", "GPIO 0 2", "GPIO 0 z", "GPIO 0 1 junk", "GPIO 0", "GPIO 0 10"};
 unsigned before=calls;
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){assert(burst_gpio_command(bad[i]));assert(!strcmp(response,"ERR gpio_args\n"));}
 assert(calls==before);assert(!burst_gpio_command("FREQ 2412"));
 /* State reflects acknowledged requests, and survives subsequent queries. */
 assert(burst_gpio_command("GPIO 0 1"));
 assert(burst_gpio_command("GPIO?"));assert(strstr(response," 0:1"));
 fail=1;assert(burst_gpio_command("GPIO 0 0"));assert(!strcmp(response,"ERR gpio_io\n"));
 assert(burst_gpio_command("GPIO?"));assert(strstr(response," 0:1"));
 return 0;
}
''')
            for target, (count, usb, uart, excluded) in TARGETS.items():
                for uart_enabled in (0, 1):
                    with self.subTest(target=target, uart=uart_enabled):
                        # Pin 5 stands for an additional IDF-owned peripheral/remapped memory pin.
                        unavailable = excluded | {5} | set(usb or ()) | (set(uart) if uart_enabled else set())
                        expected = sum(1 << p for p in range(count) if p not in unavailable)
                        (tmp/'soc/soc_caps.h').write_text(f'''#define SOC_GPIO_PIN_COUNT {count}
#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK ((UINT64_C(1)<<{count})-1)
#define SOC_USB_SERIAL_JTAG_SUPPORTED {int(usb is not None and target != 'ESP32S2')}
''')
                        (tmp/'soc/io_mux_reg.h').write_text('' if usb is None else f'#define USB_INT_PHY0_DM_GPIO_NUM {usb[0]}\n#define USB_INT_PHY0_DP_GPIO_NUM {usb[1]}\n')
                        # Mimic silicon holes/input-only pins separately from the service's memory exclusions.
                        invalid = {p for p in excluded if (target=='ESP32' and p>=34) or (target=='ESP32S2' and p==46) or (target=='ESP32S3' and p in range(22,26)) or (target=='ESP32S31' and p==41)}
                        with (tmp/'soc/soc_caps.h').open('a') as f:
                            if invalid:
                                f.write(f'#undef SOC_GPIO_VALID_OUTPUT_GPIO_MASK\n#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK {sum(1 << p for p in range(count) if p not in invalid)}ULL\n')
                        binary = tmp/'gpio-test'
                        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            f'-DCONFIG_IDF_TARGET_{target}=1', f'-DCONFIG_ESP_SDR_UART_ENABLED={uart_enabled}',
                            f'-DCONFIG_ESP_SDR_UART_TX_PIN={uart[0]}', f'-DCONFIG_ESP_SDR_UART_RX_PIN={uart[1]}',
                            f'-DEXPECTED_MASK={expected}ULL', '-I'+str(tmp), '-I'+str(ROOT/'main/common'),
                            str(ROOT/'main/common/burst_gpio.c'), str(harness), '-o', str(binary)], check=True)
                        subprocess.run([str(binary)], check=True)

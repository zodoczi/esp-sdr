"""Version wire response and metadata must match the bytes exported to clients."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('version_exporter', ROOT/'tools/export_web_firmware.py')
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)
RECORD = dict(revision='a'*40+'-dirty', build_date='2026-10-06',
              build_timestamp='2026-10-06T22:12:34Z', profile='esp32c61')

class FirmwareVersion(unittest.TestCase):
    def test_embedded_metadata_validation(self):
        def image(value):
            return b'prefixESP-SDR-VERSION:'+json.dumps(value).encode()+b'\0suffix'
        self.assertEqual(exporter.firmware_version(image(RECORD)), RECORD)
        self.assertIsNone(exporter.firmware_version(b'old firmware'))
        for patch in [dict(build_date='2026-10-05'), dict(build_timestamp='2026-02-30T00:00:00Z'),
                      dict(build_timestamp='2026-10-06T22:12:34'), dict(revision='invalid'), dict(profile='../bad')]:
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                exporter.firmware_version(image(dict(RECORD, **patch)))

    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_production_version_command(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory)
            record='ESP-SDR-VERSION:'+json.dumps(RECORD,separators=(',',':'))
            (tmp/'build_version.h').write_text('#define ESP_SDR_VERSION_RECORD '+json.dumps(record)+'\n')
            (tmp/'harness.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "burst_serial.h"
#include "burst_version.h"
static char output[512];
bool burst_serial_send(const void *s,size_t n){strncat(output,s,n);return true;}
int main(void){
 assert(!burst_version_command("VERSION? junk"));assert(!output[0]);
 assert(!burst_version_command("INFO"));assert(!output[0]);
 assert(burst_version_command("VERSION?"));fputs(output,stdout);return 0;
}
''')
            binary=tmp/'version'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-I'+str(tmp),'-I'+str(ROOT/'main/common'),
                            str(ROOT/'main/common/burst_version.c'),str(tmp/'harness.c'),'-o',str(binary)],check=True)
            response=subprocess.check_output([str(binary)],text=True)
            self.assertTrue(response.startswith('VERSION '))
            self.assertEqual(json.loads(response[8:]),RECORD)
            self.assertEqual(exporter.firmware_version(binary.read_bytes()),RECORD)

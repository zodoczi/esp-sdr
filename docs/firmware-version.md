# Firmware version reporting

All burst receivers advertise `VERSION` in `CAPS`. Send `VERSION?` to receive
one newline-terminated response:

```text
VERSION {"revision":"10f9b3c4e0b78c91da98c93a1e1317c0581f00d5-dirty","build_date":"2026-10-06","build_timestamp":"2026-10-06T20:06:11Z","profile":"esp32c61"}
```

The revision is the full Git commit, with `-dirty` when tracked files have
local changes, or `unknown` when Git metadata is unavailable. The build
timestamp uses UTC with second precision, including for incremental builds.
`build_date` is its date-only form. `profile` identifies the burst firmware's
chip target. The separate S31 streaming application does not use this protocol.

CMake runs `tools/generate_version.py` to embed the metadata in the application.
`tools/export_web_firmware.py` extracts that same record from the binary into
the flasher manifest variant's `git_revision`, `build_date`, and
`build_timestamp` fields. Export time and the packager's Git checkout do not
affect the recorded firmware age. Existing release labels remain unchanged.

On connection, esp-web-sdr reads this information and fetches the flasher's
`firmware/manifest.json` without caching. It compares the matching variant's
build timestamp and shows an update warning with a link to the flasher if
the installed build is older. Equal or newer builds do not warn. Git revisions
are informational and never determine age. Older manifests with only
`build_date` are compared at date resolution.

Firmware without the `VERSION` capability, or returning `ERR command` to
`VERSION?`, is considered out of date even when the manifest is unavailable.
An unavailable catalog, missing matching variant, or malformed version metadata
does not otherwise trigger an age warning. The catalog request does not block
reception. Hovering over the device name shows its reported revision and build
timestamp.

# Task 1 — Espressif Encoder Dependency Report

## Scope and files changed

Vendored the following files under both `moody-tx/libraries/esp_new_jpeg/`
and `moody-tx 2/libraries/esp_new_jpeg/`:

- `LICENSE`
- `library.properties`
- `src/esp_jpeg_common.h`
- `src/esp_jpeg_enc.h`
- `src/esp32s3/libesp_new_jpeg.a`

The Task 1 temporary include/link probe is present in both transmitter sketches
for Task 2 to replace when its wrapper consumes the API. The two `.ino` files
are byte-identical.

## Provenance

- Component: Espressif `esp_new_jpeg` v1.0.2.
- Official source: <https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2>.
- Official archive endpoint: <https://components.espressif.com/api/downloads/?object_type=component&object_id=f346478b-a1a6-4eed-b903-b8658e031d87>.
- Registry manifest upstream commit: `daee964980d14d7fcf9da10821827399ef600577` in `espressif/esp-adf-libs`, path `esp_new_jpeg`.
- Downloaded archive SHA-256: `c15430c5a07ba1ed76c88589dd8bb2a020c6343e1255fcf0ebc235477f99fba9`.

The vendored artefacts have the upstream SHA-256 values below:

| File | SHA-256 |
| --- | --- |
| `LICENSE` | `0bf7f2018fffa1caff5ecbf10d79707b218eea4e26b8c705d744de4556552815` |
| `src/esp_jpeg_common.h` | `c0852f3a5322af939c8f59fe7ba1ab231a143b6397def11847a7d955bfdb39c6` |
| `src/esp_jpeg_enc.h` | `a73eeee202e52b9285bcfa6cbf2d8d803c2f96e2b3dd6338d26736c3e6eb5deb` |
| `src/esp32s3/libesp_new_jpeg.a` | `2d08f7ed9e0e265173e2a651bf889190db8cc172fa2dff6ef63ffa8c73d24cd8` |

`library.properties` pins `version=1.0.2`, `architectures=esp32`, and
`precompiled=true`, with the v1.0.2 source URL in comments. It explicitly sets
`dot_a_linkage=false`: Arduino CLI must use the official archive filename
`libesp_new_jpeg.a`, rather than its dot-linkage `esp_new_jpeg.a` convention.

## Compile evidence

Baseline command requested by the plan:

```bash
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi' \
  --output-dir /tmp/moody-tx-build moody-tx
```

Pre-vendor result: failed as expected with:

```text
fatal error: esp_jpeg_enc.h: No such file or directory
```

The baseline command remains unable to discover a sketch-local `libraries/`
directory under Arduino CLI 1.5.1 (`Alternatives for esp_jpeg_enc.h: []`). The
project-local canonical command is therefore:

```bash
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi' \
  --libraries moody-tx/libraries \
  --output-dir /tmp/moody-tx-build moody-tx
```

With the temporary probe calling both `jpeg_enc_open` and
`jpeg_enc_process`, it succeeded (exit 0) and printed:

```text
Library moody-esp-new-jpeg has been declared precompiled:
Using precompiled library in .../libraries/esp_new_jpeg/src/esp32s3
Sketch uses 1023403 bytes (32%) of program storage space.
Global variables use 65076 bytes (19%) of dynamic memory.
```

The same real-symbol probe succeeded (exit 0) for the second TX mirror when
compiled from a temporary directory named `moody-tx`. This is necessary only
because Arduino requires a sketch directory's name to match its primary `.ino`
file, while the permanent mirror directory is intentionally named
`moody-tx 2`. The project-local helper `scripts/compile-tx.sh` copies that
mirror to a temporary correctly named directory and runs both canonical
compiles; invoke it as `sh scripts/compile-tx.sh`.

## Mirror and cleanup verification

Commands and results:

```bash
cmp -s moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino'  # exit 0
diff -qr moody-tx/libraries/esp_new_jpeg 'moody-tx 2/libraries/esp_new_jpeg'  # exit 0
rg -n 'esp_jpeg_enc|encoderLinkProbe' \
  moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino'  # matching include and declaration in both mirrors
```

## Deviation

The plan's original bare compile command cannot discover a vendor-local Arduino
library. The approved project-local deviation is `--libraries <sketch>/libraries`
in `scripts/compile-tx.sh`; it does not install or rely on a global library.
No renamed archive was added. The temporary API probe was removed after both
link proofs in the initial handoff, then restored by review; it remains in
place for Task 2 to replace when its wrapper consumes the API.

## Correction evidence

Review restored the Task 1 probe in both TX sketches, consistent with the
plan's requirement that Task 2 removes it only after the wrapper consumes the
API. Each sketch now contains:

```cpp
#include <esp_jpeg_enc.h>
static jpeg_enc_handle_t encoderLinkProbe = nullptr;
```

`sh scripts/compile-tx.sh` completed with exit 0 after this restoration. Both
compile outputs reported the sketch-local precompiled package at
`libraries/esp_new_jpeg/src/esp32s3`. `cmp` of the sketches and recursive
`diff` of the vendor directories both exited 0. The Task 1 plan metadata
snippet now also explicitly specifies `dot_a_linkage=false` and its reason:
retaining Espressif's canonical `libesp_new_jpeg.a` filename.

### Forced-symbol probe correction

The static handle alone does not force archive symbols. The final temporary
probe is therefore a `__attribute__((used, noinline, retain))` helper that
directly invokes the header's type-correct APIs:

```cpp
(void)jpeg_enc_open(&config, &encoderLinkProbe);
(void)jpeg_enc_process(encoderLinkProbe, input, sizeof(input), output,
                       sizeof(output), &outputSize);
```

`setup()` only calls the helper behind an internal `volatile bool` initialized
to `false`, so ordinary firmware execution does not invoke it, while the
linker must retain the code and resolve both symbols. This guard was necessary
because the target linker garbage-collected an unreferenced `used` helper.

Final command:

```bash
sh scripts/compile-tx.sh
```

Result: exit 0 for both TX mirrors; each used 1023439 bytes (32%) of program
storage and 65076 bytes (19%) of global memory. ELF verification used:

```bash
/Users/bencartwright/Library/Arduino15/packages/esp32/tools/esp-x32/2511/bin/xtensa-esp32s3-elf-nm -C \
  /tmp/moody-tx-build/moody-tx.ino.elf
```

and the corresponding `/tmp/moody-tx-2-build/moody-tx.ino.elf`. Both printed:

```text
42002db4 t compileOnlyEncoderLinkProbe()
42094e6c T jpeg_enc_open
42094fd8 T jpeg_enc_process
```

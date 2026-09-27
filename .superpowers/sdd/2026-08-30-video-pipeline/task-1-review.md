# Task 1 Review — Vendor encoder dependency

## Final re-review verdict: accepted

The final compile-only probe directly references both required APIs, remains
inert in normal firmware execution, and is byte-identical in the two TX
mirrors. All Task 1 review findings are resolved.

### Final verification

- Fresh `sh scripts/compile-tx.sh` completed for both mirrors using the
  prescribed ESP32-S3 FQBN and project-local library paths.
- Both generated ELFs contain `compileOnlyEncoderLinkProbe()`,
  `jpeg_enc_open`, and `jpeg_enc_process` (`xtensa-esp32s3-elf-nm -C`).
- `encoderLinkProbeEnabled` is a translation-unit-local `volatile bool`
  initialized to `false`, has no writers, and is the only condition guarding
  the probe call in `setup()`. Normal firmware execution therefore does not
  call the deliberately invalid one-byte probe.
- `cmp -s` confirmed matching TX sketches; `diff -qr` confirmed matching
  vendored dependency trees.

---

## Previous re-review history: rejected — one Important issue remained

### Resolved

1. **Metadata/plan mismatch resolved.** Task 1 now specifies
   `dot_a_linkage=false` and explains why that is required for the official
   `libesp_new_jpeg.a`. Both mirrors retain that setting, and the Arduino
   builder selects it with `-lesp_new_jpeg`.

### Important remaining issue

1. **The restored probe is include-only, not a real-symbol link probe.** Both
   sketches now include `esp_jpeg_enc.h` and declare
   `encoderLinkProbe`, so the fresh helper build proves header discovery and
   library selection. However, neither sketch calls or otherwise references
   `jpeg_enc_open` or `jpeg_enc_process`. Fresh inspection of both produced
   ELFs found neither symbol. Consequently the helper does not prove the Task
   1 produced interface or substantiate the amended report's claim that the
   probe calls both APIs. Make the temporary probe reference both functions
   until Task 2 replaces it with the wrapper, then rerun the helper.

### Fresh verification

- `sh scripts/compile-tx.sh` exited 0 for both mirrors with the prescribed
  ESP32-S3 FQBN and reported the primary vendored precompiled package.
- `cmp -s` confirms the TX sketches are identical; `diff -qr` confirms the
  dependency trees are identical.
- `xtensa-esp32s3-elf-nm -C` of `/tmp/moody-tx-build/moody-tx.ino.elf` and
  `/tmp/moody-tx-2-build/moody-tx.ino.elf` returned no `jpeg_enc_open` or
  `jpeg_enc_process` entry.

---

## Initial-review history: changes requested

### Critical

None.

### Important

1. **The metadata deliberately differs from the written Task 1 plan.** The
   plan requires `dot_a_linkage=true`, while both vendored
   `library.properties` files set it to `false`. The actual setting is
   technically validated: Arduino CLI 1.5.1 emitted
   `-L.../src/esp32s3 -lesp_new_jpeg`, which correctly selects the official
   `libesp_new_jpeg.a`, and the link probe passed. Nevertheless, update the
   plan/ruling to record this tested correction before treating Task 1 as
   plan-complete; the present report alone cannot override its explicit
   acceptance instruction.

2. **The reproducible helper no longer proves the Task 1 interface.**
   `scripts/compile-tx.sh` successfully builds both current sketches, but
   neither sketch includes `esp_jpeg_enc.h`, so that command does not discover
   or link `esp_new_jpeg`. A separate temporary probe was required to establish
   the claimed `jpeg_enc_open`/`jpeg_enc_process` link. This is especially at
   odds with Task 1 step 5, which says to remove the probe only after Task 2's
   wrapper consumes the API. Preserve a reproducible temporary probe in the
   helper, or carry the probe through until Task 2 has landed, so the evidence
   remains independently repeatable.

### Minor

1. **The canonical project-local command is not user-facing.** It is recorded
   in the task report and encoded in `scripts/compile-tx.sh`, but no README or
   developer build documentation names `sh scripts/compile-tx.sh`. The
   invocation itself is appropriate: Arduino CLI 1.5.1 does not auto-discover
   a sketch-local `libraries/` directory, and `--libraries` keeps the
   dependency project-local. Document it when the planned README work occurs.

## Verified evidence

- Both vendored directory trees are byte-identical; both transmitter sketches
  are byte-identical; `rg` found no lingering probe identifiers.
- The four reported SHA-256 values for `LICENSE`, both headers, and
  `src/esp32s3/libesp_new_jpeg.a` match the files in the primary mirror; the
  exact mirror makes them true for the second tree as well.
- `sh scripts/compile-tx.sh` completed successfully for both TX mirrors with
  Arduino-ESP32 3.3.5 and the prescribed ESP32-S3 FQBN.
- An isolated temporary copy with a real `jpeg_enc_open` and
  `jpeg_enc_process` call compiled successfully. Verbose output resolved
  `moody-esp-new-jpeg@1.0.2`, used its precompiled ESP32-S3 directory, and
  linked with `-lesp_new_jpeg`.
- The archive exports both required symbols. The stated upstream provenance
  could not be independently downloaded during this review; the local
  artefact hashes do match the task report.

# RELEASE: bundles for the test team

One folder per release, named after the main firmware version, plus the zip that is emailed.

```
RELEASE/
  make_release.sh        builds everything from source, tests it, packages one release
  notes_to_pdf.py        RELEASE_NOTES.md -> .html and .pdf (used by make_release.sh)
  v6.9.38/
    RELEASE_NOTES.md     written by hand for each release (.html and .pdf are generated)
    SD_CARD/             copy to the root of a unit's SD card
      fw_ksn_trg.hex       main firmware, KSNDMC rain gauge
      fw_bihar.hex         main firmware, Bihar rain gauge
      fw_ksn_tws.hex       main firmware, KSNDMC weather station
      ctrl.hex             CTRL firmware (all types)
      fw.hex               old-unit conversion image (MIG); ignored by units already converted
    FACTORY_MCU-LINK/    full chip images for programming with the MCU-Link
    SHA256SUMS.txt       checksums of every image
    BUILD_INFO.txt       when it was built and from which git commit
  LPC-AIO_v6.9.38_CTRL_v1.12_MIG-1.7.zip   the bundle to email: SD_CARD/ + RELEASE_NOTES.pdf only
```

## Making a release

1. Raise the versions in the source: `FIRMWARE_VERSION` in `LPC1768/user_config.h`, `FW845_VERSION` in
   `LPC845/boot/fw845_app.h` (if the CTRL changed), `MIG_VERSION` in `TEST_JIG/migrate/main.c` (if the
   migration or the CTRL changed).
2. For a production release set `TESTING_DEBUG_LOG 0` in `LPC1768/user_config.h`.
3. Write `RELEASE/<version>/RELEASE_NOTES.md`: what changed, how to install, what to test, known limits.
4. Commit the source, then run `RELEASE/make_release.sh`. It stops if the notes are missing or any host test
   fails. `BUILD_INFO.txt` records the commit, and says so if there were uncommitted changes.
5. Email the zip (it already holds the release notes PDF). Nothing else goes in the email.
   Subject: `LPC-AIO firmware release <main version> (CTRL <ctrl version>, <MIG version>) for testing`,
   e.g. `LPC-AIO firmware release v6.9.38 (CTRL v1.12, MIG-1.7) for testing`.

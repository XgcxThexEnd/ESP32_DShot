# ESP-IDF HTTPS transport bench

This opt-in test compiles the production `main/ota_transport.c` with the actual
ESP-IDF HTTP client, lwIP, and TLS stack. It exercises verified connections,
redirect visibility, truncated responses, and total deadlines while headers or
bodies keep making partial progress. It does not initialize motors or write an
OTA partition. Use a spare ESP32-S3 with motor hardware disconnected.

1. Create a test TLS server certificate with a SAN matching the fixture's DNS
   name or LAN IP. Keep its private key outside the repository or in `secrets/`.
   The ESP32 must be able to resolve and reach that hostname.
2. Start the fixture on the bench machine:

   ```sh
   python tools/ota_https_fixture.py --bind 0.0.0.0 --cert secrets/server.pem --key secrets/server.key
   ```

3. Activate the release SDK, then configure the separate test project:

   ```sh
   cd tests/idf/ota_transport
   idf.py set-target esp32s3
   idf.py menuconfig
   ```

   Set Wi-Fi under **Example Connection Configuration** and the fixture URL
   under **OTA transport bench**. Under mbedTLS certificate bundle options,
   enable a custom certificate bundle and set its path to the test CA PEM.
   Use the CA certificate only, never its private key. Leave peer/hostname
   verification enabled. The test obtains wall time using SNTP.
4. Build and run on the bench device:

   ```sh
   idf.py build
   idf.py -p PORT flash monitor
   ```

   Save serial output. Expect five `OTA_TLS_CASE ... PASS` records and a final
   `OTA_TLS_RESULT PASS`. Slow responses use a three-second total budget and a
   two-second operation budget so the test finishes quickly. This exercises
   the same deadline implementation used by the firmware's five-minute OTA
   limit. Initial DNS resolution still uses the SDK's own timeout.
5. Repeat with the server presenting an untrusted certificate, then with a
   trusted certificate whose SAN does not match the configured URL. Enable
   `OTA_TEST_REJECT_CERT` for these runs. Also verify `/ok` with that option
   disabled first, so an unreachable fixture cannot masquerade as a successful
   certificate-rejection test. Retain the server and serial logs together.
6. Introduce more than 50 ms TCP connection latency on the bench network and
   rerun the normal case. `/ok` must still pass within the connection budget.

These results complement the downloader host tests; signed-image acceptance,
flash failures, boot selection, and rollback still use the full firmware HIL
procedure in `docs/hardware-validation.md`.

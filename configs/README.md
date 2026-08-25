# Configuration layers

Every file here is non-secret and is intended to be layered after the common
`sdkconfig.defaults` through `SDKCONFIG_DEFAULTS`.

| Layer | Purpose |
| --- | --- |
| `sdkconfig.dev.defaults` | local diagnostic build |
| `sdkconfig.production.defaults` | reversible production build defaults |
| `sdkconfig.single-fan-node.defaults` | one ESC/motor per ESP32-S3 with full-MAC-derived node identity |
| `sdkconfig.ci-safe.defaults` | smallest safety-oriented CI build |
| `sdkconfig.ci-max-four-fan.defaults` | compile the ESP32-S3 four-channel limit |
| `sdkconfig.ci-optional-features.defaults` | compile schedules, persistence, OTA/rollback, interlock, tach, communication fail-safe, and Home Assistant discovery |
| `sdkconfig.ci-mqtt-restart.defaults` | compile broker-restart policy with the continue-running lease policy |
| `sdkconfig.ci-legacy.defaults` | compile the bounded legacy-topic migration path at fan indexes 5–8 |

The CI fan GPIO lists are build fixtures, not wiring recommendations. All local
credentials belong in an ignored `sdkconfig.<environment>.local`, never in a
defaults fragment. No fragment enables Secure Boot, flash encryption, encrypted
NVS, signing-key paths, or another setting that can depend on private/eFuse
material.

Example CI layer order:

```text
sdkconfig.defaults;configs/sdkconfig.ci-safe.defaults;configs/sdkconfig.ci-optional-features.defaults
```

Example one-fan production layer order:

```text
sdkconfig.defaults;configs/sdkconfig.single-fan-node.defaults;configs/sdkconfig.production.defaults
```

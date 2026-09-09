# AI gardener companion

The gardener is a companion service, not part of the ESP32 motor-control
firmware. Keep the controller deterministic and able to remain safe without a
camera, model, database, or internet connection.

The first deployment phase is **observe and recommend only**:

```text
fixed cameras + environmental sensors
                |
                v
        measurement pipeline
                |
                v
     time-series observations + evidence
                |
                v
       gardener advisor/model
                |
                v
       advisory recommendation
                |
                v
 human review or deterministic policy gateway
                |
                v
 existing node-scoped ESP32 MQTT commands
```

The model principal must not have broker permission to publish motor, schedule,
alarm-clear, or OTA commands. It may read redacted controller telemetry and
publish only to a dedicated gardener observation/recommendation subtree. A
separate, narrowly authorized policy gateway may eventually translate an
approved, unexpired recommendation into an existing node-scoped command.

## Initial message contracts

Machine-readable contracts live in:

- `schemas/gardener-observation.schema.json`
- `schemas/gardener-recommendation.schema.json`

Every observation is tied to a capture time, camera, zone, model version,
processing time, stable subject/normalized ROI, calibration record, measurement
source identity, quality indicators, and immutable image digest. Store an
opaque object key rather than a public or credential-bearing image URL. Every
recommendation carries an ID, creation and expiry times, confidence, evidence
IDs, and an explicit `actuation_allowed: false` boundary.

Validate messages before publishing or accepting them:

```bash
python scripts/validate_gardener_message.py observation observation.json
python scripts/validate_gardener_message.py observation observation.json \
  --require-current
python scripts/validate_gardener_message.py recommendation recommendation.json \
  --require-current
```

The executable validator applies Draft 2020-12 schemas without echoing payload
values, rejects duplicate JSON members, non-finite numbers, invalid ROI/time
ordering, ambiguous object keys, and inconsistent camera identity/quality. It
limits a recommendation to a one-hour lifetime. `--require-current` allows at
most 60 seconds of future clock skew and requires an observation captured in
the preceding five minutes. A gateway must apply the current-time gate to the
recommendation and every supporting observation immediately before approval.
Downstream code must use the exact parsed object that passed validation (or a
canonical serialization of it), rather than reparsing the original bytes with
a more permissive JSON parser.

Suggested MQTT topics, owned by the companion service rather than a fan node:

```text
<root>/gardener/status
<root>/gardener/zones/<zone-id>/observations
<root>/gardener/zones/<zone-id>/recommendations
```

Do not retain individual observations or recommendations indefinitely on the
broker. Keep the latest coarse status retained, and send detailed history to a
time-series/object store with an explicit retention policy.

## What a camera can and cannot measure

A calibrated fixed camera can estimate canopy coverage, growth rate, color
distribution, leaf-count trends, visible wilting, and visual anomalies. It does
not directly measure soil moisture, air temperature, relative humidity, CO2,
nutrient concentration, root health, or actual airflow. Add physical sensors for
those quantities and let the gardener correlate them with vision results.

For repeatable vision measurements:

1. Use a fixed mount, known field of view, stable focus, and a scale/color
   reference in each zone.
2. Control or measure illumination; day/night and grow-light changes otherwise
   look like plant changes.
3. Record camera movement, obstruction, blur, glare, darkness, and missing-frame
   quality signals. Low-quality evidence must produce `uncertain` observations,
   not confident advice.
4. Identify zones and plants independently of image coordinates so a camera
   replacement does not silently rewrite history.
5. Preserve a small, consent-appropriate labeled dataset across seasons,
   cultivars, and disease states to measure false positives and model drift.

## Automation gate

Only consider automated actuation after the advisory phase has measured useful
accuracy. The deterministic gateway should enforce all of the following before
publishing an ESP32 command:

- recommendation ID has not already been consumed;
- every evidence ID resolves atomically to a trusted-producer observation from
  the same zone, with matching image digest, acceptable calibration/quality,
  and fresh capture/processing times;
- controller health is fresh;
- recommendation has not expired;
- confidence and image-quality thresholds pass;
- target node/fan is allowlisted and the requested percentage is bounded;
- minimum dwell time and maximum changes per hour are respected;
- maintenance/manual override, interlock, or any safety latch wins;
- every decision and resulting controller acknowledgement is audit logged;
- the gardener identity can never publish OTA or clear a safety latch.

Treat a deny-by-default broker ACL and a negative authorization test as release
evidence: prove that the gardener credential cannot publish a motor, schedule,
alarm-clear, or OTA command. The exact fixture belongs with the selected broker
because ACL syntax and matching behavior differ.

The gateway must fail closed to **no new AI-requested change**. That is distinct
from the controller's deployment-specific decision to continue, stop, or use a
fallback speed after communication loss.

## Recommended rollout

1. **Observe:** collect images and physical sensor data; publish dashboards and
   camera-quality alerts.
2. **Advise:** generate expiring recommendations with evidence, but require a
   person to act.
3. **Shadow:** have the policy gateway calculate decisions without publishing
   them and compare those decisions with operator actions.
4. **Constrained automation:** authorize only narrow, reversible actions such as
   a bounded ventilation adjustment in one test zone.
5. **Expand deliberately:** add zones or action types only after reviewing
   accuracy, plant outcomes, hardware faults, and unsafe near misses.

An LLM can summarize trends, explain recommendations, and converse with the
operator. Numerical measurements should come from versioned vision/sensor
pipelines, and final actuator authorization should remain deterministic.
Before selecting sensors, define a versioned metric registry with canonical
units, physical ranges, sample timestamps, calibration identity, and allowed
source types. The current schema is a safe transport envelope, not yet that
deployment-specific measurement ontology.

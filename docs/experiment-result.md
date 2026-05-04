# Results overview

| Type | DT – negative (FP%) | DT – positive (TPR%) | MLP – negative (FP%) | MLP – positive (TPR%) |
|---|---|---|---|---|
| Regular | 25% FP | 19% detected | 16% FP | 76% detected |
| Irregular | 23% FP | 13% detected | 10% FP | 58% detected |
| Plow | 18% FP | 65% detected | 27% FP | 70% detected |

## 1. The DT is effectively broken — for a different reason than expected
At first glance the DT's false-positive rate (~20%) and false-negative rate on regular trains (~81%) looks like it's simply a bad model. But the patterns reveal something more specific.

False positives — what triggers them?
Looking at the dt-0-regular false positive runs, many have completely flat distance sequences. The trigger is end-of-sequence drift of ±1–3 cm from baseline:

```
RUN 046: all values 32.79–33.20, last two are 33.20/33.20 — perfectly flat → CLOSE
RUN 052: flat sequence, ends at 34.81 → CLOSE
RUN 059: flat, ends at 34.81/35.23 → CLOSE
RUN 038: flat, ends at 34.81 → CLOSE
```

A ~1.5 cm uptick at the end (which is just the stopped train settling, or sensor thermal drift) fires the gate. The DT is not reading these as "approaching" — it's reading tiny oscillations as meaningful. This almost certainly reflects a velocity or acceleration feature (v[i] or a[i]) in the training data format that the DT learned as a proxy for motion, and that feature has no noise floor.

### False negatives — the DT is blind to clean approaches

Looking at dt-1-regular FN runs, the patterns are unambiguous real approaches:

```
RUN 003:  33.97 → 33.97 → ... → 32.38 → 29.60 → 26.65 → 16.96 → 14.01  → OPEN (missed)
RUN 008:  33.99 → ... → 32.81 → 31.38 → 25.90 → 22.52 → 18.30 → 13.82  → OPEN (missed)
RUN 018:  33.56 → ... → 32.81 → 31.86 → 25.04 → 16.96 → 12.91 → 10.72  → OPEN (missed)
```

These are clean, monotonic drops to 10–14 cm — the most obvious possible approach signals. The DT cannot see them. Looking at which runs it does detect (the 19 CLOSEs in dt-1-regular), they tend to share two characteristics: either there are rejected samples (311+ cm substituted to low values) at the end, or the approach signal peaks and then partially recovers (e.g., drops to 14 cm then bounces to 23 cm). The DT appears to have learned "approach followed by partial recovery" as its CLOSE signature, not "approach to minimum." This makes sense if the training data consisted mostly of runs where the train crossed the sensor entirely (creating a large upward spike at the end), leaving behind a pattern the DT keyed on — but which doesn't appear when the train simply parks close to the sensor.

## 2. The DT plow surprise: 65% detection rate
The plow train is detected far more reliably by the DT than any other train type (65% vs 19% and 13%). This is not because the DT became more capable — it's because the plow creates the patterns the DT was accidentally trained to recognize:

The spinning plow generates intermittent -1 cm and 180–310 cm rejected readings, especially as the train gets close
The substituted values for these rejections are the last valid reading (which is already low, e.g., 19–25 cm)
This creates the "approach then partial bounce" signature (approach, rejection, substituted value, more rejections) that the DT's learned features trigger on
19% of plow positive runs have at least one rejection vs ~10% for regular train
The DT is not detecting the plow's motion — it's detecting the artifacts of the spinning sensor interference. This is a brittle coincidence: the plow's noise profile happens to match the DT's accidental training signature.

## 3. The overshoot pattern confirmed at scale
Yes — the "sensor reads far background after train passes" pattern is real and responsible for a substantial portion of MLP false negatives on the regular train:

From mlp-1-regular, looking at false negative patterns:

```
RUN 001: flat → 27.94 → 24.23 → 17.08 → 21.21 → 26.43           (minimum then recovery)
RUN 024: flat → 29.12 → 54.54 → 54.54 → 14.90                    (huge 54 cm spike mid-sequence)
RUN 026: flat → 31.98 → 26.69 → 18.68 → 34.93                    (drops to 18 then bounces to 35)
RUN 080: flat → 29.10 → 27.59 → 25.18 → 36.01                    (25 → 36 cm at the end)
RUN 091: flat → 33.17 → 31.13 → 26.58 → 42.33                    (26 → 42 cm at the end)
```

In each of these the train has clearly already crossed the nearest point and the sensor is now reading either the back of the train or the background. The MLP, trained only on regular-train data, has never seen a pattern that looks like "approach + overshoot spike" and classifies it as OPEN. The confidence values on these runs are consistently very low (0.001–0.25), not near 0.5 — the MLP is confidently wrong, not uncertain.

There is also a subtler version: the train reaches its minimum distance within the window but the last 2–3 samples start creeping back up due to deceleration (the train slowed enough to reverse the apparent distance decrease). The MLP interprets this as "stopped just past Mark 1 = safe."

## 4. Why the MLP did not need retraining — and where it starts to break
Why it generalizes: The MLP learned the temporal shape of an approach: roughly 7–9 stable samples at baseline, followed by a monotonically decreasing sequence. For any train whose solid body eventually presents a flat reflective face to the sensor, this shape is the same regardless of what's on the front. The irregular and plow trains both have a solid flat back section (the green frame, same on both) — the same frame used as the "body" of the regular train. So once the front geometry passes through the sensor range, the MLP sees the familiar shape.

Where it starts to break: The irregular train causes an extra 18 percentage points of false negatives (42% vs 24%). The mechanism is the tilted front: as it passes under the sensor beam, the angled surface deflects the echo sideways instead of back to the receiver. The sensor reads high (40–65 cm) during this phase — the train appears to be moving away — before the solid body brings it back close. The MLP sees:

```
stable at 34 cm → spike up to 45 cm → drop to 18 cm
```

The upward spike in the middle looks, to the MLP, like the sensor is losing the train. It doesn't correspond to anything in the training data. This is the most structurally interesting failure: geometry changes the sign of the distance signal for a portion of the window, and the model has no prior for that.

## 5. The MLP confidence split
Across all MLP files, the OPEN/CLOSE confidence distribution has a consistent structure:

True negatives (correct OPEN on -0- files): confidence almost always 0.10–0.50, tightly clustered. The model "knows it doesn't know" — it's not confidently saying OPEN, it's just below 0.5.
True positives (correct CLOSE on -1- files): confidence mostly 0.60–1.00, frequently above 0.90. When the pattern is clear, the model commits.
False negatives (incorrect OPEN on -1- files): confidence collapses to 0.001–0.20. The model sees an anomalous pattern and interprets it as strongly NOT approaching.
This means the MLP's failure mode is not "uncertain and wrong" — it's confidently wrong in a specific direction. The patterns that confuse it (overshoot, mid-window spike, flat substituted tail) all have one thing in common: they resemble "sensor reading far away" more than "sensor reading close." The MLP interprets "far reading" as "safe" and commits to that interpretation at very high confidence.

## 6. Rejected sample patterns by train type

| Model | Negative runs with rejections | Positive runs with rejections |
|---|---|---|
| DT regular | ~1% | ~10% |
| DT irregular | ~6% | 31% |
| DT plow | ~3% | 19% |
| MLP regular | ~2% | ~15% |
| MLP irregular | ~18% | 32% |
| MLP plow | ~7% | 13% |

The irregular and plow trains reject samples at 3–15× the rate of the regular train, and almost entirely at indices 12–14 (the last quarter of the window). This is the moment when the train is closest to the sensor and either:

the spinning plow causes reflections at impossible angles (-1 cm or 300+ cm), or
the tilted front deflects the beam completely (180–310 cm, far out-of-range)
The substitution policy (carry forward last valid value) then creates a flat plateau at whatever the last valid reading was, hiding the continuation of the approach. This is a consistent, geometry-driven failure: both models see the last 3–5 samples as "frozen" at some intermediate distance, when in reality the train may have continued moving significantly.

### Key asymmetry between the two models

| Metric | DT | MLP |
|---|---|---|
| Failure dominant mode | False negatives (misses real approaches) | False negatives (misses anomalous approaches) |
| False positive rate | ~22% average | ~18% average |
| Regular train sensitivity | ~19% | ~76% |
| Geometry sensitivity | Very high — different geometry → near-zero detection | Moderate — different geometry → measurable but partial degradation |
| When it confidently fires wrong | Fires CLOSE on flat-or-drifting sequences | Fires OPEN on overshoot/mid-spike sequences |
| What makes it "click" | Velocity/acceleration artifacts from rejections | Correct approach shape (flat → decreasing monotonic) |

The DT's 65% plow detection is a red herring — it's detecting the sensor artifacts caused by the spinning plow, not the train's motion. If the plow geometry changed or the rejection substitution policy changed, that 65% would likely collapse. The MLP's 76% on the regular train and 70% on the plow represent genuine generalization; the 58% on the irregular train represents a partial breakdown caused by a qualitatively new geometric phenomenon (mid-window beam deflection) that is entirely absent from its training distribution.


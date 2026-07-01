- [ ] make the loss function swapeble via function pointer

# Flight Controller Research: Targeting Position Instead of Aligning Orientation

Deep research notes for improving `flightControl.c`. The current code (V1..V6) iteratively
searches for aileron/elevator/rudder values that minimize a loss made of:
`angularAlignment + distanceWeight * distanceToTarget`. This document proposes ideas that stay
close to the existing "simulate-ahead + minimize loss" structure but shift the objective from
**aligning the nose perfectly** toward **hitting the target with minimal miss distance**.

Pseudo-code uses Python syntax. None of it changes the data structures; it maps directly to the
existing `Plane`, `Controller`, `ControllerOutput` and the `updatePlane` rollout.

---

## 1. The core problem with the current objective

The losses `rollLoss`, `pitchLoss`, `yawLoss` and `alignmentLoss` all measure *orientation*:
"is the nose pointing at the target right now". A plane is not a laser. To hit a point you must
care about where the **trajectory** ends up, not where the nose points. Pure alignment causes:

- Overshoot near the target (nose locks on, plane flies past, can't turn tight enough).
- Ignoring lateral momentum (velocity vector lags the nose, so distance keeps growing).
- No notion of "lead" or curved interception.

**Key reframe:** the controller should minimize *predicted miss distance* over the lookahead,
not instantaneous angle. Angle alignment is a useful regularizer, not the goal.

```python
# current implicit goal
loss = angle(forward, toTarget) + w * distance

# better goal: smallest distance the predicted trajectory ever reaches the target
loss = min(distance_to_target(t) for t in rollout) + small * angle_penalty
```

---

## 2. Loss redesign (highest impact, lowest effort)

### 2.1 Closest-point-of-approach (CPA) loss 
- [ ] Didnt work look into it more but kept the selcting the best one
Instead of distance at the last lookahead step, track the **minimum** distance across the whole
rollout. This directly optimizes "hit", not "be near at step N".

```python
def cpa_loss(plane, controls, target, steps, dt):
    p = clone(plane); apply(p, controls)
    best = INF
    for _ in range(steps):
        update(p, dt)
        best = min(best, length(target - p.position))
    return best
```

### 2.2 Closing-rate loss (velocity toward target)
Reward shrinking distance, not pointing. Use velocity, which already exists (`alignmentLossVelocity`):

```python
def closing_loss(plane, target):
    to_t = normalize(target - plane.position)
    closing_speed = dot(plane.velocity, to_t)   # m/s actually closing
    return -closing_speed                        # more negative = better
```

### 2.3 Lateral-miss decomposition (along / cross track)
Split error into "distance along the sight line" vs "lateral offset". Lateral is what you must
kill; along-track is just throttle. Penalize lateral much harder.

```python
def miss_loss(plane, target):
    to_t = target - plane.position
    dir = normalize(to_t)
    along = dot(plane.velocity, dir)             # closing
    lateral = plane.velocity - dir * along       # sideways drift = miss
    return length(lateral) - 0.1 * along
```

This is the single most useful change: it tells the optimizer the real cost is sideways drift.

---

## 3. Proportional Navigation (the classic guidance answer)

Real missiles do not align with the target; they null the **line-of-sight rotation rate**. If the
bearing to the target stops rotating, you are on a collision course. This is cheap and excellent.

```python
def pro_nav(plane, target, dt, N=4.0):
    los = normalize(target - plane.position)
    los_rate = (los - prev_los) / dt            # rotation of sight line
    closing = -dot(plane.velocity, los)
    accel_cmd = N * closing * cross(los, los_rate)  # accel perpendicular to LOS
    return accel_cmd                             # convert to pitch/yaw demand
```

Map `accel_cmd` to surfaces by projecting onto plane up/right axes -> elevator/rudder demand,
then roll so lift points the right way. Use your simulate-ahead loop only to pick the surface
values that produce `accel_cmd` over the lookahead. Far fewer iterations needed.

---

## 4. Keep gradient descent (V4/V5) but fix the loss + tuning

V5 already does central-difference gradient + momentum. Good base. Improvements:

- Replace `evaluateLossV2` core with **CPA + lateral miss** (sections 2.1/2.3).
- Warm-start `values` from previous frame instead of resetting to 0.5 (huge: trajectory is smooth).
- Larger `epsilon` early, shrink it (matches recenter idea) for stable then fine gradient.
- Decoupled per-axis learning rate (roll converges faster than yaw).

```python
def control(prev, target, dt):
    v = prev.copy()                  # warm start
    m = [0,0,0]; lr = 0.1; eps = 0.05
    for it in range(iters):
        g = central_diff(v, eps)
        g = g / (norm(g)+1e-6)
        m = 0.9*m - lr*g
        v = clamp01(v + m)
        lr *= 0.97; eps = max(0.01, eps*0.98)
    return v
```

---

## 5. Two-loop architecture: guidance + stabilization

Split the job. Outer loop decides *where to point* (desired turn rate). Inner PID converts that to
surface deflection. This is robust and standard.

```python
# OUTER: pro-nav or lateral-miss gives desired body rates
desired_rate = guidance(plane, target)
# INNER: PID tracks desired_rate -> surfaces
elevator = pid_pitch(desired_rate.y - body_rate.y)
rudder   = pid_yaw  (desired_rate.z - body_rate.z)
aileron  = pid_roll (roll_to_align_lift)
```

Tune PID with the simulate-ahead loop as the cost (autotune). Replaces brute search at runtime.

---

## 6. Sampling / search upgrades (still your style)

- **CEM (cross-entropy method):** sample N control triples from a Gaussian, keep best K, refit mean/std, repeat. Robust, parallel, no gradients. ~50 samples beats 384 line-search steps.
- **MPPI (model-predictive path integral):** sample control *sequences*, weight by exp(-loss). Naturally targets trajectory, ideal for hit-the-point.

```python
def cem(target, steps=4, samples=64, elite=8):
    mean=[0.5]*3; std=[0.3]*3
    for _ in range(steps):
        pop=[clip(gauss(mean,std)) for _ in range(samples)]
        pop.sort(key=lambda c: cpa_loss(plane,c,target))
        best=pop[:elite]
        mean=avg(best); std=stddev(best)
    return mean
```

---

## 7. Better rollouts

- Multi-horizon blend (already a TODO): combine miss at 8/16/32 steps; near-term avoids overshoot, far-term avoids dead-ends.
- Variable lookahead: long when far, short when close (avoid overshoot guidance).
- Re-plan only when LOS-rate angle change exceeds threshold (the `LossAngle` idea) to save cost.

---

## 8. Throttle/energy

Add throttle to the optimized vector. Pull throttle to tighten turns near target; full when far.
Penalize stall (speed below corner). Energy management often fixes overshoot better than tuning gains.

---

## Recommended order
1. Swap loss to lateral-miss + CPA (sec 2). Biggest gain, least code.
2. Warm-start + per-axis tuning in V5 (sec 4).
3. Add pro-nav outer loop (sec 3) or CEM (sec 6).
4. PID inner loop + throttle (sec 5, 8).

Metric to track in CSV: minimum distance reached, not final distance.

---

# Why `evaluateLossV3` and `evaluateLossV4` perform worse than `evaluateLossV2`

Deep dive requested: the two "experimental" loss functions (both marked
`// TODO: doesn't work look into it`) consistently under-perform the baseline `evaluateLossV2`
when driven through `getControllerOutputV5`. Below is the term-by-term reason why, the exact
mechanism that breaks each one, and concrete fixes. No code was changed; this is analysis only.

## 0. Context that determines how the losses are used

Two facts about the surrounding machinery change how a loss "reads", so they must be kept in mind:

1. **`LookaheadSteps = 16`** (`LOOKAHEAD_STEPS` in `flightControl.h`), `dt = 1/60`. Each loss
   evaluation rolls the plane ~0.27 s. At F-16 speeds (~250-350 m/s) the plane travels
   ~70-95 m per evaluation, so `finalDist - currentDist` is on the order of **tens of meters
   (magnitude ~ -90..+90)**, whereas every alignment term is bounded to **[-1, 1]**. In raw
   *value* the distance term dwarfs all alignment terms.

2. **`getControllerOutputV5` normalizes the gradient to unit length** before stepping:
   ```c
   gradient /= sqrt(gx*gx + gy*gy + gz*gz);   // only DIRECTION survives
   ```
   This is the crucial subtlety. Because only the *direction* of the 3-axis gradient is used,
   what matters is not the absolute magnitude of a term but its **sensitivity to the controls**
   (`d(term)/d(control)`) *relative to the other terms*. A term with a large value but a flat
   response to the controls contributes little to the descent direction; a small-valued term
   with a sharp response can dominate the step. This is exactly where V3 and V4 go wrong: they
   inject terms whose *gradient* competes with, cancels, or reverses the alignment gradient.

## 1. Baseline: what makes `evaluateLossV2` well-behaved

```c
loss = finalAlignment                            // -cos(theta_nose),   full weight  [-1,1]
     + finalAlignmentVelocityVector              // -cos(theta_vel),    full weight  [-1,1]
     + runningAlignmentLoss / N                  // avg nose alignment, [-1,1]
     + runningAlignmentLossVelocityVector / N    // avg vel alignment,  [-1,1]
     + (finalDist - currentDist);                // radial progress, meters
```

Two properties make V2 a good descent target:

- **Terminal + integral pairing.** Each angular objective appears twice: once as the *terminal*
  value at the end of the rollout (`finalAlignment`, `finalAlignmentVelocityVector`) and once as
  the *running average* over the rollout. The terminal term supplies a strong, well-localized
  gradient ("where do I end up pointing"), the running term smooths it. Effective angular weight
  per objective is ~2x.
- **Monotone, smooth angular terms.** `alignmentLoss = -dot(forward, toTarget) = -cos(theta)` and
  `alignmentLossVelocity = -cos(theta_vel)` are both monotonically *decreasing in alignment* over
  the entire range theta in [0, pi]: pointing more at the target always lowers the loss, pointing
  away always raises it. The gradient never lies. This is the key virtue V4 destroys.
- **Radial term is a signed integral.** `finalDist - currentDist` is a clean, signed measure of
  progress. It has a consistent sign (closing = negative = good).

Net: V2 gives the normalized-gradient optimizer a coherent direction where "turn toward target"
and "close on target" reinforce each other.

## 2. `evaluateLossV3` — double-counted radial objective + halved angular weight

```c
for (step) {
    update(simPlane, dt);
    runningLoss += alignmentLoss(simPlane, target);          // -cos(theta_nose)
    runningLoss += alignmentLossVelocity(simPlane, target);  // -cos(theta_vel)
    float closingSpeed = dot(simPlane.velocity, toTarget);   // m/s, raw (unnormalized)
    runningLoss -= closingSpeed * (1.0f/340.0f);             // reward closing
}
return (runningLoss / N) + (finalDist - currentDist);
```

Three compounding problems:

### 2.1 The closing-speed reward is redundant with the distance term (double counting)
Radial progress and closing speed are the *same physical quantity integrated*:
$$ \text{finalDist} - \text{currentDist} \;\approx\; -\int_0^{T} v \cdot \hat{u}_{\text{LOS}}\; dt \;=\; -\int_0^{T} \text{closingSpeed}\; dt. $$
So `(finalDist - currentDist)` already *is* an (integrated) closing-speed reward. Adding
`-(1/N) * sum(closingSpeed/340)` on top injects the **radial objective a second time**. The two
radial contributions have the same sign and their gradients add, so the effective weight on
"fly straight at the point as fast as possible" is roughly doubled relative to the angular terms.
The optimizer collapses toward **pure pursuit** (nose-on-now), which is exactly the overshoot /
can't-turn-tight failure mode described in section 1 of the original notes.

### 2.2 Angular weight is halved versus V2
V3 dropped V2's dedicated terminal terms (`finalAlignment`, `finalAlignmentVelocityVector`). It
keeps only the running average `runningLoss/N`, i.e. **one** averaged copy of each angular
objective instead of V2's terminal+running pair (~2x). So at the same time V3 *doubled* the
radial pull it *halved* the angular pull. The alignment signal that lets the plane set up a turn
is marginalized. Ratio of angular:radial guidance is roughly 4x worse than V2.

### 2.3 Scale/units inconsistency of the closing term
`closingSpeed` uses the **raw** velocity (m/s), while `alignmentLossVelocity` uses the
**normalized** velocity. `closingSpeed/340` is ~0.7-1.0 while approaching at cruise, but is
unbounded above (a diving F-16 past 340 m/s, or high closing geometry, pushes it >1). Inside
`runningLoss` it therefore intermittently **out-scales** the [-1,1] alignment terms, so the
per-step gradient direction is dominated by "go faster at the point" precisely when the geometry
most needs a turning correction. Because V5 normalizes the gradient, this hijacks the whole step.

**Symptoms you should see in the CSV:** distance shrinks initially (good closing) but nose/velocity
alignment stalls, then the plane overshoots and the distance curve bottoms out early and climbs —
minimum-distance reached is worse than V2, even though early closing looked better.

### Fix for V3
- Remove the double count. Do **not** reward closing speed *and* subtract the distance delta.
  Keep one. Simplest: drop the explicit `closingSpeed` term and rely on `finalDist - currentDist`,
  or replace the distance delta with a pure normalized closing reward but not both.
- Restore V2's terminal angular terms so angular weight is not halved.
- If you keep an explicit closing term, normalize it and weight it small so it cannot exceed the
  angular band, e.g. `-w_close * clamp(closingSpeed / maxSpeed, -1, 1)` with `w_close ~ 0.1-0.25`.
- Better still (matches section 2 of the original notes): replace radial progress with a
  closest-point-of-approach (CPA) term so the objective is "smallest miss", not "fastest closing".

```c
// balanced V3: single radial objective, full angular weight, bounded closing
loss  = finalAlignment + finalAlignmentVelocityVector;          // terminal angular (as V2)
loss += (runningAlignmentLoss + runningAlignmentVel) / N;       // integral angular (as V2)
loss += (finalDist - currentDist);                              // radial ONCE
// optional small extra shaping, bounded so it can't dominate:
loss += -0.15f * fminf(1.0f, fmaxf(-1.0f, avgClosing / maxSpeed));
```

## 3. `evaluateLossV4` — a non-monotone lateral term with a reversed gradient

```c
for (step) {
    update(simPlane, dt);
    runningLoss += alignmentLoss(simPlane, target);          // -cos(theta_nose)
    runningLoss += alignmentLossVelocity(simPlane, target);  // -cos(theta_vel)
    float3 velDir  = normalize(simPlane.velocity);
    float  along   = dot(velDir, toTarget);
    float3 lateral = velDir - toTarget * along;              // perpendicular component
    runningLoss   += length(lateral);                        // "penalize sideways drift"
}
return (runningLoss / N) + (finalDist - currentDist);
```

This one is worse than V3, and the reason is a genuine gradient bug, not just weighting.

### 3.1 The lateral term is `sin(theta_vel)` — it is redundant with `alignmentLossVelocity`
Both `velDir` and `toTarget` are unit vectors, so `along = cos(theta_vel)` and
`length(lateral) = |velDir - toTarget*cos(theta)| = sin(theta_vel)`. But the loop **already**
adds `alignmentLossVelocity = -cos(theta_vel)`. So V4 is measuring the *same* velocity/LOS angle
twice, through `sin` and `-cos`. Redundant, and worse, mismatched in shape.

### 3.2 `sin(theta)` is non-monotone: the gradient reverses past 90 degrees
Over theta_vel in [0, pi], `sin(theta)` rises from 0 to a maximum at 90 deg, then **falls back to
0 at 180 deg**. That means:

| velocity vs LOS                | sin(theta) penalty | direction of gradient           |
|--------------------------------|--------------------|---------------------------------|
| pointing at target (theta -> 0)| ~0, rising slope   | pushes toward alignment (OK)    |
| broadside (theta = 90 deg)     | max = 1            | penalty peaks                   |
| pointing away (theta in 90..180)| **decreasing**    | **rewards pointing further away** |

For any geometry where the velocity vector is more than 90 deg off the target (very common right
after launch, during a hard turn, or after an overshoot), the lateral term's gradient points the
**wrong way**: increasing misalignment toward 180 deg *reduces* `sin(theta)`, so the optimizer is
rewarded for turning the velocity vector *away* from the target. This directly fights the
`-cos(theta)` term and can create a stable-but-wrong equilibrium where the plane flies away.
Under V5's gradient normalization the bogus lateral gradient can win the axis and steer the search
into the wrong basin.

### 3.3 It also drops V2's terminal terms (same halving as V3)
Like V3, V4 has no dedicated `finalAlignment` / `finalAlignmentVelocityVector`, so the reliable
terminal angular gradient is gone, leaving the broken `sin` term relatively more influential.

**Symptoms in the CSV:** erratic / oscillating heading, velocity alignment that gets *worse* once
the plane is pointed away, and occasional runaway trajectories where distance grows monotonically.

### Fix for V4
- **Drop the redundancy.** `alignmentLossVelocity` already captures velocity/LOS alignment
  correctly and monotonically. If you want a stronger lateral penalty, scale that term up rather
  than adding `sin`.
- If a true cross-track penalty is desired, use a **monotone** formulation that keeps rising as
  alignment worsens, e.g. `(1 - cos(theta)) = 1 + alignmentLossVelocity` (range [0,2], strictly
  increasing with misalignment), instead of `sin(theta)`.
- Alternatively penalize the **actual lateral velocity magnitude in m/s** (do not normalize
  `velDir`), which is the physically meaningful "sideways drift" the comment intends:
  ```c
  float along        = dot(simPlane.velocity, toTarget);        // raw m/s
  float3 lateralVel  = simPlane.velocity - toTarget * along;    // raw m/s sideways
  runningLoss += w_lat * length(lateralVel) / maxSpeed;         // bounded, monotone-ish
  ```
  This still has the theta>90 caveat, so pair it with the along-track (closing) sign so the
  optimizer always prefers reducing theta. The `(1 - cos)` form is the safest.
- Restore the terminal angular terms as in V2.

## 4. Common root causes (summary)

Both experimental losses share the same three structural mistakes relative to V2:

1. **Removed the terminal angular terms** -> halved, less-localized angular guidance.
2. **Added a term that is redundant with an existing one** (V3: closing vs distance-delta;
   V4: `sin` vs `-cos`) -> the optimizer over-weights one physical objective.
3. **Introduced a term with bad scaling or a reversed gradient** (V3: unbounded raw closing speed;
   V4: non-monotone `sin(theta)`), which, under V5's *gradient normalization*, can capture the
   descent direction and steer into the wrong basin.

## 5. Recommended concrete changes (priority order)

1. Keep V2's terminal + integral angular structure in any new loss; never delete the terminal
   `finalAlignment` / `finalAlignmentVelocityVector` terms.
2. Never reward closing speed *and* subtract the distance delta - they are the same integral.
   Prefer replacing the distance delta with the CPA (minimum-distance) term (original notes 2.1).
3. Any auxiliary shaping term must be (a) bounded to the same [-1,1] band as the angular terms and
   (b) **monotone** in the quantity it penalizes. Use `(1 - cos)` not `sin`, and normalize raw
   speeds by a reference `maxSpeed`.
4. Because V5 normalizes the gradient, add explicit tunable weights per term and verify that no
   single term's *gradient* (not value) dominates - a quick check is to log per-term gradients for
   one representative geometry and confirm they are within ~1 order of magnitude of each other.


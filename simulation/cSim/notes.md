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

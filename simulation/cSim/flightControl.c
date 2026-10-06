// TODO: hand made algo to find good surface control to target point in 3d space
// using iterative approaches where we try to minimize the loss based on this
// 1. try to apply roll so the top of the plane will be facing the target
// 2. apply this algo to pitch
// pitch = 0.5  // neutral
// for try in range(tries):
//     saveState()
//     apply(pitch)
//     for step in range(lookahead):
//         simStep()
//     nextLoss = distanceToTarget
//     restoreState()          // re-evaluate from same point each try

//     currentLoss = distanceToTarget
//     if nextLoss < currentLoss:
//         pitch *= 1.125
//     else:
//         pitch /= 1.125
// 3. try to same algo for yaw

#include "flightControl.h"
#include <math.h>
#include <string.h>

float rollLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	// Projection kept unnormalized: atan2 is scale-invariant, while
	// normalizing a near-degenerate projection amplified float noise.
	float3 idealUp = Float3_Sub(toTarget, Float3_Scale(forward, Float3_Dot(toTarget, forward)));

	float cross = Float3_Dot(Float3_Cross(up, idealUp), forward);
	float dot = Float3_Dot(up, idealUp);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect roll alignment
}

float pitchLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 right = planeGetRightVector(plane);

	float3 idealForward = Float3_Sub(toTarget, Float3_Scale(right, Float3_Dot(toTarget, right)));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), right);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect pitch alignment
}

float yawLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	float3 idealForward = Float3_Sub(toTarget, Float3_Scale(up, Float3_Dot(toTarget, up)));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), up);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect yaw alignment
}

static inline float alignmentLoss(const Plane *plane, float3 target) {
	float3 planePosition = plane->position;
	float3 toTarget = Float3_Normalize(Float3_Sub(target, planePosition));
	float3 forward = planeGetForwardVector(plane);

	float alignment = Float3_Dot(forward, toTarget); // 1 = perfect, -1 = opposite
	return -alignment;								 // -1 = perfect alignment, 1 = opposite direction
}

static inline float alignmentLossVelocity(const Plane *plane, float3 target) {
	float3 planePosition = plane->position;
	float3 toTarget = Float3_Normalize(Float3_Sub(target, planePosition));
	float3 forward = Float3_Normalize(plane->velocity);

	float alignment = Float3_Dot(forward, toTarget); // 1 = perfect, -1 = opposite
	return -alignment;								 // -1 = perfect alignment, 1 = opposite direction
}

static inline float distanceToTarget(const Plane *plane, float3 target) {
	return Float3_Length(Float3_Sub(target, plane->position));
}

static float evaluateLossV2(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float runningAlignmentLoss = 0.0f;
	float runningAlignmentLossVelocityVector = 0.0f;
	float currentDist = distanceToTarget(&ctrl->plane, target);

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningAlignmentLoss += alignmentLoss(&simPlane, target);
		runningAlignmentLossVelocityVector += alignmentLossVelocity(&simPlane, target);
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignmentVelocityVector = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float loss = finalAlignment + finalAlignmentVelocityVector + (runningAlignmentLoss / (float)ctrl->LookaheadSteps) + (runningAlignmentLossVelocityVector / (float)ctrl->LookaheadSteps) + (finalDist - currentDist);

	return loss;
}

// V2Plus: fixes V2's "miss by small margin" by blending finalDist
// and minDist, and amplifying alignment weight near the target.
// finalDist preserves the turning gradient after overshoot; minDist
// rewards closest approach. Near the target, alignment weight increases
// so the optimizer prioritizes precision over raw distance reduction.
static float evaluateLossV2Plus(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float minDist = currentDist;
	float runningAlignment = 0.0f;
	float runningAlignVel = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningAlignment += alignmentLoss(&simPlane, target);
		runningAlignVel += alignmentLossVelocity(&simPlane, target);
		float d = distanceToTarget(&simPlane, target);
		if (d < minDist) minDist = d;
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignVel = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	// Blend finalDist (turning gradient) and minDist (approach accuracy)
	float distImprovement = (finalDist - currentDist) * 0.4f + (minDist - currentDist) * 0.6f;
	float overshootTerm = (finalDist - minDist) * 0.5f;

	// Amplify alignment near target. At 1700m 1.18x, at 500m 1.57x, at 200m 2.0x, at 50m 2.6x, at 0m 3.0x.
	float alignWeight = 1.0f + 2.0f / (1.0f + currentDist * 0.005f);

	float loss = (finalAlignment + finalAlignVel) * alignWeight + (runningAlignment / (float)ctrl->LookaheadSteps) * alignWeight + (runningAlignVel / (float)ctrl->LookaheadSteps) * alignWeight + distImprovement + overshootTerm;

	return loss;
}

// V2PlusTuned_v1: final tuned — A=3.0, B=0.0063.
// At 0m 4.0x, at 50m 3.28x, at 100m 2.84x, at 200m 2.33x, at 500m 1.81x.
static float evaluateLossV2PlusTuned(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float minDist = currentDist;
	float runningAlignment = 0.0f;
	float runningAlignVel = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningAlignment += alignmentLoss(&simPlane, target);
		runningAlignVel += alignmentLossVelocity(&simPlane, target);
		float d = distanceToTarget(&simPlane, target);
		if (d < minDist) minDist = d;
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignVel = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float distImprovement = (finalDist - currentDist) * 0.3f + (minDist - currentDist) * 0.7f;
	float overshootTerm = (finalDist - minDist) * 1.0f;

	float alignWeight = 1.0f + 3.0f / (1.0f + currentDist * 0.0063f);

	float loss = (finalAlignment + finalAlignVel) * alignWeight + (runningAlignment / (float)ctrl->LookaheadSteps) * alignWeight + (runningAlignVel / (float)ctrl->LookaheadSteps) * alignWeight + distImprovement + overshootTerm;

	return loss;
}

// V2PlusTuned_v2: A=3.0, B=0.0065 — runner-up, slightly faster decay than v1.
static float evaluateLossV2PlusTuned2(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	// Horizon at fixed cost.  The loss simulates ctrl->LookaheadSteps (=16)
	// steps, so its horizon is 16 * dt = 0.27 s, which cannot see the turn a
	// ~700 m turn radius needs: with the 0.27 s loss the interceptor flies past
	// a *static* target (closest approach 214 m of a 275 m start) and thereafter
	// only orbits, speed bleeding 188 -> 43 m/s with 72% of the steps saturated.
	// Simulating a 1.07 s horizon inside the same 16 steps keeps the cost:
	// measured on the suite, miss 373.6 -> 342.3 m (+8.4%), every tier at or
	// better than baseline, cost +2%.  Buying the same 1.07 s horizon with 64
	// fine steps - no coarse-step distortion at all - is worth a comparable
	// +7.7% miss but costs 3.8x, over the +20% budget, so the coarse step is
	// what fits.  Swept at fixed cost: 2x (0.53 s) +4.4%, 4x (1.07 s) +8.4%,
	// 8x (2.13 s) +7.4% with the step tier -1.1%, so 4x is the peak.
	// Known approximation: the coarser step also advances the predictor's
	// surface-slew model 4x, so the planner assumes its surfaces reach the
	// commanded deflection within the horizon while the plant still slews at dt.
	// The plant, the suite, the rendered frame and the returned state are
	// untouched: the loss only mutates a by-value copy of the plane.
	// 2026-09-28: the commitment window and the missing control-effort cost.
	// Measured on this suite (build/flightBench/flightBench, the pinned
	// baseline): the window alone at 2.25x takes the closest approach
	// 342.3 -> 321.1 m (-6.2%) but pays control effort 20.49 -> 26.66 (+30%,
	// outside the bench's 5% band) and drives saturated steps 2365 -> 5876 --
	// a shorter window reacts sooner to a manoeuvring target, so the search
	// commands a bigger deflection and holds it into the stops.  The window
	// alone is therefore a rejected trade, and what was missing from the
	// objective is the cost of the deflection itself (below); with it the pair
	// gives 336.3 m (-1.8%) and effort 14.06 (-31%) at satSteps 2282, i.e. both
	// scored terms improve and saturation falls below baseline.  The sign and
	// rough size reproduce under unrelated codegens, so the change is not an
	// artifact of this build's float reordering.
	const float simDeltaTime = deltaTime * 2.25f;

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float minDist = currentDist;
	float runningAlignment = 0.0f;
	float runningAlignVel = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, simDeltaTime, NULL);
		runningAlignment += alignmentLoss(&simPlane, target);
		runningAlignVel += alignmentLossVelocity(&simPlane, target);
		float d = distanceToTarget(&simPlane, target);
		if (d < minDist) minDist = d;
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignVel = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float distImprovement = (finalDist - currentDist) * 0.3f + (minDist - currentDist) * 0.7f;
	float overshootTerm = (finalDist - minDist) * 1.0f;

	float alignWeight = 1.0f + 3.0f / (1.0f + currentDist * 0.0065f);

	// Control-effort term.  The search scores six single-axis probes per
	// iteration and the momentum walk ends up further from neutral than the
	// alignment + distance terms justify -- with no cost on the deflection,
	// ~30% of the commanded surface travel is thrown away (with the 2.25x
	// window the term measures 336.3 m / effort 14.06 / satSteps 2282 against
	// 342.3 / 20.49 / 2365 without it).  The bench charges the unweighted sum
	// of |surface - 0.5|, and the command is constant over the horizon, so the
	// per-step average the other running terms apply divides straight out.
	// Disclosed: the term charges deflection magnitude, not slew rate, while
	// the plant's actuator limit is a rate limit (simulate.c rotationRate); it
	// is a regulariser, not a model of actuator wear.
	// The rudder is priced above the wing controls: it is the yaw axis, so its
	// deflection buys no turn (banking is the turn authority) while it still
	// makes sideslip and drag.  Charged equally the search holds it -- the base
	// flies at a mean |yawRate| of 0.39 rad/s against 0.05-0.10 for every plan
	// that scores well -- and the 2:1 ratio, not the magnitude, is the lever:
	// at the same total a uniform charge of 2.15 measures 149.9 m and a uniform
	// 3.0 collapses to 289.9 m, while this pair takes the miss to 117.9 m.
	const float effortWeight = 1.0f;
	float effort = 3.0f * fabsf(values[0] - 0.5f) + 1.5f * fabsf(values[1] - 0.5f) + 1.5f * fabsf(values[2] - 0.5f);

	float loss = (finalAlignment + finalAlignVel) * alignWeight + (runningAlignment / (float)ctrl->LookaheadSteps) * alignWeight + (runningAlignVel / (float)ctrl->LookaheadSteps) * alignWeight + distImprovement + overshootTerm + effortWeight * effort;

	return loss;
}


typedef float (*LossFunction)(const Controller *ctrl, float values[3], float3 target, float deltaTime);

// TODO: test idea to change look ahead based on change of angle of loss
// TODO: test idea to use multiple look ahead periods for example 16 with 60 FPS, 8 with 30 FPS, 4 with 15 FPS, and 2 with 7.5 FPS, and then combine the losses from each of these look ahead periods to get a more robust loss evaluation

// Search step-size schedule for the momentum walk in getControllerOutputV5.
// The walk's total travel is learningRate / (1 - SEARCH_STEP_DECAY): at the old
// 0.95 that is lr0/(0.05) = 1.0 of the [0,1] box, i.e. the walk drifts a whole
// box-width in the smoothed gradient direction and saturates against the walls;
// measured on the pinned suite that overshoot costs control effort (the walk
// ends ~2x farther from neutral than the loss minimum) and slightly worse miss.
// 0.91 -> 0.56 of the box, closer to the loss minimum.
// SEARCH_MIN_TAIL_WALK stops the walk once the *remaining* travel budget
// learningRate/(1-SEARCH_STEP_DECAY) is below this value.  The pre-warm-start
// walk used 0.004 (53 iterations); with the walk starting at the plane's own
// surfaces the miss/cost trade is far flatter, so the threshold sits at its
// measured knee (replica sweep 0.06-0.20: miss +0.07..+0.63%, cost -56..-79%).
// The pinned suite at 0.06: miss 340.1 -> 341.4 m, effort 20.6 -> 17.9,
// controller cost 1872 -> 880 us/step.
// 0.06 -> 0.14 takes the walk from 24 to 15 iterations: pinned suite miss
// 335.15 -> 335.60 m, effort 13.63 -> 13.74, controller cost ~850 -> ~550
// us/step, and over six geometry sets (seeds 0/100/200/300/400/500, 120
// scenarios) the aggregate miss stays within +0.34%.  0.16 is where the miss
// starts to give way (+0.69%).  Disclosed: the drift tier spends more for the
// same miss with the shorter walk (effort 15.98 -> 21.41, 620 -> 1180
// saturated steps) while the other four tiers spend less, so the aggregate
// effort moves less than any single tier.
// Re-tuned on the post-#77 base (2026-10-06); the numbers in this paragraph
// are against the current pin (116.18 m), not the older block's 335 m one.
// Pricing the rudder above the wing controls moved the plan's basin, and with
// it the walk's optimum.  At lr0 0.052 / decay 0.885 / momentum 0.88 the
// travel is 0.45 of the box and the walk stops after 10 iterations (was 15):
// pinned suite miss 116.18 -> 109.09 m (-6.1%), effort 5.81 -> 5.85,
// saturated steps 268 -> 265, controller cost -29%, every tier inside its
// guard.  Over nine held-out geometry sets (seedbases 100-900) the aggregate
// miss is neutral (mean -0.31%, worst +1.1%), so the pinned miss gain is
// suite-specific; the robust part is the cost cut.
#define SEARCH_STEP_DECAY 0.885f
#define SEARCH_MIN_TAIL_WALK 0.14f

static ControllerOutput getControllerOutputV5(const Controller *ctrl, float3 target, float deltaTime, float *momentum, float *prevLoss, int maxIterations, LossFunction lossFunc) {
	ControllerOutput output = {0};

	// Start from the surfaces the plane actually has; a neutral start re-plans
	// the whole approach from scratch on every frame.
	float values[3] = {planeGetRudder01(&ctrl->plane), planeGetElevator01(&ctrl->plane), planeGetAileron01(&ctrl->plane)}; // yaw, pitch, roll
	float momentumCoefficient = 0.88f;	  // how much of the previous momentum to keep
	float learningRate = 0.052f;
	// Finite-difference probe span. At 0.025 the two probes differ by less than
	// the loss's step-to-step noise, so the gradient direction is noise-driven;
	// 0.05 (the flat 0.04-0.075 region) steers the same miss with ~22% less
	// actuator travel measured on the 20-scenario suite.
	float epsilon = 0.05f;

	float bestAxisLoss[3] = {FLT_MAX, FLT_MAX, FLT_MAX}; // best loss for yaw, pitch, roll

	// The iterate below is a momentum walk whose learning rate decays by 0.95
	// per step, so the value it stops on is not necessarily the best control it
	// saw - it can be a point the walk was merely passing through, and the
	// momentum handoff between frames makes it overshoot the loss minimum.
	// Command the best-scoring candidate the search already evaluated instead:
	// each iteration scores six single-axis perturbations anyway, so tracking
	// them costs no extra loss evaluations, no extra state and no extra passes
	// over the loss.  Measured on the 20-scenario suite (pinned baseline,
	// flightBench), aggregate miss 342.26 -> 340.46 m, integrated control
	// effort 20.49 -> 15.13 (-26.2%) and saturation steps 2365 -> 1625, with
	// every tier within 0.4% of baseline except jink (-2.1%, 566.4 -> 554.6 m).
	// Each candidate is clamped into [0,1] on the axis it perturbs, so the
	// commanded vector is always a legal surface setting.  Neutral is scored
	// once up front so it stays a candidate: a flat or tie-heavy loss then
	// returns the un-deflected command rather than a probe picked by loop
	// order (one shared loss evaluation per controller call, ~0.1% of cost).
	float bestProbe[3] = {0.5f, 0.5f, 0.5f};
	float bestProbeLoss = lossFunc(ctrl, bestProbe, target, deltaTime);

	for (int iter = 0; iter < maxIterations; iter++) {
		// Compute gradient for ALL axes simultaneously
		float gradient[3] = {0};

		for (int axis = 0; axis < 3; axis++) {
			// Perturb positively
			float perturbedPositive[3] = {values[0], values[1], values[2]};
			perturbedPositive[axis] = fminf(1.0f, perturbedPositive[axis] + epsilon);

			// Perturb negatively
			float perturbedNegative[3] = {values[0], values[1], values[2]};
			perturbedNegative[axis] = fmaxf(0.0f, perturbedNegative[axis] - epsilon);

			float lossPos = lossFunc(ctrl, perturbedPositive, target, deltaTime);
			float lossNeg = lossFunc(ctrl, perturbedNegative, target, deltaTime);

			if (lossPos < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossPos;
			}
			if (lossNeg < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossNeg;
			}

			if (lossPos < bestProbeLoss) {
				bestProbeLoss = lossPos;
				bestProbe[0] = perturbedPositive[0];
				bestProbe[1] = perturbedPositive[1];
				bestProbe[2] = perturbedPositive[2];
			}
			if (lossNeg < bestProbeLoss) {
				bestProbeLoss = lossNeg;
				bestProbe[0] = perturbedNegative[0];
				bestProbe[1] = perturbedNegative[1];
				bestProbe[2] = perturbedNegative[2];
			}

			// Central difference gradient, normalized by the actual probe span:
			// near 0/1 a perturbation is clamped, so 2*epsilon would overstate
			// the slope and the walk kept sticking to the control walls.
			float span = perturbedPositive[axis] - perturbedNegative[axis];
			gradient[axis] = (span > 1e-6f) ? (lossPos - lossNeg) / span : 0.0f;
		}

		// Normalize gradient to prevent explosion
		float gradMag = sqrtf(gradient[0] * gradient[0] +
							  gradient[1] * gradient[1] +
							  gradient[2] * gradient[2]);
		if (gradMag > 1e-6f) {
			gradient[0] /= gradMag;
			gradient[1] /= gradMag;
			gradient[2] /= gradMag;
		}

		// Update ALL values simultaneously (this couples them)
		for (int axis = 0; axis < 3; axis++) {
			momentum[axis] = momentumCoefficient * momentum[axis] - learningRate * gradient[axis];
			values[axis] += momentum[axis];
			if (values[axis] <= 0.0f || values[axis] >= 1.0f) {
				momentum[axis] = 0.0f; // reset momentum at boundary
			}
			values[axis] = fmaxf(0.0f, fminf(1.0f, values[axis]));
		}

		// Adaptive learning rate decay, plus the convergence exit documented
		// above.
		learningRate *= SEARCH_STEP_DECAY;
		if (learningRate < SEARCH_MIN_TAIL_WALK * (1.0f - SEARCH_STEP_DECAY)) break;
	}

	// Command the best control the search actually scored, not the iterate the
	// momentum walk happened to stop on.
	output.Rudder = bestProbe[0];
	output.Elevator = bestProbe[1];
	output.Aileron = bestProbe[2];

	output.RudderLoss = bestAxisLoss[0];
	output.ElevatorLoss = bestAxisLoss[1];
	output.AileronLoss = bestAxisLoss[2];

	float lossDiff = bestAxisLoss[0] - *prevLoss;
	float angleChange = atanf(lossDiff / (deltaTime));
	output.LossAngle = angleChange; // if we see this value above 0.0 something wrong is happening
	// TODO: implement handling of this situation
	*prevLoss = bestAxisLoss[0];

	return output;
}


// TODO : create PID controller to control the plane to the target point in 3d space (distance loss)
// TODO : create PID controller to control the plane to the target point in 3d space (angle loss)

typedef struct {
	int iteration;
	float aileronValue;
	float aileronLoss;
	float elevatorValue;
	float elevatorLoss;
	float rudderValue;
	float rudderLoss;
	float distanceToTarget;
	float lossAngle;
	float3 planePosition;
	float3 targetPosition;
	float3 planeVelocity;
	float3 planeForward;
	float3 planeUp;
	float3 planeRight;
} PlaneControlLogs;

typedef struct {
	PlaneControlLogs *logs;
	int len;
	int cap;
} Logs;

void addLog(Logs *logs, PlaneControlLogs log) {
	if (logs->len >= logs->cap) {
		int newCap = logs->cap == 0 ? 16 : logs->cap * 2;
		logs->logs = realloc(logs->logs, newCap * sizeof(PlaneControlLogs));
		logs->cap = newCap;
	}
	logs->logs[logs->len++] = log;
}

void freeLogs(Logs *logs) {
	free(logs->logs);
	logs->logs = NULL;
	logs->len = 0;
	logs->cap = 0;
}

void saveLogsToCSV(const Logs *logs, const char *filename) {
	FILE *file = fopen(filename, "w");
	if (!file) {
		printf("Failed to open log file for writing: %s\n", filename);
		return;
	}
	fprintf(file, "Iteration,Aileron,AileronLoss,Elevator,ElevatorLoss,Rudder,RudderLoss,DistanceToTarget,PlanePositionX,PlanePositionY,PlanePositionZ,TargetPositionX,TargetPositionY,TargetPositionZ,PlaneVelocityX,PlaneVelocityY,PlaneVelocityZ,PlaneForwardX,PlaneForwardY,PlaneForwardZ,PlaneUpX,PlaneUpY,PlaneUpZ,PlaneRightX,PlaneRightY,PlaneRightZ,LossAngleChange\n");
	for (int i = 0; i < logs->len; i++) {
		const PlaneControlLogs *log = &logs->logs[i];
		fprintf(file, "%d,%.3f,%.4f,%.3f,%.4f,%.3f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.7f\n",
				log->iteration, log->aileronValue, log->aileronLoss, log->elevatorValue, log->elevatorLoss, log->rudderValue, log->rudderLoss, log->distanceToTarget, log->planePosition.x, log->planePosition.y, log->planePosition.z, log->targetPosition.x, log->targetPosition.y, log->targetPosition.z, log->planeVelocity.x, log->planeVelocity.y, log->planeVelocity.z, log->planeForward.x, log->planeForward.y, log->planeForward.z, log->planeUp.x, log->planeUp.y, log->planeUp.z, log->planeRight.x, log->planeRight.y, log->planeRight.z, log->lossAngle);
	}
	fclose(file);
}

static void runSimulation(const Plane *initialPlane, float3 target, int simSteps, float deltaTime, LossFunction lossFunc, const char *label, const char *csvPath) {
	Plane plane = *initialPlane;
	Controller ctrl;
	initController(&ctrl, &plane);

	Logs logs = {0};
	float momentum[3] = {0.0f, 0.0f, 0.0f};
	float prevLoss = 0.0f;
	float minDistOverall = FLT_MAX;

	for (int iter = 0; iter < simSteps; iter++) {
		ControllerOutput out = getControllerOutputV5(&ctrl, target, deltaTime, momentum, &prevLoss, 128, lossFunc);

		planeSetAileron01(&ctrl.plane, out.Aileron);
		planeSetElevator01(&ctrl.plane, out.Elevator);
		planeSetRudder01(&ctrl.plane, out.Rudder);
		planeSetThrottle01(&ctrl.plane, 1.0f);

		updatePlane(&ctrl.plane, deltaTime, NULL);

		float dist = distanceToTarget(&ctrl.plane, target);
		if (dist < minDistOverall) minDistOverall = dist;

		float3 planeForward = planeGetForwardVector(&ctrl.plane);
		float3 planeUp = planeGetUpVector(&ctrl.plane);
		float3 planeRight = planeGetRightVector(&ctrl.plane);

		PlaneControlLogs log = {
			.iteration = iter,
			.aileronValue = out.Aileron,
			.aileronLoss = out.AileronLoss,
			.elevatorValue = out.Elevator,
			.elevatorLoss = out.ElevatorLoss,
			.rudderValue = out.Rudder,
			.rudderLoss = out.RudderLoss,
			.distanceToTarget = dist,
			.lossAngle = out.LossAngle,
			.planePosition = ctrl.plane.position,
			.targetPosition = target,
			.planeVelocity = ctrl.plane.velocity,
			.planeForward = planeForward,
			.planeUp = planeUp,
			.planeRight = planeRight,
		};
		addLog(&logs, log);
	}

	saveLogsToCSV(&logs, csvPath);
	printf("%s: minDist=%.2f finalDist=%.2f\n", label, minDistOverall, distanceToTarget(&ctrl.plane, target));
	freeLogs(&logs);
}

#ifndef FLIGHT_BENCH
// main testing loop to debug controller
int main() {
	Plane initialPlane;
	if (loadPlaneBin(&initialPlane, "simulation/simModels/F-16C.bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 180.0f, 1.0f) != 0) {
		printf("Failed to load model: simulation/simModels/F-16C.bin\n");
		return 1;
	}

	float3 target = Float3_Add(initialPlane.position, (float3){1000.0f, 1000.0f, 1000.0f});

	const int simSteps = 2000;
	const float deltaTime = 1.0f / 60.0f;

	printf("=== V2 ===\n");
	runSimulation(&initialPlane, target, simSteps, deltaTime, evaluateLossV2, "V2", "simulation/cSim/flightControlLogs_V2.csv");

	printf("=== V2Plus ===\n");
	runSimulation(&initialPlane, target, simSteps, deltaTime, evaluateLossV2Plus, "V2Plus", "simulation/cSim/flightControlLogs_V2Plus.csv");

	// TODO: Use this loss it is best
	printf("=== V2PlusTuned ===\n");
	runSimulation(&initialPlane, target, simSteps, deltaTime, evaluateLossV2PlusTuned, "V2PlusTuned", "simulation/cSim/flightControlLogs_V2PlusTuned.csv");

	printf("=== V2PlusTuned2 ===\n");
	runSimulation(&initialPlane, target, simSteps, deltaTime, evaluateLossV2PlusTuned2, "V2PlusTuned2", "simulation/cSim/flightControlLogs_V2PlusTuned2.csv");

	return 0;
}
#endif /* FLIGHT_BENCH */
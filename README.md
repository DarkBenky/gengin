## TODO

- [ ] Plane controls
  - [ ] Use something like this but we will simplified it
    - [ ] Example

      ```c
      struct Plane {
          // Core simulation
          float position[3];                    // World coordinates (x, y, z) in meters
          float velocity[3];                    // Velocity vector (m/s)
          float acceleration[3];                // Current acceleration vector (m/s²)

          // Orientation & rotation
          float bodyOrientation[3];             // Aircraft's forward direction (unit vector)
          float upVector[3];                    // Aircraft's up direction (unit vector)
          float rightVector[3];                 // Aircraft's right direction (unit vector)
          float angularVelocity[3];             // Roll, pitch, yaw rates (rad/s)
          float eulerAngles[3];                 // Roll, pitch, yaw angles (radians)

          // Aerodynamic state
          float angleOfAttack;                  // Angle between body and velocity vector (radians)
          float sideslipAngle;                  // Side-slip angle for crosswind effects (radians)
          float bankAngle;                      // Roll angle from level flight (radians)

          // Mass properties
          float emptyMass;                      // Aircraft empty weight without fuel/payload (kg)
          float fuelMass;                       // Remaining fuel mass (kg)
          float payloadMass;                    // Weapons, sensors, cargo mass (kg)
          float totalMass;                      // Current total mass (empty + fuel + payload) (kg)
          float momentOfInertia[3];             // Resistance to rotation [roll, pitch, yaw] (kg·m²)
          float centerOfGravity[3];             // CG position relative to reference point (meters)

          // Propulsion
          float thrust;                         // Current thrust output (Newtons)
          float maxThrust;                      // Maximum available thrust at current altitude (N)
          float militaryThrust;                 // Non-afterburner thrust limit (N)
          float afterburnerThrust;              // Maximum thrust with afterburner (N)
          float throttle;                       // Throttle position (0-1, >1 for afterburner)
          bool afterburnerActive;               // Afterburner engagement state
          float fuelConsumption;                // Current fuel burn rate (kg/s)
          float fuelCapacity;                   // Maximum fuel capacity (kg)
          float specificFuelConsumption;        // Fuel efficiency (kg/N/s)
          float afterburnerSFC;                 // Fuel consumption with afterburner (kg/N/s)
          float engineTemp;                     // Engine exhaust temperature (Kelvin)
          float engineSignature;                // IR signature intensity for detection

          // Aerodynamic coefficients
          float zeroLiftDrag;                   // Base drag coefficient at zero lift (Cd0)
          float liftSlope;                      // Lift curve slope (per radian)
          float maxLiftCoeff;                   // Maximum achievable lift coefficient (Cl_max)
          float stallAoA;                       // Angle of attack where stall occurs (radians)
          float dragSlope;                      // Induced drag factor
          float crossSectionArea;               // Frontal area for drag calculations (m²)
          float wingArea;                       // Wing reference area for lift (m²)
          float wingspan;                       // Wing span (meters)
          float aspectRatio;                    // Wing aspect ratio (span²/area)
          float oswaldEfficiency;               // Wing efficiency factor (0.7-0.95)

          // Control surfaces
          float aileronDeflection[2];           // Left/Right aileron angles (radians)
          float elevatorDeflection[2];          // Left/Right elevator angles (radians)
          float rudderDeflection;               // Rudder angle (radians)
          float flapDeflection;                 // Flap extension angle (radians)
          float leadingEdgeFlaps;               // Leading edge flap angle (radians)
          float airbrakeDeflection;             // Speed brake extension (0-1)

          float aileronMaxDeflection;           // Maximum aileron angle (radians)
          float elevatorMaxDeflection;          // Maximum elevator angle (radians)
          float rudderMaxDeflection;            // Maximum rudder angle (radians)
          float flapMaxDeflection;              // Maximum flap angle (radians)

          float controlSurfaceRate[PLANE_CONTROL_SURFACES];   // Actuator speeds (rad/s)
          float controlEffectiveness[PLANE_CONTROL_SURFACES]; // Control power multipliers (0-1)

          // Flight control system
          float rollRate;                       // Current roll rate (rad/s)
          float pitchRate;                      // Current pitch rate (rad/s)
          float yawRate;                        // Current yaw rate (rad/s)
          float maxRollRate;                    // Maximum roll rate capability (rad/s)
          float maxPitchRate;                   // Maximum pitch rate capability (rad/s)
          float maxYawRate;                     // Maximum yaw rate capability (rad/s)

          float rollDamping;                    // Natural roll damping coefficient
          float pitchDamping;                   // Natural pitch damping coefficient
          float yawDamping;                     // Natural yaw damping coefficient

          // Performance limits
          float maxGPull;                       // Maximum sustained g-force capability
          float instantGLimit;                  // Structural g-load limit before damage
          float currentGForce;                  // Current g-force being experienced
          float maxDynamicPressure;             // Structural limit for dynamic pressure (Pa)
          float maxAoA;                         // Maximum safe angle of attack (radians)
          float maxSpeed;                       // Maximum velocity limit (m/s)
          float cornerVelocity;                 // Speed for maximum turn rate (m/s)
          float stallSpeed;                     // Minimum flying speed (m/s)

          // Flight envelope
          float serviceCeiling;                 // Maximum operational altitude (meters)
          float maxMach;                        // Maximum Mach number
          float optimalCruiseSpeed;             // Most efficient cruise speed (m/s)
          float optimalCruiseAltitude;          // Most efficient cruise altitude (meters)

          // Autopilot & guidance
          bool autopilotEnabled;                // Autopilot master switch
          float targetAltitude;                 // Desired altitude (meters)
          float targetSpeed;                    // Desired airspeed (m/s)
          float targetHeading;                  // Desired heading (radians)
          float targetPosition[3];              // Waypoint coordinates (meters)

          float pidRollGains[3];                // PID gains for roll control [P, I, D]
          float pidPitchGains[3];               // PID gains for pitch control [P, I, D]
          float pidYawGains[3];                 // PID gains for yaw control [P, I, D]
          float pidAltitudeGains[3];            // PID gains for altitude hold [P, I, D]
          float pidSpeedGains[3];               // PID gains for speed control [P, I, D]

          // Sensors & avionics
          struct IRSearchAndTrack irst;         // Infrared Search and Track system
          struct Camera cockpitCamera;          // Pilot's view camera
          float radarRange;                     // Radar detection range (meters)
          float radarFOV;                       // Radar field of view (radians)
          bool radarActive;                     // Radar emission state

          // Weapons & countermeasures
          struct Missile *loadedMissiles[MAX_PLANE_MISSILES]; // Missile hardpoints
          int missileCount;                     // Number of loaded missiles
          bool missileLaunched[MAX_PLANE_MISSILES];           // Track which missiles have been fired

          float chaffCount;                     // Remaining chaff cartridges
          float flareCount;                     // Remaining flare cartridges
          float heatAspect[6];                  // Heat radiation from each face of the missile

          // Visual effects
          struct FireSOA *engineExhaust;        // Engine plume particles
          struct Triangles *planeModel;         // 3D model geometry

          // Cached values (updated each frame)
          float machNumber;                     // Current Mach number
          float dynamicPressure;                // Current dynamic pressure 0.5*rho*v^2 (Pa)
          float indicatedAirspeed;              // Airspeed indicator reading (m/s)
          float trueAirspeed;                   // Actual airspeed (m/s)
          float groundSpeed;                    // Speed over ground (m/s)
          float altitude;                       // Current altitude above sea level (meters)
          float verticalSpeed;                  // Rate of climb/descent (m/s)
          float turnRadius;                     // Current turn radius (meters)
          float turnRate;                       // Current turn rate (rad/s)
      };
      ```

    - [ ] Flight model should be physics-based only — derived values like turn rate should not be hardcoded constants
    - [ ] Control by providing a target nose vector (like War Thunder)
      - [ ] Add damping to controls to avoid oscillations

- [ ] Radar / heat seeker simulation for missiles
  - [ ] Simulate radar scanning by sampling object ID buffer over a small cone area and computing RCS on hits
    - [ ] Non-Doppler radar: average terrain clutter hits with target hits to simulate ground return noise
    - [ ] Doppler radar: filter out stationary objects (terrain), track only moving targets (missiles, planes); requires relative velocity per object

- [ ] Missile guidance and control

- [ ] Server integration for multiplayer

- [ ] GPU rendering (keep it simple — port current CPU pipeline)

- [X] Add screen space reflection

  Example:

  ```cl
  float3 sampleScreenSpaceReflectionFiltered(
      __global const float *ScreenColors,
      __global const float *ScreenDistances,
      const float3 rayOrigin,
      const float3 rayDirection,
      const float3 camPos,
      const float3 camDir,
      const float fov,
      const int screenWidth,
      const int screenHeight,
      const float maxDistance,
      const int maxSteps,
      const float stepSize) {
      float3 fallbackColor = (float3)(0.0f, 0.0f, 0.0f);

      float3 forward = normalize(camDir);
      float3 camUp = (float3)(0.0f, 1.0f, 0.0f);
      float3 right = normalize(cross(forward, camUp));
      float3 up = cross(right, forward);

      float3 currentPos = rayOrigin;
      float distanceTraveled = 0.0f;

      currentPos += rayDirection * stepSize * 0.5f;

      for (int step = 0; step < maxSteps; step++) {
          currentPos += rayDirection * stepSize;
          distanceTraveled += stepSize;

          if (distanceTraveled > maxDistance) {
              break;
          }

          float3 relativePos = currentPos - camPos;
          float depth = dot(relativePos, forward);

          if (depth <= 0.01f || depth > maxDistance) {
              continue;
          }

          float fovScale = 1.0f / (depth * fov);
          float screenRight = dot(relativePos, right) * fovScale;
          float screenUpward = dot(relativePos, up) * fovScale;

          float halfWidth = screenWidth * 0.5f;
          float halfHeight = screenHeight * 0.5f;

          float screenX = screenRight * halfWidth + halfWidth;
          float screenY = -screenUpward * halfHeight + halfHeight;

          if (screenX < 1.0f || screenX >= (screenWidth - 1.0f) ||
              screenY < 1.0f || screenY >= (screenHeight - 1.0f)) {
              continue;
          }

          int pixelX = (int)screenX;
          int pixelY = (int)screenY;
          int pixelIndex = pixelY * screenWidth + pixelX;

          if (pixelIndex < 0 || pixelIndex >= screenWidth * screenHeight) {
              continue;
          }

          float sceneDepth = ScreenDistances[pixelIndex];

          float depthThreshold = stepSize * 1.5f + depth * 0.001f;
          float depthDifference = depth - sceneDepth;

          if (sceneDepth > 0.01f && depthDifference > 0.0f && depthDifference < depthThreshold) {
              float fx = screenX - pixelX;
              float fy = screenY - pixelY;

              int x0 = clamp(pixelX, 0, screenWidth - 1);
              int x1 = clamp(pixelX + 1, 0, screenWidth - 1);
              int y0 = clamp(pixelY, 0, screenHeight - 1);
              int y1 = clamp(pixelY + 1, 0, screenHeight - 1);

              int idx00 = (y0 * screenWidth + x0) * 3;
              int idx10 = (y0 * screenWidth + x1) * 3;
              int idx01 = (y1 * screenWidth + x0) * 3;
              int idx11 = (y1 * screenWidth + x1) * 3;

              if (idx00 >= 0 && idx11 < screenWidth * screenHeight * 3) {
                  float3 color00 = (float3)(ScreenColors[idx00], ScreenColors[idx00 + 1], ScreenColors[idx00 + 2]);
                  float3 color10 = (float3)(ScreenColors[idx10], ScreenColors[idx10 + 1], ScreenColors[idx10 + 2]);
                  float3 color01 = (float3)(ScreenColors[idx01], ScreenColors[idx01 + 1], ScreenColors[idx01 + 2]);
                  float3 color11 = (float3)(ScreenColors[idx11], ScreenColors[idx11 + 1], ScreenColors[idx11 + 2]);

                  float3 colorTop = mix(color00, color10, fx);
                  float3 colorBottom = mix(color01, color11, fx);
                  float3 finalColor = mix(colorTop, colorBottom, fy);

                  if (length(finalColor) > 0.01f) {
                      return finalColor;
                  }
              }
          }
      }

      return fallbackColor;
  }
  ```

- [x] Test if using multiple rows per ray trace task improves performance (e.g. 8 rows per task)
  - Tested: it is better to use one task per row when there is a lot of work (**more work == fewer rows per task**, **less work == more rows per task**)

  ```
  Scene loaded. Total triangles: 136292
  ========================================
  Single-threaded Ray Performance:
  Average Time: 0.026903 seconds
  Median Time:  0.027284 seconds
  Min Time:     0.025808 seconds
  Max Time:     0.027431 seconds
  Variance:     0.000000
  99th Pct:     0.027431 seconds
  ========================================
  Multi-threaded (1 rows/task, 600 tasks):
  Average Time: 0.005078 seconds
  Median Time:  0.004794 seconds
  Min Time:     0.004677 seconds
  Max Time:     0.007933 seconds
  Variance:     0.000001
  99th Pct:     0.007933 seconds
  ========================================
  Multi-threaded (2 rows/task, 300 tasks):
  Average Time: 0.005364 seconds
  Median Time:  0.005025 seconds
  Min Time:     0.004868 seconds
  Max Time:     0.009057 seconds
  Variance:     0.000001
  99th Pct:     0.009057 seconds
  ========================================
  Multi-threaded (4 rows/task, 150 tasks):
  Average Time: 0.005452 seconds
  Median Time:  0.005324 seconds
  Min Time:     0.004900 seconds
  Max Time:     0.008086 seconds
  Variance:     0.000000
  99th Pct:     0.008086 seconds
  ========================================
  Multi-threaded (16 rows/task, 38 tasks):
  Average Time: 0.006851 seconds
  Median Time:  0.006668 seconds
  Min Time:     0.006153 seconds
  Max Time:     0.009899 seconds
  Variance:     0.000001
  99th Pct:     0.009899 seconds
  ========================================
  Multi-threaded (32 rows/task, 19 tasks):
  Average Time: 0.007200 seconds
  Median Time:  0.007292 seconds
  Min Time:     0.006357 seconds
  Max Time:     0.008172 seconds
  Variance:     0.000000
  99th Pct:     0.008172 seconds
  ========================================
  Multi-threaded (64 rows/task, 10 tasks):
  Average Time: 0.010450 seconds
  Median Time:  0.010735 seconds
  Min Time:     0.008400 seconds
  Max Time:     0.012504 seconds
  Variance:     0.000002
  99th Pct:     0.012504 seconds
  ========================================
  ========================================
  Multi-threaded (1 rows/task, 600 tasks):
  Average Time: 0.005038 seconds
  Median Time:  0.004943 seconds
  Min Time:     0.004715 seconds
  Max Time:     0.006126 seconds
  Variance:     0.000000
  99th Pct:     0.006126 seconds
  Correctness check passed.
  ```

## Current Render

![img](./img.png)

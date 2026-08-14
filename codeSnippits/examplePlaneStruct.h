
struct Plane {
	// Core simulation
	float position[3];	   // World coordinates (x, y, z) in meters
	float velocity[3];	   // Velocity vector (m/s)
	float acceleration[3]; // Current acceleration vector (m/s²)

	// Orientation & rotation
	float bodyOrientation[3]; // Aircraft's forward direction (unit vector)
	float upVector[3];		  // Aircraft's up direction (unit vector)
	float rightVector[3];	  // Aircraft's right direction (unit vector)
	float angularVelocity[3]; // Roll, pitch, yaw rates (rad/s)
	float eulerAngles[3];	  // Roll, pitch, yaw angles (radians)

	// Aerodynamic state
	float angleOfAttack; // Angle between body and velocity vector (radians)
	float sideslipAngle; // Side-slip angle for crosswind effects (radians)
	float bankAngle;	 // Roll angle from level flight (radians)

	// Mass properties
	float emptyMass;		  // Aircraft empty weight without fuel/payload (kg)
	float fuelMass;			  // Remaining fuel mass (kg)
	float payloadMass;		  // Weapons, sensors, cargo mass (kg)
	float totalMass;		  // Current total mass (empty + fuel + payload) (kg)
	float momentOfInertia[3]; // Resistance to rotation [roll, pitch, yaw] (kg·m²)
	float centerOfGravity[3]; // CG position relative to reference point (meters)

	// Propulsion
	float thrust;				   // Current thrust output (Newtons)
	float maxThrust;			   // Maximum available thrust at current altitude (N)
	float militaryThrust;		   // Non-afterburner thrust limit (N)
	float afterburnerThrust;	   // Maximum thrust with afterburner (N)
	float throttle;				   // Throttle position (0-1, >1 for afterburner)
	bool afterburnerActive;		   // Afterburner engagement state
	float fuelConsumption;		   // Current fuel burn rate (kg/s)
	float fuelCapacity;			   // Maximum fuel capacity (kg)
	float specificFuelConsumption; // Fuel efficiency (kg/N/s)
	float afterburnerSFC;		   // Fuel consumption with afterburner (kg/N/s)
	float engineTemp;			   // Engine exhaust temperature (Kelvin)
	float engineSignature;		   // IR signature intensity for detection

	// Aerodynamic coefficients
	float zeroLiftDrag;		// Base drag coefficient at zero lift (Cd0)
	float liftSlope;		// Lift curve slope (per radian)
	float maxLiftCoeff;		// Maximum achievable lift coefficient (Cl_max)
	float stallAoA;			// Angle of attack where stall occurs (radians)
	float dragSlope;		// Induced drag factor
	float crossSectionArea; // Frontal area for drag calculations (m²)
	float wingArea;			// Wing reference area for lift (m²)
	float wingspan;			// Wing span (meters)
	float aspectRatio;		// Wing aspect ratio (span²/area)
	float oswaldEfficiency; // Wing efficiency factor (0.7-0.95)

	// Control surfaces
	float aileronDeflection[2];	 // Left/Right aileron angles (radians)
	float elevatorDeflection[2]; // Left/Right elevator angles (radians)
	float rudderDeflection;		 // Rudder angle (radians)
	float flapDeflection;		 // Flap extension angle (radians)
	float leadingEdgeFlaps;		 // Leading edge flap angle (radians)
	float airbrakeDeflection;	 // Speed brake extension (0-1)

	float aileronMaxDeflection;	 // Maximum aileron angle (radians)
	float elevatorMaxDeflection; // Maximum elevator angle (radians)
	float rudderMaxDeflection;	 // Maximum rudder angle (radians)
	float flapMaxDeflection;	 // Maximum flap angle (radians)

	float controlSurfaceRate[PLANE_CONTROL_SURFACES];	// Actuator speeds (rad/s)
	float controlEffectiveness[PLANE_CONTROL_SURFACES]; // Control power multipliers (0-1)

	// Flight control system
	float rollRate;		// Current roll rate (rad/s)
	float pitchRate;	// Current pitch rate (rad/s)
	float yawRate;		// Current yaw rate (rad/s)
	float maxRollRate;	// Maximum roll rate capability (rad/s)
	float maxPitchRate; // Maximum pitch rate capability (rad/s)
	float maxYawRate;	// Maximum yaw rate capability (rad/s)

	float rollDamping;	// Natural roll damping coefficient
	float pitchDamping; // Natural pitch damping coefficient
	float yawDamping;	// Natural yaw damping coefficient

	// Performance limits
	float maxGPull;			  // Maximum sustained g-force capability
	float instantGLimit;	  // Structural g-load limit before damage
	float currentGForce;	  // Current g-force being experienced
	float maxDynamicPressure; // Structural limit for dynamic pressure (Pa)
	float maxAoA;			  // Maximum safe angle of attack (radians)
	float maxSpeed;			  // Maximum velocity limit (m/s)
	float cornerVelocity;	  // Speed for maximum turn rate (m/s)
	float stallSpeed;		  // Minimum flying speed (m/s)

	// Flight envelope
	float serviceCeiling;		 // Maximum operational altitude (meters)
	float maxMach;				 // Maximum Mach number
	float optimalCruiseSpeed;	 // Most efficient cruise speed (m/s)
	float optimalCruiseAltitude; // Most efficient cruise altitude (meters)

	// Autopilot & guidance
	bool autopilotEnabled;	 // Autopilot master switch
	float targetAltitude;	 // Desired altitude (meters)
	float targetSpeed;		 // Desired airspeed (m/s)
	float targetHeading;	 // Desired heading (radians)
	float targetPosition[3]; // Waypoint coordinates (meters)

	float pidRollGains[3];	   // PID gains for roll control [P, I, D]
	float pidPitchGains[3];	   // PID gains for pitch control [P, I, D]
	float pidYawGains[3];	   // PID gains for yaw control [P, I, D]
	float pidAltitudeGains[3]; // PID gains for altitude hold [P, I, D]
	float pidSpeedGains[3];	   // PID gains for speed control [P, I, D]

	// Sensors & avionics
	struct IRSearchAndTrack irst; // Infrared Search and Track system
	struct Camera cockpitCamera;  // Pilot's view camera
	float radarRange;			  // Radar detection range (meters)
	float radarFOV;				  // Radar field of view (radians)
	bool radarActive;			  // Radar emission state

	// Weapons & countermeasures
	struct Missile *loadedMissiles[MAX_PLANE_MISSILES]; // Missile hardpoints
	int missileCount;									// Number of loaded missiles
	bool missileLaunched[MAX_PLANE_MISSILES];			// Track which missiles have been fired

	float chaffCount;	 // Remaining chaff cartridges
	float flareCount;	 // Remaining flare cartridges
	float heatAspect[6]; // Heat radiation from each face of the missile

	// Visual effects
	struct FireSOA *engineExhaust; // Engine plume particles
	struct Triangles *planeModel;  // 3D model geometry

	// Cached values (updated each frame)
	float machNumber;		 // Current Mach number
	float dynamicPressure;	 // Current dynamic pressure 0.5*rho*v^2 (Pa)
	float indicatedAirspeed; // Airspeed indicator reading (m/s)
	float trueAirspeed;		 // Actual airspeed (m/s)
	float groundSpeed;		 // Speed over ground (m/s)
	float altitude;			 // Current altitude above sea level (meters)
	float verticalSpeed;	 // Rate of climb/descent (m/s)
	float turnRadius;		 // Current turn radius (meters)
	float turnRate;			 // Current turn rate (rad/s)
};

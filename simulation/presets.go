package simulation

// NewF16 returns a Plane approximating the F-16C Block 50.
//
// Key sources:
//
//	Wing area  : 28 m^2   (USAF fact sheet)
//	Wingspan   : 9.96 m   -> half-span ~= 4.98 m
//	Airfoil    : NACA 64A-204 (near-symmetric, design CL = 0.2)
//	Aspect ratio: effective ~= 3.2 with blended strake lift
//	H-tail     : all-moving stabilators, total ~= 5.5 m^2
//	V-tail     : single fin, ~= 4 m^2
//	Stall (cropped delta + strake): ~= 28 deg
//
// Coordinate convention: +X forward, +Y up, +Z right.
// All positions are relative to the centre of mass.
func NewF16() Plane {
	const (
		tailX    float32 = -6.5
		wingSpan float32 = 3.2
		tailSpan float32 = 1.4
	)

	// Cropped delta: high stall angle, moderate CL.
	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.2, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    3.0,  // trim incidence for level flight at 220 m/s, 5000 m
			surfaceArea:      14.0, // half of 28 m^2 total
			liftCoefficient:  1.1,
			dragCoefficient:  0.025,
			aspectRatio:      3.2,
			efficiency:       0.82,
			stallAngle:       28.0,
			camber:           0.02, // NACA 64A-204 design CL ~= 0.2
			active:           false,
		}
	}

	// Flaperons: trailing-edge roll & high-lift combined.
	flaperon := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.8, 0, side * (wingSpan + 1.2)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 35.0,
			minRotationAngle: -35.0,
			rotationRate:     80.0,
			surfaceArea:      2.5,
			liftCoefficient:  1.0,
			dragCoefficient:  0.030,
			aspectRatio:      2.0,
			efficiency:       0.75,
			stallAngle:       25.0,
			camber:           0.01,
			active:           true,
		}
	}

	// Leading-edge flaps: extend for high AoA.
	lef := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.8, 0, side * wingSpan * 0.6},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: 0.0,
			rotationRate:     30.0,
			surfaceArea:      3.0,
			liftCoefficient:  1.5,
			dragCoefficient:  0.040,
			aspectRatio:      1.5,
			efficiency:       0.80,
			stallAngle:       20.0,
			camber:           0.04,
			active:           true,
		}
	}

	// All-moving stabilators (split left/right for pitch and roll).
	stabilator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     100.0,
			surfaceArea:      2.75, // half of ~5.5 m^2 total
			liftCoefficient:  0.75,
			dragCoefficient:  0.020,
			aspectRatio:      1.8,
			efficiency:       0.80,
			stallAngle:       30.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'F', '-', '1', '6', 'C'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  flaperon(-1),
		rightAileron: flaperon(1),

		leftFlap:  lef(-1),
		rightFlap: lef(1),

		// No fixed horizontal surface; stabilators do the full job.
		horizontalStabilizer: Surface{active: false},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 0.8, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      4.0,
			liftCoefficient:  0.65,
			dragCoefficient:  0.020,
			aspectRatio:      1.5,
			efficiency:       0.75,
			stallAngle:       25.0,
			active:           false,
		},

		leftElevator:  stabilator(-1),
		rightElevator: stabilator(1),

		rudder: Surface{
			relativePosition: Float3{tailX, 1.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     80.0,
			surfaceArea:      3.8,
			liftCoefficient:  0.60,
			dragCoefficient:  0.025,
			aspectRatio:      0.9,
			efficiency:       0.72,
			stallAngle:       35.0,
			active:           true,
		},

		// F110-GE-129 afterburner: 131.6 kN; empty 8,573 kg; internal fuel 3,175 kg
		maxTrust:               131600.0,
		baseMass:               8573.0,
		fuelMass:               3175.0,
		currentFuelPercentage:  1.0,
		burnRate:               2.5,
		burnWithoutAfterburner: 1.2,
	}
}

// NewSu27 returns a Plane approximating the Su-27SK.
//
// Key sources:
//
//	Wing area  : 62 m^2
//	Wingspan   : 14.7 m  -> half-span ~= 7.35 m
//	Aspect ratio: effective ~= 3.5 (ogival-delta with LERX)
//	H-tail     : all-moving stabilators, total ~= 15 m^2
//	V-tail     : twin fins, each ~= 5.5 m^2
//	Stall      : ~= 35 deg (Cobra at 120 deg AoA demonstrated)
func NewSu27() Plane {
	const (
		tailX    float32 = -8.5
		wingSpan float32 = 5.5
		tailSpan float32 = 2.2
		finY     float32 = 1.0
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.0, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      31.0, // half of 62 m^2
			liftCoefficient:  1.15,
			dragCoefficient:  0.022,
			aspectRatio:      3.5,
			efficiency:       0.83,
			stallAngle:       35.0,
			camber:           0.01,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-1.0, 0, side * (wingSpan + 1.5)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      4.0,
			liftCoefficient:  1.0,
			dragCoefficient:  0.025,
			aspectRatio:      2.0,
			efficiency:       0.80,
			stallAngle:       30.0,
			camber:           0.01,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 45.0,
			minRotationAngle: 0.0,
			rotationRate:     30.0,
			surfaceArea:      7.0,
			liftCoefficient:  1.8,
			dragCoefficient:  0.050,
			aspectRatio:      2.5,
			efficiency:       0.85,
			stallAngle:       18.0,
			camber:           0.06,
			active:           true,
		}
	}

	// All-moving horizontal stabilators (very large on the Su-27).
	stabilator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 35.0,
			minRotationAngle: -35.0,
			rotationRate:     90.0,
			surfaceArea:      7.5, // half of ~15 m^2 total
			liftCoefficient:  0.80,
			dragCoefficient:  0.018,
			aspectRatio:      2.2,
			efficiency:       0.85,
			stallAngle:       35.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'S', 'u', '-', '2', '7'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{active: false},

		// Twin vertical fins combined in this single structural slot.
		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, finY, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      11.0, // ~5.5 m^2 each fin x 2
			liftCoefficient:  0.70,
			dragCoefficient:  0.018,
			aspectRatio:      1.6,
			efficiency:       0.78,
			stallAngle:       20.0,
			active:           false,
		},

		leftElevator:  stabilator(-1),
		rightElevator: stabilator(1),

		// Both rudder panels represented as a single combined surface.
		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, finY + 2.0, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      3.5,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.0,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		// 2x AL-31F afterburner: 245.2 kN total; empty 16,380 kg; internal fuel 9,400 kg
		maxTrust:               245200.0,
		baseMass:               16380.0,
		fuelMass:               9400.0,
		currentFuelPercentage:  1.0,
		burnRate:               6.5,
		burnWithoutAfterburner: 2.8,
	}
}

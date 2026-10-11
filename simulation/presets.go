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

// NewMiG29: light twin-engine 4th gen with LERX and RD-33s.
func NewMiG29() Plane {
	const (
		tailX    float32 = -7.0
		wingSpan float32 = 4.2
		tailSpan float32 = 2.0
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.0, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      19.0,
			liftCoefficient:  1.15,
			dragCoefficient:  0.022,
			aspectRatio:      3.4,
			efficiency:       0.83,
			stallAngle:       32.0,
			camber:           0.01,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-1.0, 0, side * (wingSpan + 1.2)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     80.0,
			surfaceArea:      3.0,
			liftCoefficient:  1.0,
			dragCoefficient:  0.025,
			aspectRatio:      2.0,
			efficiency:       0.80,
			stallAngle:       28.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 40.0,
			minRotationAngle: 0.0,
			rotationRate:     30.0,
			surfaceArea:      5.0,
			liftCoefficient:  1.7,
			dragCoefficient:  0.045,
			aspectRatio:      2.5,
			efficiency:       0.85,
			stallAngle:       18.0,
			camber:           0.05,
			active:           true,
		}
	}

	stabilator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 32.0,
			minRotationAngle: -32.0,
			rotationRate:     90.0,
			surfaceArea:      4.5,
			liftCoefficient:  0.8,
			dragCoefficient:  0.018,
			aspectRatio:      2.0,
			efficiency:       0.84,
			stallAngle:       32.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'M', 'i', 'G', '-', '2', '9'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{active: false},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.2, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      8.0,
			liftCoefficient:  0.7,
			dragCoefficient:  0.018,
			aspectRatio:      1.6,
			efficiency:       0.78,
			stallAngle:       22.0,
			active:           false,
		},

		leftElevator:  stabilator(-1),
		rightElevator: stabilator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, 2.4, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      3.0,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.1,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               81400.0,
		baseMass:               11000.0,
		fuelMass:               3500.0,
		currentFuelPercentage:  1.0,
		burnRate:               3.5,
		burnWithoutAfterburner: 1.5,
	}
}

// NewF15C: heavy air-superiority 4th gen, big swept wing, twin F100s.
func NewF15C() Plane {
	const (
		tailX    float32 = -8.0
		wingSpan float32 = 5.0
		tailSpan float32 = 2.6
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.0, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    1.5,
			surfaceArea:      28.0,
			liftCoefficient:  1.2,
			dragCoefficient:  0.020,
			aspectRatio:      3.0,
			efficiency:       0.85,
			stallAngle:       26.0,
			camber:           0.02,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-1.2, 0, side * (wingSpan + 1.5)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      4.0,
			liftCoefficient:  1.0,
			dragCoefficient:  0.025,
			aspectRatio:      2.5,
			efficiency:       0.82,
			stallAngle:       26.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 40.0,
			minRotationAngle: 0.0,
			rotationRate:     25.0,
			surfaceArea:      6.0,
			liftCoefficient:  1.7,
			dragCoefficient:  0.045,
			aspectRatio:      2.5,
			efficiency:       0.85,
			stallAngle:       18.0,
			camber:           0.05,
			active:           true,
		}
	}

	stabilator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0.4, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     80.0,
			surfaceArea:      6.0,
			liftCoefficient:  0.8,
			dragCoefficient:  0.018,
			aspectRatio:      2.4,
			efficiency:       0.85,
			stallAngle:       30.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'F', '-', '1', '5', 'C'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		// Horizontal tail is all-moving; no fixed surface.
		horizontalStabilizer: Surface{active: false},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      10.0,
			liftCoefficient:  0.7,
			dragCoefficient:  0.018,
			aspectRatio:      1.4,
			efficiency:       0.80,
			stallAngle:       25.0,
			active:           false,
		},

		leftElevator:  stabilator(-1),
		rightElevator: stabilator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, 3.0, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      4.0,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               107000.0,
		baseMass:               12900.0,
		fuelMass:               7300.0,
		currentFuelPercentage:  1.0,
		burnRate:               4.0,
		burnWithoutAfterburner: 1.8,
	}
}

// NewMirage2000C: tailless pure delta; elevons in the elevator slots.
func NewMirage2000C() Plane {
	const (
		tailX    float32 = -6.0
		wingSpan float32 = 3.6
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.5, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      20.5,
			liftCoefficient:  1.0,
			dragCoefficient:  0.024,
			aspectRatio:      2.2,
			efficiency:       0.80,
			stallAngle:       30.0,
			camber:           0.01,
			active:           false,
		}
	}

	// Elevons: rear trailing edge of the delta, pitch + roll.
	elevon := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * 2.2},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     90.0,
			surfaceArea:      4.0,
			liftCoefficient:  0.9,
			dragCoefficient:  0.025,
			aspectRatio:      1.6,
			efficiency:       0.78,
			stallAngle:       28.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'M', 'i', 'r', '2', '0', '0', '0'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		// No separate ailerons on a delta; elevons roll via the elevator slots.
		leftAileron:  Surface{active: false},
		rightAileron: Surface{active: false},

		leftFlap:  Surface{active: false},
		rightFlap: Surface{active: false},

		horizontalStabilizer: Surface{active: false},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.4, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      5.5,
			liftCoefficient:  0.68,
			dragCoefficient:  0.020,
			aspectRatio:      1.3,
			efficiency:       0.76,
			stallAngle:       24.0,
			active:           false,
		},

		leftElevator:  elevon(-1),
		rightElevator: elevon(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 0.8, 2.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      2.8,
			liftCoefficient:  0.62,
			dragCoefficient:  0.024,
			aspectRatio:      1.0,
			efficiency:       0.74,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               95000.0,
		baseMass:               7500.0,
		fuelMass:               3000.0,
		currentFuelPercentage:  1.0,
		burnRate:               2.2,
		burnWithoutAfterburner: 1.0,
	}
}

// NewTyphoon: cranked delta with canards in the elevator slots.
func NewTyphoon() Plane {
	const (
		tailX    float32 = -7.2
		wingSpan float32 = 4.0
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      25.0,
			liftCoefficient:  1.05,
			dragCoefficient:  0.021,
			aspectRatio:      2.4,
			efficiency:       0.83,
			stallAngle:       30.0,
			camber:           0.01,
			active:           false,
		}
	}

	// Close-coupled canards mapped to the elevator slots.
	canard := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{2.8, 0, side * 1.8},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 35.0,
			minRotationAngle: -35.0,
			rotationRate:     120.0,
			surfaceArea:      3.5,
			liftCoefficient:  1.0,
			dragCoefficient:  0.025,
			aspectRatio:      1.4,
			efficiency:       0.80,
			stallAngle:       30.0,
			active:           true,
		}
	}

	// Rear elevons handle roll.
	elevon := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * (wingSpan + 0.8)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     100.0,
			surfaceArea:      3.5,
			liftCoefficient:  0.95,
			dragCoefficient:  0.024,
			aspectRatio:      1.8,
			efficiency:       0.80,
			stallAngle:       28.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'T', 'y', 'p', 'h', 'o', 'o', 'n'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  elevon(-1),
		rightAileron: elevon(1),

		leftFlap:  Surface{active: false},
		rightFlap: Surface{active: false},

		horizontalStabilizer: Surface{active: false},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.5, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      7.0,
			liftCoefficient:  0.7,
			dragCoefficient:  0.018,
			aspectRatio:      1.5,
			efficiency:       0.78,
			stallAngle:       25.0,
			active:           false,
		},

		leftElevator:  canard(-1),
		rightElevator: canard(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, 2.8, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     80.0,
			surfaceArea:      3.2,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.1,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               180000.0,
		baseMass:               11000.0,
		fuelMass:               4500.0,
		currentFuelPercentage:  1.0,
		burnRate:               3.5,
		burnWithoutAfterburner: 1.5,
	}
}

// NewA10A: straight-wing CAS, slow with huge lift.
func NewA10A() Plane {
	const (
		tailX    float32 = -8.0
		wingSpan float32 = 6.5
		tailSpan float32 = 2.8
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.5, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    2.0,
			surfaceArea:      23.7,
			liftCoefficient:  1.4,
			dragCoefficient:  0.030,
			aspectRatio:      6.5,
			efficiency:       0.88,
			stallAngle:       18.0,
			camber:           0.04,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-1.0, 0, side * (wingSpan - 0.5)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 20.0,
			minRotationAngle: -20.0,
			rotationRate:     50.0,
			surfaceArea:      3.5,
			liftCoefficient:  1.0,
			dragCoefficient:  0.030,
			aspectRatio:      2.5,
			efficiency:       0.80,
			stallAngle:       20.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan * 0.45},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 50.0,
			minRotationAngle: 0.0,
			rotationRate:     20.0,
			surfaceArea:      6.0,
			liftCoefficient:  2.0,
			dragCoefficient:  0.060,
			aspectRatio:      2.5,
			efficiency:       0.85,
			stallAngle:       16.0,
			camber:           0.07,
			active:           true,
		}
	}

	elevator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     50.0,
			surfaceArea:      4.5,
			liftCoefficient:  0.8,
			dragCoefficient:  0.020,
			aspectRatio:      3.0,
			efficiency:       0.85,
			stallAngle:       22.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'A', '-', '1', '0', 'A'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{
			relativePosition: Float3{tailX, 0, 0},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      6.0,
			liftCoefficient:  0.6,
			dragCoefficient:  0.018,
			aspectRatio:      3.5,
			efficiency:       0.88,
			stallAngle:       16.0,
			active:           false,
		},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 2.0, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      6.0,
			liftCoefficient:  0.7,
			dragCoefficient:  0.020,
			aspectRatio:      1.6,
			efficiency:       0.78,
			stallAngle:       20.0,
			active:           false,
		},

		leftElevator:  elevator(-1),
		rightElevator: elevator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, 3.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     50.0,
			surfaceArea:      3.0,
			liftCoefficient:  0.65,
			dragCoefficient:  0.025,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       28.0,
			active:           true,
		},

		maxTrust:               48900.0,
		baseMass:               11300.0,
		fuelMass:               4900.0,
		currentFuelPercentage:  1.0,
		burnRate:               1.8,
		burnWithoutAfterburner: 1.0,
	}
}

// NewCessna172: light straight-wing trainer (experimental here).
func NewCessna172() Plane {
	const (
		tailX    float32 = -5.5
		wingSpan float32 = 4.5
		tailSpan float32 = 1.8
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.5, 0.8, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    2.5,
			surfaceArea:      8.1,
			liftCoefficient:  1.5,
			dragCoefficient:  0.028,
			aspectRatio:      6.3,
			efficiency:       0.90,
			stallAngle:       17.0,
			camber:           0.05,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.6, 0.8, side * (wingSpan - 0.4)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 15.0,
			minRotationAngle: -15.0,
			rotationRate:     40.0,
			surfaceArea:      1.2,
			liftCoefficient:  1.0,
			dragCoefficient:  0.030,
			aspectRatio:      2.5,
			efficiency:       0.80,
			stallAngle:       18.0,
			active:           true,
		}
	}

	elevator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0.4, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 20.0,
			minRotationAngle: -20.0,
			rotationRate:     40.0,
			surfaceArea:      1.6,
			liftCoefficient:  0.8,
			dragCoefficient:  0.020,
			aspectRatio:      3.0,
			efficiency:       0.85,
			stallAngle:       20.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'C', 'e', 's', 's', 'n', 'a'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  Surface{active: false},
		rightFlap: Surface{active: false},

		horizontalStabilizer: Surface{
			relativePosition: Float3{tailX, 0.4, 0},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    -1.5,
			surfaceArea:      2.0,
			liftCoefficient:  0.6,
			dragCoefficient:  0.020,
			aspectRatio:      3.5,
			efficiency:       0.88,
			stallAngle:       16.0,
			active:           false,
		},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.2, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      1.5,
			liftCoefficient:  0.7,
			dragCoefficient:  0.022,
			aspectRatio:      1.5,
			efficiency:       0.78,
			stallAngle:       20.0,
			active:           false,
		},

		leftElevator:  elevator(-1),
		rightElevator: elevator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 0.6, 1.8, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     40.0,
			surfaceArea:      1.0,
			liftCoefficient:  0.65,
			dragCoefficient:  0.025,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       25.0,
			active:           true,
		},

		maxTrust:               2400.0,
		baseMass:               800.0,
		fuelMass:               180.0,
		currentFuelPercentage:  1.0,
		burnRate:               0.06,
		burnWithoutAfterburner: 0.04,
	}
}

// NewF4E: 3rd gen swept wing, no LERX.
func NewF4E() Plane {
	const (
		tailX    float32 = -8.5
		wingSpan float32 = 4.4
		tailSpan float32 = 2.4
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.5, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    1.0,
			surfaceArea:      24.6,
			liftCoefficient:  1.05,
			dragCoefficient:  0.024,
			aspectRatio:      2.8,
			efficiency:       0.80,
			stallAngle:       20.0,
			camber:           0.01,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-1.2, 0, side * (wingSpan + 1.0)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 20.0,
			minRotationAngle: -20.0,
			rotationRate:     60.0,
			surfaceArea:      3.5,
			liftCoefficient:  0.95,
			dragCoefficient:  0.028,
			aspectRatio:      2.2,
			efficiency:       0.78,
			stallAngle:       20.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.5, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 45.0,
			minRotationAngle: 0.0,
			rotationRate:     25.0,
			surfaceArea:      5.5,
			liftCoefficient:  1.7,
			dragCoefficient:  0.050,
			aspectRatio:      2.2,
			efficiency:       0.82,
			stallAngle:       16.0,
			camber:           0.05,
			active:           true,
		}
	}

	elevator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, -0.6, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      5.0,
			liftCoefficient:  0.75,
			dragCoefficient:  0.020,
			aspectRatio:      2.4,
			efficiency:       0.82,
			stallAngle:       24.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'F', '-', '4', 'E'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{
			relativePosition: Float3{tailX, -0.6, 0},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      5.0,
			liftCoefficient:  0.55,
			dragCoefficient:  0.020,
			aspectRatio:      2.8,
			efficiency:       0.85,
			stallAngle:       18.0,
			active:           false,
		},

		// Anhedral-mounted twin tails combined in one slot.
		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.4, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      9.0,
			liftCoefficient:  0.68,
			dragCoefficient:  0.020,
			aspectRatio:      1.4,
			efficiency:       0.76,
			stallAngle:       22.0,
			active:           false,
		},

		leftElevator:  elevator(-1),
		rightElevator: elevator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 1.0, 2.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     60.0,
			surfaceArea:      3.5,
			liftCoefficient:  0.62,
			dragCoefficient:  0.024,
			aspectRatio:      1.1,
			efficiency:       0.74,
			stallAngle:       28.0,
			active:           true,
		},

		maxTrust:               105000.0,
		baseMass:               13800.0,
		fuelMass:               7400.0,
		currentFuelPercentage:  1.0,
		burnRate:               4.5,
		burnWithoutAfterburner: 2.0,
	}
}

// NewMiG21bis: small 3rd gen delta interceptor.
func NewMiG21bis() Plane {
	const (
		tailX    float32 = -5.5
		wingSpan float32 = 2.9
		tailSpan float32 = 1.5
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.3, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      11.5,
			liftCoefficient:  0.95,
			dragCoefficient:  0.026,
			aspectRatio:      1.9,
			efficiency:       0.78,
			stallAngle:       26.0,
			camber:           0.01,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.8, 0, side * (wingSpan + 0.4)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 20.0,
			minRotationAngle: -20.0,
			rotationRate:     60.0,
			surfaceArea:      1.8,
			liftCoefficient:  0.9,
			dragCoefficient:  0.030,
			aspectRatio:      1.6,
			efficiency:       0.75,
			stallAngle:       24.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.4, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 35.0,
			minRotationAngle: 0.0,
			rotationRate:     25.0,
			surfaceArea:      2.5,
			liftCoefficient:  1.6,
			dragCoefficient:  0.050,
			aspectRatio:      1.6,
			efficiency:       0.80,
			stallAngle:       16.0,
			camber:           0.05,
			active:           true,
		}
	}

	elevator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      2.4,
			liftCoefficient:  0.75,
			dragCoefficient:  0.020,
			aspectRatio:      2.0,
			efficiency:       0.82,
			stallAngle:       26.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'M', 'i', 'G', '-', '2', '1'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{
			relativePosition: Float3{tailX, 0, 0},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      2.6,
			liftCoefficient:  0.55,
			dragCoefficient:  0.020,
			aspectRatio:      2.4,
			efficiency:       0.85,
			stallAngle:       18.0,
			active:           false,
		},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.3, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      4.5,
			liftCoefficient:  0.7,
			dragCoefficient:  0.020,
			aspectRatio:      1.5,
			efficiency:       0.78,
			stallAngle:       24.0,
			active:           false,
		},

		leftElevator:  elevator(-1),
		rightElevator: elevator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 0.8, 2.4, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     60.0,
			surfaceArea:      2.2,
			liftCoefficient:  0.62,
			dragCoefficient:  0.024,
			aspectRatio:      1.1,
			efficiency:       0.74,
			stallAngle:       28.0,
			active:           true,
		},

		maxTrust:               73000.0,
		baseMass:               7250.0,
		fuelMass:               3300.0,
		currentFuelPercentage:  1.0,
		burnRate:               2.8,
		burnWithoutAfterburner: 1.2,
	}
}

// NewF104G: minimal wing, poor turn, high thrust.
func NewF104G() Plane {
	const (
		tailX    float32 = -6.0
		wingSpan float32 = 2.6
		tailSpan float32 = 1.4
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.0, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      9.1,
			liftCoefficient:  0.8,
			dragCoefficient:  0.022,
			aspectRatio:      1.4,
			efficiency:       0.70,
			stallAngle:       16.0,
			camber:           0.0,
			active:           false,
		}
	}

	// Boundary-control flaperons: the only roll control, very effective.
	flaperon := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.6, 0, side * (wingSpan + 0.3)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     80.0,
			surfaceArea:      2.0,
			liftCoefficient:  1.2,
			dragCoefficient:  0.030,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       25.0,
			active:           true,
		}
	}

	stabilator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     90.0,
			surfaceArea:      2.2,
			liftCoefficient:  0.8,
			dragCoefficient:  0.020,
			aspectRatio:      1.8,
			efficiency:       0.82,
			stallAngle:       28.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'F', '-', '1', '0', '4'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  flaperon(-1),
		rightAileron: flaperon(1),

		leftFlap:  Surface{active: false},
		rightFlap: Surface{active: false},

		horizontalStabilizer: Surface{active: false},

		// T-tail.
		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.6, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      4.0,
			liftCoefficient:  0.7,
			dragCoefficient:  0.020,
			aspectRatio:      1.4,
			efficiency:       0.78,
			stallAngle:       24.0,
			active:           false,
		},

		leftElevator:  stabilator(-1),
		rightElevator: stabilator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 0.8, 2.8, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      2.0,
			liftCoefficient:  0.62,
			dragCoefficient:  0.024,
			aspectRatio:      1.1,
			efficiency:       0.74,
			stallAngle:       28.0,
			active:           true,
		},

		maxTrust:               79600.0,
		baseMass:               7000.0,
		fuelMass:               3200.0,
		currentFuelPercentage:  1.0,
		burnRate:               3.0,
		burnWithoutAfterburner: 1.3,
	}
}

// NewF5E: light 3rd gen aggressor.
func NewF5E() Plane {
	const (
		tailX    float32 = -5.8
		wingSpan float32 = 3.0
		tailSpan float32 = 1.6
	)

	mainWing := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{0.2, 0, side * wingSpan},
			rotationAxis:     Float3{0, 0, 1},
			rotationAngle:    1.5,
			surfaceArea:      7.8,
			liftCoefficient:  1.1,
			dragCoefficient:  0.022,
			aspectRatio:      3.2,
			efficiency:       0.83,
			stallAngle:       22.0,
			camber:           0.02,
			active:           false,
		}
	}

	aileron := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.8, 0, side * (wingSpan + 0.8)},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     70.0,
			surfaceArea:      1.6,
			liftCoefficient:  1.0,
			dragCoefficient:  0.026,
			aspectRatio:      2.2,
			efficiency:       0.80,
			stallAngle:       24.0,
			active:           true,
		}
	}

	flap := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{-0.4, 0, side * wingSpan * 0.5},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 40.0,
			minRotationAngle: 0.0,
			rotationRate:     25.0,
			surfaceArea:      2.0,
			liftCoefficient:  1.7,
			dragCoefficient:  0.045,
			aspectRatio:      2.0,
			efficiency:       0.84,
			stallAngle:       17.0,
			camber:           0.05,
			active:           true,
		}
	}

	elevator := func(side float32) Surface {
		return Surface{
			relativePosition: Float3{tailX, 0.3, side * tailSpan},
			rotationAxis:     Float3{0, 0, 1},
			maxRotationAngle: 28.0,
			minRotationAngle: -28.0,
			rotationRate:     80.0,
			surfaceArea:      2.0,
			liftCoefficient:  0.8,
			dragCoefficient:  0.018,
			aspectRatio:      2.2,
			efficiency:       0.84,
			stallAngle:       28.0,
			active:           true,
		}
	}

	return Plane{
		planeName: [16]byte{'F', '-', '5', 'E'},
		forward:   Float3{1, 0, 0},

		leftWing:  mainWing(-1),
		rightWing: mainWing(1),

		leftAileron:  aileron(-1),
		rightAileron: aileron(1),

		leftFlap:  flap(-1),
		rightFlap: flap(1),

		horizontalStabilizer: Surface{
			relativePosition: Float3{tailX, 0.3, 0},
			rotationAxis:     Float3{0, 0, 1},
			surfaceArea:      2.2,
			liftCoefficient:  0.55,
			dragCoefficient:  0.018,
			aspectRatio:      2.6,
			efficiency:       0.86,
			stallAngle:       18.0,
			active:           false,
		},

		verticalStabilizer: Surface{
			relativePosition: Float3{tailX, 1.4, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      3.2,
			liftCoefficient:  0.7,
			dragCoefficient:  0.020,
			aspectRatio:      1.5,
			efficiency:       0.78,
			stallAngle:       24.0,
			active:           false,
		},

		leftElevator:  elevator(-1),
		rightElevator: elevator(1),

		rudder: Surface{
			relativePosition: Float3{tailX - 0.8, 2.5, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     60.0,
			surfaceArea:      1.8,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               54400.0,
		baseMass:               4350.0,
		fuelMass:               2200.0,
		currentFuelPercentage:  1.0,
		burnRate:               1.6,
		burnWithoutAfterburner: 0.7,
	}
}

package simulation

// Custom archetypes for testing: each is extreme in one direction.

func testSurface(relPos Float3, area, cl, cd float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		surfaceArea:      area,
		liftCoefficient:  cl,
		dragCoefficient:  cd,
		aspectRatio:      6.0,
		efficiency:       0.9,
		stallAngle:       20.0,
		camber:           0.03,
		active:           false,
	}
}

func testControl(relPos Float3, area, cl float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		maxRotationAngle: 30.0,
		minRotationAngle: -30.0,
		rotationRate:     90.0,
		surfaceArea:      area,
		liftCoefficient:  cl,
		dragCoefficient:  0.025,
		aspectRatio:      3.0,
		efficiency:       0.82,
		stallAngle:       28.0,
		active:           true,
	}
}

// NewTestTurnKing: huge wing, high CL, light — best sustained turn, slow.
func NewTestTurnKing() Plane {
	p := Plane{
		planeName: [16]byte{'T', 'U', 'R', 'N', 'K', 'I', 'N', 'G'},
		forward:   Float3{1, 0, 0},

		leftWing:  testSurface(Float3{0, 0, -5}, 35, 1.6, 0.02),
		rightWing: testSurface(Float3{0, 0, 5}, 35, 1.6, 0.02),

		leftAileron:  testControl(Float3{-1, 0, -6}, 5, 1.2),
		rightAileron: testControl(Float3{-1, 0, 6}, 5, 1.2),

		horizontalStabilizer: testSurface(Float3{-8, 0, 0}, 10, 0.6, 0.015),
		verticalStabilizer: Surface{
			relativePosition: Float3{-8, 1.5, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      7,
			liftCoefficient:  0.7,
			dragCoefficient:  0.018,
			aspectRatio:      1.6,
			efficiency:       0.8,
			stallAngle:       22.0,
			active:           false,
		},

		leftElevator:  testControl(Float3{-9, 0, -2.5}, 4, 0.9),
		rightElevator: testControl(Float3{-9, 0, 2.5}, 4, 0.9),

		rudder: Surface{
			relativePosition: Float3{-9, 2.5, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      3,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               60000.0,
		baseMass:               5000.0,
		fuelMass:               1000.0,
		currentFuelPercentage:  1.0,
		burnRate:               1.5,
		burnWithoutAfterburner: 0.7,
	}
	return p
}

// NewTestSpeedKing: tiny wing, huge thrust — fastest, poor turn.
func NewTestSpeedKing() Plane {
	p := Plane{
		planeName: [16]byte{'S', 'P', 'E', 'E', 'D', 'K', 'I', 'N', 'G'},
		forward:   Float3{1, 0, 0},

		leftWing:  testSurface(Float3{0, 0, -2.5}, 6, 0.7, 0.015),
		rightWing: testSurface(Float3{0, 0, 2.5}, 6, 0.7, 0.015),

		leftAileron:  testControl(Float3{-1, 0, -3}, 2, 0.9),
		rightAileron: testControl(Float3{-1, 0, 3}, 2, 0.9),

		horizontalStabilizer: testSurface(Float3{-6, 0, 0}, 3, 0.6, 0.015),
		verticalStabilizer: Surface{
			relativePosition: Float3{-6, 1.2, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      3.5,
			liftCoefficient:  0.7,
			dragCoefficient:  0.018,
			aspectRatio:      1.5,
			efficiency:       0.8,
			stallAngle:       22.0,
			active:           false,
		},

		leftElevator:  testControl(Float3{-7, 0, -1.5}, 2, 0.8),
		rightElevator: testControl(Float3{-7, 0, 1.5}, 2, 0.8),

		rudder: Surface{
			relativePosition: Float3{-7, 2.0, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 30.0,
			minRotationAngle: -30.0,
			rotationRate:     70.0,
			surfaceArea:      2,
			liftCoefficient:  0.65,
			dragCoefficient:  0.022,
			aspectRatio:      1.2,
			efficiency:       0.75,
			stallAngle:       30.0,
			active:           true,
		},

		maxTrust:               150000.0,
		baseMass:               6000.0,
		fuelMass:               2000.0,
		currentFuelPercentage:  1.0,
		burnRate:               4.0,
		burnWithoutAfterburner: 1.8,
	}
	return p
}

// NewTestBrick: heavy and underpowered — bad at everything.
func NewTestBrick() Plane {
	p := Plane{
		planeName: [16]byte{'B', 'R', 'I', 'C', 'K'},
		forward:   Float3{1, 0, 0},

		leftWing:  testSurface(Float3{0, 0, -3}, 8, 0.8, 0.04),
		rightWing: testSurface(Float3{0, 0, 3}, 8, 0.8, 0.04),

		leftAileron:  testControl(Float3{-1, 0, -3.5}, 1.5, 0.8),
		rightAileron: testControl(Float3{-1, 0, 3.5}, 1.5, 0.8),

		horizontalStabilizer: testSurface(Float3{-7, 0, 0}, 3, 0.5, 0.03),
		verticalStabilizer: Surface{
			relativePosition: Float3{-7, 1.2, 0},
			rotationAxis:     Float3{0, 1, 0},
			surfaceArea:      3,
			liftCoefficient:  0.6,
			dragCoefficient:  0.03,
			aspectRatio:      1.4,
			efficiency:       0.7,
			stallAngle:       20.0,
			active:           false,
		},

		leftElevator:  testControl(Float3{-8, 0, -1.5}, 1.5, 0.7),
		rightElevator: testControl(Float3{-8, 0, 1.5}, 1.5, 0.7),

		rudder: Surface{
			relativePosition: Float3{-8, 2.0, 0},
			rotationAxis:     Float3{0, 1, 0},
			maxRotationAngle: 25.0,
			minRotationAngle: -25.0,
			rotationRate:     50.0,
			surfaceArea:      1.5,
			liftCoefficient:  0.6,
			dragCoefficient:  0.03,
			aspectRatio:      1.1,
			efficiency:       0.7,
			stallAngle:       25.0,
			active:           true,
		},

		maxTrust:               30000.0,
		baseMass:               14000.0,
		fuelMass:               2000.0,
		currentFuelPercentage:  1.0,
		burnRate:               2.0,
		burnWithoutAfterburner: 1.0,
	}
	return p
}

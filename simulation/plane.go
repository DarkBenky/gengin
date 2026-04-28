package simulation

import (
	"bufio"
	"math"
	"os"
	"unsafe"
)

type Float3 struct {
	X, Y, Z float32
}

func (f Float3) Length() float32 {
	return float32(math.Sqrt(float64(f.X*f.X + f.Y*f.Y + f.Z*f.Z)))
}

func (f Float3) Normalize() Float3 {
	length := f.Length()
	if length == 0 {
		return Float3{0, 0, 0}
	}
	return Float3{f.X / length, f.Y / length, f.Z / length}
}

func (f Float3) Dot(other Float3) float32 {
	return f.X*other.X + f.Y*other.Y + f.Z*other.Z
}

func (f Float3) Cross(other Float3) Float3 {
	return Float3{
		X: f.Y*other.Z - f.Z*other.Y,
		Y: f.Z*other.X - f.X*other.Z,
		Z: f.X*other.Y - f.Y*other.X,
	}
}

func (f Float3) Scale(scalar float32) Float3 {
	return Float3{f.X * scalar, f.Y * scalar, f.Z * scalar}
}

func (f Float3) Add(other Float3) Float3 {
	return Float3{f.X + other.X, f.Y + other.Y, f.Z + other.Z}
}

func (f Float3) Sub(other Float3) Float3 {
	return Float3{f.X - other.X, f.Y - other.Y, f.Z - other.Z}
}

func (f Float3) Negate() Float3 {
	return Float3{-f.X, -f.Y, -f.Z}
}

type Surface struct {
	relativePosition Float3
	rotationAxis     Float3
	rotationAngle    float32 // in degrees can rotate if active around the rotationAxis
	rotationRate     float32 // degrees per second
	maxRotationAngle float32 // maximum angle the surface can rotate to
	minRotationAngle float32 // minimum angle the surface can rotate to
	surfaceArea      float32 // in square meters
	liftCoefficient  float32
	dragCoefficient  float32
	aspectRatio      float32
	efficiency       float32
	stallAngle       float32
	camber           float32
	active           bool
}

type Plane struct {
	planeName              [16]byte
	position               Float3
	forward                Float3 // along this vector is the plane's nose pointing and trust is applied
	rotation               Float3
	leftWing               Surface
	rightWing              Surface
	verticalStabilizer     Surface
	horizontalStabilizer   Surface
	leftAileron            Surface
	rightAileron           Surface
	rudder                 Surface
	leftFlap               Surface
	rightFlap              Surface
	leftElevator           Surface
	rightElevator          Surface
	maxTrust               float32 // in Newtons in max afterburner
	currentTrustPercentage float32 // 0.0 to 1.0
	baseMass               float32 // in kg without fuel
	fuelMass               float32 // in kg
	currentFuelPercentage  float32 // 0.0 to 1.0
	burnRate               float32 // in kg/s at max afterburner
	burnWithoutAfterburner float32 // in kg/s at max without afterburner
}

func newWing(relPos Float3, side float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		rotationAngle:    2.0,
		surfaceArea:      25.0,
		liftCoefficient:  1.2,
		dragCoefficient:  0.02,
		aspectRatio:      7.0,
		efficiency:       0.90,
		stallAngle:       16.0,
		camber:           0.02,
		active:           false, // fixed surface
	}
}

func newAileron(relPos Float3, side float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		maxRotationAngle: 25.0,
		minRotationAngle: -25.0,
		surfaceArea:      3.0,
		liftCoefficient:  1.0,
		dragCoefficient:  0.025,
		aspectRatio:      3.5,
		efficiency:       0.80,
		stallAngle:       20.0,
		camber:           0.01,
		active:           true,
	}
}

func newFlap(relPos Float3, side float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		maxRotationAngle: 40.0,
		minRotationAngle: 0.0,
		surfaceArea:      6.0,
		liftCoefficient:  1.8,
		dragCoefficient:  0.05,
		aspectRatio:      3.0,
		efficiency:       0.85,
		stallAngle:       18.0,
		camber:           0.06,
		active:           true,
	}
}

func newHorizontalStabilizer(relPos Float3) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		rotationAngle:    -2.0,
		surfaceArea:      8.0,
		liftCoefficient:  0.6,
		dragCoefficient:  0.015,
		aspectRatio:      4.5,
		efficiency:       0.88,
		stallAngle:       14.0,
		active:           false,
	}
}

func newVerticalStabilizer(relPos Float3) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 1, 0},
		surfaceArea:      6.0,
		liftCoefficient:  0.7,
		dragCoefficient:  0.018,
		aspectRatio:      1.8,
		efficiency:       0.82,
		stallAngle:       20.0,
		active:           false,
	}
}

func newElevator(relPos Float3, side float32) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 0, 1},
		maxRotationAngle: 30.0,
		minRotationAngle: -30.0,
		surfaceArea:      3.0,
		liftCoefficient:  0.8,
		dragCoefficient:  0.020,
		aspectRatio:      3.0,
		efficiency:       0.80,
		stallAngle:       25.0,
		active:           true,
	}
}

func newRudder(relPos Float3) Surface {
	return Surface{
		relativePosition: relPos,
		rotationAxis:     Float3{0, 1, 0},
		maxRotationAngle: 35.0,
		minRotationAngle: -35.0,
		surfaceArea:      2.5,
		liftCoefficient:  0.65,
		dragCoefficient:  0.022,
		aspectRatio:      1.5,
		efficiency:       0.78,
		stallAngle:       30.0,
		active:           true,
	}
}

func NewPlane(position Float3) Plane {
	var wingSpan float32 = 5.5
	var tailX float32 = -8.0
	var tailSpan float32 = 2.5

	return Plane{
		planeName: [16]byte{'G', 'E', 'N', 'E', 'R', 'I', 'C'},
		position:  position,
		forward:   Float3{1, 0, 0},

		leftWing:  newWing(Float3{0, 0, -wingSpan}, -1),
		rightWing: newWing(Float3{0, 0, wingSpan}, 1),

		leftAileron:  newAileron(Float3{-1.0, 0, -(wingSpan + 2.5)}, -1),
		rightAileron: newAileron(Float3{-1.0, 0, wingSpan + 2.5}, 1),

		leftFlap:  newFlap(Float3{-0.5, 0, -wingSpan * 0.5}, -1),
		rightFlap: newFlap(Float3{-0.5, 0, wingSpan * 0.5}, 1),

		horizontalStabilizer: newHorizontalStabilizer(Float3{tailX, 0, 0}),
		verticalStabilizer:   newVerticalStabilizer(Float3{tailX, 1.5, 0}),

		leftElevator:  newElevator(Float3{tailX - 1.0, 0, -tailSpan}, -1),
		rightElevator: newElevator(Float3{tailX - 1.0, 0, tailSpan}, 1),

		rudder: newRudder(Float3{tailX - 1.0, 2.5, 0}),

		maxTrust:               80000.0,
		baseMass:               8000.0,
		fuelMass:               3000.0,
		currentFuelPercentage:  1.0,
		burnRate:               1.5,
		burnWithoutAfterburner: 0.7,
	}
}

func uint32ToBytes(value uint32) []byte {
	return []byte{
		byte(value & 0xFF),
		byte((value >> 8) & 0xFF),
		byte((value >> 16) & 0xFF),
		byte((value >> 24) & 0xFF),
	}
}

func float32ToBytes(value float32) []byte {
	bits := uint32(*(*uint32)(unsafe.Pointer(&value)))
	return []byte{
		byte(bits & 0xFF),
		byte((bits >> 8) & 0xFF),
		byte((bits >> 16) & 0xFF),
		byte((bits >> 24) & 0xFF),
	}
}

func float3ToBytes(f Float3) []byte {
	bytes := make([]byte, 12)
	copy(bytes[0:4], float32ToBytes(f.X))
	copy(bytes[4:8], float32ToBytes(f.Y))
	copy(bytes[8:12], float32ToBytes(f.Z))
	return bytes
}

func boolToByte(b bool) byte {
	if b {
		return 1
	}
	return 0
}

func surfaceToBytes(s Surface) []byte {
	bytes := make([]byte, 69)
	copy(bytes[0:12], float3ToBytes(s.relativePosition))
	copy(bytes[12:24], float3ToBytes(s.rotationAxis))
	copy(bytes[24:28], float32ToBytes(s.rotationAngle))
	copy(bytes[28:32], float32ToBytes(s.rotationRate))
	copy(bytes[32:36], float32ToBytes(s.maxRotationAngle))
	copy(bytes[36:40], float32ToBytes(s.minRotationAngle))
	copy(bytes[40:44], float32ToBytes(s.surfaceArea))
	copy(bytes[44:48], float32ToBytes(s.liftCoefficient))
	copy(bytes[48:52], float32ToBytes(s.dragCoefficient))
	copy(bytes[52:56], float32ToBytes(s.aspectRatio))
	copy(bytes[56:60], float32ToBytes(s.efficiency))
	copy(bytes[60:64], float32ToBytes(s.stallAngle))
	copy(bytes[64:68], float32ToBytes(s.camber))
	bytes[68] = boolToByte(s.active)
	return bytes
}

func (p *Plane) ExportPlaneToBinary(filePath string) error {
	file, err := os.Create(filePath)
	if err != nil {
		return err
	}
	defer file.Close()

	w := bufio.NewWriter(file)
	// Write plane name (16 bytes)
	_, err = w.Write(p.planeName[:16])
	if err != nil {
		return err
	}
	// Write position
	_, err = w.Write(float3ToBytes(p.position))
	if err != nil {
		return err
	}
	// Write forward vector
	_, err = w.Write(float3ToBytes(p.forward))
	if err != nil {
		return err
	}
	// Write rotation
	_, err = w.Write(float3ToBytes(p.rotation))
	if err != nil {
		return err
	}

	// Write surfaces
	surfaces := []Surface{
		p.leftWing, p.rightWing,
		p.verticalStabilizer, p.horizontalStabilizer,
		p.leftAileron, p.rightAileron,
		p.rudder,
		p.leftFlap, p.rightFlap,
		p.leftElevator, p.rightElevator,
	}
	for _, surface := range surfaces {
		_, err = w.Write(surfaceToBytes(surface))
		if err != nil {
			return err
		}
	}

	// Write engine and mass fields
	for _, v := range []float32{p.maxTrust, p.currentTrustPercentage, p.baseMass, p.fuelMass, p.currentFuelPercentage, p.burnRate, p.burnWithoutAfterburner} {
		_, err = w.Write(float32ToBytes(v))
		if err != nil {
			return err
		}
	}

	return w.Flush()
}

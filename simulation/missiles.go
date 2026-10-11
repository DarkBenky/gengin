package simulation

import "os"

// Missile presets. .bin layout: "MSSL", uint32 version, name[16], floats in
// struct order. Keep in sync with simulation/cSim/missileImport.c.

type MissileStage struct {
	Thrust   float32 // N
	BurnTime float32 // s
}

type MissileSpec struct {
	Name [16]byte

	Length, Diameter          float32 // m
	FinSpanFront, FinSpanBack float32 // m
	EmptyMass, PropellantMass float32 // kg
	InertiaRoll               float32 // kg*m^2
	InertiaPitch, InertiaYaw  float32

	CnAlpha                  float32 // fin lift slope, per rad
	Cd0                      float32
	DampingRoll              float32 // N*m per rad/s
	DampingPitch, DampingYaw float32
	StallAngle               float32 // deg
	MaxG                     float32

	FrontArea, FrontMaxDeg, FrontRateDeg float32 // canards: m^2, deg, deg/s
	BackArea, BackMaxDeg, BackRateDeg    float32 // tail fins
	TvcMaxDeg, TvcRateDeg                float32 // thrust vectoring

	StageCount int
	Stages     [3]MissileStage

	PnGain     float32
	FuseRadius float32 // m
	Lifetime   float32 // s
}

func missileName(s string) [16]byte {
	var n [16]byte
	copy(n[:], s)
	return n
}

func (m MissileSpec) ExportMissileToBinary(filePath string) error {
	var buf []byte
	buf = append(buf, "MSSL"...)
	buf = append(buf, uint32ToBytes(1)...)
	buf = append(buf, m.Name[:]...)
	for _, v := range []float32{
		m.Length, m.Diameter, m.FinSpanFront, m.FinSpanBack,
		m.EmptyMass, m.PropellantMass,
		m.InertiaRoll, m.InertiaPitch, m.InertiaYaw,
		m.CnAlpha, m.Cd0,
		m.DampingRoll, m.DampingPitch, m.DampingYaw,
		m.StallAngle, m.MaxG,
		m.FrontArea, m.FrontMaxDeg, m.FrontRateDeg,
		m.BackArea, m.BackMaxDeg, m.BackRateDeg,
		m.TvcMaxDeg, m.TvcRateDeg,
	} {
		buf = append(buf, float32ToBytes(v)...)
	}
	buf = append(buf, uint32ToBytes(uint32(m.StageCount))...)
	for _, s := range m.Stages {
		buf = append(buf, float32ToBytes(s.Thrust)...)
		buf = append(buf, float32ToBytes(s.BurnTime)...)
	}
	for _, v := range []float32{m.PnGain, m.FuseRadius, m.Lifetime} {
		buf = append(buf, float32ToBytes(v)...)
	}
	return os.WriteFile(filePath, buf, 0666)
}

// Shared defaults: slender-body inertia, damping = 4x inertia, 30 deg / 180 deg/s fins.
func baseMissile(name string, length, diameter, emptyMass, propellantMass, frontArea, backArea, cd0, maxG float32) MissileSpec {
	total := emptyMass + propellantMass
	pitch := total * length * length / 12.0
	roll := total * diameter * 0.3 * diameter * 0.3 / 2.0
	return MissileSpec{
		Name:           missileName(name),
		Length:         length,
		Diameter:       diameter,
		EmptyMass:      emptyMass,
		PropellantMass: propellantMass,
		InertiaRoll:    roll,
		InertiaPitch:   pitch,
		InertiaYaw:     pitch,
		CnAlpha:        1.5,
		Cd0:            cd0,
		DampingRoll:    roll * 4.0,
		DampingPitch:   pitch * 4.0,
		DampingYaw:     pitch * 4.0,
		StallAngle:     35.0,
		MaxG:           maxG,
		FrontArea:      frontArea,
		FrontMaxDeg:    30.0,
		FrontRateDeg:   180.0,
		BackArea:       backArea,
		BackMaxDeg:     30.0,
		BackRateDeg:    180.0,
		StageCount:     1,
		PnGain:         4.0,
		FuseRadius:     15.0,
		Lifetime:       60.0,
	}
}

// NewAIM9B: first production Sidewinder (Mk.4, 1956).
func NewAIM9B() MissileSpec {
	m := baseMissile("AIM-9B", 2.79, 0.127, 55, 30, 0.04, 0.10, 0.35, 20)
	m.FinSpanFront, m.FinSpanBack = 0.35, 0.45
	m.Stages[0] = MissileStage{26400, 2.5}
	return m
}

// NewAIM9X: high-off-boresight IR missile, thrust vectored (2003).
func NewAIM9X() MissileSpec {
	m := baseMissile("AIM-9X", 3.0, 0.127, 100, 55, 0.05, 0.12, 0.30, 50)
	m.FinSpanFront, m.FinSpanBack = 0.40, 0.50
	m.TvcMaxDeg, m.TvcRateDeg = 30.0, 200.0
	m.Stages[0] = MissileStage{48000, 2.5}
	return m
}

// NewAIM7E: early semi-active radar Sparrow (1965).
func NewAIM7E() MissileSpec {
	m := baseMissile("AIM-7E", 3.66, 0.203, 140, 90, 0, 0.15, 0.40, 22)
	m.FinSpanFront, m.FinSpanBack = 0, 0.60
	m.Stages[0] = MissileStage{39600, 5.0}
	return m
}

// NewAIM120C: fire-and-forget ARH BVR missile (1999).
func NewAIM120C() MissileSpec {
	m := baseMissile("AIM-120C", 3.66, 0.178, 95, 62, 0, 0.12, 0.35, 45)
	m.FinSpanFront, m.FinSpanBack = 0, 0.45
	m.Stages[0] = MissileStage{54600, 2.5}
	return m
}

// NewAIM54C: very long range two-stage Phoenix (1976).
func NewAIM54C() MissileSpec {
	m := baseMissile("AIM-54C", 3.85, 0.38, 300, 163, 0, 0.22, 0.50, 6)
	m.FinSpanFront, m.FinSpanBack = 0, 0.90
	m.StageCount = 2
	m.Stages[0] = MissileStage{48600, 2.7}
	m.Stages[1] = MissileStage{15500, 14.7}
	return m
}

// NewR3S: first Soviet IR missile, AIM-9B copy (1961).
func NewR3S() MissileSpec {
	m := baseMissile("R-3S", 2.8, 0.127, 57, 30, 0.04, 0.10, 0.36, 20)
	m.FinSpanFront, m.FinSpanBack = 0.35, 0.45
	m.Stages[0] = MissileStage{26400, 2.5}
	return m
}

// NewR73: agile IR missile with thrust vectoring (1984).
func NewR73() MissileSpec {
	m := baseMissile("R-73", 2.93, 0.17, 70, 35, 0.06, 0.12, 0.32, 45)
	m.FinSpanFront, m.FinSpanBack = 0.40, 0.50
	m.TvcMaxDeg, m.TvcRateDeg = 40.0, 200.0
	m.Stages[0] = MissileStage{33500, 2.3}
	return m
}

// NewR27ER: long-range two-stage SARH missile (1980).
func NewR27ER() MissileSpec {
	m := baseMissile("R-27ER", 4.85, 0.23, 400, 220, 0, 0.20, 0.45, 20)
	m.FinSpanFront, m.FinSpanBack = 0, 0.70
	m.StageCount = 2
	m.Stages[0] = MissileStage{62900, 2.7}
	m.Stages[1] = MissileStage{26000, 11.7}
	return m
}

// NewR77: ARH BVR missile with grid fins, high drag (1994).
func NewR77() MissileSpec {
	m := baseMissile("R-77", 3.71, 0.20, 110, 65, 0, 0.14, 0.55, 30)
	m.FinSpanFront, m.FinSpanBack = 0, 0.40
	m.Stages[0] = MissileStage{57200, 2.5}
	return m
}

// NewR33: long-range, low-agility S-300V-derived AAM (1980s).
func NewR33() MissileSpec {
	m := baseMissile("R-33", 4.15, 0.38, 320, 168, 0, 0.20, 0.45, 8)
	m.FinSpanFront, m.FinSpanBack = 0, 0.90
	m.StageCount = 2
	m.Stages[0] = MissileStage{48400, 2.5}
	m.Stages[1] = MissileStage{17600, 14.1}
	return m
}

// NewMissile: generic template to copy for new missiles.
func NewMissile() MissileSpec {
	m := baseMissile("MISSILE", 3.5, 0.18, 100, 60, 0.04, 0.12, 0.35, 30)
	m.FinSpanFront, m.FinSpanBack = 0.40, 0.50
	m.Stages[0] = MissileStage{52800, 2.5}
	return m
}

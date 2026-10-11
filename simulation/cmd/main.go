package main

import (
	"fmt"
	"os"
	"simulation"
)

func main() {
	planes := []struct {
		name  string
		plane simulation.Plane
	}{
		{"../simModels/generic", simulation.NewPlane(simulation.Float3{})},
		{"../simModels/F-16C", simulation.NewF16()},
		{"../simModels/Su-27", simulation.NewSu27()},
		{"../simModels/MiG-29", simulation.NewMiG29()},
		{"../simModels/F-15C", simulation.NewF15C()},
		{"../simModels/Mirage2000", simulation.NewMirage2000C()},
		{"../simModels/Typhoon", simulation.NewTyphoon()},
		{"../simModels/A-10A", simulation.NewA10A()},
		{"../simModels/Cessna172", simulation.NewCessna172()},
		{"../simModels/F-4E", simulation.NewF4E()},
		{"../simModels/MiG-21", simulation.NewMiG21bis()},
		{"../simModels/F-104", simulation.NewF104G()},
		{"../simModels/F-5E", simulation.NewF5E()},
		{"../simModels/TurnKing", simulation.NewTestTurnKing()},
		{"../simModels/SpeedKing", simulation.NewTestSpeedKing()},
		{"../simModels/Brick", simulation.NewTestBrick()},
	}

	for _, p := range planes {
		path := p.name + ".bin"
		if err := p.plane.ExportPlaneToBinary(path); err != nil {
			fmt.Fprintf(os.Stderr, "export %s: %v\n", p.name, err)
			os.Exit(1)
		}
		fmt.Printf("exported %s -> %s\n", p.name, path)
	}

	missiles := []struct {
		name string
		spec simulation.MissileSpec
	}{
		{"../simModels/AIM-9B", simulation.NewAIM9B()},
		{"../simModels/AIM-9X", simulation.NewAIM9X()},
		{"../simModels/AIM-7E", simulation.NewAIM7E()},
		{"../simModels/AIM-120C", simulation.NewAIM120C()},
		{"../simModels/AIM-54C", simulation.NewAIM54C()},
		{"../simModels/R-3S", simulation.NewR3S()},
		{"../simModels/R-73", simulation.NewR73()},
		{"../simModels/R-27ER", simulation.NewR27ER()},
		{"../simModels/R-77", simulation.NewR77()},
		{"../simModels/R-33", simulation.NewR33()},
		{"../simModels/MISSILE", simulation.NewMissile()},
	}

	for _, m := range missiles {
		path := m.name + ".bin"
		if err := m.spec.ExportMissileToBinary(path); err != nil {
			fmt.Fprintf(os.Stderr, "export %s: %v\n", m.name, err)
			os.Exit(1)
		}
		fmt.Printf("exported %s -> %s\n", m.name, path)
	}
}

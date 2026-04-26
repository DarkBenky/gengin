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
	}

	for _, p := range planes {
		path := p.name + ".bin"
		if err := p.plane.ExportPlaneToBinary(path); err != nil {
			fmt.Fprintf(os.Stderr, "export %s: %v\n", p.name, err)
			os.Exit(1)
		}
		fmt.Printf("exported %s -> %s\n", p.name, path)
	}
}

package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"
	"unsafe"
)

type Vertex struct {
	X, Y, Z float32
}

type Triangle struct {
	Vertex1, Vertex2, Vertex3, Normal Vertex
	Roughness                         float32
	Metallic                          float32
	Emission                          float32
	Color                             [3]float32
	index                             int32
}

type FileObject struct {
	FileSize           uint32
	TriangleStructSize uint32
	Triangles          []Triangle
}

func cross(a, b, c Vertex) float32 {
	return (b.X-a.X)*(c.Y-a.Y) - (b.Y-a.Y)*(c.X-a.X)
}

func pointInTriangle(a, b, c, p Vertex) bool {
	d1 := cross(p, a, b)
	d2 := cross(p, b, c)
	d3 := cross(p, c, a)

	hasNeg := (d1 < 0) || (d2 < 0) || (d3 < 0)
	hasPos := (d1 > 0) || (d2 > 0) || (d3 > 0)

	return !(hasNeg && hasPos)
}

func isEar(vertices []Vertex, prev, curr, next int) bool {
	n := len(vertices)
	a := vertices[prev%n]
	b := vertices[curr%n]
	c := vertices[next%n]

	if cross(a, b, c) <= 0 {
		return false
	}

	for i := range n {
		if i == prev || i == curr || i == next {
			continue
		}
		if pointInTriangle(a, b, c, vertices[i]) {
			return false
		}
	}
	return true
}

func polygonArea(vertices []Vertex) float32 {
	n := len(vertices)
	if n < 3 {
		return 0
	}
	area := float32(0)
	for i := range n {
		j := (i + 1) % n
		area += vertices[i].X * vertices[j].Y
		area -= vertices[j].X * vertices[i].Y
	}
	return area / 2.0
}

func ensureCounterClockwise(vertices []Vertex) []Vertex {
	if polygonArea(vertices) < 0 {
		result := make([]Vertex, len(vertices))
		for i := range vertices {
			result[i] = vertices[len(vertices)-1-i]
		}
		return result
	}
	return vertices
}

func Normalize(v Vertex) Vertex {
	length := float32(math.Sqrt(float64(v.X*v.X + v.Y*v.Y + v.Z*v.Z)))
	if length == 0 {
		return Vertex{0, 0, 0}
	}
	invLength := 1.0 / length
	return Vertex{v.X * invLength, v.Y * invLength, v.Z * invLength}
}

func CalculateTriangleNormal(v1, v2, v3 Vertex) Vertex {
	edge1 := Vertex{v2.X - v1.X, v2.Y - v1.Y, v2.Z - v1.Z}
	edge2 := Vertex{v3.X - v1.X, v3.Y - v1.Y, v3.Z - v1.Z}

	normal := Vertex{
		edge1.Y*edge2.Z - edge1.Z*edge2.Y,
		edge1.Z*edge2.X - edge1.X*edge2.Z,
		edge1.X*edge2.Y - edge1.Y*edge2.X,
	}
	return Normalize(normal)
}

func ValidateAndFixWindingOrder(tri *Triangle) bool {
	calculatedNormal := CalculateTriangleNormal(tri.Vertex1, tri.Vertex2, tri.Vertex3)

	if tri.Normal.X == 0 && tri.Normal.Y == 0 && tri.Normal.Z == 0 {
		tri.Normal = calculatedNormal
		return true
	}

	dot := calculatedNormal.X*tri.Normal.X + calculatedNormal.Y*tri.Normal.Y + calculatedNormal.Z*tri.Normal.Z

	if dot < 0 {
		tri.Vertex2, tri.Vertex3 = tri.Vertex3, tri.Vertex2
		tri.Normal = CalculateTriangleNormal(tri.Vertex1, tri.Vertex2, tri.Vertex3)
		return false
	}

	tri.Normal = calculatedNormal
	return true
}

func EnsureConsistentWinding(triangles []Triangle) int {
	fixedCount := 0
	for i := range triangles {
		if !ValidateAndFixWindingOrder(&triangles[i]) {
			fixedCount++
		}
	}
	return fixedCount
}

func Triangulate(v []Vertex) []Triangle {
	if len(v) < 3 {
		return nil
	}
	if len(v) == 3 {
		normal := CalculateTriangleNormal(v[0], v[1], v[2])
		return []Triangle{{
			Vertex1: v[0],
			Vertex2: v[1],
			Vertex3: v[2],
			Normal:  normal,
		}}
	}

	vertices := ensureCounterClockwise(v)
	n := len(vertices)

	indices := make([]int, n)
	for i := range n {
		indices[i] = i
	}

	var triangles []Triangle

	for len(indices) > 3 {
		earFound := false

		for i := 0; i < len(indices); i++ {
			prev := (i - 1 + len(indices)) % len(indices)
			curr := i
			next := (i + 1) % len(indices)

			if isEar(vertices, indices[prev], indices[curr], indices[next]) {
				v1 := vertices[indices[prev]]
				v2 := vertices[indices[curr]]
				v3 := vertices[indices[next]]
				normal := CalculateTriangleNormal(v1, v2, v3)

				triangle := Triangle{
					Vertex1: v1,
					Vertex2: v2,
					Vertex3: v3,
					Normal:  normal,
				}
				triangles = append(triangles, triangle)

				newIndices := make([]int, len(indices)-1)
				copy(newIndices[:curr], indices[:curr])
				copy(newIndices[curr:], indices[curr+1:])
				indices = newIndices

				earFound = true
				break
			}
		}

		if !earFound {
			break
		}
	}

	if len(indices) == 3 {
		v1 := vertices[indices[0]]
		v2 := vertices[indices[1]]
		v3 := vertices[indices[2]]
		normal := CalculateTriangleNormal(v1, v2, v3)

		triangle := Triangle{
			Vertex1: v1,
			Vertex2: v2,
			Vertex3: v3,
			Normal:  normal,
		}
		triangles = append(triangles, triangle)
	}

	return triangles
}

type Material struct {
	Name  string
	Kd    [3]float32
	Ks    [3]float32
	Ke    [3]float32
	Ns    float32
	Ni    float32
	D     float32
	Illum int
}

type TriangleMaterial struct {
	Name      string
	Roughness float32
	Metallic  float32
	Emission  float32
	Color     [3]float32
}

func extractMaterials(filename string) ([]Material, error) {
	file, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	var materials []Material
	var current *Material

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		parts := strings.Fields(line)
		if len(parts) == 0 {
			continue
		}

		switch parts[0] {
		case "newmtl":
			if len(parts) < 2 {
				continue
			}
			if current != nil {
				materials = append(materials, *current)
			}
			current = &Material{Name: parts[1]}

		case "Kd":
			if current != nil && len(parts) == 4 {
				current.Kd = [3]float32{parseFloat(parts[1]), parseFloat(parts[2]), parseFloat(parts[3])}
			}
		case "Ks":
			if current != nil && len(parts) == 4 {
				current.Ks = [3]float32{parseFloat(parts[1]), parseFloat(parts[2]), parseFloat(parts[3])}
			}
		case "Ke":
			if current != nil && len(parts) == 4 {
				current.Ke = [3]float32{parseFloat(parts[1]), parseFloat(parts[2]), parseFloat(parts[3])}
			}
		case "Ns":
			if current != nil && len(parts) == 2 {
				current.Ns = parseFloat(parts[1])
			}
		case "Ni":
			if current != nil && len(parts) == 2 {
				current.Ni = parseFloat(parts[1])
			}
		case "d":
			if current != nil && len(parts) == 2 {
				current.D = parseFloat(parts[1])
			}
		case "illum":
			if current != nil && len(parts) == 2 {
				current.Illum = int(parseFloat(parts[1]))
			}
		}
	}

	if current != nil {
		materials = append(materials, *current)
	}

	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return materials, nil
}

func parseFloat(s string) float32 {
	f, _ := strconv.ParseFloat(s, 32)
	return float32(f)
}

func extractTriangleMaterials(filename string) ([]TriangleMaterial, error) {
	materials, err := extractMaterials(filename)
	if err != nil {
		return nil, err
	}

	var triangleMaterials []TriangleMaterial
	for _, mat := range materials {
		roughness := float32(0.99)
		metallic := float32(0.01)
		emission := float32(0.0)

		triangleMaterials = append(triangleMaterials, TriangleMaterial{
			Name:      mat.Name,
			Roughness: roughness,
			Metallic:  metallic,
			Emission:  emission,
			Color:     mat.Kd,
		})
	}

	return triangleMaterials, nil
}

func clamp(val, min, max float32) float32 {
	if val < min {
		return min
	}
	if val > max {
		return max
	}
	return val
}

func parseObjFile(filename string) (*FileObject, error) {
	materialPath := strings.TrimSuffix(filename, ".obj") + ".mtl"
	triangleMaterials, err := extractTriangleMaterials(materialPath)
	if err != nil {
		fmt.Printf("Warning: Could not load materials from %s: %v\n", materialPath, err)
	}

	materialMap := make(map[string]TriangleMaterial)
	for _, mat := range triangleMaterials {
		materialMap[mat.Name] = mat
	}

	file, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	var vertices []Vertex
	var faces [][]int
	var currentMaterial string
	var faceMaterials []string

	scanner := bufio.NewScanner(file)

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		parts := strings.Fields(line)
		if len(parts) == 0 {
			continue
		}

		switch parts[0] {
		case "v":
			if len(parts) >= 4 {
				x, err1 := strconv.ParseFloat(parts[1], 32)
				y, err2 := strconv.ParseFloat(parts[2], 32)
				z, err3 := strconv.ParseFloat(parts[3], 32)
				if err1 == nil && err2 == nil && err3 == nil {
					vertices = append(vertices, Vertex{
						X: float32(x),
						Y: float32(y),
						Z: float32(z),
					})
				}
			}
		case "usemtl":
			if len(parts) >= 2 {
				currentMaterial = parts[1]
			}
		case "f":
			if len(parts) >= 4 {
				var faceIndices []int
				for i := 1; i < len(parts); i++ {
					indexParts := strings.Split(parts[i], "/")
					if len(indexParts) > 0 && indexParts[0] != "" {
						index, err := strconv.Atoi(indexParts[0])
						if err == nil {
							if index < 0 {
								index = len(vertices) + index + 1
							}
							if index > 0 && index <= len(vertices) {
								faceIndices = append(faceIndices, index-1)
							}
						}
					}
				}
				if len(faceIndices) >= 3 {
					faces = append(faces, faceIndices)
					faceMaterials = append(faceMaterials, currentMaterial)
				}
			}
		}
	}

	if err := scanner.Err(); err != nil {
		return nil, err
	}

	var allTriangles []Triangle
	triangleIndex := int32(0)

	for faceIdx, face := range faces {
		var material TriangleMaterial
		if faceIdx < len(faceMaterials) && faceMaterials[faceIdx] != "" {
			if mat, exists := materialMap[faceMaterials[faceIdx]]; exists {
				material = mat
			} else {
				material = TriangleMaterial{
					Name:      "default",
					Roughness: 0.5,
					Metallic:  0.5,
					Emission:  0.5,
					Color:     [3]float32{0.8, 0.8, 0.8},
				}
			}
		} else {
			material = TriangleMaterial{
				Name:      "default",
				Roughness: 0.5,
				Metallic:  0.5,
				Emission:  0.5,
				Color:     [3]float32{0.8, 0.8, 0.8},
			}
		}

		if len(face) == 3 {
			if face[0] >= 0 && face[0] < len(vertices) &&
				face[1] >= 0 && face[1] < len(vertices) &&
				face[2] >= 0 && face[2] < len(vertices) {

				v1, v2, v3 := vertices[face[0]], vertices[face[1]], vertices[face[2]]
				normal := CalculateTriangleNormal(v1, v2, v3)

				triangle := Triangle{
					Vertex1:   v1,
					Vertex2:   v2,
					Vertex3:   v3,
					Normal:    normal,
					Roughness: material.Roughness,
					Metallic:  material.Metallic,
					Emission:  material.Emission,
					Color:     material.Color,
					index:     triangleIndex,
				}
				allTriangles = append(allTriangles, triangle)
				triangleIndex++
			}
		} else if len(face) > 3 {
			faceVertices := make([]Vertex, len(face))
			valid := true
			for i, idx := range face {
				if idx >= 0 && idx < len(vertices) {
					faceVertices[i] = vertices[idx]
				} else {
					valid = false
					break
				}
			}
			if valid {
				triangles := Triangulate(faceVertices)
				for _, tri := range triangles {
					triangle := Triangle{
						Vertex1:   tri.Vertex1,
						Vertex2:   tri.Vertex2,
						Vertex3:   tri.Vertex3,
						Normal:    tri.Normal,
						Roughness: material.Roughness,
						Metallic:  material.Metallic,
						Emission:  material.Emission,
						Color:     material.Color,
						index:     triangleIndex,
					}
					allTriangles = append(allTriangles, triangle)
					triangleIndex++
				}
			}
		}
	}

	fixedCount := EnsureConsistentWinding(allTriangles)
	if fixedCount > 0 {
		fmt.Printf("Fixed winding order for %d triangles\n", fixedCount)
	}

	fileObj := &FileObject{
		Triangles: allTriangles,
	}

	fmt.Printf("Loaded %d triangles\n", len(allTriangles))
	if len(triangleMaterials) > 0 {
		fmt.Printf("Found %d materials in MTL file\n", len(triangleMaterials))
	}

	return fileObj, nil
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

func writeFile(filename string, obj *FileObject, color *[3]float32) error {
	file, err := os.Create(filename)
	if err != nil {
		return err
	}
	defer file.Close()
	w := bufio.NewWriter(file)

	triangleStructSize := uint32(unsafe.Sizeof(Triangle{}))
	fileSize := uint32(8) + uint32(len(obj.Triangles))*triangleStructSize

	w.Write(uint32ToBytes(fileSize))
	w.Write(uint32ToBytes(triangleStructSize))

	for _, tri := range obj.Triangles {
		w.Write(float32ToBytes(tri.Vertex1.X))
		w.Write(float32ToBytes(tri.Vertex1.Y))
		w.Write(float32ToBytes(tri.Vertex1.Z))
		w.Write(float32ToBytes(tri.Vertex2.X))
		w.Write(float32ToBytes(tri.Vertex2.Y))
		w.Write(float32ToBytes(tri.Vertex2.Z))
		w.Write(float32ToBytes(tri.Vertex3.X))
		w.Write(float32ToBytes(tri.Vertex3.Y))
		w.Write(float32ToBytes(tri.Vertex3.Z))
		w.Write(float32ToBytes(tri.Normal.X))
		w.Write(float32ToBytes(tri.Normal.Y))
		w.Write(float32ToBytes(tri.Normal.Z))
		w.Write(float32ToBytes(tri.Roughness))
		w.Write(float32ToBytes(tri.Metallic))
		w.Write(float32ToBytes(tri.Emission))
		if color != nil {
			w.Write(float32ToBytes(clamp(color[0], 0, 1)))
			w.Write(float32ToBytes(clamp(color[1], 0, 1)))
			w.Write(float32ToBytes(clamp(color[2], 0, 1)))
		} else {
			w.Write(float32ToBytes(tri.Color[0]))
			w.Write(float32ToBytes(tri.Color[1]))
			w.Write(float32ToBytes(tri.Color[2]))
		}
		w.Write(uint32ToBytes(uint32(tri.index)))
	}

	return w.Flush()
}

func main() {
	if len(os.Args) < 3 {
		fmt.Println("Usage: parseObj <input.obj> <output.bin>")
		os.Exit(1)
	}

	inputFile := os.Args[1]
	outputFile := os.Args[2]

	obj, err := parseObjFile(inputFile)
	if err != nil {
		fmt.Printf("Error parsing OBJ file: %v\n", err)
		os.Exit(1)
	}

	err = writeFile(outputFile, obj, nil)
	if err != nil {
		fmt.Printf("Error writing output file: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Successfully converted %s to %s\n", inputFile, outputFile)
	fmt.Printf("Total triangles: %d\n", len(obj.Triangles))
}

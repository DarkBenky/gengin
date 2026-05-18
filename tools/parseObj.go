package main

import (
	"bufio"
	"fmt"
	"image"
	_ "image/png"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"
)

const texSize = 4096

type Vertex struct {
	X, Y, Z float32
}

type Triangle struct {
	Vertex1, Vertex2, Vertex3, Normal Vertex
	Roughness, Metallic, Emission     float32
	Color                             [3]float32
	index                             int32
	UV1, UV2, UV3                     [2]uint16
}

type FileObject struct {
	// Header
	FileSize           uint32
	TriangleStructSize uint32
	HasTextures        bool
	// Textures
	ColorMap    [texSize][texSize]uint32   // 64MB RGBA
	NormalMap   [texSize][texSize][3]uint8 // 48MB RGB
	MaterialMap [texSize][texSize][2]uint8 // 32MB [roughness, metallic]
	// Dynamic data
	Triangles []Triangle
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
	Name                           string
	Kd                             [3]float32
	Ks, Ke                         [3]float32
	Ns, Ni, D                      float32
	Illum                          int
	MapKd, MapNs, MapRefl, MapBump string
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
		case "map_Kd":
			if current != nil && len(parts) >= 2 {
				current.MapKd = parts[len(parts)-1]
			}
		case "map_Ns":
			if current != nil && len(parts) >= 2 {
				current.MapNs = parts[len(parts)-1]
			}
		case "map_refl":
			if current != nil && len(parts) >= 2 {
				current.MapRefl = parts[len(parts)-1]
			}
		case "map_Bump", "bump":
			if current != nil && len(parts) >= 2 {
				current.MapBump = parts[len(parts)-1]
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

func encodeUV(u, v float32) [2]uint16 {
	uc := clamp(u, 0, 1)
	vc := clamp(1-v, 0, 1) // flip V: OBJ origin is bottom-left, stored top-left
	return [2]uint16{uint16(uc * 65535), uint16(vc * 65535)}
}

// resizeBilinear resamples img to texSize×texSize using bilinear interpolation.
// Returns separate R, G, B, A channels as uint8 slices of length texSize*texSize.
func resizeBilinear(img image.Image) (rOut, gOut, bOut, aOut []uint8) {
	bounds := img.Bounds()
	srcW := bounds.Max.X - bounds.Min.X
	srcH := bounds.Max.Y - bounds.Min.Y

	// Flatten to RGBA for fast access (avoids repeated interface dispatch in inner loop)
	stride := srcW * 4
	flat := make([]uint8, srcH*stride)
	for y := 0; y < srcH; y++ {
		for x := 0; x < srcW; x++ {
			r, g, b, a := img.At(bounds.Min.X+x, bounds.Min.Y+y).RGBA()
			i := (y*srcW + x) * 4
			flat[i] = uint8(r >> 8)
			flat[i+1] = uint8(g >> 8)
			flat[i+2] = uint8(b >> 8)
			flat[i+3] = uint8(a >> 8)
		}
	}

	n := texSize * texSize
	rOut = make([]uint8, n)
	gOut = make([]uint8, n)
	bOut = make([]uint8, n)
	aOut = make([]uint8, n)

	lerp := func(a, b, t float64) float64 { return a + t*(b-a) }

	for y := 0; y < texSize; y++ {
		for x := 0; x < texSize; x++ {
			srcX := (float64(x)+0.5)*float64(srcW)/float64(texSize) - 0.5
			srcY := (float64(y)+0.5)*float64(srcH)/float64(texSize) - 0.5

			x0 := int(srcX)
			y0 := int(srcY)
			x1 := x0 + 1
			y1 := y0 + 1
			if x0 < 0 {
				x0 = 0
			}
			if y0 < 0 {
				y0 = 0
			}
			if x1 >= srcW {
				x1 = srcW - 1
			}
			if y1 >= srcH {
				y1 = srcH - 1
			}
			fx := srcX - float64(x0)
			fy := srcY - float64(y0)
			if fx < 0 {
				fx = 0
			}
			if fy < 0 {
				fy = 0
			}

			p00 := (y0*srcW + x0) * 4
			p10 := (y0*srcW + x1) * 4
			p01 := (y1*srcW + x0) * 4
			p11 := (y1*srcW + x1) * 4

			dst := y*texSize + x
			rOut[dst] = uint8(lerp(lerp(float64(flat[p00]), float64(flat[p10]), fx), lerp(float64(flat[p01]), float64(flat[p11]), fx), fy))
			gOut[dst] = uint8(lerp(lerp(float64(flat[p00+1]), float64(flat[p10+1]), fx), lerp(float64(flat[p01+1]), float64(flat[p11+1]), fx), fy))
			bOut[dst] = uint8(lerp(lerp(float64(flat[p00+2]), float64(flat[p10+2]), fx), lerp(float64(flat[p01+2]), float64(flat[p11+2]), fx), fy))
			aOut[dst] = uint8(lerp(lerp(float64(flat[p00+3]), float64(flat[p10+3]), fx), lerp(float64(flat[p01+3]), float64(flat[p11+3]), fx), fy))
		}
	}
	return
}

func loadTextures(mtlDir string, mat *Material, obj *FileObject) bool {
	if mat.MapKd == "" && mat.MapNs == "" && mat.MapRefl == "" && mat.MapBump == "" {
		return false
	}

	// Default normal map pointing straight up.
	for y := range texSize {
		for x := range texSize {
			obj.NormalMap[y][x] = [3]uint8{128, 128, 255}
		}
	}

	openAndResize := func(name string) ([]uint8, []uint8, []uint8, []uint8) {
		if name == "" {
			return nil, nil, nil, nil
		}
		f, err := os.Open(filepath.Join(mtlDir, name))
		if err != nil {
			fmt.Printf("Warning: could not open texture %s: %v\n", name, err)
			return nil, nil, nil, nil
		}
		defer f.Close()
		img, _, err := image.Decode(f)
		if err != nil {
			fmt.Printf("Warning: could not decode texture %s: %v\n", name, err)
			return nil, nil, nil, nil
		}
		return resizeBilinear(img)
	}

	if r, g, b, a := openAndResize(mat.MapKd); r != nil {
		for y := range texSize {
			for x := range texSize {
				i := y*texSize + x
				obj.ColorMap[y][x] = uint32(r[i]) | uint32(g[i])<<8 | uint32(b[i])<<16 | uint32(a[i])<<24
			}
		}
	}
	if r, g, b, _ := openAndResize(mat.MapBump); r != nil {
		for y := range texSize {
			for x := range texSize {
				i := y*texSize + x
				obj.NormalMap[y][x] = [3]uint8{r[i], g[i], b[i]}
			}
		}
	}
	if r, _, _, _ := openAndResize(mat.MapNs); r != nil {
		for y := range texSize {
			for x := range texSize {
				obj.MaterialMap[y][x][0] = r[y*texSize+x]
			}
		}
	}
	if r, _, _, _ := openAndResize(mat.MapRefl); r != nil {
		for y := range texSize {
			for x := range texSize {
				obj.MaterialMap[y][x][1] = r[y*texSize+x]
			}
		}
	}

	return true
}

func parseObjFile(filename string) (*FileObject, error) {
	materialPath := strings.TrimSuffix(filename, ".obj") + ".mtl"
	materials, err := extractMaterials(materialPath)
	if err != nil {
		fmt.Printf("Warning: Could not load materials from %s: %v\n", materialPath, err)
	}

	triangleMaterials, _ := extractTriangleMaterials(materialPath)
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
	var normals []Vertex
	var uvCoords [][2]float32
	var faces [][]int
	var faceNormals [][]int
	var faceUVs [][]int
	var currentMaterial string
	var faceMaterials []string

	scanner := bufio.NewScanner(bufio.NewReaderSize(file, 1<<20))

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
					vertices = append(vertices, Vertex{float32(x), float32(y), float32(z)})
				}
			}
		case "vt":
			if len(parts) >= 3 {
				u, err1 := strconv.ParseFloat(parts[1], 32)
				v, err2 := strconv.ParseFloat(parts[2], 32)
				if err1 == nil && err2 == nil {
					uvCoords = append(uvCoords, [2]float32{float32(u), float32(v)})
				}
			}
		case "vn":
			if len(parts) >= 4 {
				x, err1 := strconv.ParseFloat(parts[1], 32)
				y, err2 := strconv.ParseFloat(parts[2], 32)
				z, err3 := strconv.ParseFloat(parts[3], 32)
				if err1 == nil && err2 == nil && err3 == nil {
					normals = append(normals, Normalize(Vertex{float32(x), float32(y), float32(z)}))
				}
			}
		case "usemtl":
			if len(parts) >= 2 {
				currentMaterial = parts[1]
			}
		case "f":
			if len(parts) >= 4 {
				var faceIndices, faceNormalIndices, faceUVIndices []int
				for i := 1; i < len(parts); i++ {
					tok := strings.Split(parts[i], "/")
					if len(tok) > 0 && tok[0] != "" {
						idx, err := strconv.Atoi(tok[0])
						if err == nil {
							if idx < 0 {
								idx = len(vertices) + idx + 1
							}
							if idx > 0 && idx <= len(vertices) {
								faceIndices = append(faceIndices, idx-1)
							}
						}
					}
					if len(tok) >= 2 && tok[1] != "" {
						ui, err := strconv.Atoi(tok[1])
						if err == nil {
							if ui < 0 {
								ui = len(uvCoords) + ui + 1
							}
							faceUVIndices = append(faceUVIndices, ui-1)
						}
					}
					if len(tok) >= 3 && tok[2] != "" {
						ni, err := strconv.Atoi(tok[2])
						if err == nil {
							if ni < 0 {
								ni = len(normals) + ni + 1
							}
							faceNormalIndices = append(faceNormalIndices, ni-1)
						}
					}
				}
				if len(faceIndices) >= 3 {
					faces = append(faces, faceIndices)
					faceNormals = append(faceNormals, faceNormalIndices)
					faceUVs = append(faceUVs, faceUVIndices)
					faceMaterials = append(faceMaterials, currentMaterial)
				}
			}
		}
	}

	if err := scanner.Err(); err != nil {
		return nil, err
	}

	defaultMaterial := TriangleMaterial{
		Name:      "default",
		Roughness: 0.5,
		Metallic:  0.5,
		Emission:  0.0,
		Color:     [3]float32{0.8, 0.8, 0.8},
	}

	faceNormal := func(faceIdx int, verts [3]Vertex) Vertex {
		fn := faceNormals[faceIdx]
		n := len(fn)
		if n >= 3 && fn[0] >= 0 && fn[0] < len(normals) &&
			fn[1] >= 0 && fn[1] < len(normals) &&
			fn[2] >= 0 && fn[2] < len(normals) {
			return Normalize(Vertex{
				X: (normals[fn[0]].X + normals[fn[1]].X + normals[fn[2]].X) / 3,
				Y: (normals[fn[0]].Y + normals[fn[1]].Y + normals[fn[2]].Y) / 3,
				Z: (normals[fn[0]].Z + normals[fn[1]].Z + normals[fn[2]].Z) / 3,
			})
		}
		return CalculateTriangleNormal(verts[0], verts[1], verts[2])
	}

	faceUVAt := func(faceIdx, slot int) [2]uint16 {
		fu := faceUVs[faceIdx]
		if slot < len(fu) && fu[slot] >= 0 && fu[slot] < len(uvCoords) {
			uv := uvCoords[fu[slot]]
			return encodeUV(uv[0], uv[1])
		}
		return [2]uint16{}
	}

	var allTriangles []Triangle
	triangleIndex := int32(0)

	for faceIdx, face := range faces {
		material := defaultMaterial
		if faceIdx < len(faceMaterials) && faceMaterials[faceIdx] != "" {
			if mat, exists := materialMap[faceMaterials[faceIdx]]; exists {
				material = mat
			}
		}

		if len(face) == 3 {
			if face[0] < 0 || face[0] >= len(vertices) ||
				face[1] < 0 || face[1] >= len(vertices) ||
				face[2] < 0 || face[2] >= len(vertices) {
				continue
			}
			v1, v2, v3 := vertices[face[0]], vertices[face[1]], vertices[face[2]]
			triangle := Triangle{
				Vertex1:   v1,
				Vertex2:   v2,
				Vertex3:   v3,
				Normal:    faceNormal(faceIdx, [3]Vertex{v1, v2, v3}),
				Roughness: material.Roughness,
				Metallic:  material.Metallic,
				Emission:  material.Emission,
				Color:     material.Color,
				index:     triangleIndex,
				UV1:       faceUVAt(faceIdx, 0),
				UV2:       faceUVAt(faceIdx, 1),
				UV3:       faceUVAt(faceIdx, 2),
			}
			allTriangles = append(allTriangles, triangle)
			triangleIndex++
		} else if len(face) > 3 {
			// Fan triangulation from vertex 0: correct UV mapping for convex polygons.
			valid := true
			for _, idx := range face {
				if idx < 0 || idx >= len(vertices) {
					valid = false
					break
				}
			}
			if !valid {
				continue
			}
			fu := faceUVs[faceIdx]
			fn := faceNormals[faceIdx]
			for i := 1; i < len(face)-1; i++ {
				i0, i1, i2 := 0, i, i+1
				v1, v2, v3 := vertices[face[i0]], vertices[face[i1]], vertices[face[i2]]

				var normal Vertex
				if len(fn) == len(face) &&
					fn[i0] >= 0 && fn[i0] < len(normals) &&
					fn[i1] >= 0 && fn[i1] < len(normals) &&
					fn[i2] >= 0 && fn[i2] < len(normals) {
					normal = Normalize(Vertex{
						X: (normals[fn[i0]].X + normals[fn[i1]].X + normals[fn[i2]].X) / 3,
						Y: (normals[fn[i0]].Y + normals[fn[i1]].Y + normals[fn[i2]].Y) / 3,
						Z: (normals[fn[i0]].Z + normals[fn[i1]].Z + normals[fn[i2]].Z) / 3,
					})
				} else {
					normal = CalculateTriangleNormal(v1, v2, v3)
				}

				uvAt := func(slot int) [2]uint16 {
					if slot < len(fu) && fu[slot] >= 0 && fu[slot] < len(uvCoords) {
						uv := uvCoords[fu[slot]]
						return encodeUV(uv[0], uv[1])
					}
					return [2]uint16{}
				}

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
					UV1:       uvAt(i0),
					UV2:       uvAt(i1),
					UV3:       uvAt(i2),
				}
				allTriangles = append(allTriangles, triangle)
				triangleIndex++
			}
		}
	}

	fileObj := &FileObject{
		Triangles: allTriangles,
	}

	// Load textures from the first material that references texture maps.
	mtlDir := filepath.Dir(materialPath)
	for i := range materials {
		if materials[i].MapKd != "" || materials[i].MapBump != "" ||
			materials[i].MapNs != "" || materials[i].MapRefl != "" {
			fmt.Printf("Loading textures from material %q\n", materials[i].Name)
			fileObj.HasTextures = loadTextures(mtlDir, &materials[i], fileObj)
			break
		}
	}

	fmt.Printf("Loaded %d triangles\n", len(allTriangles))
	if len(triangleMaterials) > 0 {
		fmt.Printf("Found %d materials in MTL file\n", len(triangleMaterials))
	}
	fmt.Printf("UV coordinates: %d\n", len(uvCoords))

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

func uint16ToBytes(value uint16) []byte {
	return []byte{byte(value & 0xFF), byte(value >> 8)}
}

func writeFile(filename string, obj *FileObject, color *[3]float32) error {
	file, err := os.Create(filename)
	if err != nil {
		return err
	}
	defer file.Close()
	w := bufio.NewWriterSize(file, 1<<20)

	// Binary layout:
	// Header (16 bytes):
	//   fileSize          uint32
	//   triangleStructSize uint32  — 108 bytes
	//   triangleCount     uint32
	//   hasTextures       uint32   — 0 or 1
	//
	// Texture data (if hasTextures):
	//   ColorMap     texSize*texSize*4 bytes (RGBA uint8)
	//   NormalMap    texSize*texSize*3 bytes (RGB uint8)
	//   MaterialMap  texSize*texSize*2 bytes ([roughness, metallic] uint8)
	//
	// Per triangle (108 bytes):
	//   v1 v2 v3 normal   4 × float3 padded to float4 = 4×16 = 64
	//   roughness metallic emission                    = 12
	//   color float3 padded                            = 16
	//   uv1[2] uv2[2] uv3[2] + 4 byte pad             = 16

	const triangleStructSize = uint32(4*16 + 12 + 16 + 16) // 108
	triCount := uint32(len(obj.Triangles))
	hasTextures := uint32(0)
	if obj.HasTextures {
		hasTextures = 1
	}

	textureBytes := uint32(0)
	if obj.HasTextures {
		textureBytes = texSize * texSize * (4 + 3 + 1 + 1)
	}
	fileSize := uint32(16) + triCount*triangleStructSize + textureBytes

	w.Write(uint32ToBytes(fileSize))
	w.Write(uint32ToBytes(triangleStructSize))
	w.Write(uint32ToBytes(triCount))
	w.Write(uint32ToBytes(hasTextures))

	if obj.HasTextures {
		for y := range texSize {
			for x := range texSize {
				p := obj.ColorMap[y][x]
				w.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16), byte(p >> 24)})
			}
		}
		for y := range texSize {
			for x := range texSize {
				w.Write(obj.NormalMap[y][x][:])
			}
		}
		for y := range texSize {
			for x := range texSize {
				w.Write(obj.MaterialMap[y][x][:])
			}
		}
	}

	zero4 := float32ToBytes(0)
	zeroPad := []byte{0, 0, 0, 0}

	for _, tri := range obj.Triangles {
		w.Write(float32ToBytes(tri.Vertex1.X))
		w.Write(float32ToBytes(tri.Vertex1.Y))
		w.Write(float32ToBytes(tri.Vertex1.Z))
		w.Write(zero4)
		w.Write(float32ToBytes(tri.Vertex2.X))
		w.Write(float32ToBytes(tri.Vertex2.Y))
		w.Write(float32ToBytes(tri.Vertex2.Z))
		w.Write(zero4)
		w.Write(float32ToBytes(tri.Vertex3.X))
		w.Write(float32ToBytes(tri.Vertex3.Y))
		w.Write(float32ToBytes(tri.Vertex3.Z))
		w.Write(zero4)
		w.Write(float32ToBytes(tri.Normal.X))
		w.Write(float32ToBytes(tri.Normal.Y))
		w.Write(float32ToBytes(tri.Normal.Z))
		w.Write(zero4)
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
		w.Write(zero4) // color padding
		w.Write(uint16ToBytes(tri.UV1[0]))
		w.Write(uint16ToBytes(tri.UV1[1]))
		w.Write(uint16ToBytes(tri.UV2[0]))
		w.Write(uint16ToBytes(tri.UV2[1]))
		w.Write(uint16ToBytes(tri.UV3[0]))
		w.Write(uint16ToBytes(tri.UV3[1]))
		w.Write(zeroPad) // UV block padding
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

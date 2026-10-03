package main

import (
	"bufio"
	"fmt"
	"image"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"
)

const defaultTexSize = 4096

const (
	boostSatMax    = 1.8
	boostBrightMax = 1.35
)

type Vertex struct {
	X, Y, Z float32
}

type Triangle struct {
	Vertex1, Vertex2, Vertex3, Normal Vertex
	Roughness, Metallic, Emission     float32
	Color                             [3]float32
	index                             int32
	matSlot                           int
	UV1, UV2, UV3                     [2]uint16
}

type FileObject struct {
	// Header
	FileSize           uint32
	TriangleStructSize uint32
	HasTextures        bool
	TexSize            int
	// Textures
	ColorMap    []uint32 // TexSize*TexSize RGBA
	NormalMap   []uint8  // TexSize*TexSize RGB
	MaterialMap []uint8  // TexSize*TexSize [roughness, metallic]
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
	Pr, Pm                         float32
	HasPr, HasPm                   bool
	Illum                          int
	MapKd, MapNs, MapRefl, MapBump string
	MapPr, MapPm                   string
}

// matInfo is the per-material data the converter needs: values for the
// per-triangle attributes plus the texture sources packed into the atlas.
type matInfo struct {
	name       string
	color      [3]float32
	roughness  float32
	metallic   float32
	baseColor  string
	normal     string
	metalRough string // packed glTF metallic-roughness map: G=roughness, B=metallic
	roughMap   string // single-channel roughness (MTL map_Ns / map_Pr)
	metalMap   string // single-channel metallic (MTL map_refl / map_Pm)
	slot       int
	cx, cy     int
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
		case "Pr":
			if current != nil && len(parts) == 2 {
				current.Pr = parseFloat(parts[1])
				current.HasPr = true
			}
		case "Pm":
			if current != nil && len(parts) == 2 {
				current.Pm = parseFloat(parts[1])
				current.HasPm = true
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
		case "map_Pr":
			if current != nil && len(parts) >= 2 {
				current.MapPr = parts[len(parts)-1]
			}
		case "map_Pm":
			if current != nil && len(parts) >= 2 {
				current.MapPm = parts[len(parts)-1]
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

// probeFile resolves maps by the common glTF export naming convention
// ({material}_{suffix}) for MTLs that do not reference every map — Blender
// writes map_Bump but not map_Kd when the image is not linked to Base Color.
func probeFile(dir, base string) string {
	if base == "" {
		return ""
	}
	for _, ext := range []string{".png", ".jpg", ".jpeg"} {
		name := base + ext
		if st, err := os.Stat(filepath.Join(dir, name)); err == nil && !st.IsDir() {
			return name
		}
	}
	return ""
}

func buildMaterialInfos(materials []Material, mtlDir string) []matInfo {
	infos := make([]matInfo, 0, len(materials))
	for _, mat := range materials {
		roughness := float32(0.5)
		if mat.HasPr {
			roughness = clamp(mat.Pr, 0, 1)
		} else if mat.Ns > 0 {
			roughness = clamp(1.0-mat.Ns/1000.0, 0.02, 1)
		}
		metallic := float32(0)
		if mat.HasPm {
			metallic = clamp(mat.Pm, 0, 1)
		}

		inf := matInfo{
			name:      mat.Name,
			color:     mat.Kd,
			roughness: roughness,
			metallic:  metallic,
			baseColor: mat.MapKd,
			normal:    mat.MapBump,
			roughMap:  mat.MapNs,
			metalMap:  mat.MapRefl,
			slot:      -1,
		}
		if inf.roughMap == "" {
			inf.roughMap = mat.MapPr
		}
		if inf.metalMap == "" {
			inf.metalMap = mat.MapPm
		}
		if inf.baseColor == "" {
			inf.baseColor = probeFile(mtlDir, mat.Name+"_baseColor")
		}
		if inf.normal == "" {
			inf.normal = probeFile(mtlDir, mat.Name+"_normal")
		}
		if inf.roughMap == "" && inf.metalMap == "" {
			inf.metalRough = probeFile(mtlDir, mat.Name+"_metallicRoughness")
		}
		infos = append(infos, inf)
	}
	return infos
}

// atlasHasSources reports whether any material references a texture; when false
// the model is converted without textures, exactly as before.
func atlasHasSources(infos []matInfo) bool {
	for i := range infos {
		if infos[i].baseColor != "" || infos[i].normal != "" || infos[i].metalRough != "" ||
			infos[i].roughMap != "" || infos[i].metalMap != "" {
			return true
		}
	}
	return false
}

// assignAtlasSlots gives every material one grid cell in the atlas. The grid is
// rounded up to a divisor of texSize so cells divide evenly: 25 materials in a
// 4096 atlas -> 8x8 grid of 512px cells, or 5x5 native cells in a 2560 atlas.
func assignAtlasSlots(infos []matInfo, texSize int) (grid, cell int) {
	grid = 1
	for grid*grid < len(infos) {
		grid++
	}
	for texSize%grid != 0 && grid < 64 {
		grid++
	}
	cell = texSize / grid
	for i := range infos {
		infos[i].slot = i
		infos[i].cx = i % grid
		infos[i].cy = i / grid
	}
	return grid, cell
}

// remapTriangleUVs scales each triangle's stored UVs into its material's cell,
// computing in texture-pixel space so cell edges land exactly on the cell's
// first/last pixel (a plain [0,1] scale makes u=0 sample the previous cell's
// last pixel column).
func remapTriangleUVs(triangles []Triangle, infos []matInfo, cell int, texSize int) {
	// keep samples off the far cell edge so a UV of exactly 1 cannot bleed into
	// the neighbouring cell
	const edge = 0.9995
	lastPixel := float64(texSize - 1)
	storedPerPixel := 65535.0 / lastPixel
	remap := func(v uint16, cellIndex int) uint16 {
		t := float32(v) / 65535.0
		if t > edge {
			t = edge
		}
		p := float64(cellIndex*cell) + float64(t)*float64(cell)
		if p > lastPixel {
			p = lastPixel
		}
		s := math.Floor(p*storedPerPixel) + 1
		if s > 65535 {
			s = 65535
		}
		return uint16(s)
	}
	for i := range triangles {
		t := &triangles[i]
		if t.matSlot < 0 || t.matSlot >= len(infos) {
			continue
		}
		inf := &infos[t.matSlot]
		t.UV1 = [2]uint16{remap(t.UV1[0], inf.cx), remap(t.UV1[1], inf.cy)}
		t.UV2 = [2]uint16{remap(t.UV2[0], inf.cx), remap(t.UV2[1], inf.cy)}
		t.UV3 = [2]uint16{remap(t.UV3[0], inf.cx), remap(t.UV3[1], inf.cy)}
	}
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

func boostFactors(boost float32) (satFactor, brightFactor float32) {
	return 1 + (boostSatMax-1)*boost, 1 + (boostBrightMax-1)*boost
}

// boostColor pushes channels away from luminance (saturation), then scales brightness.
func boostColor(p uint32, satFactor, brightFactor float32) uint32 {
	if satFactor == 1 && brightFactor == 1 {
		return p
	}
	rf := float32(p&0xFF) / 255.0
	gf := float32((p>>8)&0xFF) / 255.0
	bf := float32((p>>16)&0xFF) / 255.0
	alpha := uint8((p >> 24) & 0xFF)
	lum := 0.299*rf + 0.587*gf + 0.114*bf
	red := uint8(clamp((lum+satFactor*(rf-lum))*brightFactor*255.0, 0, 255))
	green := uint8(clamp((lum+satFactor*(gf-lum))*brightFactor*255.0, 0, 255))
	blue := uint8(clamp((lum+satFactor*(bf-lum))*brightFactor*255.0, 0, 255))
	return uint32(red) | uint32(green)<<8 | uint32(blue)<<16 | uint32(alpha)<<24
}

func encodeUV(u, v float32) [2]uint16 {
	uc := clamp(u, 0, 1)
	vc := clamp(1-v, 0, 1) // flip V: OBJ origin is bottom-left, stored top-left
	return [2]uint16{uint16(uc * 65535), uint16(vc * 65535)}
}

func minF(a, b float32) float32 {
	if a < b {
		return a
	}
	return b
}

func maxF(a, b float32) float32 {
	if a > b {
		return a
	}
	return b
}

type uvPoint struct {
	u, v float32
}

// clipPolygonUV runs one Sutherland-Hodgman half-plane clip.
func clipPolygonUV(poly []uvPoint, inside func(uvPoint) bool, intersect func(a, b uvPoint) uvPoint) []uvPoint {
	if len(poly) == 0 {
		return nil
	}
	out := make([]uvPoint, 0, len(poly)+1)
	n := len(poly)
	for i := 0; i < n; i++ {
		cur := poly[i]
		prev := poly[(i+n-1)%n]
		curIn := inside(cur)
		prevIn := inside(prev)
		if curIn {
			if !prevIn {
				out = append(out, intersect(prev, cur))
			}
			out = append(out, cur)
		} else if prevIn {
			out = append(out, intersect(prev, cur))
		}
	}
	return out
}

func clipPolygonUVToRect(poly []uvPoint, u0, u1, v0, v1 float32) []uvPoint {
	lerp := func(a, b uvPoint, t float32) uvPoint {
		return uvPoint{a.u + (b.u-a.u)*t, a.v + (b.v-a.v)*t}
	}
	poly = clipPolygonUV(poly, func(p uvPoint) bool { return p.u >= u0 }, func(a, b uvPoint) uvPoint {
		d := b.u - a.u
		if d == 0 {
			return a
		}
		return lerp(a, b, (u0-a.u)/d)
	})
	poly = clipPolygonUV(poly, func(p uvPoint) bool { return p.u <= u1 }, func(a, b uvPoint) uvPoint {
		d := b.u - a.u
		if d == 0 {
			return a
		}
		return lerp(a, b, (u1-a.u)/d)
	})
	poly = clipPolygonUV(poly, func(p uvPoint) bool { return p.v >= v0 }, func(a, b uvPoint) uvPoint {
		d := b.v - a.v
		if d == 0 {
			return a
		}
		return lerp(a, b, (v0-a.v)/d)
	})
	poly = clipPolygonUV(poly, func(p uvPoint) bool { return p.v <= v1 }, func(a, b uvPoint) uvPoint {
		d := b.v - a.v
		if d == 0 {
			return a
		}
		return lerp(a, b, (v1-a.v)/d)
	})
	return poly
}

// appendUnfoldedTriangle emits one triangle, split along integer UV tile borders
// so every emitted triangle lies within a single tile, which is then folded to
// [0,1]. The .bin stores per-vertex UVs normalized to [0,1], so tiled UVs (walls
// repeating one texture many times) would otherwise clamp to the texture edge
// and smear; folding keeps the tiling intact (one repeat per atlas cell).
func appendUnfoldedTriangle(out *[]Triangle, triIndex *int32, v1, v2, v3, normal Vertex, uv1, uv2, uv3 [2]float32, unfold bool, mat matInfo, slot int) {
	push := func(a, b, c Vertex, t1, t2, t3 [2]float32) {
		*out = append(*out, Triangle{
			Vertex1:   a,
			Vertex2:   b,
			Vertex3:   c,
			Normal:    normal,
			Roughness: mat.roughness,
			Metallic:  mat.metallic,
			Color:     mat.color,
			index:     *triIndex,
			matSlot:   slot,
			UV1:       encodeUV(t1[0], t1[1]),
			UV2:       encodeUV(t2[0], t2[1]),
			UV3:       encodeUV(t3[0], t3[1]),
		})
		*triIndex++
	}

	if !unfold {
		push(v1, v2, v3, uv1, uv2, uv3)
		return
	}

	// fold the triangle into the tile containing its minimum UV
	baseU := float32(math.Floor(float64(minF(uv1[0], minF(uv2[0], uv3[0])))))
	baseV := float32(math.Floor(float64(minF(uv1[1], minF(uv2[1], uv3[1])))))
	a1 := [2]float32{uv1[0] - baseU, uv1[1] - baseV}
	a2 := [2]float32{uv2[0] - baseU, uv2[1] - baseV}
	a3 := [2]float32{uv3[0] - baseU, uv3[1] - baseV}
	maxU := maxF(maxF(a1[0], a2[0]), a3[0])
	maxV := maxF(maxF(a1[1], a2[1]), a3[1])

	if maxU <= 1.0001 && maxV <= 1.0001 {
		push(v1, v2, v3, a1, a2, a3)
		return
	}

	denom := (a2[1]-a3[1])*(a1[0]-a3[0]) + (a3[0]-a2[0])*(a1[1]-a3[1])
	iMax := int(math.Floor(float64(maxU)))
	jMax := int(math.Floor(float64(maxV)))
	if denom == 0 || (iMax+1)*(jMax+1) > 256 {
		// degenerate or pathological UVs: fall back to the clamped single triangle
		push(v1, v2, v3, a1, a2, a3)
		return
	}

	triUV := []uvPoint{{a1[0], a1[1]}, {a2[0], a2[1]}, {a3[0], a3[1]}}
	for i := 0; i <= iMax; i++ {
		for j := 0; j <= jMax; j++ {
			poly := clipPolygonUVToRect(append([]uvPoint(nil), triUV...), float32(i), float32(i+1), float32(j), float32(j+1))
			if len(poly) < 3 {
				continue
			}
			for k := 1; k < len(poly)-1; k++ {
				p := [3]uvPoint{poly[0], poly[k], poly[k+1]}
				var verts [3]Vertex
				var uvs [3][2]float32
				for m := 0; m < 3; m++ {
					w1 := ((a2[1]-a3[1])*(p[m].u-a3[0]) + (a3[0]-a2[0])*(p[m].v-a3[1])) / denom
					w2 := ((a3[1]-a1[1])*(p[m].u-a3[0]) + (a1[0]-a3[0])*(p[m].v-a3[1])) / denom
					w3 := 1 - w1 - w2
					verts[m] = Vertex{
						X: v1.X*w1 + v2.X*w2 + v3.X*w3,
						Y: v1.Y*w1 + v2.Y*w2 + v3.Y*w3,
						Z: v1.Z*w1 + v2.Z*w2 + v3.Z*w3,
					}
					uvs[m] = [2]float32{p[m].u - float32(i), p[m].v - float32(j)}
				}
				push(verts[0], verts[1], verts[2], uvs[0], uvs[1], uvs[2])
			}
		}
	}
}

// resizeBilinear resamples img to outW×outH using bilinear interpolation.
// Returns separate R, G, B, A channels as uint8 slices of length outW*outH.
func resizeBilinear(img image.Image, outW, outH int) (rOut, gOut, bOut, aOut []uint8) {
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

	n := outW * outH
	rOut = make([]uint8, n)
	gOut = make([]uint8, n)
	bOut = make([]uint8, n)
	aOut = make([]uint8, n)

	lerp := func(a, b, t float64) float64 { return a + t*(b-a) }

	for y := 0; y < outH; y++ {
		for x := 0; x < outW; x++ {
			srcX := (float64(x)+0.5)*float64(srcW)/float64(outW) - 0.5
			srcY := (float64(y)+0.5)*float64(srcH)/float64(outH) - 0.5

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

			dst := y*outW + x
			rOut[dst] = uint8(lerp(lerp(float64(flat[p00]), float64(flat[p10]), fx), lerp(float64(flat[p01]), float64(flat[p11]), fx), fy))
			gOut[dst] = uint8(lerp(lerp(float64(flat[p00+1]), float64(flat[p10+1]), fx), lerp(float64(flat[p01+1]), float64(flat[p11+1]), fx), fy))
			bOut[dst] = uint8(lerp(lerp(float64(flat[p00+2]), float64(flat[p10+2]), fx), lerp(float64(flat[p01+2]), float64(flat[p11+2]), fx), fy))
			aOut[dst] = uint8(lerp(lerp(float64(flat[p00+3]), float64(flat[p10+3]), fx), lerp(float64(flat[p01+3]), float64(flat[p11+3]), fx), fy))
		}
	}
	return
}

// buildAtlas packs every material's maps into the single texture atlas the
// .bin format supports, one grid cell per material. The caller assigns the
// slots/cells first and remaps the triangle UVs to them. Materials without a
// given map fall back to their Kd color / a flat normal / Pr-Pm scalars so no
// texel is ever left letting the renderer read black + roughness 0 (mirror).
// Returns true if at least one texture image was loaded.
func buildAtlas(mtlDir string, infos []matInfo, obj *FileObject, cell int) bool {
	texSize := obj.TexSize
	loadedAny := false
	obj.ColorMap = make([]uint32, texSize*texSize)
	obj.NormalMap = make([]uint8, texSize*texSize*3)
	obj.MaterialMap = make([]uint8, texSize*texSize*2)

	// default normal map pointing straight up, for every cell that has none
	for i := 0; i < texSize*texSize; i++ {
		obj.NormalMap[i*3] = 128
		obj.NormalMap[i*3+1] = 128
		obj.NormalMap[i*3+2] = 255
	}

	load := func(name string) ([]uint8, []uint8, []uint8, []uint8, bool) {
		if name == "" {
			return nil, nil, nil, nil, false
		}
		f, err := os.Open(filepath.Join(mtlDir, name))
		if err != nil {
			fmt.Printf("Warning: could not open texture %s: %v\n", name, err)
			return nil, nil, nil, nil, false
		}
		defer f.Close()
		img, _, err := image.Decode(f)
		if err != nil {
			fmt.Printf("Warning: could not decode texture %s: %v\n", name, err)
			return nil, nil, nil, nil, false
		}
		r, g, b, a := resizeBilinear(img, cell, cell)
		return r, g, b, a, true
	}

	for i := range infos {
		inf := &infos[i]
		ox, oy := inf.cx*cell, inf.cy*cell

		// base color: image if available, otherwise the MTL Kd color
		cr := uint8(clamp(inf.color[0], 0, 1) * 255.0)
		cg := uint8(clamp(inf.color[1], 0, 1) * 255.0)
		cb := uint8(clamp(inf.color[2], 0, 1) * 255.0)
		for y := 0; y < cell; y++ {
			for x := 0; x < cell; x++ {
				obj.ColorMap[(oy+y)*texSize+ox+x] = uint32(cr) | uint32(cg)<<8 | uint32(cb)<<16 | 0xFF000000
			}
		}
		if r, g, b, a, ok := load(inf.baseColor); ok {
			loadedAny = true
			for y := 0; y < cell; y++ {
				for x := 0; x < cell; x++ {
					j := y*cell + x
					obj.ColorMap[(oy+y)*texSize+ox+x] = uint32(r[j]) | uint32(g[j])<<8 | uint32(b[j])<<16 | uint32(a[j])<<24
				}
			}
		}

		// normal map: image if available, otherwise the flat default stays
		if r, g, b, _, ok := load(inf.normal); ok {
			loadedAny = true
			for y := 0; y < cell; y++ {
				for x := 0; x < cell; x++ {
					j := y*cell + x
					dst := ((oy+y)*texSize + ox + x) * 3
					obj.NormalMap[dst] = r[j]
					obj.NormalMap[dst+1] = g[j]
					obj.NormalMap[dst+2] = b[j]
				}
			}
		}

		// roughness/metallic: scalar fallback, then maps override per channel
		roughByte := uint8(clamp(inf.roughness, 0, 1) * 255.0)
		metalByte := uint8(clamp(inf.metallic, 0, 1) * 255.0)
		for y := 0; y < cell; y++ {
			for x := 0; x < cell; x++ {
				dst := ((oy+y)*texSize + ox + x) * 2
				obj.MaterialMap[dst] = roughByte
				obj.MaterialMap[dst+1] = metalByte
			}
		}
		if inf.metalRough != "" {
			if _, g, b, _, ok := load(inf.metalRough); ok {
				loadedAny = true
				for y := 0; y < cell; y++ {
					for x := 0; x < cell; x++ {
						j := y*cell + x
						dst := ((oy+y)*texSize + ox + x) * 2
						obj.MaterialMap[dst] = g[j] // glTF pack: G=roughness, B=metallic
						obj.MaterialMap[dst+1] = b[j]
					}
				}
			}
		} else if inf.roughMap != "" || inf.metalMap != "" {
			var roughPix, metalPix []uint8
			if r, _, _, _, ok := load(inf.roughMap); ok {
				roughPix = r
				loadedAny = true
			}
			if r, _, _, _, ok := load(inf.metalMap); ok {
				metalPix = r
				loadedAny = true
			}
			for y := 0; y < cell; y++ {
				for x := 0; x < cell; x++ {
					j := y*cell + x
					rb, mb := roughByte, metalByte
					if roughPix != nil {
						rb = roughPix[j]
					}
					if metalPix != nil {
						mb = metalPix[j]
					}
					dst := ((oy+y)*texSize + ox + x) * 2
					obj.MaterialMap[dst] = rb
					obj.MaterialMap[dst+1] = mb
				}
			}
		}
	}

	return loadedAny
}

func parseObjFile(filename string, texSize int) (*FileObject, error) {
	materialPath := strings.TrimSuffix(filename, ".obj") + ".mtl"
	materials, err := extractMaterials(materialPath)
	if err != nil {
		fmt.Printf("Warning: Could not load materials from %s: %v\n", materialPath, err)
	}

	mtlDir := filepath.Dir(materialPath)
	infos := buildMaterialInfos(materials, mtlDir)

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

	defaultMaterial := matInfo{
		name:      "default",
		roughness: 0.5,
		metallic:  0.5,
		color:     [3]float32{0.8, 0.8, 0.8},
		slot:      -1,
	}

	slotByName := make(map[string]int, len(infos))
	for i := range infos {
		slotByName[infos[i].name] = i
	}
	hasUnknownMaterial := false
	for _, name := range faceMaterials {
		if name == "" {
			continue
		}
		if _, ok := slotByName[name]; !ok {
			hasUnknownMaterial = true
			break
		}
	}
	if hasUnknownMaterial {
		infos = append(infos, defaultMaterial)
		slotByName["__default__"] = len(infos) - 1
	}
	resolveMaterial := func(name string) (int, matInfo) {
		if idx, ok := slotByName[name]; ok {
			return idx, infos[idx]
		}
		if idx, ok := slotByName["__default__"]; ok {
			return idx, infos[idx]
		}
		return -1, defaultMaterial
	}
	useAtlas := atlasHasSources(infos)

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

	faceUVAt := func(faceIdx, slot int) ([2]float32, bool) {
		fu := faceUVs[faceIdx]
		if slot < len(fu) && fu[slot] >= 0 && fu[slot] < len(uvCoords) {
			return uvCoords[fu[slot]], true
		}
		return [2]float32{}, false
	}

	var allTriangles []Triangle
	triangleIndex := int32(0)

	for faceIdx, face := range faces {
		slot := -1
		material := defaultMaterial
		if faceIdx < len(faceMaterials) && faceMaterials[faceIdx] != "" {
			slot, material = resolveMaterial(faceMaterials[faceIdx])
		}

		if len(face) == 3 {
			if face[0] < 0 || face[0] >= len(vertices) ||
				face[1] < 0 || face[1] >= len(vertices) ||
				face[2] < 0 || face[2] >= len(vertices) {
				continue
			}
			v1, v2, v3 := vertices[face[0]], vertices[face[1]], vertices[face[2]]
			uvA, okA := faceUVAt(faceIdx, 0)
			uvB, okB := faceUVAt(faceIdx, 1)
			uvC, okC := faceUVAt(faceIdx, 2)
			appendUnfoldedTriangle(&allTriangles, &triangleIndex, v1, v2, v3,
				faceNormal(faceIdx, [3]Vertex{v1, v2, v3}), uvA, uvB, uvC, useAtlas && okA && okB && okC, material, slot)
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

				uvA, okA := faceUVAt(faceIdx, i0)
				uvB, okB := faceUVAt(faceIdx, i1)
				uvC, okC := faceUVAt(faceIdx, i2)
				appendUnfoldedTriangle(&allTriangles, &triangleIndex, v1, v2, v3, normal,
					uvA, uvB, uvC, useAtlas && okA && okB && okC, material, slot)
			}
		}
	}

	fileObj := &FileObject{
		TexSize:   texSize,
		Triangles: allTriangles,
	}

	if atlasHasSources(infos) {
		grid, cell := assignAtlasSlots(infos, texSize)
		fileObj.HasTextures = buildAtlas(mtlDir, infos, fileObj, cell)
		if fileObj.HasTextures {
			remapTriangleUVs(fileObj.Triangles, infos, cell, texSize)
			fmt.Printf("Packed %d materials into a %dx%d atlas (%dpx cells)\n", len(infos), grid, grid, cell)
		}
	}

	fmt.Printf("Loaded %d triangles\n", len(allTriangles))
	if len(materials) > 0 {
		fmt.Printf("Found %d materials in MTL file\n", len(materials))
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

func writeFile(filename string, obj *FileObject, color *[3]float32, boost float32, cull uint32) error {
	file, err := os.Create(filename)
	if err != nil {
		return err
	}
	defer file.Close()
	w := bufio.NewWriterSize(file, 1<<20)

	// Binary layout:
	// Header (20 bytes):
	//   fileSize          uint32
	//   triangleStructSize uint32  — 108 bytes
	//   triangleCount     uint32
	//   hasTextures       uint32   — bit0: textures present, bit1: cull backfaces
	//   textureSize       uint32   — texture edge length
	//
	// Texture data (if hasTextures):
	//   ColorMap     textureSize*textureSize*4 bytes (RGBA uint8)
	//   NormalMap    textureSize*textureSize*3 bytes (RGB uint8)
	//   MaterialMap  textureSize*textureSize*2 bytes ([roughness, metallic] uint8)
	//
	// Per triangle (108 bytes):
	//   v1 v2 v3 normal   4 × float3 padded to float4 = 4×16 = 64
	//   roughness metallic emission                    = 12
	//   color float3 padded                            = 16
	//   uv1[2] uv2[2] uv3[2] + 4 byte pad             = 16

	const triangleStructSize = uint32(4*16 + 12 + 16 + 16) // 108
	texSize := obj.TexSize
	triCount := uint32(len(obj.Triangles))
	hasTextures := uint32(0)
	if obj.HasTextures {
		hasTextures = 1
	}
	hasTextures |= cull

	textureBytes := uint32(0)
	if obj.HasTextures {
		textureBytes = uint32(texSize) * uint32(texSize) * (4 + 3 + 1 + 1)
	}
	fileSize := uint32(20) + triCount*triangleStructSize + textureBytes

	w.Write(uint32ToBytes(fileSize))
	w.Write(uint32ToBytes(triangleStructSize))
	w.Write(uint32ToBytes(triCount))
	w.Write(uint32ToBytes(hasTextures))
	w.Write(uint32ToBytes(uint32(texSize)))

	if obj.HasTextures {
		satFactor, brightFactor := boostFactors(boost)
		for i := 0; i < texSize*texSize; i++ {
			p := boostColor(obj.ColorMap[i], satFactor, brightFactor)
			w.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16), byte(p >> 24)})
		}
		for i := 0; i < texSize*texSize; i++ {
			w.Write(obj.NormalMap[i*3 : i*3+3])
		}
		for i := 0; i < texSize*texSize; i++ {
			w.Write(obj.MaterialMap[i*2 : i*2+2])
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

// saveAtlasImages dumps the packed atlas as inspectable PNGs. Names end in
// _atlas so probeFile can never resolve them as material source textures.
func saveAtlasImages(obj *FileObject, dir, name string, boost float32) {
	texSize := obj.TexSize
	color := image.NewRGBA(image.Rect(0, 0, texSize, texSize))
	normal := image.NewRGBA(image.Rect(0, 0, texSize, texSize))
	material := image.NewRGBA(image.Rect(0, 0, texSize, texSize))
	satFactor, brightFactor := boostFactors(boost)
	for i := 0; i < texSize*texSize; i++ {
		p := boostColor(obj.ColorMap[i], satFactor, brightFactor)
		color.Pix[i*4] = byte(p)
		color.Pix[i*4+1] = byte(p >> 8)
		color.Pix[i*4+2] = byte(p >> 16)
		color.Pix[i*4+3] = byte(p >> 24)
		normal.Pix[i*4] = obj.NormalMap[i*3]
		normal.Pix[i*4+1] = obj.NormalMap[i*3+1]
		normal.Pix[i*4+2] = obj.NormalMap[i*3+2]
		normal.Pix[i*4+3] = 0xFF
		material.Pix[i*4] = obj.MaterialMap[i*2]
		material.Pix[i*4+1] = obj.MaterialMap[i*2+1]
		material.Pix[i*4+3] = 0xFF
	}

	encoder := png.Encoder{CompressionLevel: png.BestSpeed}
	write := func(path string, img image.Image) {
		f, err := os.Create(path)
		if err != nil {
			fmt.Printf("Warning: could not write atlas dump %s: %v\n", path, err)
			return
		}
		encoder.Encode(f, img)
		f.Close()
		fmt.Printf("Saved atlas dump: %s\n", path)
	}
	write(filepath.Join(dir, name+"_color_atlas.png"), color)
	write(filepath.Join(dir, name+"_normal_atlas.png"), normal)
	write(filepath.Join(dir, name+"_material_atlas.png"), material)
}

func main() {
	if len(os.Args) < 3 {
		fmt.Println("Usage: parseObj <input.obj> <output.bin> [textureSize] [boost 0-1] [cull 0-1]")
		os.Exit(1)
	}

	inputFile := os.Args[1]
	outputFile := os.Args[2]

	textureSize := defaultTexSize
	if len(os.Args) >= 4 {
		v, err := strconv.Atoi(os.Args[3])
		if err != nil || v < 256 || v > 16384 {
			fmt.Println("Error: textureSize must be between 256 and 16384")
			os.Exit(1)
		}
		textureSize = v
	}

	boost := float32(0)
	if len(os.Args) >= 5 {
		f, err := strconv.ParseFloat(os.Args[4], 32)
		if err != nil {
			fmt.Printf("Error parsing boost: %v\n", err)
			os.Exit(1)
		}
		boost = clamp(float32(f), 0, 1)
	}

	cull := uint32(0)
	if len(os.Args) >= 6 {
		v, err := strconv.Atoi(os.Args[5])
		if err != nil {
			fmt.Printf("Error parsing cull: %v\n", err)
			os.Exit(1)
		}
		if v != 0 {
			cull = 2
		}
	}

	obj, err := parseObjFile(inputFile, textureSize)
	if err != nil {
		fmt.Printf("Error parsing OBJ file: %v\n", err)
		os.Exit(1)
	}

	err = writeFile(outputFile, obj, nil, boost, cull)
	if err != nil {
		fmt.Printf("Error writing output file: %v\n", err)
		os.Exit(1)
	}

	if obj.HasTextures {
		base := strings.TrimSuffix(filepath.Base(outputFile), filepath.Ext(outputFile))
		saveAtlasImages(obj, filepath.Dir(inputFile), base, boost)
	}

	fmt.Printf("Successfully converted %s to %s\n", inputFile, outputFile)
	fmt.Printf("Total triangles: %d\n", len(obj.Triangles))
}

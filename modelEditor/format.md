# Face map

Little endian. One face per file, or six in one.

```go
type FaceMap struct {
	Magic      uint32    // "FMF2" one face, "FEM2" six
	Version    uint32    // 2
	FileSize   uint32    // bytes, header included
	Size       uint32    // grid edge
	Components uint32    // 3: r, g, b
	FaceCount  uint32    // 1 or 6
	BbMin      [3]float32
	BbMax      [3]float32
	Face       []float32 // FaceCount*Size*Size*3, rows top first,
	                     // front back right left top bottom
}
```

`fileSize = 48 + faceCount*size*size*12` — size 16: 3,120 one face, 18,480 six; size 32: 12,336 / 73,776.

`Face[f][y][x]` is `EmissionMap.emissionMap[y][x]`. v1 files (`FEM1`/`FMF1`) still load.

# Face map

Little endian. Always six faces in one file.

```go
type FaceMap struct {
	Magic    uint32    // "FEM3"
	Version  uint32    // 3
	FileSize uint32    // bytes, header included
	Size     uint32    // grid edge
	Face     []float32 // 6*Size*Size*3, rows top first,
	                   // front back right left top bottom
}
```

`fileSize = 16 + size*size*72` — size 16: 18,448; size 32: 73,744.

`Face[f][y][x]` is `EmissionMap.emissionMap[y][x]`, with `f` in the order above, which is the
`maps[6]` order in `CalculateFaceEmissions`, so a loader copies the payload straight into the six
structs. Bounds are not stored: the grids are baked in the object's local mesh space and
`SampleEmission` maps a hit back with the object's own `BBmin`/`BBmax`.

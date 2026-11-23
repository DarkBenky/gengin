# Face map

Little endian. Always six faces in one file.

```go
type float3 [4]float64

type FaceMap struct {
	FileSize uint32   // bytes, header included
	Size     uint32   // grid edge
	Face     []float3 // 6*Size*Size, rows top first,
	                  // front back right left top bottom
}
```

`float3` is the padded engine vector: `x y z` plus one pad float, 16 bytes, so a loader reads every
cell with one `fread` straight into `EmissionMap.emissionMap`.

`fileSize = 8 + size*size*96` — size 16: 24,584; size 32: 98,312.

`Face[f][y][x]` is `EmissionMap.emissionMap[y][x]`, with `f` in the order above, which is the
`maps[6]` order in `CalculateFaceEmissions`, so a loader copies the payload straight into the six
structs. Bounds are not stored: the grids are baked in the object's local mesh space and
`SampleEmission` maps a hit back with the object's own `BBmin`/`BBmax`.

# Face map file

Little endian. One file per face, or all six faces in one file.

```
u32   magic        0x32464D46 "FMF2" one face   0x324D4546 "FEM2" six faces
u32   version      2
u32   fileSize     total bytes, header included
u32   size         grid edge, cells
u32   components   3 (r, g, b, each f32)
u32   faceCount    1 or 6
f32   bbMin[3]     model bounds the sweeps used
f32   bbMax[3]
48

f32   face[faceCount][size][size][3]    cells, rows top first
                                        order front back right left top bottom
```

```
fileSize = 48 + faceCount * size * size * 3 * 4
```

| size  | one face  | six faces |
|  ---  |    ---    |    ---    |
| 16    | 3,120     | 18,480    |
| 32    | 12,336    | 73,776    |
| 96    | 110,640   | 663,600   |

`face[f][y][x]` is `EmissionMap.emissionMap[y][x]` from `object/object.h`, so the payload copies
straight into `frontFaceEmission`, `backFaceEmission`, and so on.

Version 1 files (`FEM1`/`FMF1`, 20 or 16 byte header, no bounds, no version) still load.

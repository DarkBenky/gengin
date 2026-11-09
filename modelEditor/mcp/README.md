# modelEditor MCP tools

Local stdio MCP server for inspecting and checking gengin `.bin` models and emission BMPs
without opening the page. Uses `mcp` (already in `llmOpt/requirements-mcp.txt`) plus the Python
standard library only; `binlib.py` has no third-party imports.

```
python3 modelEditor/mcp/server.py
```

Register it with any MCP client, for example in `.vscode/mcp.json`:

```json
{
  "servers": {
    "gengin-model-editor": {
      "type": "stdio",
      "command": "python3",
      "args": ["/absolute/path/to/modelEditor/mcp/server.py"]
    }
  }
}
```

## Tools

| tool | what it does |
|---|---|
| `bin_info(path)` | header, layout (current or legacy), triangle count, flags, atlas size, size equation, sha256 |
| `bin_verify(path, grid=0)` | full record pass: non finite values, zero normals, non zero padding, material ranges, UV range, per cell containment |
| `bin_atlas_stats(path, grid=0)` | per cell color variety; a cell that is one flat color was never filled |
| `bin_convert(in, out)` | rewrite any supported file with the current 20 byte header, atlas and triangles untouched |
| `bin_atlas_export(path, out)` | write the color map as a 24 bit BMP for viewing |
| `bmp_info(path)` | BMP dimensions, bpp, compression, stride, top down flag, sha256 |
| `bmp_sample(path, points)` | RGB at `[[x, y], ...]` texel positions |
| `bmp_compare(a, b)` | differing pixel count and the first few differences |
| `emission_blank(size, out)` | all black emission BMP template |
| `bin_emission_check(bin, bmp)` | atlas/BMP size match plus the non black fraction |
| `emission_face_maps(bin, bmp, resolution, out)` | bake the emission side file into the engine's six `EmissionMap` grids, same orthographic sweep as `CalculateFaceEmissions`; `out_dir` writes one binary per face, `out_bin` all six in one file, `out_path` JSON |
| `emission_faces_info(path)` | read a face map back (single face or bundle): header, non zero cells per face, peak value |

## Face map binary

Layout, sizes and field order: `../format.md`. The payload of each face is exactly
`EmissionMap.emissionMap`, so a loader can copy it straight into the six `EmissionMap` structs.
`resolution` is a tool parameter, so 8, 32, 96, 128 ... are all valid.

Errors are returned as `{"ok": false, "error": "..."}`, never raised, so a model can keep going
after a bad file.

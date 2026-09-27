#ifndef WEIGHTS_JSON_H
#define WEIGHTS_JSON_H
// Generic reader/writer for plain-JSON weight checkpoints ("kgen-weights" v1).
//
// Format:
//   {"format":"kgen-weights","version":1,"layers":[
//     {"name":"conv1","type":"conv","w":28,"h":28,"c":1,"f":3,"n":4,"weights":[...36],"bias":[...4]},
//     {"name":"dense1","type":"dense","in":196,"out":10,"weights":[...1960],"bias":[...10]}]}
//
// Describe the layers with a KgwtLayerSpec table, hand in one array per layer and go:
//   Kgwt_SaveJson(path, count, specs, weights, biases);   // weights/biases = array of pointers
//   Kgwt_LoadJson(path, count, specs, weights, biases);   // fills caller-allocated arrays
//
// - shapeKeys/shape are free metadata (written verbatim); only weight/bias counts are validated
// - arrays are plain floats written with %.9g (float32 round-trip safe)
// - biasCount == 0 omits the "bias" field entirely (bias-less layers)
// - conv weights must be laid out [n][kh][kw][c] (the generator's layout; torch [N,C,F,F] must permute)
// - the loader scans for "weights"/"bias" markers, so hand edits (whitespace, extra fields) are fine

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;         // "conv1"
    const char *type;         // "conv" / "dense" (free text)
    const char *shapeKeys[8]; // {"w","h","c","f","n"} -> written as "w": 28, ...
    int shape[8];
    int shapeLen;             // number of shape entries to write
    int weightCount;
    int biasCount;            // 0 = no bias field
} KgwtLayerSpec;

static inline void Kgwt_WriteFloatArray(FILE *f, const float *values, int count) {
    fputc('[', f);
    for (int i = 0; i < count; i++) {
        if (i > 0) fputc(',', f);
        if (i % 16 == 0) fputs("\n      ", f);
        fprintf(f, "%.9g", values[i]);
    }
    fputs("\n    ]", f);
}

static inline int Kgwt_ParseFloatArray(const char **cursor, float *dst, int expected, const char *layer, const char *what) {
    const char *p = strchr(*cursor, '[');
    if (p == NULL) {
        fprintf(stderr, "kgen-weights: no '[' before %s.%s\n", layer, what);
        return 0;
    }
    p++;
    int count = 0;
    while (1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        char *end = NULL;
        float v = strtof(p, &end);
        if (end == p) {
            fprintf(stderr, "kgen-weights: bad float in %s.%s\n", layer, what);
            return 0;
        }
        if (count < expected) dst[count] = v;
        count++;
        p = end;
    }
    if (count != expected) {
        fprintf(stderr, "kgen-weights: %s.%s has %d floats, expected %d\n", layer, what, count, expected);
        return 0;
    }
    *cursor = p + 1;
    return 1;
}

static inline int Kgwt_SaveJson(const char *path, int layerCount, const KgwtLayerSpec *specs,
                                const float *const *weights, const float *const *biases) {
    if (path == NULL || layerCount <= 0 || specs == NULL || weights == NULL || biases == NULL) return 0;
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "could not open %s for writing\n", path);
        return 0;
    }

    fputs("{\n  \"format\": \"kgen-weights\",\n  \"version\": 1,\n  \"layers\": [\n", f);
    for (int i = 0; i < layerCount; i++) {
        const KgwtLayerSpec *spec = &specs[i];
        fprintf(f, "    {\"name\": \"%s\", \"type\": \"%s\"", spec->name, spec->type);
        for (int s = 0; s < spec->shapeLen; s++)
            fprintf(f, ", \"%s\": %d", spec->shapeKeys[s], spec->shape[s]);
        fputs(", \"weights\": ", f);
        Kgwt_WriteFloatArray(f, weights[i], spec->weightCount);
        if (spec->biasCount > 0) {
            fputs(", \"bias\": ", f);
            Kgwt_WriteFloatArray(f, biases[i], spec->biasCount);
        }
        fputs(i + 1 < layerCount ? "},\n" : "}\n", f);
    }
    fputs("  ]\n}\n", f);
    fclose(f);
    return 1;
}

static inline int Kgwt_LoadJson(const char *path, int layerCount, const KgwtLayerSpec *specs,
                                float *const *weights, float *const *biases) {
    if (path == NULL || layerCount <= 0 || specs == NULL || weights == NULL || biases == NULL) return 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0; // no file yet is not an error
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return 0;
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);

    if (strstr(buf, "kgen-weights") == NULL) {
        fprintf(stderr, "%s: missing kgen-weights format tag\n", path);
        free(buf);
        return 0;
    }

    int ok = 1;
    const char *cur = buf;
    for (int i = 0; i < layerCount && ok; i++) {
        const KgwtLayerSpec *spec = &specs[i];
        cur = strstr(cur, "\"weights\"");
        if (cur == NULL) {
            fprintf(stderr, "%s: missing \"weights\" for %s\n", path, spec->name);
            ok = 0;
            break;
        }
        ok = Kgwt_ParseFloatArray(&cur, weights[i], spec->weightCount, spec->name, "weights");
        if (!ok || spec->biasCount <= 0) continue;
        cur = strstr(cur, "\"bias\"");
        if (cur == NULL) {
            fprintf(stderr, "%s: missing \"bias\" for %s\n", path, spec->name);
            ok = 0;
            break;
        }
        ok = Kgwt_ParseFloatArray(&cur, biases[i], spec->biasCount, spec->name, "bias");
    }
    free(buf);
    return ok;
}

#endif // WEIGHTS_JSON_H

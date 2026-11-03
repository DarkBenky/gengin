#ifndef JSON_H
#define JSON_H

#include "format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum jsonType {
    JSON_TYPE_STRING,
    JSON_TYPE_NUMBER,
    JSON_TYPE_OBJECT,
    JSON_TYPE_ARRAY,
    JSON_TYPE_BOOLEAN,
    JSON_TYPE_NULL
} jsonType;

typedef struct jsonRecord {
    char *key;
    char *value;
    jsonType type;
} jsonRecord;

typedef struct jsonObject {
    jsonRecord *records;
    int len;
    int cap;
} jsonObject;

static inline char *jsonCopyString(const char *src) {
    if (!src) return NULL;
    size_t size = strlen(src) + 1;
    char *copy = malloc(size);
    if (!copy) return NULL;
    memcpy(copy, src, size);
    return copy;
}

static inline jsonObject *newJsonObject(void) {
    return calloc(1, sizeof(jsonObject));
}

static inline void freeJsonObject(jsonObject *obj) {
    if (!obj) return;
    for (int i = 0; i < obj->len; i++) {
        free(obj->records[i].key);
        free(obj->records[i].value);
    }
    free(obj->records);
    free(obj);
}

static inline void addJsonField(jsonObject *obj, char *key, char *value, jsonType type) {
    if (!obj || !key) return;
    if (obj->len == obj->cap) {
        int cap = obj->cap > 0 ? obj->cap * 2 : 8;
        jsonRecord *records = realloc(obj->records, sizeof(jsonRecord) * (size_t)cap);
        if (!records) return;
        obj->records = records;
        obj->cap = cap;
    }
    char *keyCopy = jsonCopyString(key);
    char *valueCopy = jsonCopyString(value);
    if (!keyCopy || (value && !valueCopy)) {
        free(keyCopy);
        free(valueCopy);
        return;
    }
    obj->records[obj->len++] = (jsonRecord){keyCopy, valueCopy, type};
}

static inline void jsonPrintString(const char *value) {
    putchar('"');
    if (value) {
        for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
            switch (*c) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*c < 0x20) printf("\\u%04x", *c);
                else putchar(*c);
                break;
            }
        }
    }
    putchar('"');
}

static inline void printJsonObject(jsonObject *obj) {
    if (!obj) return;
    putchar('{');
    for (int i = 0; i < obj->len; i++) {
        const jsonRecord *record = &obj->records[i];
        if (i > 0) fputs(", ", stdout);
        jsonPrintString(record->key);
        fputs(": ", stdout);
        if (record->type == JSON_TYPE_STRING) {
            jsonPrintString(record->value);
        } else if (!record->value) {
            fputs("null", stdout);
        } else {
            // OBJECT and ARRAY values are passed already serialized
            fputs(record->value, stdout);
        }
    }
    putchar('}');
}

#endif
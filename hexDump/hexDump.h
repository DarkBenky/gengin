#ifndef HEXDUMP_H
#define HEXDUMP_H

#include <stdint.h>

// Prints data in annotated hex format with ANSI color (works in zsh/bash).
// Zero-only rows are collapsed into a single summary line.
void hexDump(const void *data, uint32_t size);

#endif // HEXDUMP_H

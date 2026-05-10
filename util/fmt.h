#pragma once
#include <stdio.h>
#include <string.h>


static inline const char *fmtInt(long v) {
	static char buf[32];
	char tmp[32];
	int neg = v < 0;
	unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
	int len = snprintf(tmp, sizeof(tmp), "%lu", u);
	int out = 0;
	if (neg) buf[out++] = '-';
	for (int i = 0; i < len; i++) {
		int remaining = len - i;
		if (i > 0 && remaining % 3 == 0) buf[out++] = ' ';
		buf[out++] = tmp[i];
	}
	buf[out] = '\0';
	return buf;
}

// TODO: fmtFloat
// e.g. 1234567.89 -> "1 234 567.89"

// TODO: fmtIntShort
// e.g. 123 456 -> "123.456K", 1 234 567 -> "1.23M", 1 234 567 890 -> "1.23B" 

// TODO: fmtFloatShort
// e.g. 1234.56 -> "1.23K", 123456.78 -> "123.46K", 1234567.89 -> "1.23M"
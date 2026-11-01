#pragma once
#include <stdio.h>
#include <string.h>

static inline const char *fmtInt(long v) {
	static char buf[32];
	char tmp[32];
	int neg = v < 0;
	unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
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

// e.g. 1234567.89 -> "1 234 567.89"
static inline const char *fmtFloat(double v, int precision) {
	static char buf[96];
	char tmp[64] = {0};
	int len = snprintf(tmp, sizeof(tmp), "%.*f", precision, v);
	if (len < 0) len = 0;
	if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
	int firstDigit = tmp[0] == '-' ? 1 : 0;
	int intLen = firstDigit;
	while (intLen < len && tmp[intLen] != '.') intLen++;
	int out = 0;
	for (int i = 0; i < len; i++) {
		char c = tmp[i];
		if (c != '.' && i > firstDigit && i < intLen && (intLen - i) % 3 == 0) buf[out++] = ' ';
		buf[out++] = c;
	}
	buf[out] = '\0';
	return buf;
}

// e.g. 123 456 -> "123.456K", 1 234 567 -> "1.23M", 1 234 567 890 -> "1.23B"
static inline const char *fmtIntShort(long v) {
	static char buf[32];
	char suffix = '\0';
	double scaled = (double)v;
	if (v >= 1000000000) {
		suffix = 'B';
		scaled = v / 1000000000.0;
	} else if (v >= 1000000) {
		suffix = 'M';
		scaled = v / 1000000.0;
	} else if (v >= 1000) {
		suffix = 'K';
		scaled = v / 1000.0;
	}
	int len;
	if (suffix)
		len = snprintf(buf, sizeof(buf), "%.2f%c", scaled, suffix);
	else
		len = snprintf(buf, sizeof(buf), "%ld", v);
	if (len < 0) len = 0;
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	buf[len] = '\0';
	return buf;
}

// e.g. 1234.56 -> "1.23K", 123456.78 -> "123.46K", 1234567.89 -> "1.23M"
static inline const char *fmtFloatShort(double v, int precision) {
	static char buf[64];
	char suffix = '\0';
	double scaled = v;
	if (v >= 1000000000.0) {
		suffix = 'B';
		scaled = v / 1000000000.0;
	} else if (v >= 1000000.0) {
		suffix = 'M';
		scaled = v / 1000000.0;
	} else if (v >= 1000.0) {
		suffix = 'K';
		scaled = v / 1000.0;
	}
	int len;
	if (suffix)
		len = snprintf(buf, sizeof(buf), "%.*f%c", precision, scaled, suffix);
	else
		len = snprintf(buf, sizeof(buf), "%.*f", precision, v);
	if (len < 0) len = 0;
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	buf[len] = '\0';
	return buf;
}
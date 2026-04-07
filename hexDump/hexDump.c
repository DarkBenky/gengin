#include "hexDump.h"
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#define COL_RESET "\x1b[0m"
#define COL_HEADER "\x1b[1;37m" // bold white  — column header
#define COL_ROWNUM "\x1b[36m"	// cyan        — row index
#define COL_ZERO "\x1b[2;37m"	// dim white   — zero bytes
#define COL_BYTE "\x1b[97m"		// bright white— non-zero bytes
#define COL_ASCII "\x1b[32m"	// green       — printable ASCII
#define COL_DOT "\x1b[2;37m"	// dim white   — non-printable placeholder
#define COL_PIPE "\x1b[2;37m"	// dim white   — | delimiters
#define COL_ZEROS "\x1b[33m"	// yellow      — collapsed zero-row summary

#define ROW 16

static int isZeroRow(const uint8_t *p, uint32_t len) {
	for (uint32_t i = 0; i < len; i++)
		if (p[i]) return 0;
	return 1;
}

void hexDump(const void *data, uint32_t size) {
	const uint8_t *p = (const uint8_t *)data;

	// header
	printf(COL_HEADER "    ");
	for (int i = 0; i < ROW; i++)
		printf("%02X ", i);
	printf(COL_RESET "\n");

	uint32_t numRows = (size + ROW - 1) / ROW;
	uint32_t zeroStart = 0;
	int inZeroRun = 0;

	for (uint32_t row = 0; row < numRows; row++) {
		uint32_t offset = row * ROW;
		uint32_t rowLen = (offset + ROW <= size) ? ROW : (size - offset);
		const uint8_t *rp = p + offset;

		int zero = (rowLen == ROW) && isZeroRow(rp, rowLen);

		if (zero) {
			if (!inZeroRun) {
				zeroStart = row;
				inZeroRun = 1;
			}
			// peek: if next row is also zero, keep accumulating
			uint32_t nextOffset = (row + 1) * ROW;
			int nextZero = (row + 1 < numRows) &&
						   (nextOffset + ROW <= size) &&
						   isZeroRow(p + nextOffset, ROW);
			if (nextZero) continue;
			// flush zero run
			if (row == zeroStart) goto print_row; // single zero row — print normally
			printf(COL_ROWNUM "%03X " COL_ZEROS
							  "==============| zeros %03X-%03X |================" COL_RESET "  " COL_PIPE "|" COL_RESET
							  "                " COL_PIPE "|" COL_RESET "\n",
				   zeroStart, zeroStart, row);
			inZeroRun = 0;
			continue;
		}

		inZeroRun = 0;
	print_row:
		printf(COL_ROWNUM "%03X " COL_RESET, row);

		// hex bytes
		for (uint32_t i = 0; i < ROW; i++) {
			if (i < rowLen) {
				if (rp[i] == 0)
					printf(COL_ZERO "00 " COL_RESET);
				else
					printf(COL_BYTE "%02X " COL_RESET, rp[i]);
			} else {
				printf("   ");
			}
		}

		// ASCII panel
		printf(" " COL_PIPE "|" COL_RESET);
		for (uint32_t i = 0; i < ROW; i++) {
			if (i < rowLen) {
				if (isprint(rp[i]))
					printf(COL_ASCII "%c" COL_RESET, rp[i]);
				else
					printf(COL_DOT "." COL_RESET);
			} else {
				printf(" ");
			}
		}
		printf(COL_PIPE "|" COL_RESET "\n");
	}
}

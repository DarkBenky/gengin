#include "hexDump.h"
#include <string.h>
#include <stdint.h>

int main(void) {
	// matches the pattern in fromat.md: short string, zero run, then repeated data
	uint8_t data[7 * 16];
	memset(data, 0, sizeof(data));

	data[0] = 0x00;
	data[1] = 0x04;
	data[2] = 't';
	data[3] = 'e';
	data[4] = 's';
	data[5] = 't';

	// rows 1-5 stay zero

	// row 6: "testtesttesttest"
	memcpy(data + 6 * 16, "testtesttesttest", 16);

	hexDump(data, sizeof(data));
	return 0;
}

#pragma once

#include "qemu/bitops.h"

// LH15KAH2
// Sharp LH15KAH2 130x130 color LCD module, firmware panel ID 0x28, controller unidentified
/* Window start column, also loads the column cursor */
#define	LH15KAH2_X_START			0x04
#define	LH15KAH2_X_START_ADDR		MAKE_64BIT_MASK(0, 8)	 // Column address
#define	LH15KAH2_X_START_ADDR_SHIFT	0

/* Window start row, also loads the row cursor */
#define	LH15KAH2_Y_START			0x05
#define	LH15KAH2_Y_START_ADDR		MAKE_64BIT_MASK(0, 8)	 // Row address
#define	LH15KAH2_Y_START_ADDR_SHIFT	0

/* Window end column */
#define	LH15KAH2_X_END				0x06
#define	LH15KAH2_X_END_ADDR			MAKE_64BIT_MASK(0, 8)	 // Column address
#define	LH15KAH2_X_END_ADDR_SHIFT	0

/* Window end row */
#define	LH15KAH2_Y_END				0x07
#define	LH15KAH2_Y_END_ADDR			MAKE_64BIT_MASK(0, 8)	 // Row address
#define	LH15KAH2_Y_END_ADDR_SHIFT	0

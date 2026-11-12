#pragma once

#include "qemu/bitops.h"
#include "qemu/debug-regs.h"

// PMB627X
// Infineon PMB6270 and PMB6272 SMARTi RF transceivers
/* Fractional synthesizer channel word, low part */
#define PMB627X_CHANNEL1                        0x00
#define PMB627X_CHANNEL1_FRACTION               MAKE_64BIT_MASK(4, 20)   // Fractional channel word bits 19..0
#define PMB627X_CHANNEL1_FRACTION_SHIFT         4

/* Synthesizer channel and operating mode */
#define PMB627X_CHANNEL2                        0x02
#define PMB627X_CHANNEL2_FRACTION               MAKE_64BIT_MASK(4, 3)    // Fractional channel word bits 22..20
#define PMB627X_CHANNEL2_FRACTION_SHIFT         4
#define PMB627X_CHANNEL2_INTEGER                MAKE_64BIT_MASK(7, 8)    // Integer channel word
#define PMB627X_CHANNEL2_INTEGER_SHIFT          7
#define PMB627X_CHANNEL2_POWER_MODE             MAKE_64BIT_MASK(15, 2)   // Transceiver power mode
#define PMB627X_CHANNEL2_POWER_MODE_SHIFT       15
#define PMB627X_CHANNEL2_POWER_MODE_ALL_OFF     0x0
#define PMB627X_CHANNEL2_POWER_MODE_UNUSED      0x8000
#define PMB627X_CHANNEL2_POWER_MODE_PLL_TEST    0x10000
#define PMB627X_CHANNEL2_POWER_MODE_ALL_ON      0x18000
#define PMB627X_CHANNEL2_TRX                    BIT(18)                  // Receive or transmit path
#define PMB627X_CHANNEL2_TRX_SHIFT              18
#define PMB627X_CHANNEL2_TRX_RX                 0x0
#define PMB627X_CHANNEL2_TRX_TX                 0x40000
#define PMB627X_CHANNEL2_BAND                   MAKE_64BIT_MASK(19, 2)   // RF band
#define PMB627X_CHANNEL2_BAND_SHIFT             19
#define PMB627X_CHANNEL2_BAND_GSM850            0x0
#define PMB627X_CHANNEL2_BAND_GSM900            0x80000
#define PMB627X_CHANNEL2_BAND_DCS1800           0x100000
#define PMB627X_CHANNEL2_BAND_PCS1900           0x180000

/* Receiver gain and burst activation */
#define PMB627X_RXTX                            0x04
#define PMB627X_RXTX_RXGAIN                     MAKE_64BIT_MASK(4, 2)    // Receiver gain
#define PMB627X_RXTX_RXGAIN_SHIFT               4
#define PMB627X_RXTX_RXGAIN_LOW                 0x0
#define PMB627X_RXTX_RXGAIN_MEDIUM              0x10
#define PMB627X_RXTX_RXGAIN_UNSUPPORTED         0x20
#define PMB627X_RXTX_RXGAIN_HIGH                0x30
#define PMB627X_RXTX_RXCORR                     MAKE_64BIT_MASK(6, 4)    // Receiver gain correction
#define PMB627X_RXTX_RXCORR_SHIFT               6
#define PMB627X_RXTX_RXCM                       MAKE_64BIT_MASK(11, 2)   // Mandatory receiver common-mode setting
#define PMB627X_RXTX_RXCM_SHIFT                 11
#define PMB627X_RXTX_RXGS0                      BIT(13)                  // Enable +12 dB receiver gain step
#define PMB627X_RXTX_RXGS0_SHIFT                13
#define PMB627X_RXTX_RXGS1                      BIT(14)                  // Enable +6 dB receiver gain step
#define PMB627X_RXTX_RXGS1_SHIFT                14
#define PMB627X_RXTX_RXGS2                      BIT(15)                  // Enable -6 dB receiver gain step
#define PMB627X_RXTX_RXGS2_SHIFT                15
#define PMB627X_RXTX_RXGS3                      BIT(16)                  // Enable -3 dB receiver gain step
#define PMB627X_RXTX_RXGS3_SHIFT                16
#define PMB627X_RXTX_OFC                        BIT(17)                  // Enable DC-offset compensation
#define PMB627X_RXTX_OFC_SHIFT                  17

/* Crystal oscillator mode and calibration */
#define PMB627X_XO_INIT1                        0x06
#define PMB627X_XO_INIT1_XOMODE0                BIT(4)                   // Use an external oscillator signal
#define PMB627X_XO_INIT1_XOMODE0_SHIFT          4
#define PMB627X_XO_INIT1_XOMODE1                BIT(5)                   // Mandatory oscillator-mode bit
#define PMB627X_XO_INIT1_XOMODE1_SHIFT          5
#define PMB627X_XO_INIT1_XOMODE3                BIT(7)                   // Mandatory oscillator-mode bit
#define PMB627X_XO_INIT1_XOMODE3_SHIFT          7
#define PMB627X_XO_INIT1_XOSETUP                MAKE_64BIT_MASK(16, 4)   // Crystal-core capacitance adaptation
#define PMB627X_XO_INIT1_XOSETUP_SHIFT          16
#define PMB627X_XO_INIT1_XOCAL                  MAKE_64BIT_MASK(20, 3)   // Crystal subrange selection
#define PMB627X_XO_INIT1_XOCAL_SHIFT            20
#define PMB627X_XO_INIT1_CRYSTAL_SIZE           BIT(23)                  // Crystal load capacitance
#define PMB627X_XO_INIT1_CRYSTAL_SIZE_SHIFT     23
#define PMB627X_XO_INIT1_CRYSTAL_SIZE_10PF      0x0
#define PMB627X_XO_INIT1_CRYSTAL_SIZE_8PF       0x800000

/* Crystal oscillator linearization configuration */
#define PMB627X_XO_INIT3                        0x08
#define PMB627X_XO_INIT3_VALUE                  MAKE_64BIT_MASK(4, 20)   // Undocumented configuration value
#define PMB627X_XO_INIT3_VALUE_SHIFT            4

/* Crystal oscillator linearization coefficients */
#define PMB627X_XO_INIT2                        0x0A
#define PMB627X_XO_INIT2_ALPHA                  MAKE_64BIT_MASK(4, 10)   // LUXO alpha coefficient
#define PMB627X_XO_INIT2_ALPHA_SHIFT            4
#define PMB627X_XO_INIT2_GAMMA                  MAKE_64BIT_MASK(14, 10)  // LUXO gamma coefficient
#define PMB627X_XO_INIT2_GAMMA_SHIFT            14

/* Crystal oscillator frequency correction */
#define PMB627X_XO_TUNE                         0x0C
#define PMB627X_XO_TUNE_AFC                     MAKE_64BIT_MASK(4, 17)   // Frequency-correction value
#define PMB627X_XO_TUNE_AFC_SHIFT               4

extern const DebugRegs pmb627x_debug_regs;

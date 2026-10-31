/* SPDX-License-Identifier: MIT */
/*
 * wasm64 target-specific constraint sets.
 */

C_O0_I1(r)
C_O0_I2(r, r)
C_O0_I2(r, rC)
C_O0_I3(r, r, r)
C_O0_I4(r, r, r, r)
C_O1_I1(r, r)
C_O1_I2(r, r, r)
C_O1_I2(r, r, rC)
C_O1_I2(r, rC, r)
C_O1_I3(r, r, r, r)
C_O1_I4(r, r, r, r, r)
C_O1_I4(r, r, rC, rC, rC)
C_O2_I1(r, r, r)
C_O2_I2(r, r, r, r)
C_O2_I4(r, r, r, r, r, r)

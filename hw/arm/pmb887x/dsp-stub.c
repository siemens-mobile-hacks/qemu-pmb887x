/*
 * The host-side DSP: the same device as dsp.c, built to answer the firmware's
 * runtime commands rather than execute the mask ROM. See the note in dsp.c.
 */
#define STUB_DSP 1
#include "dsp.c"

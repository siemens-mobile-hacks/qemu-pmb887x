#pragma once

#include "qemu/osdep.h"

GMatchInfo *regexp_match(GRegex *regexp, const char *input);

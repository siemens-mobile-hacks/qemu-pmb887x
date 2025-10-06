#include "hw/arm/pmb887x/utils/strings.h"

bool str_ends_with(const char *str, const char *suffix) {
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr)
		return false;
	return strcmp(str + lenstr - lensuffix, suffix) == 0;
}

bool str_starts_with(const char *str, const char *prefix) {
	size_t lenstr = strlen(str);
	size_t lenprefix = strlen(prefix);
	if (lenprefix > lenstr)
		return false;
	return strncmp(str, prefix, lenprefix) == 0;
}

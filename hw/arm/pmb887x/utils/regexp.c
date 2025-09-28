#include "hw/arm/pmb887x/utils/regexp.h"

GMatchInfo *regexp_match(GRegex *regexp, const char *input) {
	GMatchInfo *m = NULL;
	g_regex_match(regexp, input, 0, &m);
	if (g_match_info_matches(m))
		return m;
	g_match_info_free(m);
	return NULL;
}

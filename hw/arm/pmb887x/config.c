#include "hw/arm/pmb887x/config.h"
#include "qemu/osdep.h"
#include "qemu/error-report.h"

enum InitParserState {
	// States
	INI_STATE_NONE,
	INI_STATE_COMMENT,
	INI_STATE_SECTION,
	INI_STATE_KEY,
	INI_STATE_DELIM,
	INI_STATE_VALUE_START,
	INI_STATE_VALUE_QUOTED,
	INI_STATE_VALUE_RAW,
	
	// Errors
	INI_STATE_ERROR,
};

static bool is_valid_key(char c) {
	return isdigit(c) || isalpha(c) || c == '_' || c == '-' || c == '.';
}

static GString *read_file(const char *file) {
	GString *text = g_string_new("");
	FILE *fp = fopen(file, "r");
	if (fp) {
		while (!feof(fp)) {
			char buffer[4096];
			int readBytesCount = fread(buffer, 1, sizeof(buffer), fp);
			if (readBytesCount > 0)
				g_string_append_len(text, buffer, readBytesCount);
		}
		fclose(fp);
	} else {
		error_report("[pmb887x-config] fopen(%s): %s", file, strerror(errno));
		return NULL;
	}
	return text;
}

size_t pmb887x_cfg_sections_cnt(pmb887x_cfg_t *cfg, const char *name) {
	size_t count = 0;
	for (size_t i = 0; i < cfg->sections_count; i++) {
		pmb887x_cfg_section_t *s = &cfg->sections[i];
		if (strcmp(s->name, name) == 0)
			count++;
	}
	return count;
}

pmb887x_cfg_section_t *pmb887x_cfg_section(pmb887x_cfg_t *cfg, const char *name, ssize_t index, bool required) {
	uint32_t section_n = 0;
	for (size_t i = 0; i < cfg->sections_count; i++) {
		pmb887x_cfg_section_t *s = &cfg->sections[i];
		if (strcmp(s->name, name) == 0) {
			if (index == -1 || section_n == index)
				return s;
			section_n++;
		}
	}

	if (required) {
		error_report("[pmb887x-config] %s: [%s] not found.", cfg->file, name);
		exit(1);
	}

	return NULL;
}

const char *pmb887x_cfg_get(pmb887x_cfg_t *cfg, const char *section_name, const char *key, bool required) {
	pmb887x_cfg_section_t *section = pmb887x_cfg_section(cfg, section_name, -1, false);
	if (section)
		return pmb887x_cfg_section_get(section, key, required);
	
	if (required) {
		error_report("[pmb887x-config] %s: %s.%s not found.", cfg->file, section_name, key);
		exit(1);
	}

	return NULL;
}

const char *pmb887x_cfg_section_get(pmb887x_cfg_section_t *section, const char *key, bool required) {
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		if (strcmp(item->key, key) == 0)
			return item->value;
	}

	if (required) {
		error_report("[pmb887x-config] %s: %s.%s not found.", section->parent->file, section->name, key);
		exit(1);
	}

	return NULL;
}

pmb887x_cfg_t *pmb887x_cfg_parse(const char *file) {
	enum InitParserState state = INI_STATE_NONE;
	
	g_autoptr(GString) text = read_file(file);
	if (!text)
		return NULL;
	
	pmb887x_cfg_t *cfg = g_new0(pmb887x_cfg_t, 1);
	cfg->file = g_strdup(file);
	
	g_autoptr(GString) section_name = g_string_new("");
	g_autoptr(GString) key = g_string_new("");
	g_autoptr(GString) value = g_string_new("");
	
	char quote_char = '\0';
	
	int line_n = 1;
	int char_n = 1;
	bool escape = false;
	
	int i = 0;
	while (text->str[i]) {
		char c = text->str[i];
		switch (state) {
			case INI_STATE_NONE:
				if (c == ';' || c == '#') {
					state = INI_STATE_COMMENT;
				} else if (c == '[') {
					g_string_assign(section_name, "");
					state = INI_STATE_SECTION;
				} else if (is_valid_key(c)) {
					g_string_truncate(key, 0);
					g_string_append_len(key, &c, 1);
					state = INI_STATE_KEY;
				} else if (!isspace(c)) {
					state = INI_STATE_ERROR;
				}
				break;
			
			case INI_STATE_COMMENT:
				if (c == '\r' || c == '\n' || c == '\0')
					state = INI_STATE_NONE;
				break;
			
			case INI_STATE_SECTION:
				if (c == ']') {
					cfg->sections_count++;
					cfg->sections = g_realloc(cfg->sections, sizeof(pmb887x_cfg_section_t) * cfg->sections_count);
					cfg->sections[cfg->sections_count - 1].name = g_strdup(section_name->str);
					cfg->sections[cfg->sections_count - 1].items = NULL;
					cfg->sections[cfg->sections_count - 1].items_count = 0;
					cfg->sections[cfg->sections_count - 1].parent = cfg;
					state = INI_STATE_NONE;
				} else if (is_valid_key(c)) {
					g_string_append_len(section_name, &c, 1);
				} else {
					state = INI_STATE_ERROR;
				}
				break;
			
			case INI_STATE_KEY:
				if (!cfg->sections_count) {
					state = INI_STATE_ERROR;
				} else if (isspace(c) || c == '=') {
					state = c == '=' ? INI_STATE_VALUE_START : INI_STATE_DELIM;
				} else if (is_valid_key(c)) {
					g_string_append_len(key, &c, 1);
				} else {
					state = INI_STATE_ERROR;
				}
				break;
			
			case INI_STATE_DELIM:
				if (c == '=') {
					state = INI_STATE_VALUE_START;
				} else if (!isspace(c)) {
					state = INI_STATE_ERROR;
				}
				break;
			
			case INI_STATE_VALUE_START:
				if (c == '\'' || c == '"') {
					quote_char = c;
					g_string_assign(value, "");
					state = INI_STATE_VALUE_QUOTED;
					escape = false;
				} else if (c == '\r' || c == '\n' || c == '\0') {
					g_string_assign(value, "");
					state = INI_STATE_NONE;
				} else if (isspace(c)) {
					// skip
				} else {
					g_string_truncate(value, 0);
					g_string_append_len(value, &c, 1);
					state = INI_STATE_VALUE_RAW;
				}
				break;
			
			case INI_STATE_VALUE_RAW:
				if (c == '\r' || c == '\n' || c == '\0' || c == ';' || c == '#') {
					while (value->len && isspace(value->str[value->len - 1]))
						g_string_erase(value, value->len - 1, 1);
					
					pmb887x_cfg_section_t *section = &cfg->sections[cfg->sections_count - 1];
					section->items_count++;
					section->items = g_realloc(section->items, sizeof(pmb887x_cfg_item_t) * section->items_count);
					
					pmb887x_cfg_item_t *item = &section->items[section->items_count - 1];
					item->key = g_strdup(key->str);
					item->value = g_strdup(value->str);
					
					if (c == ';' || c == '#') {
						state = INI_STATE_COMMENT;
					} else {
						state = INI_STATE_NONE;
					}
				} else {
					g_string_append_len(value, &c, 1);
				}
				break;
			
			case INI_STATE_VALUE_QUOTED:
				if (escape) {
					if (quote_char != c)
						g_string_append_len(value, "\\", 1);
					g_string_append_len(value, &c, 1);
					escape = false;
				} else {
					if (c == '\\') {
						escape = true;
					} else if (c == quote_char) {
						pmb887x_cfg_section_t *section = &cfg->sections[cfg->sections_count - 1];
						section->items_count++;
						section->items = g_realloc(section->items, section->items_count);
						
						pmb887x_cfg_item_t *item = &section->items[section->items_count - 1];
						item->key = g_strdup(key->str);
						item->value = g_strdup(value->str);
						
						state = INI_STATE_NONE;
					} else {
						g_string_append_len(value, &c, 1);
					}
				}
				break;
			
			case INI_STATE_ERROR:
				error_report("[pmb887x-config] Invalid char '%c' at: %s:%d:%d", c, file, line_n, char_n);
				pmb887x_cfg_free(cfg);
				return NULL;
		}
		
		if (state < INI_STATE_ERROR) {
			if (c == '\n') {
				line_n++;
				char_n = 1;
			} else {
				char_n++;
			}
			i++;
		}
	}
	
	if (state != INI_STATE_NONE) {
		error_report("[pmb887x-config] Unexpected EOF in %s", file);
		pmb887x_cfg_free(cfg);
		return NULL;
	}
	
	return cfg;
}

void pmb887x_cfg_free(pmb887x_cfg_t *cfg) {
	for (size_t i = 0; i < cfg->sections_count; i++) {
		pmb887x_cfg_section_t *section = &cfg->sections[i];
		for (size_t j = 0; j < section->items_count; j++) {
			pmb887x_cfg_item_t *item = &section->items[j];
			g_free(item->key);
			g_free(item->value);
		}
		g_free(section->name);
		g_free(section->items);
	}
	g_free(cfg->file);
	g_free(cfg->sections);
	g_free(cfg);
}

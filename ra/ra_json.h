#pragma once

// ra_json -- a tiny JSON reader/writer for the launcher protocol files
// (/tmp/dreamster/*.json, see ra_menu.h). Strict enough to reject garbage,
// lenient where it costs nothing (trailing whitespace/NULs, invalid UTF-8 is
// passed through). Nesting is limited to 32 levels, strings are unescaped
// to UTF-8 (\uXXXX incl. surrogate pairs; a lone surrogate becomes '?').

#include <stddef.h>
#include <string>
#include <vector>

struct RaJson {
	enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
	Type type = NUL;
	bool b = false;
	double num = 0;
	std::string str;                  // STR
	std::vector<RaJson> items;        // ARR elements / OBJ values
	std::vector<std::string> keys;    // OBJ keys (same order as items)

	// OBJ member (the last one if a key repeats), NULL if absent / not an object
	const RaJson* get(const char* key) const;
	// string member, NULL if absent or not a string
	const char* get_str(const char* key) const;
};

// Parses text[0..len). False (and *out reset) on any syntax error.
bool ra_json_parse(const char* text, size_t len, RaJson* out);

// Appends s as a JSON string literal: quotes, backslash and control
// characters escaped; bytes >= 0x80 (UTF-8) are copied as they are.
void ra_json_quote(std::string* out, const char* s);

// ra_json.cpp -- see ra_json.h

#include "ra_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

const RaJson* RaJson::get(const char* key) const
{
	if (type != OBJ)
		return nullptr;
	for (size_t i = keys.size(); i-- > 0;)
		if (keys[i] == key)
			return &items[i];
	return nullptr;
}

const char* RaJson::get_str(const char* key) const
{
	const RaJson* v = get(key);
	return v && v->type == STR ? v->str.c_str() : nullptr;
}

namespace {

struct Parser {
	const char* p;
	const char* end;
	int depth;

	void ws()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}
	bool lit(const char* s)
	{
		size_t n = strlen(s);
		if ((size_t)(end - p) < n || memcmp(p, s, n))
			return false;
		p += n;
		return true;
	}
	static int hexval(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
	bool hex4(uint32_t* v)
	{
		if (end - p < 4)
			return false;
		uint32_t r = 0;
		for (int i = 0; i < 4; i++) {
			int h = hexval(p[i]);
			if (h < 0)
				return false;
			r = (r << 4) | (uint32_t)h;
		}
		p += 4;
		*v = r;
		return true;
	}
	static void put_utf8(std::string* s, uint32_t cp)
	{
		if (cp < 0x80) {
			s->push_back((char)cp);
		} else if (cp < 0x800) {
			s->push_back((char)(0xC0 | (cp >> 6)));
			s->push_back((char)(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			s->push_back((char)(0xE0 | (cp >> 12)));
			s->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			s->push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			s->push_back((char)(0xF0 | (cp >> 18)));
			s->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			s->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			s->push_back((char)(0x80 | (cp & 0x3F)));
		}
	}
	bool string(std::string* s)
	{
		if (p >= end || *p != '"')
			return false;
		p++;
		s->clear();
		while (p < end) {
			unsigned char c = (unsigned char)*p++;
			if (c == '"')
				return true;
			if (c < 0x20)
				return false;   // raw control characters are not allowed
			if (c != '\\') {
				s->push_back((char)c);
				continue;
			}
			if (p >= end)
				return false;
			char e = *p++;
			switch (e) {
			case '"': s->push_back('"'); break;
			case '\\': s->push_back('\\'); break;
			case '/': s->push_back('/'); break;
			case 'b': s->push_back('\b'); break;
			case 'f': s->push_back('\f'); break;
			case 'n': s->push_back('\n'); break;
			case 'r': s->push_back('\r'); break;
			case 't': s->push_back('\t'); break;
			case 'u': {
				uint32_t cp;
				if (!hex4(&cp))
					return false;
				if (cp >= 0xD800 && cp < 0xDC00) {
					uint32_t lo;
					const char* save = p;
					if (end - p >= 6 && p[0] == '\\' && p[1] == 'u' && (p += 2, hex4(&lo)) &&
						lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					} else {
						p = save;
						cp = '?';
					}
				} else if (cp >= 0xDC00 && cp < 0xE000) {
					cp = '?';
				}
				put_utf8(s, cp);
				break;
			}
			default:
				return false;
			}
		}
		return false;   // unterminated
	}
	bool number(double* v)
	{
		const char* s = p;
		if (p < end && *p == '-')
			p++;
		if (p >= end || *p < '0' || *p > '9')
			return false;
		while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
			p++;
		char buf[64];
		size_t n = (size_t)(p - s);
		if (n >= sizeof(buf))
			return false;
		memcpy(buf, s, n);
		buf[n] = '\0';
		char* e = nullptr;
		*v = strtod(buf, &e);
		return e == buf + n;
	}
	bool value(RaJson* out)
	{
		ws();
		if (p >= end)
			return false;
		switch (*p) {
		case '{': {
			if (++depth > 32)
				return false;
			p++;
			out->type = RaJson::OBJ;
			ws();
			if (p < end && *p == '}') {
				p++;
				depth--;
				return true;
			}
			for (;;) {
				ws();
				std::string key;
				if (!string(&key))
					return false;
				ws();
				if (p >= end || *p != ':')
					return false;
				p++;
				out->keys.push_back(key);
				out->items.emplace_back();
				if (!value(&out->items.back()))
					return false;
				ws();
				if (p < end && *p == ',') {
					p++;
					continue;
				}
				if (p < end && *p == '}') {
					p++;
					depth--;
					return true;
				}
				return false;
			}
		}
		case '[': {
			if (++depth > 32)
				return false;
			p++;
			out->type = RaJson::ARR;
			ws();
			if (p < end && *p == ']') {
				p++;
				depth--;
				return true;
			}
			for (;;) {
				out->items.emplace_back();
				if (!value(&out->items.back()))
					return false;
				ws();
				if (p < end && *p == ',') {
					p++;
					continue;
				}
				if (p < end && *p == ']') {
					p++;
					depth--;
					return true;
				}
				return false;
			}
		}
		case '"':
			out->type = RaJson::STR;
			return string(&out->str);
		case 't':
			out->type = RaJson::BOOL;
			out->b = true;
			return lit("true");
		case 'f':
			out->type = RaJson::BOOL;
			out->b = false;
			return lit("false");
		case 'n':
			out->type = RaJson::NUL;
			return lit("null");
		default:
			out->type = RaJson::NUM;
			return number(&out->num);
		}
	}
};

} // namespace

bool ra_json_parse(const char* text, size_t len, RaJson* out)
{
	*out = RaJson();
	if (!text)
		return false;
	Parser ps = { text, text + len, 0 };
	// a UTF-8 BOM is tolerated
	if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		ps.p += 3;
	if (!ps.value(out)) {
		*out = RaJson();
		return false;
	}
	// only whitespace (or NUL padding) may follow
	ps.ws();
	while (ps.p < ps.end && *ps.p == '\0')
		ps.p++;
	if (ps.p != ps.end) {
		*out = RaJson();
		return false;
	}
	return true;
}

void ra_json_quote(std::string* out, const char* s)
{
	static const char hex[] = "0123456789abcdef";
	out->push_back('"');
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"': out->append("\\\""); break;
		case '\\': out->append("\\\\"); break;
		case '\n': out->append("\\n"); break;
		case '\r': out->append("\\r"); break;
		case '\t': out->append("\\t"); break;
		case '\b': out->append("\\b"); break;
		case '\f': out->append("\\f"); break;
		default:
			if (c < 0x20) {
				out->append("\\u00");
				out->push_back(hex[c >> 4]);
				out->push_back(hex[c & 15]);
			} else {
				out->push_back((char)c);
			}
		}
	}
	out->push_back('"');
}

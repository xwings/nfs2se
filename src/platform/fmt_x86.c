/* vsprintf over a 32-bit x86 argument area. The game hands vsprintf_wrap a
 * pointer into its emulated x86 stack, where every argument fills 4-byte slots
 * and doubles take two slots with only 4-byte alignment. The wasm va_list ABI
 * aligns doubles to 8 bytes, so the pointer cannot be forwarded to libc. Each
 * conversion is re-formatted individually with sprintf.
 *
 * Like the game's own vsprintf, the output is unbounded: the caller's buffer
 * size is unknown. */
#include "platform.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL };

/* One parsed conversion. Width and precision reach sprintf as int arguments
 * ("%*.*"), so the rebuilt spec has a fixed size whatever the format holds. */
typedef struct
{
	char flags[6]; /* each of "-+ #0" at most once */
	int width;     /* negative left-justifies, as a negative '*' argument does */
	int precision; /* negative when absent */
	int length;    /* LEN_* */
	char conv;     /* '\0' when the format ends inside the conversion */
} Spec;

#define SPEC_TEXT 16 /* '%', five flags, "*.*", "ll", the conversion and '\0' */

static uint32_t take32(const uint8_t **args)
{
	uint32_t v;
	memcpy(&v, *args, 4);
	*args += 4;
	return v;
}

static uint64_t take64(const uint8_t **args)
{
	uint64_t v;
	memcpy(&v, *args, 8);
	*args += 8;
	return v;
}

/* A decimal field; false when it does not fit an int. Every digit is consumed
 * either way. */
static bool parse_number(const char **fmt, int *out)
{
	bool fits = true;
	*out = 0;
	for (; isdigit((unsigned char)**fmt); ++*fmt)
	{
		const int digit = **fmt - '0';
		fits = fits && *out <= (INT_MAX - digit) / 10;
		*out = fits ? *out * 10 + digit : 0;
	}
	return fits;
}

/* A width or precision; '*' takes it from the arguments. */
static bool parse_field(const char **fmt, const uint8_t **args, int *out)
{
	if (**fmt != '*')
		return parse_number(fmt, out);
	++*fmt;
	*out = (int32_t)take32(args);
	return true;
}

/* Parses the conversion after its '%'. False when a field is out of range;
 * a width of INT_MIN is too, as it has no positive counterpart to left-justify
 * with. */
static bool parse_spec(const char **fmt, const uint8_t **args, Spec *spec)
{
	static const struct
	{
		const char *text;
		int length;
	} lengths[] = {
		{"hh", LEN_HH}, {"h", LEN_H}, {"ll", LEN_LL}, {"l", LEN_L}, {"I64", LEN_LL},
		{"L", LEN_L}, /* long double is double on the original MSVC target */
	};

	*spec = (Spec){.precision = -1};
	for (; **fmt && strchr("-+ #0", **fmt); ++*fmt)
	{
		if (!strchr(spec->flags, **fmt))
			spec->flags[strlen(spec->flags)] = **fmt;
	}
	bool valid = parse_field(fmt, args, &spec->width) && spec->width != INT_MIN;
	if (**fmt == '.')
	{
		++*fmt;
		valid = parse_field(fmt, args, &spec->precision) && valid;
	}
	for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i)
	{
		const size_t n = strlen(lengths[i].text);
		if (strncmp(*fmt, lengths[i].text, n) == 0)
		{
			spec->length = lengths[i].length;
			*fmt += n;
			break;
		}
	}
	spec->conv = **fmt;
	if (spec->conv)
		++*fmt;
	return valid;
}

static long long take_signed(const uint8_t **args, int length)
{
	if (length == LEN_LL)
		return (int64_t)take64(args);
	const uint32_t v = take32(args);
	return length == LEN_HH ? (int8_t)v : length == LEN_H ? (int16_t)v : (int32_t)v;
}

static unsigned long long take_unsigned(const uint8_t **args, int length)
{
	if (length == LEN_LL)
		return take64(args);
	const uint32_t v = take32(args);
	return length == LEN_HH ? (uint8_t)v : length == LEN_H ? (uint16_t)v : v;
}

/* "%<flags>*" then ".*" when precision is set, then suffix. */
static void spec_text(char *text, const Spec *spec, bool precision, const char *suffix)
{
	snprintf(text, SPEC_TEXT, "%%%s*%s%s", spec->flags, precision ? ".*" : "", suffix);
}

/* Formats one conversion at o, taking its arguments; written is the length
 * of the output so far. Returns the characters written, or -1 when the
 * conversion is unknown or sprintf fails. */
static int format_conversion(char *o, const Spec *spec, const uint8_t **args, int32_t written)
{
	char text[SPEC_TEXT];
	switch (spec->conv)
	{
	case 'd':
	case 'i':
		spec_text(text, spec, true, "lld");
		return sprintf(o, text, spec->width, spec->precision, take_signed(args, spec->length));
	case 'u':
	case 'x':
	case 'X':
	case 'o':
		spec_text(text, spec, true, (const char[]){'l', 'l', spec->conv, '\0'});
		return sprintf(o, text, spec->width, spec->precision, take_unsigned(args, spec->length));
	case 'c':
		spec_text(text, spec, false, "c"); /* a precision is undefined for %c */
		return sprintf(o, text, spec->width, (int)(uint8_t)take32(args));
	case 's':
	{
		const char *s = (const char *)(uintptr_t)take32(args);
		spec_text(text, spec, true, "s");
		return sprintf(o, text, spec->width, spec->precision, s ? s : "(null)");
	}
	case 'p':
		/* MSVC prints pointers as 8 upper-case hex digits */
		return sprintf(o, "%08X", take32(args));
	case 'f':
	case 'F':
	case 'e':
	case 'E':
	case 'g':
	case 'G':
	case 'a':
	case 'A':
	{
		const uint64_t bits = take64(args);
		double d;
		memcpy(&d, &bits, sizeof d);
		spec_text(text, spec, true, (const char[]){spec->conv, '\0'});
		return sprintf(o, text, spec->width, spec->precision, d);
	}
	case 'n':
	{
		int32_t *count = (int32_t *)(uintptr_t)take32(args);
		if (count)
			*count = written;
		return 0;
	}
	}
	return -1;
}

int32_t fmt_x86_vsprintf(char *out, const char *fmt, const uint8_t *args)
{
	char *o = out;
	while (*fmt)
	{
		if (*fmt != '%')
		{
			*o++ = *fmt++;
			continue;
		}
		const char *start = fmt++;
		if (*fmt == '%')
		{
			*o++ = '%';
			++fmt;
			continue;
		}

		const uint8_t *specArgs = args;
		Spec spec;
		const bool valid = parse_spec(&fmt, &args, &spec);
		if (!spec.conv)
			break; /* the format ends inside the conversion, which is dropped */
		const int n = valid ? format_conversion(o, &spec, &args, (int32_t)(o - out)) : -1;
		if (n >= 0)
		{
			o += n;
			continue;
		}
		/* Invalid or unknown: copied verbatim, consuming no argument */
		args = specArgs;
		memcpy(o, start, (size_t)(fmt - start));
		o += fmt - start;
	}
	*o = '\0';
	return (int32_t)(o - out);
}

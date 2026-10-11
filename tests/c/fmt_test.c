/* fmt_x86.c: vsprintf over a 32-bit x86 argument area, where every argument
 * takes 4-byte slots and doubles are only 4-byte aligned. */
#include "check.h"
#include "platform.h"

#include <stdlib.h>

typedef struct
{
	uint8_t bytes[256];
	size_t size;
} Args;

static void put(Args *args, const void *value, size_t size)
{
	memcpy(args->bytes + args->size, value, size);
	args->size += size;
}

static void put32(Args *args, uint32_t value)
{
	put(args, &value, sizeof value);
}

static void put64(Args *args, uint64_t value)
{
	put(args, &value, sizeof value);
}

static void put_double(Args *args, double value)
{
	put(args, &value, sizeof value);
}

static void put_str(Args *args, const char *s)
{
	put32(args, (uint32_t)(uintptr_t)s);
}

static void check_format(const char *fmt, const Args *args, const char *expected)
{
	char out[512];
	memset(out, '#', sizeof out);
	CHECK_INT(fmt_x86_vsprintf(out, fmt, args->bytes), strlen(expected));
	CHECK_STR(out, expected);
}

static void test_integers(void)
{
	Args a = {0};
	put32(&a, (uint32_t)-42);
	put32(&a, 4000000000u);
	put32(&a, 0xBEEF);
	put32(&a, 0xBEEF);
	put32(&a, 8);
	check_format("%d %u %x %X %o", &a, "-42 4000000000 beef BEEF 10");

	Args b = {0};
	put32(&b, 7);
	put32(&b, 7);
	put32(&b, 7);
	put32(&b, 6);
	put32(&b, 7);
	put32(&b, (uint32_t)-3);
	check_format("[%5d|%-5d|%05d|%*d|%+i]", &b, "[    7|7    |00007|     7|-3]");

	Args c = {0};
	put32(&c, 0x12345);
	put32(&c, 0x1FF);
	put32(&c, 0x1FFFF);
	put64(&c, (uint64_t)-5000000000LL);
	put64(&c, 0x123456789ULL);
	check_format("%hd %hhd %hu %I64d %llx", &c, "9029 -1 65535 -5000000000 123456789");
}

static void test_doubles(void)
{
	/* The double follows a single int, so it sits at offset 4: unaligned for
	 * the wasm va_list ABI but correct for the game's x86 stack. */
	Args a = {0};
	put32(&a, 1);
	put_double(&a, 3.25);
	put32(&a, 2);
	check_format("%d %.2f %d", &a, "1 3.25 2");

	Args b = {0};
	put32(&b, 3);
	put_double(&b, 2.0 / 3.0);
	put_double(&b, 12345.678);
	put_double(&b, 0.0001);
	put_double(&b, 1.5);
	check_format("%.*f %e %g %Lf", &b, "0.667 1.234568e+04 0.0001 1.500000");
}

static void test_text(void)
{
	Args a = {0};
	put32(&a, 'Z' | 0x4100); /* only the low byte is the character */
	put_str(&a, "track");
	put_str(&a, NULL);
	put_str(&a, "ab");
	put32(&a, 0xABC);
	check_format("%c|%s|%s|%-4s|%p", &a, "Z|track|(null)|ab  |00000ABC");
}

/* Widths and precisions from '*' or long digit runs, as a malformed text table
 * can hold them. */
static void test_fields(void)
{
	/* A negative '*' width left-justifies and a negative '*' precision counts as
	 * absent; repeated flags have no length limit. */
	Args a = {0};
	put32(&a, (uint32_t)-4);
	put32(&a, 7);
	put32(&a, (uint32_t)-1);
	put_double(&a, 1.5);
	put32(&a, 7);
	check_format("[%*d|%.*f|%-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-0-04d]", &a, "[7   |1.500000|7   ]");

	/* A width outside int range, from '*' or digits, makes the conversion
	 * invalid: it is copied verbatim and consumes no argument. */
	Args b = {0};
	put32(&b, (uint32_t)INT32_MIN);
	put32(&b, (uint32_t)INT32_MIN);
	check_format("%-+ #0-+ #0-+ #0-+ #0-+ *.*d|%99999999999d|%d", &b,
	             "%-+ #0-+ #0-+ #0-+ #0-+ *.*d|%99999999999d|-2147483648");
}

static void test_special(void)
{
	int32_t count = -1;
	Args a = {0};
	put32(&a, 12);
	put32(&a, (uint32_t)(uintptr_t)&count);
	put32(&a, 34);
	check_format("100%% %d%n %y %d", &a, "100% 12 %y 34");
	CHECK_INT(count, 7);

	Args none = {0};
	check_format("plain text", &none, "plain text");
	check_format("cut %", &none, "cut ");
}

int main(void)
{
	test_integers();
	test_doubles();
	test_text();
	test_fields();
	test_special();
	return check_exit();
}

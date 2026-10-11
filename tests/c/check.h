/* Minimal checks for the C tests: a failed check prints its location and the
 * values, and check_exit() reports the result as the process exit status. */
#ifndef NFS2SE_CHECK_H
#define NFS2SE_CHECK_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int s_checkFailures, s_checkCount;

#define CHECK(cond)                                                                  \
	do                                                                               \
	{                                                                                \
		++s_checkCount;                                                              \
		if (!(cond))                                                                 \
		{                                                                            \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
			++s_checkFailures;                                                       \
		}                                                                            \
	} while (0)

#define CHECK_INT(actual, expected)                                                       \
	do                                                                                    \
	{                                                                                     \
		const int64_t a_ = (int64_t)(actual), e_ = (int64_t)(expected);                   \
		++s_checkCount;                                                                   \
		if (a_ != e_)                                                                     \
		{                                                                                 \
			fprintf(stderr, "%s:%d: %s is %" PRId64 ", expected %" PRId64 "\n", __FILE__, \
			        __LINE__, #actual, a_, e_);                                           \
			++s_checkFailures;                                                            \
		}                                                                                 \
	} while (0)

#define CHECK_STR(actual, expected)                                                                \
	do                                                                                             \
	{                                                                                              \
		const char *a_ = (actual), *e_ = (expected);                                               \
		++s_checkCount;                                                                            \
		if (!a_ || !e_ || strcmp(a_, e_))                                                          \
		{                                                                                          \
			fprintf(stderr, "%s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, \
			        a_ ? a_ : "(null)", e_ ? e_ : "(null)");                                       \
			++s_checkFailures;                                                                     \
		}                                                                                          \
	} while (0)

static inline int check_exit(void)
{
	printf("%d checks, %d failed\n", s_checkCount, s_checkFailures);
	return s_checkFailures ? 1 : 0;
}

#endif /* NFS2SE_CHECK_H */

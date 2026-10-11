/* records.c: the track records table rebuilt from a track's defaults file. */
#include "check.h"
#include "platform.h"

#include <emscripten.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* The game's binary table: the best lap, then three groups of ten entries. */
typedef struct
{
	char name[10];
	int16_t car;
	int32_t time;
	int32_t mode;
} Entry;

enum
{
	ENTRIES = 31,
	TRACK = 3,
};

/* Imported by the game, which declares it itself. */
int32_t fetchTrackRecords(void *game, uint32_t track, int32_t clear);

static const char *s_defaultsPath = ".\\Defaults\\T%d.TXT";
static const char *s_tablePath = ".\\Out\\T%d.DAT";
static int32_t s_pathIndices[4];
static int s_pathCalls;
static int s_dirtyMarks;

/* The game's path lookup: index track + 20 names the defaults, index track the table. */
void sub_41B250(void *game, int32_t index, int32_t pathBuf)
{
	(void)game;
	if (s_pathCalls < 4)
		s_pathIndices[s_pathCalls] = index;
	++s_pathCalls;
	snprintf((char *)(uintptr_t)pathBuf, MAX_PATH, index >= 20 ? s_defaultsPath : s_tablePath, index % 20);
}

void web_mark_fs_dirty(void)
{
	++s_dirtyMarks;
}

static int32_t entry_car(int i)
{
	return (int16_t)(i * 7 - 50);
}

static int32_t entry_time(int i)
{
	return 100000 + i * 1234;
}

static int32_t entry_mode(int i)
{
	return i % 3 - 1;
}

static void write_defaults(const char *eol)
{
	FILE *f = fopen("defaults/t3.txt", "w");
	CHECK(f != NULL);
	if (!f)
		return;
	fprintf(f, "Records%s42%sBest lap%s", eol, eol, eol);
	fprintf(f, "Lap\tTab%s%d%s%d%s%d%s", eol, entry_car(0), eol, entry_time(0), eol, entry_mode(0), eol);
	for (int i = 1; i < ENTRIES; ++i)
	{
		if ((i - 1) % 10 == 0)
			fprintf(f, "Group %d%s", (i - 1) / 10 + 1, eol);
		else if (i == 15)
			fputs(eol, f); /* blank line between entries */
		fprintf(f, "Name%d%s%d%s%d%s%d%s", i, eol, entry_car(i), eol, entry_time(i), eol, entry_mode(i), eol);
	}
	fclose(f);
}

/* Reads the table; returns false when it is not exactly ENTRIES entries. */
static bool read_table(const char *path, Entry *table)
{
	struct stat st;
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	const bool ok = fread(table, sizeof *table, ENTRIES, f) == ENTRIES && fstat(fileno(f), &st) == 0 &&
	                st.st_size == ENTRIES * (off_t)sizeof *table;
	fclose(f);
	return ok;
}

static void fetch(bool clear)
{
	s_pathCalls = 0;
	s_dirtyMarks = 0;
	CHECK_INT(fetchTrackRecords(NULL, TRACK, clear), 0);
}

static void check_zero_table(void)
{
	Entry table[ENTRIES];
	CHECK(read_table("out/t3.dat", table));
	static const Entry zero;
	for (int i = 0; i < ENTRIES; ++i)
		CHECK(memcmp(&table[i], &zero, sizeof zero) == 0);
}

static void test_defaults(const char *eol)
{
	write_defaults(eol);
	fetch(false);
	CHECK_INT(s_pathCalls, 2);
	CHECK_INT(s_pathIndices[0], TRACK + 20);
	CHECK_INT(s_pathIndices[1], TRACK);
	CHECK_INT(s_dirtyMarks, 1);

	Entry table[ENTRIES];
	CHECK(read_table("out/t3.dat", table));
	CHECK_STR(table[0].name, "Lap"); /* the name ends at a tab */
	char name[16];
	for (int i = 0; i < ENTRIES; ++i)
	{
		if (i > 0)
		{
			snprintf(name, sizeof name, "Name%d", i);
			CHECK_STR(table[i].name, name);
		}
		CHECK_INT(table[i].car, entry_car(i));
		CHECK_INT(table[i].time, entry_time(i));
		CHECK_INT(table[i].mode, entry_mode(i));
	}
}

static void test_missing_defaults(void)
{
	CHECK_INT(unlink("defaults/t3.txt"), 0);
	fetch(false);
	CHECK_INT(s_pathCalls, 2);
	CHECK_INT(s_dirtyMarks, 1);
	check_zero_table();
}

static void test_clear(void)
{
	write_defaults("\n");
	fetch(true);
	CHECK_INT(s_pathCalls, 1);
	CHECK_INT(s_pathIndices[0], TRACK);
	CHECK_INT(s_dirtyMarks, 1);
	check_zero_table();
}

/* An overlong name is cut to nine characters and does not shift the numbers. */
static void test_long_name(void)
{
	FILE *f = fopen("defaults/t3.txt", "w");
	CHECK(f != NULL);
	if (!f)
		return;
	fputs("Records\n1\nBest lap\nChristopherRobin\n5\n6000\n1\n", f);
	fclose(f);
	fetch(false);
	Entry table[ENTRIES];
	CHECK(read_table("out/t3.dat", table));
	CHECK_STR(table[0].name, "Christoph");
	CHECK_INT(table[0].car, 5);
	CHECK_INT(table[0].time, 6000);
	CHECK_INT(table[0].mode, 1);
	CHECK_STR(table[1].name, "");
	CHECK_INT(table[1].time, 0);
}

/* A table that cannot be written is not reported as a change to persist. */
static void test_write_failure(void)
{
	s_tablePath = ".\\Missing\\T%d.DAT";
	fetch(true);
	CHECK_INT(s_dirtyMarks, 0);

	EM_ASM(FS.createDevice('/records', 'full', null, function() { throw new Error('disk full'); }););
	s_tablePath = ".\\Full";
	fetch(true);
	CHECK_INT(s_dirtyMarks, 0);
	s_tablePath = ".\\Out\\T%d.DAT";
}

int main(void)
{
	CHECK_INT(mkdir("/records", 0777), 0);
	CHECK_INT(chdir("/records"), 0);
	CHECK_INT(mkdir("defaults", 0777), 0);
	CHECK_INT(mkdir("out", 0777), 0);
	config_set_settings_dir(NULL);

	test_defaults("\n");
	test_defaults("\r\n");
	test_missing_defaults();
	test_clear();
	test_long_name();
	test_write_failure();
	return check_exit();
}

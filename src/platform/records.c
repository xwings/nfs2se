/* Track records. The game keeps each track's best times in a binary table and
 * calls fetchTrackRecords to rebuild it: from the track's default records, a
 * text file shipped with the game data, or empty when the player clears them.
 * Implements the call contract of NFSIISE's wrapper (MIT, zaps166). */
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NAME_SIZE 10
#define GROUPS 3
#define GROUP_ENTRIES 10
#define ENTRIES (1 + GROUPS * GROUP_ENTRIES) /* the best lap, then the groups */
#define DEFAULTS_PATH_INDEX 20 /* the game's path index of a track's defaults is track + 20 */
#define LINE_SIZE 128

typedef struct
{
	char name[NAME_SIZE];
	int16_t car;
	int32_t time;
	int32_t mode;
} Entry;
_Static_assert(sizeof(Entry) * ENTRIES == 620, "the binary table is 620 bytes");

/* The defaults hold one value per line: a heading, a number the game ignores,
 * a heading and the best lap, then a heading and ten entries per group. An
 * entry is a name line followed by car, time and mode lines. */
typedef struct
{
	FILE *file;
	char line[LINE_SIZE];
} Reader;

/* Next non-blank line without its line ending, cut to LINE_SIZE - 1
 * characters; an empty string at the end of the file. */
static const char *next_line(Reader *r)
{
	while (fgets(r->line, sizeof r->line, r->file))
	{
		const size_t len = strcspn(r->line, "\r\n");
		if (r->line[len] == '\0' && !feof(r->file))
		{
			int c;
			do
				c = fgetc(r->file);
			while (c != '\n' && c != EOF);
		}
		r->line[len] = '\0';
		if (len > 0)
			return r->line;
	}
	r->line[0] = '\0';
	return r->line;
}

static int32_t next_number(Reader *r)
{
	return (int32_t)strtol(next_line(r), NULL, 10);
}

static void read_entry(Reader *r, Entry *e)
{
	const char *name = next_line(r);
	const size_t len = strcspn(name, "\t");
	memcpy(e->name, name, len < NAME_SIZE - 1 ? len : NAME_SIZE - 1);
	e->car = (int16_t)next_number(r);
	e->time = next_number(r);
	e->mode = next_number(r);
}

static void load_defaults(FILE *f, Entry *table)
{
	Reader r = {.file = f};
	next_line(&r);
	next_number(&r);
	next_line(&r);
	read_entry(&r, &table[0]);
	for (int i = 1; i < ENTRIES; ++i)
	{
		if ((i - 1) % GROUP_ENTRIES == 0)
			next_line(&r);
		read_entry(&r, &table[i]);
	}
}

/* Writes the table in one piece; a failed write leaves no partial file. */
static void save_table(const char *gamePath, const Entry *table)
{
	char *path = config_convert_path(gamePath, true);
	if (!path)
		return;
	FILE *f = fopen(path, "wb");
	if (f)
	{
		const bool written = fwrite(table, sizeof *table, ENTRIES, f) == ENTRIES;
		if (fclose(f) == 0 && written)
			web_mark_fs_dirty();
		else
			unlink(path);
	}
	free(path);
}

int32_t fetchTrackRecords(void *game, uint32_t track, int32_t clear)
{
	Entry table[ENTRIES];
	memset(table, 0, sizeof table);
	char gamePath[MAX_PATH];

	if (!clear)
	{
		sub_41B250(game, (int32_t)track + DEFAULTS_PATH_INDEX, (int32_t)(uintptr_t)gamePath);
		FILE *f = config_fopen(gamePath, "r");
		if (f)
		{
			load_defaults(f, table);
			fclose(f);
		}
	}

	sub_41B250(game, (int32_t)track, (int32_t)(uintptr_t)gamePath);
	save_table(gamePath, table);
	return 0;
}

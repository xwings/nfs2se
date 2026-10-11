/* nfs2se.conf settings and the mapping from the game's Windows paths to the
 * browser file system. Implements the conventions of NFSIISE's wrapper (MIT,
 * zaps166) so existing nfs2se.conf files and save layouts keep working. Free
 * of other platform state, so tests/c/config_test.c runs it under Node. */
#include "platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

Config g_config;

static char *s_settingsDir;

void config_reset(void)
{
	memset(&g_config, 0, sizeof g_config);
	g_config.keepAspectRatio = true;
	g_config.linearFiltering = true;
	for (int pad = 0; pad < CTL_PAD_COUNT; ++pad)
	{
		for (int axis = 0; axis < JOY_AXES; ++axis)
			g_config.joystickAxes[pad][axis] = axis;
		g_config.joystickEscButton[pad] = -1;
		g_config.joystickResetButton[pad] = -1;
		for (int i = 0; i < JOY_DPAD_BUTTONS; ++i)
			g_config.joystickDPadButtons[pad][i] = -1;
	}
}

static const struct
{
	const char *key;
	bool *value;
} s_flags[] = {
	{"KeepAspectRatio", &g_config.keepAspectRatio},
	{"LinearTextureFiltering", &g_config.linearFiltering},
	{"JoystickApplyDeadzone", &g_config.joystickApplyDeadzone},
	{"JoystickDisableAxesInMenu", &g_config.joystickDisableAxesInMenu},
};

static bool key_is(const char *key, size_t keyLen, const char *name)
{
	return strlen(name) == keyLen && !strncasecmp(key, name, keyLen);
}

/* Reads `count` comma-separated integers from *text into out and advances
 * *text past them. Returns false, leaving out unchanged, when one is missing. */
static bool parse_list(const char **text, int32_t *out, int count)
{
	int32_t values[JOY_AXES];
	const char *p = *text;
	for (int i = 0; i < count; ++i)
	{
		if (i > 0 && *p++ != ',')
			return false;
		char *end;
		values[i] = (int32_t)strtol(p, &end, 10);
		if (end == p)
			return false;
		p = end;
	}
	memcpy(out, values, (size_t)count * sizeof *values);
	*text = p;
	return true;
}

/* Joystick<pad><setting>; Axes2 holds the source axes, a colon, then the modes. */
static void parse_joystick(const char *key, size_t keyLen, const char *value)
{
	static const char prefix[] = "Joystick";
	const size_t prefixLen = sizeof prefix - 1;
	if (keyLen <= prefixLen || strncasecmp(key, prefix, prefixLen) != 0)
		return;
	const int pad = key[prefixLen] - '0';
	if (pad < 0 || pad >= CTL_PAD_COUNT)
		return;
	const char *setting = key + prefixLen + 1;
	const size_t settingLen = keyLen - prefixLen - 1;

	if (key_is(setting, settingLen, "Axes2"))
	{
		int32_t axes[JOY_AXES];
		if (parse_list(&value, axes, JOY_AXES) && *value++ == ':' &&
		    parse_list(&value, g_config.joystickAxisModes[pad], JOY_AXES))
			memcpy(g_config.joystickAxes[pad], axes, sizeof axes);
	}
	else if (key_is(setting, settingLen, "EscButton"))
		g_config.joystickEscButton[pad] = (int32_t)strtol(value, NULL, 10);
	else if (key_is(setting, settingLen, "ResetButton"))
		g_config.joystickResetButton[pad] = (int32_t)strtol(value, NULL, 10);
	else if (key_is(setting, settingLen, "DPadButtons"))
		parse_list(&value, g_config.joystickDPadButtons[pad], JOY_DPAD_BUTTONS);
}

/* Window, CPU, MSAA, vsync, sound interpolation, network port and COM port
 * keys have no browser equivalent and are ignored like unknown keys. */
static void parse_line(const char *line)
{
	const char *eq = strchr(line, '=');
	if (!eq)
		return;
	const size_t keyLen = (size_t)(eq - line);
	const char *value = eq + 1;
	for (size_t i = 0; i < sizeof s_flags / sizeof *s_flags; ++i)
	{
		if (key_is(line, keyLen, s_flags[i].key))
		{
			*s_flags[i].value = strtol(value, NULL, 10) != 0;
			return;
		}
	}
	parse_joystick(line, keyLen, value);
}

void config_parse(FILE *f)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, f)) >= 0)
	{
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len > 0 && line[0] != ' ' && line[0] != '#')
			parse_line(line);
	}
	free(line);
}

void config_set_settings_dir(const char *dir)
{
	free(s_settingsDir);
	s_settingsDir = dir ? strdup(dir) : NULL;
}

static bool has_prefix(const char *s, const char *prefix)
{
	return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_stats_name(const char *name)
{
	const size_t len = strlen(name);
	return len > 4 && strcasecmp(name + len - 4, ".stf") == 0;
}

/* The settings subdirectory holding gamePath, or NULL when it belongs to the
 * game data. *name receives the rest of gamePath, kept verbatim: backslashes
 * stay part of the file name, but a '/' in it still separates directories. */
static const char *settings_subdir(const char *gamePath, const char **name)
{
	static const struct
	{
		const char *prefix, *subdir;
		bool statsOnly; /* only .stf files with a name are redirected */
	} dirs[] = {
		{".\\fedata\\pc\\config\\", "config", false},
		{".\\fedata\\pc\\save\\", "save", false},
		{".\\gamedata\\tmptrk\\", "tmptrk", false},
		{".\\fedata\\pc\\stats\\prh\\", "stats/prh", true},
		{".\\fedata\\pc\\stats\\", "stats", true},
	};
	if (strcasecmp(gamePath, "replay.rpy") == 0)
	{
		*name = gamePath;
		return "tmptrk";
	}
	for (size_t i = 0; i < sizeof dirs / sizeof *dirs; ++i)
	{
		if (has_prefix(gamePath, dirs[i].prefix))
		{
			*name = gamePath + strlen(dirs[i].prefix);
			return dirs[i].statsOnly && !is_stats_name(*name) ? NULL : dirs[i].subdir;
		}
	}
	return NULL;
}

/* Game data paths are relative to the working directory: backslashes become
 * slashes and, for the case-sensitive file system, letters may be lowered. */
static char *game_data_path(const char *gamePath, bool toLower)
{
	char *path = strdup(gamePath);
	if (!path)
		return NULL;
	for (size_t i = 0; path[i] != '\0'; ++i)
	{
		if (path[i] == '\\')
			path[i] = '/';
		else if (toLower)
			path[i] = (char)tolower((unsigned char)path[i]);
	}
	return path;
}

/* User data (config, saves, stats, temporary tracks and replays) lives in the
 * persisted settings directory; everything else maps to the game directory. */
char *config_convert_path(const char *gamePath, bool toLower)
{
	const char *name;
	const char *subdir = s_settingsDir ? settings_subdir(gamePath, &name) : NULL;
	if (!subdir)
		return game_data_path(gamePath, toLower);
	char *path;
	return asprintf(&path, "%s/%s/%s", s_settingsDir, subdir, name) < 0 ? NULL : path;
}

FILE *config_fopen(const char *gamePath, const char *mode)
{
	char *path = config_convert_path(gamePath, true);
	if (!path)
		return NULL;
	FILE *f = fopen(path, mode);
	free(path);
	return f;
}

static bool is_directory(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The launcher supplies the support files, so only the imported data can lack
 * the directories the game loads everything from. */
const char *config_check_game_dirs(void)
{
	if (is_directory("gamedata") && is_directory("fedata/pc"))
		return NULL;
	return "The imported game data has no \"gamedata\" or \"fedata/pc\" directory. "
	       "Import the folder that contains them.";
}

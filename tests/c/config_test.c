/* config.c: nfs2se.conf parsing, path mapping and the game data check. */
#include "check.h"
#include "platform.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void parse(const char *text)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	config_parse(f);
	fclose(f);
}

static void check_path(const char *src, bool toLower, const char *expected)
{
	char *path = config_convert_path(src, toLower);
	CHECK_STR(path, expected);
	free(path);
}

static void test_defaults(void)
{
	config_reset();
	CHECK(g_config.keepAspectRatio);
	CHECK(g_config.linearFiltering);
	CHECK(!g_config.joystickApplyDeadzone);
	CHECK(!g_config.joystickDisableAxesInMenu);
	for (int pad = 0; pad < CTL_PAD_COUNT; ++pad)
	{
		for (int axis = 0; axis < JOY_AXES; ++axis)
		{
			CHECK_INT(g_config.joystickAxes[pad][axis], axis);
			CHECK_INT(g_config.joystickAxisModes[pad][axis], 0);
		}
		CHECK_INT(g_config.joystickEscButton[pad], -1);
		CHECK_INT(g_config.joystickResetButton[pad], -1);
		for (int i = 0; i < JOY_DPAD_BUTTONS; ++i)
			CHECK_INT(g_config.joystickDPadButtons[pad][i], -1);
	}
}

static void test_parse(void)
{
	config_reset();
	parse("# comment\n"
	      " KeepAspectRatio=0\n" /* lines starting with a space are ignored */
	      "\n"
	      "lineartexturefiltering=0\r\n"
	      "JoystickApplyDeadzone=1\n"
	      "JoystickDisableAxesInMenuX=1\n" /* longer key: not a flag */
	      "Joystick0Axes2=1,0,-1,3, 4,5:0,1,-1,0,0,1\n"
	      "Joystick1Axes2=1,2,3\n"                   /* incomplete: ignored */
	      "Joystick1Axes2=5,4,3,2,1,0:1,1\n"         /* incomplete modes: ignored */
	      "Joystick1EscButton=9\n"
	      "joystick0resetbutton=8\n"
	      "Joystick1DPadButtons=10,11,12,13\n"
	      "Joystick0DPadButtons=1,2\n"               /* incomplete: ignored */
	      "Joystick2EscButton=7\n"                   /* no such pad */
	      "VSync=0\n"
	      "MSAA=4\n"
	      "NoEqualsSign\n");
	CHECK(g_config.keepAspectRatio);
	CHECK(!g_config.linearFiltering);
	CHECK(g_config.joystickApplyDeadzone);
	CHECK(!g_config.joystickDisableAxesInMenu);
	const int32_t axes[JOY_AXES] = {1, 0, -1, 3, 4, 5}, modes[JOY_AXES] = {0, 1, -1, 0, 0, 1};
	for (int i = 0; i < JOY_AXES; ++i)
	{
		CHECK_INT(g_config.joystickAxes[0][i], axes[i]);
		CHECK_INT(g_config.joystickAxisModes[0][i], modes[i]);
		CHECK_INT(g_config.joystickAxes[1][i], i);
		CHECK_INT(g_config.joystickAxisModes[1][i], 0);
	}
	CHECK_INT(g_config.joystickEscButton[0], -1);
	CHECK_INT(g_config.joystickEscButton[1], 9);
	CHECK_INT(g_config.joystickResetButton[0], 8);
	CHECK_INT(g_config.joystickResetButton[1], -1);
	for (int i = 0; i < JOY_DPAD_BUTTONS; ++i)
	{
		CHECK_INT(g_config.joystickDPadButtons[0][i], -1);
		CHECK_INT(g_config.joystickDPadButtons[1][i], 10 + i);
	}
}

static void test_paths(void)
{
	config_set_settings_dir(NULL);
	check_path(".\\GameData\\Config\\X.DAT", true, "./gamedata/config/x.dat");
	check_path(".\\GameData\\Config\\X.DAT", false, "./GameData/Config/X.DAT");
	check_path(".\\fedata\\pc\\save\\a.sav", true, "./fedata/pc/save/a.sav");
	check_path("replay.rpy", true, "replay.rpy");

	config_set_settings_dir("/home/web_user/.nfs2se");
	check_path(".\\fedata\\pc\\config\\Nfs2se.cfg", true, "/home/web_user/.nfs2se/config/Nfs2se.cfg");
	check_path(".\\FEDATA\\PC\\SAVE\\a.sav", true, "/home/web_user/.nfs2se/save/a.sav");
	check_path(".\\gamedata\\tmptrk\\t.trk", true, "/home/web_user/.nfs2se/tmptrk/t.trk");
	check_path("REPLAY.RPY", true, "/home/web_user/.nfs2se/tmptrk/REPLAY.RPY");
	check_path(".\\fedata\\pc\\stats\\tr01.stf", true, "/home/web_user/.nfs2se/stats/tr01.stf");
	check_path(".\\fedata\\pc\\stats\\prh\\tr01.STF", true, "/home/web_user/.nfs2se/stats/prh/tr01.STF");
	check_path(".\\fedata\\pc\\stats\\tr01.txt", true, "./fedata/pc/stats/tr01.txt");
	check_path(".\\fedata\\pc\\stats\\.stf", true, "./fedata/pc/stats/.stf");
	check_path(".\\fedata\\pc\\stats\\prh\\.stf", true, "./fedata/pc/stats/prh/.stf");
	check_path(".\\fedata\\pc\\art\\x.qfs", true, "./fedata/pc/art/x.qfs");
	/* the rest of a redirected path stays one file name */
	check_path(".\\fedata\\pc\\save\\..\\..\\x", true, "/home/web_user/.nfs2se/save/..\\..\\x");

	config_set_settings_dir(NULL);
	check_path(".\\fedata\\pc\\save\\a.sav", true, "./fedata/pc/save/a.sav");
}

static void test_fopen(void)
{
	CHECK_INT(mkdir("/fopen", 0777), 0);
	CHECK_INT(mkdir("/fopen/settings", 0777), 0);
	CHECK_INT(mkdir("/fopen/settings/save", 0777), 0);
	CHECK_INT(mkdir("/fopen/gamedata", 0777), 0);
	CHECK_INT(chdir("/fopen"), 0);
	config_set_settings_dir("/fopen/settings");

	FILE *f = config_fopen(".\\FEDATA\\PC\\SAVE\\Slot.sav", "w");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	struct stat st;
	CHECK_INT(stat("/fopen/settings/save/Slot.sav", &st), 0);

	f = fopen("gamedata/x.dat", "w");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	f = config_fopen(".\\GAMEDATA\\X.DAT", "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	CHECK(config_fopen(".\\gamedata\\missing.dat", "r") == NULL);
	config_set_settings_dir(NULL);
}

static void test_game_dirs(void)
{
	CHECK_INT(mkdir("/check", 0777), 0);
	CHECK_INT(chdir("/check"), 0);
	CHECK(config_check_game_dirs() != NULL && strstr(config_check_game_dirs(), "\"gamedata\""));
	CHECK_INT(mkdir("gamedata", 0777), 0);
	CHECK_INT(mkdir("fedata", 0777), 0);
	CHECK(config_check_game_dirs() != NULL && strstr(config_check_game_dirs(), "\"fedata/pc\""));
	CHECK_INT(mkdir("fedata/pc", 0777), 0);
	CHECK(config_check_game_dirs() == NULL);
	CHECK_INT(rmdir("gamedata"), 0);
	FILE *f = fopen("gamedata", "w"); /* a file is not the directory */
	CHECK(f != NULL);
	if (f)
		fclose(f);
	CHECK(config_check_game_dirs() != NULL);
}

int main(void)
{
	test_defaults();
	test_parse();
	test_paths();
	test_fopen();
	test_game_dirs();
	return check_exit();
}

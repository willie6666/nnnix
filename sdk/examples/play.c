/* SPDX-License-Identifier: GPL-3.0-or-later */
/* play as a loadable program: the firmware's built-in player (user/play.c)
 * built with the SDK, an example of a larger program. The shell runs the
 * built-in `play`; this one runs as /bin/play only by its path (/bin/play). */
#define NNNIX_SDK
#include "../../user/play.c"

int main(int argc, char **argv) { return cmd_play(argc, argv); }

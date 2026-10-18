/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The entry of a program in bin/: the Makefile builds this with MAIN set to
 * the command it runs (cmd_wc, cmd_ed, ...). */
int MAIN(int argc, char **argv);

int main(int argc, char **argv) { return MAIN(argc, argv); }

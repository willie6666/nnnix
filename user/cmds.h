/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CMDS_H
#define NNNIX_CMDS_H

/* Built-in commands of nsh: int cmd(int argc, char **argv), 0 on success. */
const char *errstr(int e);
int cmd_ls(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_cp(int argc, char **argv);
int cmd_mv(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_rmdir(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_mkfile(int argc, char **argv);
int cmd_sum(int argc, char **argv);
int cmd_wc(int argc, char **argv);
int cmd_head(int argc, char **argv);
int cmd_tail(int argc, char **argv);
int cmd_grep(int argc, char **argv);
int cmd_hexdump(int argc, char **argv);

#endif

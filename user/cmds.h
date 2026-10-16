/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CMDS_H
#define NNNIX_CMDS_H

/* Built-in commands of nsh: int cmd(int argc, char **argv), 0 on success. */
const char *errstr(int e);
int nn_interrupted(void);            /* Ctrl-C typed while this command ran */
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
int cmd_date(int argc, char **argv);
int cmd_ifconfig(int argc, char **argv);
int cmd_dhcp(int argc, char **argv);
int cmd_service(int argc, char **argv);
int cmd_netstat(int argc, char **argv);
int cmd_nc(int argc, char **argv);
int cmd_ntpdate(int argc, char **argv);
int cmd_ed(int argc, char **argv);
int cmd_sysctl(int argc, char **argv);
int cmd_mkfs(int argc, char **argv);
int cmd_fwupdate(int argc, char **argv);

#endif

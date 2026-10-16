# User environment: shell, editor, clock and network (M5)

The LCD/keyboard and telnet consoles use the same `nsh`. Commands remain built
into the firmware; scripts are text files on littlefs or the USB drive.

## Quoting and variables

```
set NAME "hello world"
echo "$NAME"
echo '${NAME}'
env
unset NAME
echo $?
```

`set NAME VALUE` sets one of eight variables local to the current shell. Names
start with a letter or underscore and may then contain digits. Double quotes
allow `$NAME` and `${NAME}` expansion; single quotes preserve literal text.
`$?` is the previous command's status (0 for success). An unset variable expands
to an empty string. Expansion stays in one argument, including when unquoted;
there is no field splitting or globbing. A backslash escapes the next character
outside single quotes. Adjacent quoted and unquoted fragments form one word.

An unquoted `#` at the start of a word begins a comment. `<`, `>` and `>>` may
touch a word, as in `cat</home/in.txt>/home/out.txt`. Quoted operators are literal
text. There can be at most ten arguments after the command and four
redirections. Syntax errors are rejected before files are opened.

The interactive terminal accepts up to 94 characters per line. Script lines
may contain up to 127 bytes; expanded words share a 192-byte buffer. Excess
arguments, unclosed quotes and oversized expansions produce an error rather
than executing a partial command. Semicolon command lists are not supported.

## Pipes, background jobs and Ctrl-C

```
ls / | grep usb
help | grep kill | wc
hexdump /dev/zero | head -n 2
sleep 30 &
jobs
kill %1
wait
```

A pipeline has up to three commands joined by `|`; a trailing `&` runs it in the
background. Every command of a pipeline or job runs in its own task with a
1152-byte stack, connected by 128-byte kernel pipes, so each stage costs about
1.2 KiB of RAM while it runs. A plain command without `|` or `&` runs inside
the shell and costs nothing extra. Commands that change the shell itself (`cd`,
`set`, `unset`, `env`, `sh`, `source`, `exit`, `jobs`, `kill`, `wait`) cannot be
used in a pipeline or in the background.

A background job reads `/dev/null` unless its input is redirected; its output
still goes to the terminal. `jobs` lists running jobs, `kill %N` stops job N
(`kill PID` stops any program task), and `wait` waits for all jobs. Finished jobs
are reported before the next prompt. Logging out stops the session's jobs.

Ctrl-C stops the foreground command or pipeline. Without memory protection a
program cannot be stopped at an arbitrary point: it leaves at its next system
call, and sleeping or waiting on a pipe, the terminal or a child ends at once.
Writing to a pipe whose reader has gone also ends the writer, like SIGPIPE.

## Scripts and boot

```
sh /home/setup.sh
source /usb/SETUP.SH
```

`sh`, `source` and `.` all run a file in the current shell: variables and the
working directory are shared. Up to three scripts may be nested. This uses a
stack of file descriptors, not recursive shell tasks. `exit` in a script stops
that script and returns to its caller. LF, CRLF and a final line without a
newline are accepted. An overlong or binary line stops the script without
executing its truncated prefix. A script invocation itself cannot be redirected;
individual commands in the script can use redirection.

The optional `/etc/rc` runs once at boot, before starting the LCD and telnet
shells. Its unredirected output goes to the serial kernel log. It is absent by
default; upgrading firmware does not create or overwrite it. For example:

```
echo 'echo boot-script-ok > /home/boot.txt' > /etc/rc
reboot
cat /home/boot.txt
```

Variables in `/etc/rc` belong to the boot script, not to later interactive
sessions. Logins (LCD and telnet) start once `/etc/rc` has finished, or after
10 s if it is still running. A typical network setup is shown under
[Network](#network) below.

## Text tools

| Command | Behaviour |
| --- | --- |
| `wc [FILE...]` | Count newline characters, whitespace-separated words and bytes |
| `head [-n COUNT] [FILE...]` | Print the first lines (default 10) |
| `tail [-n COUNT] [FILE...]` | Print the last lines (default 10); requires a seekable file |
| `grep [-n] TEXT [FILE...]` | Literal substring search, optionally with line numbers |
| `hexdump [FILE]` | 16 bytes per row, offset, hexadecimal and printable ASCII |

With no file argument these read fd 0, which can be redirected. `grep` returns
0 for a match, 1 for no match and 2 for an error. It accepts lines up to 192 bytes
(including the newline); an oversized line is reported explicitly. Other tools
stream fixed-size chunks regardless of file size. `sleep` rejects non-numeric
or overflowing durations instead of accepting a partial number.

## HTTP file transfer

W5500 socket 4 serves one HTTP transfer at a time on TCP port 8080. A worker and
its small buffers exist only during the connection; the LCD and telnet shells
keep running while a client transfers a file or waits to send a header.

From a host with a route to the board:

```
python3 tools/nncp.py put local.bin /home/local.bin
python3 tools/nncp.py get /home/local.bin copy.bin
```

On the NanoPi, with the unused `eth1` adapter cabled directly to the W5500:

```
sudo python3 tools/nncp.py --iface eth1 put local.bin /home/local.bin
sudo python3 tools/nncp.py --iface eth1 get /home/local.bin copy.bin
```

`--iface` puts the adapter in a temporary `198.18.0.1/30` network namespace and
restores it afterwards. Like the test harness, it refuses an adapter with an
IPv4 address or the default route. `--host` and `--port` select a board reachable
over an already configured route. The client uses only Python's standard
library. Downloads go to a local temporary file and replace the destination
only after the declared byte count is received.

The server supports `GET` and `PUT` for literal paths below `/home/`. Names may
contain ASCII letters, digits, `.`, `_` and `-`, up to 31 bytes per component and
63 bytes per absolute path. Dot components, encoded URLs, query strings and
the reserved `.http-` temporary-file prefix are rejected. Parent directories
must already exist. A PUT needs `Content-Length`, allows at most 1 MiB, and may
use `Expect: 100-continue`. Chunked uploads are not implemented. Requests and
responses use one connection each; header and idle timeouts release a stalled
worker. Request headers are limited to 2 KiB total and 126 bytes before CRLF per line.

A PUT writes a new temporary littlefs file, closes it, then atomically renames
it into place. Disconnects and errors remove that temporary file and keep an
existing destination intact. Loss of power before the rename may leave a
`.http-*.tmp` file, which can be removed from the shell; it cannot be fetched
over HTTP. The server returns 201 for a new file, 204 for replacement and 200
for a download. TCP half-close is supported, including when FIN arrives before
the kernel daemon accepts the buffered request.

HTTP framing follows the relevant subsets of
[RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) and PUT uses
the creation/replacement status codes in
[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.4).
W5500 CLOSE_WAIT data transfer is described in the
[WIZnet datasheet](https://docs.wiznet.io/img/products/w5500/W5500_ds_v110e.pdf).


## Editor: ed

```
ed [-s] [FILE]
```

A line editor in the style of UNIX `ed`, which suits the 21x8 LCD. `*` is the
command prompt; `-s` turns off the prompt and byte counts for scripts, such as
`ed -s /home/notes < /home/edits`. The text lives in one heap block, so the
size of a file that can be edited is bounded by free RAM: growing the buffer
needs the old and the new block at once, so about 2 KiB with one telnet session
open. "? out of memory" leaves the text unchanged.

| Addresses | |
| --- | --- |
| `N` `.` `$` | line N, the current line, the last line |
| `+N` `-N` | relative to the current line (`+` alone is +1) |
| `/text/` | next line containing the literal text, wrapping around |
| `A,B` `,` `;` | a range; `,` is `1,$` and `;` is `.,$` |

| Commands | |
| --- | --- |
| (address only) | print that line; an empty line prints the next one |
| `p` `n` `z` | print, print with line numbers, print a page of 7 lines |
| `a` `i` `c` | append after, insert before, change; input ends with a line holding only `.` |
| `d` `j` | delete lines, join lines |
| `s/old/new/[g][p]` | replace literal text (any delimiter), all occurrences with `g` |
| `=` | line number (`$` by default) |
| `w [FILE]` `wq` | write the buffer (or a range); write and quit |
| `q` `Q` | quit; `q` warns once about unsaved changes, `Q` does not |
| `e FILE` `r [FILE]` `f [FILE]` | edit another file, read a file in after a line, show or set the file name |

Errors print `? ` and a reason. End of input behaves like `q`. There are no
regular expressions, undo, marks, `m`/`t` or `!`.

## Clock

```
date                      local time, or UTC without /etc/tz
date -u                   UTC
date +%s                  seconds since 1970-01-01 UTC
date -s '2026-10-16 14:30[:00]'   set the clock (local time)
ntpdate 198.18.0.1        set the clock from an (S)NTP server
echo +8 > /etc/tz         local offset: +8, -5, +05:30 or +0530
```

The NUC140 RTC runs from the board's 32.768 kHz crystal. It keeps counting
through resets and firmware updates but not through power loss: the chip has no
backup supply, so after power-up the clock starts from its reset value (2005)
until `date -s` or `ntpdate` sets it. If the crystal does not start within 5 s,
the kernel log says so and the time is kept with the system timer instead.
`ntpdate` takes an IP address (there is no DNS resolver yet), tries three times
with a 2 s timeout, and steps the clock; it does not slew it.

## Network

```
ifconfig                                   show the interface
ifconfig eth0 192.168.1.50/24 gw 192.168.1.1 dns 192.168.1.1
dhcp [-t SECONDS]                          get an address (default: wait 20 s)
netstat                                    the eight W5500 sockets
service [telnetd|httpd start|stop]         network services
nc HOST PORT        nc -l PORT             TCP client / server for one connection
nc -u HOST PORT     nc -u -l PORT          the same over UDP
```

At power-up the board uses the static address `198.18.0.2/30` (no gateway),
so it is reachable before `/etc/rc` runs, and both services are on. `/etc/rc`
can then configure the network, for example:

```
service telnetd stop
ifconfig eth0 192.168.1.50/24 gw 192.168.1.1
dhcp -t 15
service telnetd start
```

`dhcp` waits for the Ethernet link, then asks for a lease; if no server answers
(three tries over 14 s) the current address stays. A lease is renewed at half
its time (at most daily) by the kernel daemon, retried every minute if the
server is gone. When the lease ends without a renewal, or the server refuses
one, the address is removed (RFC 2131: it may belong to someone else by now)
and the client asks for a new lease, every 30 s until one comes. An address
that was set with `ifconfig` (or the static boot address) is never removed. Requests ask for broadcast replies, because the W5500
only accepts unicast frames for its current address. `ifconfig` with an address
stops DHCP.

`service telnetd stop` stops accepting logins; sessions already open continue.
The diag test service on TCP port 5000 cannot be stopped.

Socket plan (W5500 buffer per direction): 0 diag (4 KiB), 1-3 telnet (2 KiB
each), 4 HTTP (2 KiB), 5 and 6 for programs such as `nc` and `ntpdate` (2 and
1 KiB), 7 DHCP (1 KiB). So two `nc`/`ntpdate` commands can run at once.

`nc` connects (or waits for one connection) and then copies standard input to
the socket and the socket to standard output. With the terminal as input it
runs interactively until the peer closes, Ctrl-D or Ctrl-C; typed lines are
sent as you press Enter. With a file or pipe as input it sends all of it and
then prints what arrives until the peer closes (UDP: until 2 s of silence):

```
echo hello | nc 198.18.0.1 7000
nc -l 7000 > /home/upload.bin
```

A UDP read returns one datagram and makes its sender the destination of the
next write, so `nc -u -l PORT` answers whoever spoke last. W5500 sockets do not
answer a SYN when no socket listens: a stopped service or closed port looks
like a timeout, not a refusal, from the other side.

Programs use sockets through system calls (`socket`, `connect`, `listen`,
`sockinfo`, then `read`/`write`/`close` like files; see `lib/sys.h`). `connect`
and `listen` do not wait: programs poll `sockinfo` for the state, which keeps
Ctrl-C working while they wait.


## Settings and RAM

nnnix has about 13 KiB of heap. Shells are what use it up: each one, on the LCD
or over telnet, costs about 1.5-1.7 KiB (stack, task and shell state) while it
runs. `sysctl` turns such features off, or makes them start only when needed:

```
sysctl                     list the settings
sysctl -h                  explain them
sysctl console=ask         LCD shell only after Enter (the default)
sysctl idle_logout=600     log out terminals idle for 10 minutes; 0 never
sysctl telnet_max=1 httpd=off
free                       heap, and how much of it is task stacks
```

| Setting | Values | RAM effect |
| --- | --- | --- |
| `console` | `off`, `ask`, `on` | `ask` (default): the LCD shows "Press Enter to log in." and the shell exists only while someone uses it. `on`: a shell always, restarted after logout. `off`: no shell; a running one logs out at once |
| `idle_logout` | seconds, `0` = never | a terminal without input for that long reads as end of file, so its shell logs out and frees its RAM. Busy commands are not interrupted. With `console=on` a new console shell starts right away |
| `telnet_max` | `0`-`3` | how many telnet sessions are accepted at once; sessions already open stay |
| `telnetd`, `httpd` | `on`, `off` | the same as `service ... start/stop`; idle servers cost no RAM, only their sessions do |

Settings last until the next reset; put them in `/etc/rc` to apply them at boot,
for example:

```
sysctl console=ask idle_logout=900 telnet_max=2
```

Other memory is fixed: the kernel daemon, the file system task and the idle
task (about 3 KiB of stacks), and about 2.6 KiB of static data. `ps` shows each
task's stack size and the most it has used.

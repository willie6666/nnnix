# User environment: shell, editor, clock and network (M5)

The LCD/keyboard and telnet consoles use the same `nsh`. Most commands are
built into the firmware; the larger ones (`play`, `ed`, `wc`, `head`, `tail`,
`grep`, `hexdump`, `wget`, `host`) are programs in `/bin` (see "Loadable
programs"). Scripts are text files on littlefs or the USB drive.

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

These are programs in `/bin` (see [Programs](#programs)).

| Command | Behaviour |
| --- | --- |
| `wc [FILE...]` | Count newline characters, whitespace-separated words and bytes |
| `head [-n COUNT] [FILE...]` | Print the first lines (default 10) |
| `tail [-n COUNT] [FILE...]` | Print the last lines (default 10); from a pipe or the terminal, of the last 2 KiB of it |
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
`ntpdate` takes an IP address (`host NAME` looks one up), tries three times
with a 2 s timeout, and steps the clock; it does not slew it. It takes only an
answer from that server's port 123 that gives back the request's transmit time
(a server's reply, NTP version 3 or 4, stratum 1-15).

## Network

```
ifconfig                                   show the interface
ifconfig eth0 192.168.1.50/24 gw 192.168.1.1 dns 192.168.1.1
dhcp [-t SECONDS]                          get an address (default: wait 20 s)
netstat                                    the eight W5500 sockets
service [telnetd|httpd start|stop]         network services
nc HOST PORT        nc -l PORT             TCP client / server for one connection
nc -u HOST PORT     nc -u -l PORT          the same over UDP
host NAME                                  look a name up (DNS)
wget [-q] [-S] [-O FILE] URL               fetch over HTTP
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
sent as you press Enter. With a file or pipe as input it sends it while it
prints what arrives (a child task does the sending, so a peer that echoes
cannot stall it), then keeps printing until the peer closes (UDP: until 2 s of
silence):

```
echo hello | nc 198.18.0.1 7000
nc -l 7000 > /home/upload.bin
```

A UDP read returns one datagram. On a listening socket its sender becomes the
destination of the next write, so `nc -u -l PORT` answers whoever spoke last. A
connected socket (`nc -u HOST PORT`, and the DNS and NTP clients) keeps its
destination and drops datagrams from anyone else; such a read returns
`SYS_EAGAIN` when nothing else is waiting. W5500 sockets do not
answer a SYN when no socket listens: a stopped service or closed port looks
like a timeout, not a refusal, from the other side.

### Internet through the computer

`tools/nnshare.py` lets the board use the internet through the computer its
cable goes to (the USB Ethernet adapter, `eth1` by default):

```
sudo python3 tools/nnshare.py on       # start (in the background)
sudo python3 tools/nnshare.py status
sudo python3 tools/nnshare.py off      # stop and undo everything
sudo python3 tools/nnshare.py on --dry-run    # only show what it would change
```

It gives the adapter 198.18.0.1/30 (unless it has it), turns IPv4 forwarding
on (unless it is), adds nftables tables `nnshare` (NAT for 198.18.0.0/30 to
every other interface), and runs a DHCP server on the adapter and a DNS relay
on 198.18.0.1 to the computer's own resolver. `off` undoes exactly what `on`
changed. Then on the board:

```
ifconfig eth0 198.18.0.2/30 gw 198.18.0.1 dns 198.18.0.1
wget http://example.org/
```

`dhcp` works too (it gets the same address), but a lease outlives `nnshare off`
by up to an hour and then the board removes its address, so the fixed setting
is the better one; it can go in `/etc/rc`. `make test` refuses to run while
sharing is on: both would serve DHCP and DNS on the test network.

Only one `nnshare` runs at a time (it holds `/run/nnshare.lock` from its first
change to its last undo). `off` signals the server only when the pid, the start
time and the token recorded in `/run/nnshare.json` all match it, so a stale
record never stops another process; when the server is gone, `off` undoes what
it recorded. If the DHCP server or the DNS relay fails, sharing stops and
everything is undone (the log says why, `status` shows it).

`host` and `wget` (programs in `/bin`) ask the DNS server of the network
settings (`ifconfig ... dns IP`, or the one DHCP gave) for an A record, over
UDP, three tries of 2 s; a CNAME in the answer is followed. A dotted address
needs no lookup. `wget` fetches an `http://` URL:

```
wget http://example.org/              saves index.html in the current directory
wget -O /home/page.txt http://host:8080/page.txt
wget -O - http://host/notes.txt | grep todo      prints it (-O -: no progress lines)
wget -S -q -O /dev/null http://host/             only the reply's header lines
```

It asks with HTTP/1.0 and `Connection: close`, so the body ends at
`Content-Length` or when the server closes; a chunked body is decoded too
(chunked wins over a `Content-Length`). A reply whose length is not a plain
number, is given twice differently, or uses a transfer coding other than
`chunked` is refused, and so is a chunked body whose sizes or separators are
malformed. Up to five redirects are followed; a relative `Location` (`/path`,
`page.html`, `../up`, `?query`, `//host/path`) is resolved against the current
URL, and `.`/`..` segments are taken out of every URL's path (a fragment, `#...`,
is never sent). The exit status is 0 for a 2xx reply saved whole, 1 otherwise
(`wget: the server answered 404`, a body cut short or malformed, no such host,
an address off the local network while no gateway is set). It takes about 2 KiB of RAM while it runs (its stack, a
256-byte receive buffer, the URLs and header lines); a file arrives at about
55 KiB/s when it goes to the flash file system. `https://` is not supported.

Programs use sockets through system calls (`socket`, `connect`, `listen`,
`sockinfo`, then `read`/`write`/`close` like files; see `lib/sys.h`). `connect`
and `listen` do not wait: programs poll `sockinfo` for the state, which keeps
Ctrl-C working while they wait.


## The LCD console

The text on the LCD comes in two sizes, and lines that scrolled off the top
can be looked at again:

| Keys | Effect |
| --- | --- |
| Ctrl+- (or Ctrl and the keypad's -) | small text: 32 columns by 10 rows |
| Ctrl+= (Ctrl++), Ctrl+0 | normal text: 21 columns by 8 rows |
| PageUp, PageDown (with or without Shift) | a screen back or forward in the scrollback |
| Shift+Up, Shift+Down | a line back or forward |
| any other key | back to the live screen (and the key is typed) |

Changing the size keeps the text at the bottom: rows that no longer fit go to
the scrollback, and getting taller brings the latest ones back. A row written
at 32 columns keeps its end when the text is normal again, out of sight until
the next switch. While the screen is scrolled back it stays put, even when
programs print; the scrollback lives while the console shell does (512 bytes
by default, `sysctl console_history`). `sysctl console_font=small` in
`/etc/rc` starts with the small text.

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
| `watchdog` | `0`, `2`, `7`, `26` | seconds without the kernel daemon before the chip resets (default 26; 0 turns it off) |
| `console_font` | `normal`, `small` | the LCD's text: 21x8 with the 5x7 font, or 32x10 with a 3x5 one (also Ctrl+- and Ctrl+= on the keyboard). No RAM effect |
| `console_history` | bytes, `0`-`8192` | the LCD's scrollback, allocated while the console shell runs (default 512, about 30 lines); `0` keeps none. A size that does not fit in RAM is refused (`out of memory`) and the old scrollback kept |

Settings last until the next reset; put them in `/etc/rc` to apply them at boot,
for example:

```
sysctl console=ask idle_logout=900 telnet_max=2
```

Other memory is fixed: the kernel daemon, the file system task and the idle
task (about 3 KiB of stacks), and about 2.4 KiB of static data. `ps` shows each
task's stack size and the most it has used; `/proc/stacks` keeps the highest
use per stack size, also of tasks that have ended.


## File system notes

- `/usb` uses 8.3 names (case is ignored) and at most six path components,
  for example `/usb/a/b/c/d/e/file.txt`; deeper paths give "name too long".
  Several open files on the USB drive see each other's writes (the CH376S has
  one file open at a time; nnnix switches between them). An error while the
  drive writes back a file's size is reported by `close`, so `cp` and `mv`
  report it too, and `mv` then keeps the source.
- `cp` refuses to copy a file onto itself, also under another name (`./x`,
  `../dir/x`, or a different case on `/usb`); `mv` onto itself does nothing.
- At boot a blank flash is formatted. If the flash holds something that does
  not mount (damage, another format), it is left alone and `/` is unavailable;
  `mkfs /` then formats it. `mkfs` refuses while `/` is mounted.

## Programs

Commands that are not built in are loaded from `/bin/NAME` (or from a path
that contains `/`, such as `/usb/tool.nxe`); `help` lists both. `play`, `ed`
and the text tools (`wc`, `head`, `tail`, `grep`, `hexdump`) are such programs,
built with the firmware (`make`) and installed with it:

```
sudo python3 tools/nnupdate.py --iface eth1 build/nnnix.bin --bin build/bin
sudo python3 tools/nnupdate.py --iface eth1 --bin build/bin      # the programs only
```

Other programs are NXE files built with the SDK on the NanoPi and uploaded
over HTTP:

```
make -C sdk
sudo python3 tools/nncp.py --iface eth1 put sdk/build/hello.nxe /bin/hello
```

A program's code runs from the chip's flash: the first run writes it into the
APROM above the firmware (a few tenths of a second, during which the buzzer's
sound stutters), later runs and reboots find it there. Only its data, stack and
allocations take RAM. `cat /proc/xip` shows what the area holds. Each run
checks the code found there against the CRC taken when it was written (a copy
that changed is dropped and written again: `damaged` in `/proc/xip`), and the
file's header and data against its own CRC, so a damaged or altered file is
refused (`not an executable`). Programs built before NXE version 3 must be
rebuilt.

They behave like built-in commands in pipelines, with redirection and `&`,
and Ctrl-C or `kill` stops them; their RAM (code, data, stack, and anything
they allocated) comes back when they end. A file that is not a valid NXE
program, or was damaged in transfer (CRC check), is refused with "not an
executable". See [sdk/README.md](../sdk/README.md) for writing programs.

## Firmware updates

The board's LDROM holds a bootloader, and CONFIG0 makes the chip start in it at
power-on. From the NanoPi, one command updates the firmware:

```
sudo python3 tools/nnupdate.py --iface eth1 build/nnnix.bin
```

It uploads the image to `/home/nnnix.bin` and runs `fwupdate` there:

```
fwupdate FILE        stage FILE and restart through the bootloader, which installs it
fwupdate -n FILE     stage only: installed at the next boot through the bootloader
fwupdate -c          cancel a staged image
fwupdate             the update area, the bootloader and CONFIG0
reboot -p            restart like power-on (through the bootloader)
```

The image goes to an area at the end of the SPI flash, outside the file
system, and is read back before it counts as staged. The bootloader then
writes APROM and verifies it; if a reset or power loss interrupts that, it
starts over at the next power-on; if APROM already holds the staged image, it
is only marked installed. The old firmware keeps running if anything fails
before the restart. Only one update operation runs at a time. `reboot` (without `-p`) restarts APROM directly.

A staged image is also installed after programming APROM with Nu-Link (the
programmer resets the chip through the bootloader): `fwupdate -c` first if that
is not wanted. `tools/flash.py --boot aprom` sets CONFIG0 back to the factory
setting (start in APROM); `--show` reads CONFIG0, CONFIG1 and LDROM.

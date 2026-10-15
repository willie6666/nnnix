# Shell scripts and file transfers (M5, first part)

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
than executing a partial command. Pipes, `&` jobs and semicolon command lists
are still pending in M5 and currently produce an explicit error.

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
sessions. The network still uses the static address `198.18.0.2/30` from M4;
network configuration and service control in `/etc/rc` are future M5 work.

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

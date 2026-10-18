# SPDX-License-Identifier: GPL-3.0-or-later
"""GNOME (Mutter) screen capture and input for nnscreen.py and nninput.py.

Mutter's own D-Bus services: org.gnome.Mutter.ScreenCast records a monitor into
a PipeWire stream, org.gnome.Mutter.RemoteDesktop takes keyboard and pointer
input as if from a device. Both only answer programs in the desktop session,
and GNOME shows its screen sharing indicator while a session runs; a session
ends with the program that started it (or from that indicator).

The pointer's position goes through a file, so that nnscreen.py can draw it:
`nninput.py` moves the pointer to absolute positions it keeps itself, and
writes them to POINTER_FILE.
"""
import os
import time

try:
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gio, GLib, Gst
except (ImportError, ValueError):                            # the tests need none of it
    gi = None

RD_BUS, RD_PATH = 'org.gnome.Mutter.RemoteDesktop', '/org/gnome/Mutter/RemoteDesktop'
SC_BUS, SC_PATH = 'org.gnome.Mutter.ScreenCast', '/org/gnome/Mutter/ScreenCast'
POINTER_FILE = os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'nninput-pointer')

CURSOR_HIDDEN, CURSOR_EMBEDDED = 0, 1


class Error(Exception):
    pass


class Session:
    """A screen cast of one monitor ('' = the primary one), optionally tied to a
    remote desktop session for input. `stream` is the stream's object path,
    `size` the monitor's size in its own (logical) pixels, `node` the PipeWire
    node to read pictures from."""

    def __init__(self, monitor='', cursor=CURSOR_EMBEDDED, remote=False):
        if gi is None:
            raise Error('needs PyGObject with GStreamer and its PipeWire plugin (Debian/Ubuntu: '
                        'python3-gi gir1.2-gstreamer-1.0 gstreamer1.0-pipewire)')
        try:
            self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        except GLib.Error as e:
            raise Error(f'no desktop session bus: {e.message}') from None
        self.rd = None
        props = {}
        try:
            if remote:
                self.rd = self.call(RD_BUS, RD_PATH, RD_BUS, 'CreateSession', None, '(o)')[0]
                sid = self.call(RD_BUS, self.rd, 'org.freedesktop.DBus.Properties', 'Get',
                                GLib.Variant('(ss)', (RD_BUS + '.Session', 'SessionId')))[0]
                props['remote-desktop-session-id'] = GLib.Variant('s', sid)
            self.sc = self.call(SC_BUS, SC_PATH, SC_BUS, 'CreateSession',
                                GLib.Variant('(a{sv})', (props,)), '(o)')[0]
            self.stream = self.call(SC_BUS, self.sc, SC_BUS + '.Session', 'RecordMonitor',
                                    GLib.Variant('(sa{sv})', (monitor, {'cursor-mode': GLib.Variant('u', cursor)})),
                                    '(o)')[0]
        except GLib.Error as e:
            raise Error(f'GNOME refused the screen cast ({e.message}); this needs a GNOME (Mutter) desktop '
                        'session') from None
        added = []
        sub = self.bus.signal_subscribe(SC_BUS, SC_BUS + '.Stream', 'PipeWireStreamAdded', self.stream, None,
                                        Gio.DBusSignalFlags.NONE, lambda *a: added.append(a[5].unpack()[0]))
        if self.rd:
            self.call(RD_BUS, self.rd, RD_BUS + '.Session', 'Start')
        else:
            self.call(SC_BUS, self.sc, SC_BUS + '.Session', 'Start')
        ctx = GLib.MainContext.default()
        limit = time.monotonic() + 5
        while not added and time.monotonic() < limit:
            ctx.iteration(False) or time.sleep(0.01)
        self.bus.signal_unsubscribe(sub)
        if not added:
            self.stop()
            raise Error('the screen cast did not start (no PipeWire stream)')
        self.node = added[0]
        params = self.call(SC_BUS, self.stream, 'org.freedesktop.DBus.Properties', 'Get',
                           GLib.Variant('(ss)', (SC_BUS + '.Stream', 'Parameters')))[0]
        self.size = tuple(params['size'])

    def call(self, bus, path, iface, method, args=None, reply=None):
        r = self.bus.call_sync(bus, path, iface, method, args, GLib.VariantType(reply) if reply else None,
                               Gio.DBusCallFlags.NONE, -1, None)
        return r.unpack() if r is not None else ()

    def input(self, method, *args):
        """A RemoteDesktop.Session method, such as NotifyKeyboardKeycode."""
        sig = {'NotifyKeyboardKeycode': '(ub)', 'NotifyPointerButton': '(ib)',
               'NotifyPointerAxisDiscrete': '(ui)', 'NotifyPointerMotionAbsolute': '(sdd)'}[method]
        self.call(RD_BUS, self.rd, RD_BUS + '.Session', method, GLib.Variant(sig, args))

    def stop(self):
        try:
            if self.rd:
                self.call(RD_BUS, self.rd, RD_BUS + '.Session', 'Stop')
            else:
                self.call(SC_BUS, self.sc, SC_BUS + '.Session', 'Stop')
        except GLib.Error:
            pass                                             # already gone (closed from the indicator)


class Frames:
    """The newest picture of a session's stream, in grey (uint8 rows), at most
    fps times a second, scaled by `scale` (1: the monitor's own size)."""

    def __init__(self, session, fps, scale=1.0):
        import numpy as np
        self.np = np
        Gst.init(None)
        w, h = (max(1, round(v * scale)) for v in session.size)
        self.pipe = Gst.parse_launch(
            f'pipewiresrc path={session.node} always-copy=true ! video/x-raw,max-framerate={max(1, round(fps))}/1 '
            f'! videoscale method=bilinear2 ! video/x-raw,width={w},height={h},pixel-aspect-ratio=1/1 '
            '! videoconvert ! video/x-raw,format=GRAY8 ! appsink name=out max-buffers=1 drop=true sync=false')
        self.sink = self.pipe.get_by_name('out')
        self.pipe.set_state(Gst.State.PLAYING)
        self.gray = None

    def latest(self, wait=0.0):
        """The newest picture (the last one again while the screen stays the same)."""
        sample = self.sink.emit('try-pull-sample', int(wait * Gst.SECOND))
        if sample is None:
            msg = self.pipe.get_bus().pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
            if msg or self.sink.get_property('eos'):
                raise Error('the screen cast ended')
            return self.gray
        caps = sample.get_caps().get_structure(0)
        w, h = caps.get_value('width'), caps.get_value('height')
        buf = sample.get_buffer()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return self.gray
        try:
            stride = len(info.data) // h
            self.gray = self.np.frombuffer(info.data, dtype=self.np.uint8).reshape(h, stride)[:, :w].copy()
        finally:
            buf.unmap(info)
        return self.gray

    def stop(self):
        self.pipe.set_state(Gst.State.NULL)


def read_pointer():
    """(x, y) last written by nninput.py, or None while it is not running."""
    try:
        with open(POINTER_FILE) as f:
            x, y = f.read().split()[:2]
        return float(x), float(y)
    except (OSError, ValueError):
        return None


def write_pointer(x, y):
    tmp = POINTER_FILE + '.new'
    with open(tmp, 'w') as f:
        f.write(f'{x:.1f} {y:.1f}\n')
    os.replace(tmp, POINTER_FILE)


def remove_pointer():
    try:
        os.unlink(POINTER_FILE)
    except OSError:
        pass

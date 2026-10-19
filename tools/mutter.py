# SPDX-License-Identifier: GPL-3.0-or-later
"""GNOME (Mutter) screen capture and input for nnscreen.py and nninput.py.

Mutter's own D-Bus services: org.gnome.Mutter.ScreenCast records a monitor into
a PipeWire stream, org.gnome.Mutter.RemoteDesktop takes keyboard and pointer
input as if from a device. Both only answer programs in the desktop session,
and GNOME shows its screen sharing indicator while a session runs; a session
ends with the program that started it (or from that indicator).

The pointer's position and the zoom chosen on the board go through a file
(STATE_FILE), from nninput.py to nnscreen.py: nninput.py moves the pointer to
absolute positions it keeps itself, and nnscreen.py draws the pointer and
shows the part of the screen around it. A fourth field selects relative
mouse motion: the preview then stays centred without a synthetic pointer.
"""
import os
import sys
import time

try:
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gio, GLib, Gst
except (ImportError, ValueError):                            # the tests need none of it
    gi = None

RD_BUS, RD_PATH = 'org.gnome.Mutter.RemoteDesktop', '/org/gnome/Mutter/RemoteDesktop'
SC_BUS, SC_PATH = 'org.gnome.Mutter.ScreenCast', '/org/gnome/Mutter/ScreenCast'
STATE_FILE = os.environ.get('NNINPUT_STATE_FILE') or os.path.join(
    os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'nninput-state')
# Zoom steps, in screen pixels per LCD pixel; 0 is the whole screen.
ZOOMS = (0, 8, 4, 2, 1)

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
               'NotifyPointerAxisDiscrete': '(ui)', 'NotifyPointerMotionAbsolute': '(sdd)',
               'NotifyPointerMotionRelative': '(dd)'}[method]
        self.call(RD_BUS, self.rd, RD_BUS + '.Session', method, GLib.Variant(sig, args))

    def stop(self):
        try:
            if self.rd:
                self.call(RD_BUS, self.rd, RD_BUS + '.Session', 'Stop')
            else:
                self.call(SC_BUS, self.sc, SC_BUS + '.Session', 'Stop')
        except GLib.Error:
            pass                                             # already gone (closed from the indicator)


def bgrx_gray(pixels, scale):
    """Average a mapped linear BGRx frame down, then convert it to grey.

    Do the sums at the requested preview size rather than allocating a
    full-size RGB/grey copy. Alpha/padding is deliberately ignored.
    """
    import numpy as np
    h, w = pixels.shape[:2]
    ow, oh = max(1, round(w * scale)), max(1, round(h * scale))
    if (ow, oh) == (w, h):
        luma = np.multiply(pixels[:, :, 0], 29, dtype=np.uint16)
        luma += np.multiply(pixels[:, :, 1], 150, dtype=np.uint16)
        luma += np.multiply(pixels[:, :, 2], 77, dtype=np.uint16)
        return ((luma + 128) >> 8).astype(np.uint8)
    rows = np.arange(oh + 1) * h // oh
    cols = np.arange(ow + 1) * w // ow
    # Horizontal first keeps reads contiguous in the full-size source.
    sums = np.add.reduceat(pixels[:, :, :3], cols[:-1], axis=1, dtype=np.uint32)
    sums = np.add.reduceat(sums, rows[:-1], axis=0, dtype=np.uint32)
    count = np.outer(np.diff(rows), np.diff(cols))
    luma = sums[:, :, 0] * 29 + sums[:, :, 1] * 150 + sums[:, :, 2] * 77
    return ((luma + count * 128) // (count * 256)).astype(np.uint8)


class Frames:
    """The newest picture of a session's stream, in grey (uint8 rows), at most
    fps times a second, scaled by `scale` (0 < scale <= 1). Prefer DMA-BUF:
    Mutter can capture directly scanned-out fullscreen surfaces that way.
    Map its linear BGRx buffers on the CPU and scale after copying them, so
    software videoscale cannot negotiate the source back to shared memory.
    A source without DMA-BUF support falls back to the software pipeline.
    """

    def __init__(self, session, fps, scale=1.0):
        import numpy as np
        self.np = np
        self.size = session.size
        self.node, self.fps, self.scale = session.node, fps, scale
        self.gray = self.raw = None
        Gst.init(None)
        self._start(dma=True)

    def _start(self, dma):
        self.dma = dma
        source = f'pipewiresrc path={self.node} always-copy=true keepalive-time=300 ! '
        rate = max(1, round(self.fps))
        if dma:
            # BGRx, rather than DMA_DRM with a modifier, requests CPU-mappable
            # linear pixels. always-copy releases the producer's buffers early.
            convert = f'video/x-raw(memory:DMABuf),format=BGRx,max-framerate={rate}/1 ! '
        else:
            convert = (f'video/x-raw,max-framerate={rate}/1 ! videoscale method=bilinear2 ! '
                       f'capsfilter name=size caps={self.caps(self.scale)} ! '
                       'videoconvert ! video/x-raw,format=GRAY8 ! ')
        self.pipe = Gst.parse_launch(source + convert + 'appsink name=out max-buffers=1 drop=true sync=false')
        self.sink = self.pipe.get_by_name('out')
        self.filter = self.pipe.get_by_name('size')
        self.pipe.set_state(Gst.State.PLAYING)

    def caps(self, scale):
        w, h = (max(1, round(v * scale)) for v in self.size)
        return f'video/x-raw,width={w},height={h},pixel-aspect-ratio=1/1'

    def set_scale(self, scale):
        if scale != self.scale:
            self.scale = scale
            if self.filter is not None:
                self.filter.set_property('caps', Gst.Caps.from_string(self.caps(scale)))
            elif self.raw is not None:
                self.gray = bgrx_gray(self.raw, scale)

    def latest(self, wait=0.0):
        """The newest picture (the last one again while the screen stays the same)."""
        sample = self.sink.emit('try-pull-sample', int(wait * Gst.SECOND))
        if sample is None:
            msg = self.pipe.get_bus().pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
            if self.dma and self.gray is None and (msg or wait > 0):
                self.stop()
                print('nnscreen: DMA-BUF unavailable; falling back to shared-memory capture', file=sys.stderr)
                self._start(dma=False)
                return self.latest(wait)
            if msg or self.sink.get_property('eos'):
                raise Error('the screen cast ended')
            return self.gray
        caps = sample.get_caps().get_structure(0)
        w, h = caps.get_value('width'), caps.get_value('height')
        buf = sample.get_buffer()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            if self.dma and self.gray is None:
                self.stop()
                print('nnscreen: DMA-BUF could not be mapped; falling back to shared-memory capture', file=sys.stderr)
                self._start(dma=False)
                return self.latest(wait)
            return self.gray
        try:
            data = info.data                     # PyGObject copies this to bytes; fetch it only once
            stride = len(data) // h
            rows = self.np.frombuffer(data, dtype=self.np.uint8).reshape(h, stride)
            if self.dma:
                raw = rows[:, :w * 4].reshape(h, w, 4)
                # A view of owned bytes stays valid after unmap. Other GI
                # implementations may expose borrowed memory instead.
                self.raw = raw if isinstance(data, bytes) else raw.copy()
                self.gray = bgrx_gray(self.raw, self.scale)
            else:
                self.gray = rows[:, :w].copy()
        finally:
            buf.unmap(info)
        return self.gray

    def stop(self):
        self.pipe.set_state(Gst.State.NULL)


def read_state():
    """(x, y, zoom, relative), or None; also accept the old three-column file."""
    try:
        with open(STATE_FILE) as f:
            fields = f.read().split()
        x, y, zoom = fields[:3]
        relative = bool(int(fields[3])) if len(fields) > 3 else False
        return float(x), float(y), int(zoom), relative
    except (OSError, ValueError):
        return None


def write_state(x, y, zoom, relative=False):
    tmp = STATE_FILE + '.new'
    with open(tmp, 'w') as f:
        f.write(f'{x:.1f} {y:.1f} {zoom} {int(relative)}\n')
    os.replace(tmp, STATE_FILE)


def remove_state():
    try:
        os.unlink(STATE_FILE)
    except OSError:
        pass

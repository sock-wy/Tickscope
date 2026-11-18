"""Read CME captures: raw .pcap(.zst) or archived .slim.zst, plus CME's channel map.

Every reader yields the same tuple:
    (capture_ts_ns, dst_ip_int, dst_port, payload_bytes)
where payload is the UDP payload, i.e. one CME MDP3 packet.
"""
import io
import struct
import xml.etree.ElementTree as ET

import zstandard

SLIM_REC = struct.Struct("<QIHH")       # ts_ns | dst_ip | dst_port | len


def ip_int(s):
    a, b, c, d = (int(x) for x in s.split("."))
    return (a << 24) | (b << 16) | (c << 8) | d


def ip_str(i):
    return ".".join(str((i >> s) & 255) for s in (24, 16, 8, 0))


def _open(path):
    f = open(path, "rb")
    if str(path).endswith(".zst"):
        r = zstandard.ZstdDecompressor().stream_reader(f, read_size=1 << 20, read_across_frames=True)
        return io.BufferedReader(r, buffer_size=1 << 22)
    return io.BufferedReader(f, buffer_size=1 << 22)


def read_pcap(path, want=None):
    """Yield records from a classic pcap (us or ns). want: optional set of (ip_int, port) to keep."""
    s = _open(path)
    gh = s.read(24)
    magic = struct.unpack("<I", gh[:4])[0]
    e = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
    linktype = struct.unpack(e + "I", gh[20:24])[0]
    rh = struct.Struct(e + "IIII")
    while True:
        h = s.read(16)
        if len(h) < 16:
            return
        sec, frac, incl, _ = rh.unpack(h)
        d = s.read(incl)
        if len(d) < incl:
            return
        off = 0
        if linktype == 1:                               # Ethernet, skip VLAN tags
            et = d[12:14]
            off = 14
            while et in (b"\x81\x00", b"\x88\xa8"):
                et = d[off + 2:off + 4]
                off += 4
            if et != b"\x08\x00":
                continue
        if len(d) < off + 28 or d[off + 9] != 17:       # IPv4 + UDP only
            continue
        dst = int.from_bytes(d[off + 16:off + 20], "big")
        u = off + (d[off] & 0x0F) * 4
        port = (d[u + 2] << 8) | d[u + 3]
        if want is not None and (dst, port) not in want:
            continue
        ulen = (d[u + 4] << 8) | d[u + 5]
        ts = sec * 1_000_000_000 + (frac if nano else frac * 1000)
        yield ts, dst, port, d[u + 8:u + ulen]


def read_slim(path, want=None):
    """Yield records from a .slim.zst capture slice (tools/fetch_es.py or the capture archive)."""
    s = _open(path)
    while True:
        h = s.read(SLIM_REC.size)
        if len(h) < SLIM_REC.size:
            return
        ts, ip, port, n = SLIM_REC.unpack(h)
        p = s.read(n)
        if len(p) < n:
            return
        if want is None or (ip, port) in want:
            yield ts, ip, port, p


def read_any(path, want=None):
    return read_slim(path, want) if ".slim" in str(path) else read_pcap(path, want)


def load_channels(config_path):
    """Map (ip_int, port) -> dict(channel, label, conn, type, feed) from CME config.xml."""
    out = {}
    for ch in ET.parse(config_path).getroot().iter("channel"):
        for c in ch.iter("connection"):
            ip, port = c.findtext("ip"), c.findtext("port")
            if not ip or not port:
                continue
            out[(ip_int(ip), int(port))] = {
                "channel": int(ch.get("id")), "label": ch.get("label"),
                "conn": c.get("id"), "type": c.findtext("type"), "feed": c.findtext("feed"),
            }
    return out


def channel_endpoints(chmap, channel_id):
    return {k for k, v in chmap.items() if v["channel"] == channel_id}

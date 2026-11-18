"""Schema-driven decoder for CME MDP 3.0 (SBE) packets.

The decoder reads CME's templates_FixBinary.xml and builds, for every message
template and repeating group, a compiled byte layout (offsets + struct formats).
Nothing about individual templates is hard-coded, so any template present in the
schema can be decoded.

Wire layout of one UDP payload (one "packet"):

    Packet header (12 B):  MsgSeqNum u32 | SendingTime u64 (ns since epoch)
    Message 1:             MsgSize u16 | SBE header 8 B | root block | groups...
    Message 2:             ...

SBE header (8 B): blockLength u16 | templateId u16 | schemaId u16 | version u16
Group header:     groupSize (3 B: blockLength u16, numInGroup u8) or
                  groupSize8Byte (8 B: blockLength u16, numInGroup u8 at offset 7)

Prices are returned as raw int64 mantissas (exponent -9 for PRICE9). Divide by
1e9 for a decimal price. Null values are returned as None.

Usage:
    from mdp3 import Schema, iter_packet
    sch = Schema("schema/templates_FixBinary.xml")
    seq, send_ns, msgs, leftover = iter_packet(payload)
    for m in msgs:                       # m = (template_id, version, block_len, body_off, end_off)
        d = sch.decode(payload, m)       # dict with fields and lists of group entries
"""
import struct
import xml.etree.ElementTree as ET

PRIM = {
    "char": ("c", 1), "int8": ("b", 1), "uint8": ("B", 1),
    "int16": ("h", 2), "uint16": ("H", 2), "int32": ("i", 4),
    "uint32": ("I", 4), "int64": ("q", 8), "uint64": ("Q", 8),
}

PKT_HDR = struct.Struct("<IQ")          # MsgSeqNum, SendingTime
MSG_HDR = struct.Struct("<HHHHH")       # MsgSize, blockLength, templateId, schemaId, version
PKT_HDR_LEN = 12
MSG_HDR_LEN = 10                        # 2 (MsgSize) + 8 (SBE header)


def _tag(e):
    return e.tag.split("}")[-1]


def _parse_const(text, prim):
    text = (text or "").strip()
    if prim == "char":
        return text
    try:
        return int(text)
    except ValueError:
        return text


class Simple:
    def __init__(self, name, prim, length=1, presence="required", null=None, const=None):
        self.name, self.prim, self.length = name, prim, length
        self.presence, self.null, self.const = presence, null, const
        fmt, size = PRIM[prim]
        self.size = size * length
        self.fmt = f"{length}s" if (prim == "char" and length > 1) else (fmt if length == 1 else f"{length}{fmt}")

    @property
    def is_const(self):
        return self.presence == "constant"


class Composite:
    def __init__(self, name, members):
        self.name = name
        self.members = members          # list of (name, Simple|Enum|Set, offset or None)


class Enum:
    def __init__(self, name, enc, values):
        self.name, self.enc, self.values = name, enc, values   # values: raw -> name


class SetT:
    def __init__(self, name, enc, choices):
        self.name, self.enc, self.choices = name, enc, choices  # bit -> name


class Block:
    """Compiled layout of a root block or a group entry."""

    def __init__(self, fields):
        # fields: list of (name, offset, fmt, size, post, const_value, is_const)
        self.consts = [(f[0], f[5]) for f in fields if f[6]]
        live = sorted([f for f in fields if not f[6]], key=lambda f: f[1])
        self.fields = live
        fmt, pos, self.posts = "<", 0, []
        for name, off, f, size, post, _, _ in live:
            if off > pos:
                fmt += f"{off - pos}x"
            elif off < pos:
                raise ValueError(f"overlapping field {name}")
            fmt += f
            pos = off + size
            self.posts.append((name, post))
        self.size = pos
        self.st = struct.Struct(fmt)

    def decode(self, buf, off, blk_len):
        out = {}
        if blk_len >= self.size:
            vals = self.st.unpack_from(buf, off)
            i = 0
            for name, post in self.posts:
                v, i = post(vals, i)
                out[name] = v
        else:                                       # older/shorter block: decode what fits
            for name, foff, f, size, post, _, _ in self.fields:
                if foff + size <= blk_len:
                    vals = struct.unpack_from("<" + f, buf, off + foff)
                    out[name] = post(vals, 0)[0]
                else:
                    out[name] = None
        for name, v in self.consts:
            out[name] = v
        return out


class Group:
    def __init__(self, name, gid, dim_st, dim_len, n_idx, block, groups):
        self.name, self.id = name, gid
        self.dim_st, self.dim_len, self.n_idx = dim_st, dim_len, n_idx
        self.block, self.groups = block, groups


class Message:
    def __init__(self, name, tid, block, groups):
        self.name, self.id, self.block, self.groups = name, tid, block, groups


class Schema:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.id = int(root.get("id"))
        self.version = int(root.get("version"))
        self.types = {}
        for types in root.iter():
            if _tag(types) != "types":
                continue
            for t in types:
                if _tag(t) == "type" and t.get("primitiveType"):
                    self.types[t.get("name")] = self._simple(t)
        for t in root.iter():
            tag = _tag(t)
            if tag == "enum":
                enc = self._resolve_enc(t.get("encodingType"))
                vals = {_parse_const(v.text, enc.prim): v.get("name") for v in t if _tag(v) == "validValue"}
                self.types[t.get("name")] = Enum(t.get("name"), enc, vals)
            elif tag == "set":
                enc = self._resolve_enc(t.get("encodingType"))
                ch = {int(c.text): c.get("name") for c in t if _tag(c) == "choice"}
                self.types[t.get("name")] = SetT(t.get("name"), enc, ch)
        for t in root.iter():
            if _tag(t) == "composite":
                mem = []
                for m in t:
                    if _tag(m) == "type":
                        off = m.get("offset")
                        mem.append((m.get("name"), self._simple(m), int(off) if off else None))
                    elif _tag(m) == "ref":
                        off = m.get("offset")
                        mem.append((m.get("name"), self.types[m.get("type")], int(off) if off else None))
                self.types[t.get("name")] = Composite(t.get("name"), mem)
        self.messages = {}
        for m in root.iter():
            if _tag(m) == "message":
                blk, groups = self._compile_children(m)
                self.messages[int(m.get("id"))] = Message(m.get("name"), int(m.get("id")), blk, groups)

    # ---- schema parsing -------------------------------------------------
    def _simple(self, t):
        prim = t.get("primitiveType")
        presence = t.get("presence", "required")
        length = int(t.get("length", "1"))
        null = None
        if t.get("nullValue") is not None:
            null = _parse_const(t.get("nullValue"), prim)
            if prim == "char" and isinstance(null, str):
                null = None
        const = _parse_const(t.text, prim) if presence == "constant" else None
        return Simple(t.get("name"), prim, length, presence, null, const)

    def _resolve_enc(self, enc):
        if enc in PRIM:
            return Simple(enc, enc)
        return self.types[enc]

    def _field_layout(self, name, tname, off, field_presence=None, value_ref=None):
        """Return list of compiled field tuples for one schema field."""
        t = self.types.get(tname)
        if t is None and tname in PRIM:
            t = Simple(tname, tname)
        if isinstance(t, Simple):
            if t.is_const or field_presence == "constant":
                return [(name, off or 0, "", 0, None, t.const if t.const is not None else value_ref, True)]
            return [(name, off, t.fmt, t.size, self._post_simple(t), None, False)]
        if isinstance(t, (Enum, SetT)):
            enc = t.enc
            if field_presence == "constant":
                return [(name, off or 0, "", 0, None, value_ref, True)]
            return [(name, off, enc.fmt, enc.size, self._post_simple(enc), None, False)]
        if isinstance(t, Composite):
            live, pos, parts = [], off, []
            for mname, mt, moff in t.members:
                base = off + (moff if moff is not None else pos - off)
                if isinstance(mt, Simple) and mt.is_const:
                    continue
                enc = mt.enc if isinstance(mt, (Enum, SetT)) else mt
                parts.append((mname, base, enc))
                pos = base + enc.size
            if len(parts) == 1:                     # e.g. PRICE9: mantissa only
                mname, base, enc = parts[0]
                return [(name, base, enc.fmt, enc.size, self._post_simple(enc), None, False)]
            fmt = "".join(p[2].fmt for p in parts)
            contiguous = all(parts[i][1] + parts[i][2].size == parts[i + 1][1] for i in range(len(parts) - 1))
            if not contiguous:
                raise ValueError(f"non-contiguous composite {tname}")
            subs = [(p[0], self._post_simple(p[2])) for p in parts]
            size = sum(p[2].size for p in parts)

            def post(vals, i, subs=subs):
                d = {}
                for n, p in subs:
                    d[n], i = p(vals, i)
                return d, i
            return [(name, parts[0][1], fmt, size, post, None, False)]
        raise KeyError(f"unknown type {tname} for field {name}")

    @staticmethod
    def _post_simple(t):
        null = t.null
        if t.prim == "char" and t.length > 1:
            return lambda vals, i: (vals[i].split(b"\0", 1)[0].decode("ascii", "replace"), i + 1)
        if t.prim == "char":
            return lambda vals, i: (vals[i].decode("ascii", "replace"), i + 1)
        if null is not None:
            return lambda vals, i: (None if vals[i] == null else vals[i], i + 1)
        return lambda vals, i: (vals[i], i + 1)

    def _compile_children(self, node):
        fields, groups, pos = [], [], 0
        for c in node:
            tag = _tag(c)
            if tag == "field":
                off = c.get("offset")
                off = int(off) if off is not None else pos
                vr = c.get("valueRef")
                if vr:
                    vr = vr.split(".")[-1]
                for f in self._field_layout(c.get("name"), c.get("type"), off, c.get("presence"), vr):
                    fields.append(f)
                    if not f[6]:
                        pos = max(pos, f[1] + f[3])
            elif tag == "group":
                dim = self.types[c.get("dimensionType", "groupSize")]
                parts, p = [], 0
                for mname, mt, moff in dim.members:
                    base = moff if moff is not None else p
                    parts.append((mname, base, mt))
                    p = base + mt.size
                fmt, cur = "<", 0
                order = []
                for mname, base, mt in sorted(parts, key=lambda x: x[1]):
                    if base > cur:
                        fmt += f"{base - cur}x"
                    fmt += mt.fmt
                    cur = base + mt.size
                    order.append(mname)
                n_idx = order.index("numInGroup")
                blk, sub = self._compile_children(c)
                groups.append(Group(c.get("name"), int(c.get("id")), struct.Struct(fmt), cur, n_idx, blk, sub))
        return Block(fields), groups

    # ---- decoding --------------------------------------------------------
    def decode(self, buf, msg):
        """Decode one message located by iter_packet. Returns dict, or None if template unknown.

        Adds '_template', '_name' and '_consumed' (bytes used, should equal message length).
        """
        tid, ver, blk_len, body, end = msg
        m = self.messages.get(tid)
        if m is None:
            return None
        out = m.block.decode(buf, body, blk_len)
        pos = body + blk_len
        pos = self._decode_groups(buf, pos, m.groups, out)
        out["_template"], out["_name"], out["_consumed"] = tid, m.name, pos - body
        return out

    def _decode_groups(self, buf, pos, groups, out):
        for g in groups:
            hdr = g.dim_st.unpack_from(buf, pos)
            entry_len, n = hdr[0], hdr[g.n_idx]
            pos += g.dim_len
            rows = []
            for _ in range(n):
                d = g.block.decode(buf, pos, entry_len)
                pos += entry_len
                if g.groups:
                    pos = self._decode_groups(buf, pos, g.groups, d)
                rows.append(d)
            out[g.name] = rows
        return pos

    def enum_name(self, type_name, raw):
        t = self.types.get(type_name)
        return t.values.get(raw, raw) if isinstance(t, Enum) else raw


def iter_packet(payload):
    """Split one MDP3 UDP payload into messages.

    Returns (seq, sending_time_ns, [ (tid, version, block_len, body_off, end_off), ... ], leftover_bytes).
    leftover_bytes != 0 means the framing did not consume the payload exactly.
    """
    seq, send = PKT_HDR.unpack_from(payload, 0)
    off, n = PKT_HDR_LEN, len(payload)
    msgs = []
    while off + MSG_HDR_LEN <= n:
        size, blk, tid, _sid, ver = MSG_HDR.unpack_from(payload, off)
        if size < MSG_HDR_LEN or off + size > n:
            break
        msgs.append((tid, ver, blk, off + MSG_HDR_LEN, off + size))
        off += size
    return seq, send, msgs, n - off

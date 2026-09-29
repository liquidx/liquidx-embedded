#!/usr/bin/env python3
"""A reference blat host in Python, and end-to-end tests against the simulated
device (sim_device.cpp) over the stream framing.

The host side is written independently of the firmware: its own P-256
arithmetic, SPAKE2, HKDF and wire encoding, from PROTOCOL.md. Only AES-CCM
comes from a library (`cryptography`). If the two sides agree, the spec
describes both.

    python3 host_test.py build/sim_device
"""

import hashlib
import hmac
import os
import select
import struct
import subprocess
import sys
import time
import zlib

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

# --- P-256 ------------------------------------------------------------------

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def on_curve(pt):
    x, y = pt
    return (y * y - (x ** 3 - 3 * x + B)) % P == 0


def add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        m = (3 * x1 * x1 - 3) * pow(2 * y1, -1, P)
    else:
        m = (y2 - y1) * pow(x2 - x1, -1, P)
    x3 = (m * m - x1 - x2) % P
    return (x3, (m * (x1 - x3) - y1) % P)


def mul(k, pt):
    result = None
    while k:
        if k & 1:
            result = add(result, pt)
        pt = add(pt, pt)
        k >>= 1
    return result


def neg(pt):
    return (pt[0], (-pt[1]) % P)


def decompress(hexstr):
    raw = bytes.fromhex(hexstr)
    x = int.from_bytes(raw[1:], "big")
    y = pow((x ** 3 - 3 * x + B) % P, (P + 1) // 4, P)
    if y % 2 != raw[0] - 2:
        y = P - y
    return (x, y)


def encode(pt):
    return b"\x04" + pt[0].to_bytes(32, "big") + pt[1].to_bytes(32, "big")


def decode(raw):
    assert len(raw) == 65 and raw[0] == 4
    pt = (int.from_bytes(raw[1:33], "big"), int.from_bytes(raw[33:], "big"))
    assert on_curve(pt)
    return pt


# RFC 9382 section 6, P-256.
M = decompress("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f")
NN = decompress("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49")

# --- KDF, MAC, SPAKE2 ---------------------------------------------------------


def hkdf(salt, ikm, info, length):
    prk = hmac.new(salt or bytes(32), ikm, hashlib.sha256).digest()
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def code_to_w(salt, code):
    return int.from_bytes(hashlib.sha256(b"blat code" + salt + code.encode()).digest(), "big") % N


def lp(data):
    return struct.pack("<Q", len(data)) + data


class Spake2A:
    """SPAKE2 party A (the host)."""

    def __init__(self, w, id_a, id_b, aad):
        self.w, self.id_a, self.id_b, self.aad = w, id_a, id_b, aad
        self.x = int.from_bytes(os.urandom(32), "big") % (N - 1) + 1
        self.pA = encode(add(mul(self.x, G), mul(w, M)))

    def finish(self, pB_raw):
        """Returns (Ke, confirmA, expected confirmB)."""
        pB = decode(pB_raw)
        K = mul(self.x, add(pB, neg(mul(self.w, NN))))
        tt = (lp(self.id_a) + lp(self.id_b) + lp(self.pA) + lp(pB_raw) + lp(encode(K)) +
              lp(self.w.to_bytes(32, "big")))
        digest = hashlib.sha256(tt).digest()
        ke, ka = digest[:16], digest[16:]
        kc = hkdf(None, ka, b"ConfirmationKeys" + self.aad, 32)
        confirm_a = hmac.new(kc[:16], tt, hashlib.sha256).digest()
        confirm_b = hmac.new(kc[16:], tt, hashlib.sha256).digest()
        return ke, confirm_a, confirm_b


# --- Wire ---------------------------------------------------------------------

REQUEST, DATA, REPLY, DATAOUT, EVENT = 1, 2, 3, 4, 5
OK, RUNNING, BAD_REQUEST, _, UNKNOWN_ID, INVALID, READ_ONLY = 0, 1, 2, 3, 4, 5, 6
AUTH_REQUIRED, WRONG_CODE, LOCKED_OUT = 15, 16, 17
T_BOOL, T_INT, T_ENUM, T_TEXT, T_SECRET, T_ACTION, T_GROUP = 1, 2, 3, 4, 5, 7, 10


def counter_nonce(channel, n):
    return bytes([channel]) + struct.pack("<I", n) + bytes(8)


def transfer_nonce(channel, transfer_id, offset):
    return bytes([channel, transfer_id]) + struct.pack("<I", offset) + bytes(7)


def tlvs(data):
    out, i = {}, 0
    while i + 2 <= len(data):
        tag, n = data[i], data[i + 1]
        out.setdefault(tag, []).append(data[i + 2:i + 2 + n])
        i += 2 + n
    return out


def parse_schema(blob):
    assert blob[0] == 1
    count = struct.unpack_from("<H", blob, 1)[0]
    controls, at = {}, 3
    for _ in range(count):
        length = struct.unpack_from("<H", blob, at)[0]
        rec = blob[at + 2:at + 2 + length]
        cid, ctype, access, parent, flags = struct.unpack_from("<HBBHH", rec)
        attrs = tlvs(rec[8:])
        controls[cid] = {
            "id": cid, "type": ctype, "read": access & 15, "write": access >> 4, "parent": parent,
            "flags": flags, "key": attrs[1][0].decode(), "attrs": attrs,
        }
        at += 2 + length
    assert at == len(blob)
    return controls


class Sim:
    """The simulated device, spoken to over the stream framing."""

    def __init__(self, path):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.pending = []
        self.code = None

    def close(self):
        self.proc.kill()
        self.proc.wait()

    def write(self, channel, data=b""):
        self.proc.stdin.write(bytes([channel]) + struct.pack("<H", len(data)) + data)
        self.proc.stdin.flush()

    def _read_exact(self, n, deadline):
        out = b""
        fd = self.proc.stdout.fileno()
        while len(out) < n:
            if not select.select([fd], [], [], max(0, deadline - time.time()))[0]:
                raise TimeoutError("sim didn't answer")
            chunk = os.read(fd, n - len(out))
            if not chunk:
                raise EOFError("sim exited")
            out += chunk
        return out

    def _read_frame(self, deadline):
        header = self._read_exact(3, deadline)
        n = struct.unpack_from("<H", header, 1)[0]
        return header[0], self._read_exact(n, deadline)

    def read(self, channel, timeout=5.0):
        """The next message on `channel`; others are kept for later."""
        for i, (ch, data) in enumerate(self.pending):
            if ch == channel:
                return self.pending.pop(i)[1]
        deadline = time.time() + timeout
        while True:
            ch, data = self._read_frame(deadline)
            if ch == 0xF0:
                self.code = data.decode() or None
            if ch == channel:
                return data
            if ch != 0xF0:
                self.pending.append((ch, data))

    def drain(self, wait=0.1):
        """Everything that arrives within `wait` seconds."""
        deadline = time.time() + wait
        try:
            while True:
                ch, data = self._read_frame(deadline)
                if ch == 0xF0:
                    self.code = data.decode() or None
                else:
                    self.pending.append((ch, data))
        except TimeoutError:
            pass


class Host:
    """A blat host session."""

    def __init__(self, sim, mtu=247):
        self.sim = sim
        self.seq = 0
        self.keys = None  # (host->device, device->host)
        self.tx = self.rx_reply = self.rx_event = 0
        sim.pending.clear()
        sim.write(0xF0, struct.pack("<H", mtu - 3))

    def reconnect(self, mtu=247):
        self.sim.write(0xF1)
        self.__init__(self.sim, mtu)

    def request(self, op, body=b"", tamper=False):
        self.seq = (self.seq + 1) & 0xFF
        header = bytes([op, self.seq])
        if self.keys:
            body = AESCCM(self.keys[0], tag_length=8).encrypt(counter_nonce(REQUEST, self.tx), body, header)
            self.tx += 1
            if tamper:
                body = body[:-1] + bytes([body[-1] ^ 1])
        self.sim.write(REQUEST, header + body)
        msg = self.sim.read(REPLY)
        rop, rseq, status, flags, detail = struct.unpack_from("<BBBBH", msg)
        assert (rop, rseq) == (op, self.seq), (rop, rseq, op, self.seq)
        payload = msg[6:]
        if self.keys:
            try:
                payload = AESCCM(self.keys[1], tag_length=8).decrypt(
                    counter_nonce(REPLY, self.rx_reply), payload, msg[:6])
                self.rx_reply += 1
            except Exception:
                # The device couldn't open our request and ended the session:
                # its reply (authRequired, no body) is in the clear.
                assert status == AUTH_REQUIRED and payload == b"", (status, payload)
                self.keys = None
        return status, detail, flags, payload

    def event(self, timeout=2.0):
        msg = self.sim.read(EVENT, timeout)
        body = msg[1:]
        if self.keys:
            body = AESCCM(self.keys[1], tag_length=8).decrypt(counter_nonce(EVENT, self.rx_event), body, msg[:1])
            self.rx_event += 1
        return msg[0], body

    # --- Operations ---

    def hello(self, want_events=True):
        status, _, flags, body = self.request(0x01, bytes([1, 1 if want_events else 0, 4]) + b"test")
        assert status == OK
        if flags & 1 and not body:
            return None  # Info didn't fit: over BLE, read the Info characteristic
        assert body[0] == 1
        return tlvs(body[1:])

    def read_schema(self):
        status, _, _, body = self.request(0x20, bytes([0, 0, 0, 0]))
        assert status == OK, status
        tid, size, crc, chunk = struct.unpack("<BIIH", body)
        blob = b""
        while len(blob) < size:
            msg = self.sim.read(DATAOUT)
            rtid, offset = struct.unpack_from("<BI", msg)
            data = msg[5:]
            if self.keys:
                data = AESCCM(self.keys[1], tag_length=8).decrypt(
                    transfer_nonce(DATAOUT, rtid, offset), data, msg[:5])
            assert rtid == tid and offset == len(blob) and len(data) <= chunk
            blob += data
            if len(blob) == size or (len(blob) // chunk) % 8 == 0:
                self.request_noreply(0x22, struct.pack("<BI", tid, len(blob)))
        assert zlib.crc32(blob) == crc
        return blob, crc

    def request_noreply(self, op, body):
        self.seq = (self.seq + 1) & 0xFF
        header = bytes([op, self.seq])
        if self.keys:
            body = AESCCM(self.keys[0], tag_length=8).encrypt(counter_nonce(REQUEST, self.tx), body, header)
            self.tx += 1
        self.sim.write(REQUEST, header + body)

    def pair(self, code_override=None):
        status, detail, _, body = self.request(0x02, bytes([1]))
        if status != OK:
            return status, detail
        digits, attempts, expires = struct.unpack_from("<BBH", body)
        salt = body[4:20]
        assert digits == 6 and len(salt) == 16
        self.sim.drain(0.05)
        code = code_override or self.sim.code
        spake = Spake2A(code_to_w(salt, code), b"blat host", DEVICE_ID, salt)
        status, _, _, body = self.request(0x03, spake.pA)
        assert status == OK, status
        ke, confirm_a, expect_b = spake.finish(body[:65])
        confirm_ok = hmac.compare_digest(body[65:97], expect_b)
        status, detail, _, body = self.request(0x04, confirm_a)
        if status == OK:
            assert confirm_ok, "device confirmed, but its confirmB was wrong"
            keys = hkdf(salt, ke, b"blat session", 32)
            self.keys = (keys[:16], keys[16:])
            self.tx = self.rx_reply = self.rx_event = 0
            return OK, body[0]
        assert not confirm_ok  # a wrong code shows on both sides
        return status, detail

    def remember(self, name=b"test host"):
        status, _, _, body = self.request(0x06, bytes([len(name)]) + name)
        assert status == OK, status
        return body[:8], body[8:40]

    def resume(self, host_id, host_key):
        status, _, _, challenge = self.request(0x02, bytes([2]) + host_id)
        if status != OK:
            return status
        nonce = os.urandom(16)
        mac = hmac.new(host_key, b"blat host" + challenge + nonce + DEVICE_ID, hashlib.sha256).digest()
        status, _, _, body = self.request(0x05, nonce + mac)
        if status != OK:
            return status
        expect = hmac.new(host_key, b"blat device" + challenge + nonce + DEVICE_ID, hashlib.sha256).digest()
        assert hmac.compare_digest(body[:32], expect), "device MAC wrong"
        keys = hkdf(challenge + nonce, host_key, b"blat session", 32)
        self.keys = (keys[:16], keys[16:])
        self.tx = self.rx_reply = self.rx_event = 0
        return body[32]


DEVICE_ID = bytes.fromhex("5a11000000000001")


def encode_value(ctype, v):
    if ctype == T_BOOL:
        return bytes([1 if v else 0])
    if ctype == T_INT:
        return struct.pack("<i", v)
    if ctype == T_ENUM:
        return struct.pack("<H", v)
    raw = v.encode()
    return bytes([len(raw)]) + raw


def decode_values(schema, body):
    out, at = {}, 0
    while at < len(body):
        cid = struct.unpack_from("<H", body, at)[0]
        at += 2
        ctype = schema[cid]["type"]
        if ctype in (T_BOOL, T_SECRET):
            out[cid], at = body[at], at + 1
        elif ctype == T_INT:
            out[cid], at = struct.unpack_from("<i", body, at)[0], at + 4
        elif ctype == T_ENUM:
            out[cid], at = struct.unpack_from("<H", body, at)[0], at + 2
        else:
            n = body[at]
            out[cid], at = body[at + 1:at + 1 + n].decode(), at + 1 + n
    return out


# --- Tests ----------------------------------------------------------------------

def run(sim_path):
    sim = Sim(sim_path)
    try:
        host = Host(sim)
        info = host.hello()
        assert info[1][0] == b"Sim-0001" and info[2][0] == b"blat-sim" and info[4][0] == DEVICE_ID
        level, methods, digits = info[8][0]
        assert (level, digits) == (0, 6) and methods & 1 and methods & 4
        schema_crc = struct.unpack("<II", info[5][0])[0]
        print("hello: ok")

        blob, crc = host.read_schema()
        assert crc == schema_crc
        schema = parse_schema(blob)
        ids = {c["key"]: cid for cid, c in schema.items()}
        mode, bright, invert = ids["display.mode"], ids["display.brightness"], ids["display.invert"]
        ssid, password, battery = ids["wifi.ssid"], ids["wifi.password"], ids["power.battery"]
        name, restart, message = ids["device.name"], ids["system.restart"], ids["display.message"]
        msg_text, msg_seconds = ids["display.message.text"], ids["display.message.seconds"]
        assert schema[mode]["type"] == T_ENUM and schema[mode]["parent"] == ids["display"]
        assert [o[2:].decode() for o in schema[mode]["attrs"][7]] == ["Off", "Eco", "Full"]
        assert struct.unpack("<H", schema[mode]["attrs"][0x0A][0])[0] == 1  # default
        assert struct.unpack("<iii", schema[bright]["attrs"][5][0]) == (0, 100, 5)
        assert schema[battery]["flags"] & 1 and schema[battery]["write"] == 0
        assert schema[password]["write"] == 2 and 0x0A not in schema[password]["attrs"]
        assert schema[msg_text]["parent"] == message and schema[msg_text]["flags"] & 0x40
        print("schema: ok (%d controls, %d bytes)" % (len(schema), len(blob)))

        # Level 0: public values only, no writes.
        status, _, _, body = host.request(0x10)
        values = decode_values(schema, body)
        assert status == OK and values[mode] == 1 and values[bright] == 50 and name not in values
        status, detail, _, _ = host.request(0x11, struct.pack("<H", mode) + encode_value(T_ENUM, 2))
        assert (status, detail) == (AUTH_REQUIRED, 1)
        status, detail, _, _ = host.request(0x10, struct.pack("<H", name))
        assert (status, detail) == (AUTH_REQUIRED, 1)
        print("level 0: ok")

        # A wrong code, then the right one.
        status, left = host.pair(code_override="000000" if sim.code != "000000" else "111111")
        assert (status, left) == (WRONG_CODE, 2), (status, left)
        assert sim.code is not None  # still showing
        shown = sim.code
        status, level = host.pair()
        assert (status, level) == (OK, 2) and sim.code is None
        assert shown is not None
        print("pairing: ok (wrong code refused, right code accepted)")

        # Sealed from here on.
        status, _, _, body = host.request(0x10)
        values = decode_values(schema, body)
        assert status == OK and values[name] == "Sim" and values[password] == 0
        status, _, _, _ = host.request(0x11, struct.pack("<H", mode) + encode_value(T_ENUM, 2) +
                                       struct.pack("<H", ssid) + encode_value(T_TEXT, "Home") +
                                       struct.pack("<H", password) + encode_value(T_SECRET, "correct horse"))
        assert status == OK
        status, _, _, body = host.request(0x10, struct.pack("<HHH", mode, ssid, password))
        assert decode_values(schema, body) == {mode: 2, ssid: "Home", password: 1}
        # All or nothing: a bad brightness stops the mode change too.
        status, detail, _, _ = host.request(0x11, struct.pack("<H", mode) + encode_value(T_ENUM, 0) +
                                            struct.pack("<H", bright) + encode_value(T_INT, 52))
        assert (status, detail) == (INVALID, bright)
        status, _, _, body = host.request(0x10, struct.pack("<H", mode))
        assert decode_values(schema, body) == {mode: 2}
        status, detail, _, _ = host.request(0x11, struct.pack("<H", battery) + encode_value(T_INT, 5))
        assert (status, detail) == (READ_ONLY, battery)
        status, detail, _, _ = host.request(0x11, struct.pack("<H", password) + encode_value(T_SECRET, "short"))
        assert (status, detail) == (INVALID, password)
        print("set and get: ok (atomic, validated, secrets write-only)")

        # Actions, with params.
        status, _, _, _ = host.request(0x12, struct.pack("<HH", message, msg_text) + encode_value(T_TEXT, "Hi") +
                                       struct.pack("<H", msg_seconds) + encode_value(T_INT, 10))
        assert status == OK
        invoked = sim.read(0xF1)
        assert struct.unpack("<HB", invoked) == (message, 2)
        status, detail, _, _ = host.request(0x12, struct.pack("<HH", message, msg_seconds) + encode_value(T_INT, 10))
        assert (status, detail) == (INVALID, msg_text)  # required param missing
        print("invoke: ok")

        # Options, and a sealed schema transfer.
        status, _, _, body = host.request(0x13, struct.pack("<H", mode))
        assert status == OK and body[0] == 3
        blob2, _ = host.read_schema()
        assert blob2 == blob
        print("options and sealed transfer: ok")

        # Live events: the firmware changes a live value.
        sim.write(0xF2, struct.pack("<Hi", battery, 42))
        etype, body = host.event()
        assert etype == 1 and decode_values(schema, body) == {battery: 42}
        sim.write(0xF2, struct.pack("<Hi", bright, 55))  # not live: no event
        sim.drain(0.1)
        assert not [p for p in sim.pending if p[0] == EVENT]
        print("events: ok")

        # Remember, reconnect, resume at level 1.
        host_id, host_key = host.remember()
        host.reconnect()
        host.hello()
        assert host.resume(host_id, host_key) == 1
        status, _, _, body = host.request(0x10, struct.pack("<H", mode))
        assert decode_values(schema, body) == {mode: 2}  # kept across the reconnect
        status, _, _, _ = host.request(0x11, struct.pack("<H", mode) + encode_value(T_ENUM, 1))
        assert status == OK
        status, detail, _, _ = host.request(0x11, struct.pack("<H", ssid) + encode_value(T_TEXT, "Work"))
        assert (status, detail) == (AUTH_REQUIRED, 2)  # Wi-Fi needs a fresh code
        status, detail, _, _ = host.request(0x12, struct.pack("<H", restart))
        assert (status, detail) == (AUTH_REQUIRED, 2)
        # Upgrade to level 2 with a code, inside the resumed session.
        status, level = host.pair()
        assert (status, level) == (OK, 2)
        status, _, _, _ = host.request(0x11, struct.pack("<H", ssid) + encode_value(T_TEXT, "Work"))
        assert status == OK
        print("remember and resume: ok (level 1, fresh code for level 2)")

        # A wrong key can't resume.
        host.reconnect()
        assert host.resume(host_id, bytes(32)) == WRONG_CODE
        assert host.resume(b"\x00" * 8, host_key) == WRONG_CODE
        print("resume with wrong key or id: refused")

        # Tampering ends the session.
        assert host.resume(host_id, host_key) == 1
        status, _, _, _ = host.request(0x10, struct.pack("<H", mode), tamper=True)
        assert status == AUTH_REQUIRED and host.keys is None
        status, detail, _, _ = host.request(0x11, struct.pack("<H", mode) + encode_value(T_ENUM, 0))
        assert (status, detail) == (AUTH_REQUIRED, 1)  # back at level 0, in the clear
        print("tampered request: session ended")

        # Asking again while a code shows gets the same code; wrong guesses
        # across codes lead to a lockout.
        host.reconnect()
        host.request(0x02, bytes([1]))
        sim.drain(0.05)
        first = sim.code
        host.request(0x02, bytes([1]))
        sim.drain(0.05)
        assert sim.code == first
        wrong = 0
        status = None
        for _ in range(12):
            status, detail = host.pair(code_override="999999" if sim.code != "999999" else "888888")
            if status == LOCKED_OUT:
                break
            assert status == WRONG_CODE
            wrong += 1
        assert status == LOCKED_OUT and wrong == 9, (status, wrong)
        assert 0 < detail <= 60
        sim.write(0xF3, struct.pack("<I", 61000))  # a minute later
        status, level = host.pair()
        assert (status, level) == (OK, 2)
        print("code reuse and lockout: ok (locked after 9 wrong guesses)")

        # Codes expire.
        host.reconnect()
        host.request(0x02, bytes([1]))
        sim.drain(0.05)
        assert sim.code is not None
        sim.write(0xF3, struct.pack("<I", 121000))
        sim.drain(0.1)
        assert sim.code is None
        status, _, _, _ = host.request(0x03, bytes(65))
        assert status == AUTH_REQUIRED
        print("code expiry: ok")

        # A small MTU: replies are cut off with `more`, transfers use small chunks.
        host.reconnect(mtu=23)
        assert host.hello() is None
        status, _, flags, body = host.request(0x10)
        assert status == OK and flags & 1
        blob3, _ = host.read_schema()
        assert blob3 == blob
        print("small MTU: ok")
    finally:
        sim.close()
    print("host_test: ok")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "build/sim_device")

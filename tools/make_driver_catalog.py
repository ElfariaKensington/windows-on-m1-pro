#!/usr/bin/env python3
"""Build an unsigned driver catalog (.cat) identical in structure to makecat's output for a
CDF of the form used by the J316 packages:

    [CatalogHeader]  PublicVersion=0x1, EncodingType=0x00010001,
                     CATATTR1=0x10010001:OSAttr:2:10.0
    [CatalogFiles]   <HASH>file=file ...

Members carry SHA-1 hashes: the Authenticode image hash for PE files (SPC_PE_IMAGE_DATA,
PE SIP {C689AAB8-...}) and the plain file hash for everything else (SPC_CAB_DATA, flat SIP
{DE351A42-...}). Member tags are the uppercase hex hash in UTF-16LE with a terminating NUL,
sorted ascending. Sign the result with osslsigncode (it signs CAT files).

Validated against .build/j316s-fresh-input/build-v5/J316SpiInput-unsigned.cat (makecat):
everything except the random list identifier and the timestamp matches byte for byte
(tests/test_j316s_wifi.py).
"""
import argparse
import datetime
import hashlib
import os
from pathlib import Path
import struct

PE_SIP = "{C689AAB8-8E78-11D0-8C47-00C04FC295EE}"
FLAT_SIP = "{DE351A42-8E59-11D0-8C47-00C04FC295EE}"


def der(tag, body):
    n = len(body)
    if n < 0x80:
        length = bytes([n])
    else:
        raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
        length = bytes([0x80 | len(raw)]) + raw
    return bytes([tag]) + length + body


def seq(*items):
    return der(0x30, b"".join(items))


def set_(*items):
    return der(0x31, b"".join(items))


def oid(dotted):
    parts = [int(p) for p in dotted.split(".")]
    out = bytes([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        p >>= 7
        while p:
            chunk.insert(0, 0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(chunk)
    return der(0x06, out)


def integer(v):
    raw = v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big")
    return der(0x02, raw)


NULL = der(0x05, b"")


def octets(b):
    return der(0x04, b)


def bmp(s):
    return der(0x1E, s.encode("utf-16-be"))


def ctx(n, body, constructed=True):
    return der((0xA0 if constructed else 0x80) | n, body)


def authenticode_sha1(data):
    """Authenticode PE image hash (excludes CheckSum, the security directory entry and the
    certificate table), as Windows computes it for catalog membership."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    checksum = opt + 64
    datadir = opt + (96 if magic == 0x10B else 112)
    security = datadir + 4 * 8
    cert_off, cert_len = struct.unpack_from("<II", data, security)
    end = cert_off if cert_off and cert_len else len(data)
    h = hashlib.sha1()
    h.update(data[:checksum])
    h.update(data[checksum + 4:security])
    h.update(data[security + 8:end])
    tail = len(data) - end - (cert_len if cert_off and cert_len else 0)
    if tail > 0:
        h.update(data[end + cert_len:])
    return h.digest()


def is_pe(data):
    return len(data) > 0x40 and data[:2] == b"MZ" and \
        data[struct.unpack_from("<I", data, 0x3C)[0]:][:4] == b"PE\0\0"


def member(path):
    data = Path(path).read_bytes()
    if is_pe(data):
        digest = authenticode_sha1(data)
        # SpcPeImageData { flags BIT STRING 05a0 (as makecat), file [0] { [2] { [0] "" } } }
        value = seq(oid("1.3.6.1.4.1.311.2.1.15"),
                    seq(der(0x03, b"\x05\xa0"), ctx(0, ctx(2, ctx(0, b"", constructed=False)))))
        sip = PE_SIP
    else:
        digest = hashlib.sha1(data).digest()
        value = seq(oid("1.3.6.1.4.1.311.2.1.25"), ctx(2, ctx(0, b"", constructed=False)))
        sip = FLAT_SIP
    indirect = seq(value, seq(seq(oid("1.3.14.3.2.26"), NULL), octets(digest)))
    attributes = set_(
        seq(oid("1.3.6.1.4.1.311.2.1.4"), set_(indirect)),
        seq(oid("1.3.6.1.4.1.311.12.2.2"), set_(seq(bmp(sip), integer(512)))),
    )
    tag = (digest.hex().upper() + "\0").encode("utf-16-le")
    return digest, seq(octets(tag), attributes)


def build(files, list_id=None, when=None):
    list_id = list_id if list_id is not None else os.urandom(16)
    when = when or datetime.datetime.now(datetime.timezone.utc)
    members = sorted(member(f) for f in files)
    osattr = seq(bmp("OSAttr"), integer(0x10010001), octets("2:10.0\0".encode("utf-16-le")))
    ctl = seq(
        seq(oid("1.3.6.1.4.1.311.12.1.1")),
        octets(list_id),
        der(0x17, when.strftime("%y%m%d%H%M%SZ").encode()),
        seq(oid("1.3.6.1.4.1.311.12.1.2"), NULL),
        seq(*[m for _, m in members]),
        ctx(0, seq(seq(oid("1.3.6.1.4.1.311.12.2.1"), octets(osattr)))),
    )
    signed = seq(integer(1), set_(), seq(oid("1.3.6.1.4.1.311.10.1"), ctx(0, ctl)), set_())
    return seq(oid("1.2.840.113549.1.7.2"), ctx(0, signed))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("output", type=Path)
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args()
    args.output.write_bytes(build(args.files))


if __name__ == "__main__":
    main()

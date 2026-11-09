#!/usr/bin/env python3
"""Generate the J316s/T6000 Hwdata profile with the J313 profile tooling.

Input: the raw 13.5 ADT persisted by J316s-GPU-001.  Uses the same native
serializer (proxyclient AGXHWDataA/B + populate_hwdata) as the J313 profile, with
CHIP_INFO[0x6000].  max_power/rel_max_powers follow m1n1 kboot_gpu
calc_power_t600x using the chip's per-cluster leak inputs (unk_3cf4/unk_3d14).
Output header uses the J313 symbol names; consumers select it with
APPLE_AGX_PLATFORM_J316S.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "m1n1_windows" / "proxyclient"))
import generate_agx_hwdata_profile as base  # noqa: E402


def perf_table(raw, count, tables):
    vals = [struct.unpack_from("<II", raw, 8 * i) for i in range(len(raw) // 8)]
    if len(vals) < count * tables:
        raise SystemExit("perf table shorter than count*tables")
    return [vals[t * count:(t + 1) * count] for t in range(tables)]


def calc_power_t600x(core, sram, core_leak, sram_leak, core_count, ncores=8):
    """Python transcription of m1n1 src/kboot_gpu.c calc_power_t600x (T6000 path)."""
    s_sram, k_sram = 4.3547606, 0.024927923
    s_core, k_core = 1.48461742, 0.39013552
    dk_core, dk_sram, imax = 1.06975, 0.00625, 26.0
    core_leak = [u + (c / ncores) * (v - u) for v, c in zip(core_leak, core_count)
                 for u in [v * 0.0825]]
    sram_leak = [u + (c / ncores) * (v - u) for v, c in zip(sram_leak, core_count)
                 for u in [v * 0.2247]]
    count = len(core[0])
    out = [0]
    for i in range(1, count):
        total = 0
        for j in range(len(core)):
            cf, cv = core[j][i]
            sf, sv = sram[j][i]
            mw = sv / 1000 * sram_leak[j] * k_sram * math.exp(sv / 1000 * s_sram)
            mw += cv / 1000 * core_leak[j] * k_core * math.exp(cv / 1000 * s_core)
            sb = sv / 750
            mw += dk_sram * core_count[j] * (sf / 1e6) * sb ** 3
            cb = cv / 750
            mw += dk_core * core_count[j] * (cf / 1e6) * (cb ** 2 if cv < 750 else cb ** 3)
            mw = min(mw, imax * cv)
            total += int(mw)
        out.append(total * 1000)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adt", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    a = ap.parse_args()
    adt_bytes = a.adt.read_bytes()

    import construct
    from m1n1.adt import load_adt
    from m1n1.agx.initdata import CHIP_INFO, populate_hwdata
    from m1n1.constructutils import Ver
    from m1n1.fw.agx.initdata import AGXHWDataA, AGXHWDataB, IOMapping

    adt = load_adt(adt_bytes)
    chosen = adt["/chosen"]
    if chosen.chip_id != 0x6000:
        raise SystemExit("profile requires a T6000 ADT")
    fw = chosen.getprop("firmware-version", b"")
    fw = fw.encode() if isinstance(fw, str) else fw
    if fw.rstrip(b"\0") != b"iBoot-8422.141.2":
        raise SystemExit("profile requires the 13.5 stub firmware ABI")
    Ver.set_version_key("V", "V13_5")
    Ver.set_version_key("G", "G13")
    raw = base.raw_properties(adt_bytes, "/arm-io/sgx")
    count = struct.unpack("<I", raw["perf-state-count"])[0]
    tables = struct.unpack("<I", raw["perf-state-table-count"])[0]
    core = perf_table(raw["perf-states"], count, tables)
    sram = perf_table(raw["perf-states-sram"], count, tables)
    info = CHIP_INFO[0x6000]
    pwr = calc_power_t600x(core, sram, info.unk_3cf4[:tables], info.unk_3d14[:tables],
                           [8] * tables)
    max_mw = max(pwr) // 1000
    info.max_power = max_mw
    info.rel_max_powers = [p // 1000 * 100 // max_mw for p in pwr]
    sgx = adt["/arm-io/sgx"]
    ha, hb = AGXHWDataA(sgx, info), AGXHWDataB(sgx, info)
    populate_hwdata(sgx, info, ha, hb)
    hb.io_mappings = [IOMapping() for _ in range(25)]
    hb.timestamp_region_base = 0
    hb.sgx_sram_ptr = 0
    a_bytes, b_bytes = ha.build(), hb.build()
    if len(a_bytes) != 0x421c or len(b_bytes) != 0x1884:
        raise SystemExit("Hwdata layout differs from the J313 profile ABI")
    inputs = tuple((n, raw.get(n)) for n in base.INPUT_NAMES)
    sources = {n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest()
               for n in base.SOURCE_PATHS + ("tools/generate_agx_hwdata_profile_t6000.py",)}
    sources["python-package/construct-version"] = hashlib.sha256(
        construct.__version__.encode()).hexdigest()
    digest = hashlib.sha256(b"AGX-HWDATA-PROFILE\0v1\0T6000\0J316s\0G13\0V13_5\0")
    digest.update(base.canonical_inputs(inputs))
    for blob in (a_bytes, b_bytes):
        digest.update(struct.pack("<I", len(blob)) + blob)
    profile = base.Profile(a_bytes, b_bytes, inputs, sources, digest.digest(),
                           hashlib.sha256(adt_bytes).hexdigest())
    text = base.render_header(profile).replace(
        "APPLE_AGX_HWDATA_PROFILE_GENERATED_H", "APPLE_AGX_HWDATA_PROFILE_J316S_GENERATED_H")
    text = text.replace("#define AGX_HWDATA_A_BYTES",
                        f"#define AGX_HWDATA_CHIP_ID 0x6000u\n#define AGX_HWDATA_MAX_POWER_MW {max_mw}u\n"
                        "#define AGX_HWDATA_A_BYTES", 1)
    a.output.write_text(text)
    print("T6000 max power", max_mw, "mW rel", info.rel_max_powers, "per-pstate", pwr)


if __name__ == "__main__":
    main()

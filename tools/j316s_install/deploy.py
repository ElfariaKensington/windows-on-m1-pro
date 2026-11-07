"""Write only the newly reserved, hash-pinned Windows container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess

from layout import BLOCK, DISK_BYTES, INSTALL_TYPE, digest


def validate_target(info, target):
    if (info.get("DiskUUID") != target["DiskUUID"] or not info.get("Internal") or
        info.get("Content", "").upper() != INSTALL_TYPE or
        info.get("TotalSize") != DISK_BYTES or info.get("DeviceBlockSize") != BLOCK or
        info.get("PartitionMapPartitionOffset") != target["PartitionMapPartitionOffset"] or
        info.get("Mounted") or info.get("MountPoint")):
        raise RuntimeError("Installation container changed; no write performed")
    identifier=info.get("DeviceIdentifier", "")
    if not re.fullmatch(r"disk0s[1-9][0-9]*",identifier):
        raise RuntimeError("Unexpected container device identifier")
    return Path("/dev/r"+identifier)


def device_digest(path):
    h=hashlib.sha256();remaining=DISK_BYTES
    with path.open("rb",buffering=0) as f:
        while remaining:
            data=f.read(min(4*1024**2,remaining))
            if not data:raise RuntimeError("Short installation-container readback")
            h.update(data);remaining-=len(data)
    return h.hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("target","image","manifest"):p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args()
    if os.geteuid() != 0:raise RuntimeError("Administrator privileges required")
    model=subprocess.check_output(["/usr/sbin/sysctl","-n","hw.model"],text=True).strip()
    if model != "MacBookPro18,1":raise RuntimeError("Unexpected Mac model")
    target=json.loads(a.target.read_text());manifest=json.loads(a.manifest.read_text())
    info=plistlib.loads(subprocess.check_output(["/usr/sbin/diskutil","info","-plist",target["DiskUUID"]]))
    device=validate_target(info,target)
    if a.image.stat().st_size != DISK_BYTES or digest(a.image) != manifest["sha256"]:
        raise RuntimeError("Installation image hash/size mismatch")
    with device.open("rb",buffering=0) as f:
        edges=f.read(1024**2);f.seek(DISK_BYTES-1024**2);edges+=f.read(1024**2)
    if hashlib.sha256(edges).hexdigest() != target["empty_edges_sha256"]:
        raise RuntimeError("New container was modified after reservation; refusing to overwrite")
    subprocess.run(["/bin/dd",f"if={a.image}",f"of={device}","bs=4m"],check=True)
    subprocess.run(["/bin/sync"],check=True)
    actual=device_digest(device)
    if actual != manifest["sha256"]:raise RuntimeError("Full container readback mismatch")
    receipt=dict(device=str(device),disk_uuid=target["DiskUUID"],bytes=DISK_BYTES,
                 readback_sha256=actual,action="installed source and empty Windows target")
    a.manifest.with_suffix(".installed.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print(json.dumps(receipt,indent=2))


if __name__ == "__main__":main()

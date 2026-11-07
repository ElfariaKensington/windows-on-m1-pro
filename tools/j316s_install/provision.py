"""Reserve the authorized Windows area using supported macOS partitioning APIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess

from layout import BLOCK, DISK_BYTES, INSTALL_TYPE, RESERVATION_BYTES

MAC_UUID = "4851F6E1-6299-4215-9335-D6AA7EC5147E"
EFI_UUID = "9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE"
MAC_BYTES = 484_000_661_504
NEW_MAC_BYTES = MAC_BYTES - RESERVATION_BYTES - 256 * 1024**2


def command(args):
    return subprocess.check_output(args)


def info(identifier):
    return plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", identifier]))


def partitions():
    tree = plistlib.loads(command(["/usr/sbin/diskutil", "list", "-plist", "disk0"]))
    result = []
    for disk in tree["AllDisksAndPartitions"]:
        for part in disk.get("Partitions", []):
            i=info(part["DeviceIdentifier"])
            result.append({k:i.get(k) for k in ("DiskUUID", "Content", "DeviceIdentifier",
                "PartitionMapPartitionOffset", "TotalSize", "Internal", "Mounted", "MountPoint")})
    return result


def validate_plan(parts, minimum):
    by_id = {p["DiskUUID"]:p for p in parts}
    if MAC_UUID not in by_id or EFI_UUID not in by_id:
        raise RuntimeError("Expected macOS and Windows EFI partitions are absent")
    mac, efi = by_id[MAC_UUID], by_id[EFI_UUID]
    if (not mac["Internal"] or mac["Content"] != "Apple_APFS" or
            mac["TotalSize"] != MAC_BYTES or not efi["Internal"] or
            efi["Content"] != "EFI" or efi["TotalSize"] != 1_000_341_504):
        raise RuntimeError("Partition identity or size changed")
    if any(p["Content"].upper() == INSTALL_TYPE for p in parts):
        raise RuntimeError("An installation container already exists; refusing to replace it")
    if NEW_MAC_BYTES < minimum:
        raise RuntimeError("Not enough reclaimable macOS space for the 80 GB installation disk")
    start=mac["PartitionMapPartitionOffset"] + NEW_MAC_BYTES
    end=mac["PartitionMapPartitionOffset"] + MAC_BYTES
    if start % BLOCK or end % BLOCK:
        raise RuntimeError("Unaligned partition bounds")
    return dict(old_macos_bytes=MAC_BYTES, new_macos_bytes=NEW_MAC_BYTES,
        windows_bytes=DISK_BYTES, free_region_first=start, free_region_end=end,
        outer_type=INSTALL_TYPE, protected_partitions=parts,
        commands=[["/usr/sbin/diskutil", "apfs", "resizeContainer", MAC_UUID, str(NEW_MAC_BYTES)+"b"],
                  ["/usr/sbin/diskutil", "addPartition", MAC_UUID,
                   "%"+INSTALL_TYPE+"%", "%noformat%", str(RESERVATION_BYTES)+"b"]])


def validate_result(before, after, plan):
    by_id={p["DiskUUID"]:p for p in after}
    for old in before:
        new=by_id.get(old["DiskUUID"])
        expected=NEW_MAC_BYTES if old["DiskUUID"] == MAC_UUID else old["TotalSize"]
        if not new or any(new[k] != old[k] for k in ("Content","PartitionMapPartitionOffset")) or new["TotalSize"] != expected:
            raise RuntimeError("A protected partition changed unexpectedly")
    targets=[p for p in after if p["Content"].upper() == INSTALL_TYPE]
    if len(targets) != 1:
        raise RuntimeError("Expected exactly one private installation container")
    target=targets[0]; start=target["PartitionMapPartitionOffset"]
    if not target["Internal"] or target["TotalSize"] != DISK_BYTES or target.get("MountPoint") or not (
        plan["free_region_first"] <= start < start+DISK_BYTES <= plan["free_region_end"]):
        raise RuntimeError("New container is outside the reserved space")
    return target


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--output",type=Path,required=True)
    p.add_argument("--execute",action="store_true");a=p.parse_args()
    if command(["/usr/sbin/sysctl","-n","hw.model"]).decode().strip() != "MacBookPro18,1":
        raise RuntimeError("This layout is pinned to the user's J316s Mac")
    before=partitions()
    limits=plistlib.loads(command(["/usr/sbin/diskutil","apfs","resizeContainer",MAC_UUID,"limits","-plist"]))
    plan=validate_plan(before,limits["MinimumSizePreferred"])
    if not a.execute:
        print(json.dumps(plan,indent=2));return
    if os.geteuid() != 0:raise RuntimeError("Administrator privileges required")
    if a.output.exists():raise FileExistsError(a.output)
    a.output.mkdir(parents=True)
    (a.output/"plan.json").write_text(json.dumps(plan,indent=2)+"\n")
    disk=info("disk0"); size=disk["TotalSize"]
    if disk["DeviceBlockSize"] != BLOCK or not disk["Internal"]:
        raise RuntimeError("Unexpected physical disk")
    (a.output/"physical-disk-before.plist").write_bytes(plistlib.dumps(disk))
    try:
        with open("/dev/rdisk0","rb",buffering=0) as f:
            (a.output/"gpt-primary-before.bin").write_bytes(f.read(7*BLOCK))
            f.seek(size-5*BLOCK);(a.output/"gpt-secondary-before.bin").write_bytes(f.read(5*BLOCK))
    except PermissionError:
        # macOS can deny whole-device reads while allowing its supported live
        # resize API. The complete UUID/type/offset/size map is saved in plan.json.
        (a.output/"backup-method.txt").write_text(
            "Whole-device raw read denied by macOS; logical GPT map preserved in plan.json.\n")
    for i,cmd in enumerate(plan["commands"]):
        with (a.output/f"diskutil-{i}.log").open("wb") as log:
            subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    after=partitions();target=validate_result(before,after,plan)
    target["raw_device"]="/dev/r"+target["DeviceIdentifier"]
    with open(target["raw_device"],"rb",buffering=0) as f:
        edges=f.read(1024**2);f.seek(DISK_BYTES-1024**2);edges+=f.read(1024**2)
    target["empty_edges_sha256"]=hashlib.sha256(edges).hexdigest()
    (a.output/"after.json").write_text(json.dumps(after,indent=2)+"\n")
    (a.output/"target.json").write_text(json.dumps(target,indent=2)+"\n")
    print(json.dumps(target,indent=2))


if __name__ == "__main__":main()

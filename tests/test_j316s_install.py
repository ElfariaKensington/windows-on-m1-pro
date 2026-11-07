"""Geometry and live-partition preservation gates for the J316 installation disk."""
import copy
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools/j316s_install"))
import layout
import provision
import deploy


class InstallationGeometry(unittest.TestCase):
    def test_windows_space_and_four_kilobyte_gpt(self):
        d=layout.geometry()
        self.assertGreaterEqual(d["windows_unallocated"]["blocks"]*4096,64_000_000_000)
        parts=d["partitions"]
        self.assertGreaterEqual(parts[0]["blocks"]*4096,260*1024**2)
        self.assertEqual(parts[1]["blocks"]*4096,16*1024**2)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/"disk.img";layout.write_gpt(p,d)
            with p.open("rb") as f:
                f.seek(4096);h=bytearray(f.read(4096));table=f.read(16384)
                f.seek(d["bytes"]-5*4096);self.assertEqual(f.read(16384),table)
                backup=f.read(4096)
            crc=struct.unpack_from("<I",h,16)[0];struct.pack_into("<I",h,16,0)
            self.assertEqual(zlib.crc32(h[:92]),crc)
            self.assertEqual(zlib.crc32(table),struct.unpack_from("<I",h,88)[0])
            self.assertEqual(struct.unpack_from("<Q",backup,24)[0],d["bytes"]//4096-1)
            for part in parts:
                entry=table[(part["number"]-1)*128:part["number"]*128]
                self.assertEqual(entry[:16],uuid.UUID(part["type"]).bytes_le)
                first,last=struct.unpack_from("<QQ",entry,32)
                self.assertEqual((first,last-first+1),(part["first"],part["blocks"]))
            with self.assertRaises(FileExistsError):layout.write_gpt(p,d)

    def test_rejects_unaligned_or_too_small_disk(self):
        for size in [layout.DISK_BYTES-1,64*1024**3,32*1024**3]:
            with self.subTest(size=size),self.assertRaises(ValueError):layout.geometry(size)


class PartitionPreservation(unittest.TestCase):
    def setUp(self):
        self.parts=[dict(DiskUUID=provision.MAC_UUID,Content="Apple_APFS",Internal=True,
            TotalSize=provision.MAC_BYTES,PartitionMapPartitionOffset=524312576),
            dict(DiskUUID=provision.EFI_UUID,Content="EFI",Internal=True,
            TotalSize=1_000_341_504,PartitionMapPartitionOffset=487024779264)]
        self.plan=provision.validate_plan(self.parts,380_000_000_000)

    def result(self):
        parts=copy.deepcopy(self.parts);parts[0]["TotalSize"]=provision.NEW_MAC_BYTES
        parts.append(dict(DiskUUID="new",Content=layout.INSTALL_TYPE,Internal=True,
            TotalSize=layout.DISK_BYTES,PartitionMapPartitionOffset=self.plan["free_region_first"],MountPoint=""))
        return parts

    def test_only_new_private_area_and_macos_shrink_are_accepted(self):
        self.assertEqual(provision.validate_result(self.parts,self.result(),self.plan)["DiskUUID"],"new")

    def test_existing_windows_or_insufficient_space_refuses(self):
        with self.assertRaises(RuntimeError):provision.validate_plan(self.parts,provision.NEW_MAC_BYTES+4096)
        with self.assertRaises(RuntimeError):provision.validate_plan(self.result(),380_000_000_000)

    def test_protected_efi_movement_and_resize_are_rejected(self):
        for key in ["TotalSize","PartitionMapPartitionOffset"]:
            parts=self.result();parts[1][key]+=4096
            with self.subTest(key=key),self.assertRaises(RuntimeError):provision.validate_result(self.parts,parts,self.plan)

    def test_container_outside_new_free_space_or_mounted_refuses(self):
        parts=self.result();parts[2]["PartitionMapPartitionOffset"]-=4096
        with self.assertRaises(RuntimeError):provision.validate_result(self.parts,parts,self.plan)
        parts=self.result();parts[2]["MountPoint"]="/Volumes/Unexpected"
        with self.assertRaises(RuntimeError):provision.validate_result(self.parts,parts,self.plan)


class DeploymentTarget(unittest.TestCase):
    def test_only_pinned_private_partition_is_writable(self):
        target=dict(DiskUUID="new-private",PartitionMapPartitionOffset=404256538624)
        info=dict(target,Internal=True,Content=layout.INSTALL_TYPE,TotalSize=layout.DISK_BYTES,
                  DeviceBlockSize=4096,DeviceIdentifier="disk0s8",MountPoint="")
        self.assertEqual(deploy.validate_target(info,target),Path("/dev/rdisk0s8"))
        for key,value in [("DiskUUID","other"),("Content","Apple_APFS"),("Content","EFI"),
                          ("Internal",False),("Mounted",True),("DeviceBlockSize",512),
                          ("TotalSize",layout.DISK_BYTES-4096),("DeviceIdentifier","disk0"),
                          ("PartitionMapPartitionOffset",1)]:
            changed=dict(info);changed[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(RuntimeError):
                deploy.validate_target(changed,target)

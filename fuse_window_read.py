#!/usr/bin/env python3
"""Read-only dump of the GA100 fuse block shadow window via /dev/gpcprobe.

Issues ZERO writes anywhere (only GP_IOC_READ32). Reads BAR0 0x820000-
0x821000 (NV_FUSE) and decodes the security-relevant shadow registers,
so we can see what the host can actually see with the driver loaded
(PLM 0x8200F8 in effect).
"""
import fcntl
import os
import struct
import sys

GP_IOC_MAGIC = ord("G")
GP_IOC_INFO = 0x80044701  # _IO(G,1) size=4? computed below properly

# compute proper ioctl numbers like gpc_probe.py does
import array

def _io(t, n):        return (t << 8) | n
def _ior(t, n, s):    return (1 << 30) | (s << 16) | (t << 8) | n
def _iowr(t, n, s):   return (3 << 30) | (s << 16) | (t << 8) | n

GP_IOC_INFO   = _io(GP_IOC_MAGIC, 1)
GP_IOC_READ32 = _iowr(GP_IOC_MAGIC, 2, 12)

fd = os.open("/dev/gpcprobe", os.O_RDWR)
info = bytearray(4 + 16 * 4)
fcntl.ioctl(fd, GP_IOC_INFO, info, True)
count = struct.unpack_from("<I", info)[0]
print(f"cards: {count}")
idx = 0

def rd(off):
    buf = bytearray(struct.pack("<III", idx, off, 0))
    fcntl.ioctl(fd, GP_IOC_READ32, buf)
    return struct.unpack_from("<I", buf, 8)[0]

FUSE = 0x820000
KEY = {
    0x000: "FUSECTRL",
    0x004: "FUSEADDR",
    0x008: "FUSERDATA",
    0x00c: "FUSEWDATA",
    0x040: "EN_SW_OVERRIDE",
    0x050: "IFF_RECORD",
    0x084: "DISABLE_SW_OVERRIDE_STATUS",
    0x088: "IFF_SW_FUSING",
    0x0f8: "FUSECTRL_PLM",
    0x0fc: "STATUS_PLM",
    0x148: "OPT_DEBUG_FEATURE_SW_OVERRIDE_ENABLE",
    0x324: "OPT_PRIVKEY_ACC_DIS",
    0x434: "OPT_PRIV_SEC_EN",
    0x584: "OPT_DEVID_SW_OVERRIDE_DIS",
    0x5a8: "OPT_SFK_ACC_DIS",
}
print("=== key registers ===")
for off, name in sorted(KEY.items()):
    try:
        v = rd(FUSE + off)
        print(f"  0x{FUSE + off:08x} {name:42s} = 0x{v:08x}")
    except OSError as e:
        print(f"  0x{FUSE + off:08x} {name:42s} ERR {e}")

print("\n=== non-zero map 0x820000-0x821000 ===")
nz = 0
for off in range(FUSE, FUSE + 0x1000, 4):
    v = rd(off)
    if v:
        nz += 1
        if nz <= 40:
            print(f"  0x{off:08x} = 0x{v:08x}")
if nz > 40:
    print(f"  ... {nz} non-zero total")

# sanity sentinels: card should be untouched
print("\n=== sentinels (read twice, must be stable) ===")
for off, name in [(0x00000000, "PMC_BOOT_0"), (0x001536, "PMC_PTOP")]:
    a, b = rd(off), rd(off)
    print(f"  {name}: 0x{a:08x} / 0x{b:08x} {'OK' if a == b else 'CHANGED!'}")

os.close(fd)
print("\nno writes issued.")

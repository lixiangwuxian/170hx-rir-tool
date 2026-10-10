#!/usr/bin/env python3
"""Post-burn verification for the GA100 RIR experiment — READ ONLY.

Run on e5-tw (the box with the burned card) as root. The nvidia driver
owns BAR0, so ga100_rir's mmap and this script's DEBUGCTRL writes only
work with the card unbound:

    sudo systemctl stop nvidia-persistenced
    sudo pkill SRBMiner            # miner pins the device
    echo 0000:02:00.0 | sudo tee /sys/bus/pci/drivers/nvidia/unbind
    sudo python3 rir_verify_remote.py
    echo 0000:02:00.0 | sudo tee /sys/bus/pci/drivers/nvidia/bind

Reads the full 8-row RIR macro through the DEBUGCTRL RWL (read-with-
lock) aperture — the same path ga100_rir --dump-rir uses. FUSEWDATA and
CMD_WRITE are never touched; no fuse is modified.
"""
import fcntl
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.expanduser("~/gpcprobe"))
import gpc_probe as gp  # noqa: E402

NV_FUSE_FUSEADDR = 0x820004
NV_FUSE_FUSERDATA = 0x820008
NV_FUSE_DEBUGCTRL = 0x820060
NUM_MAIN_ROWS = 512

STATUS = [
    ("PMC_BOOT_0", 0x00000000),
    ("FUSECTRL", 0x00820000),
    ("ENABLE_FUSE_PROGRAM_STATUS", 0x00820078),
    ("DISABLE_FUSE_PROGRAM_STATUS", 0x0082007C),
    ("BYPASS_FUSES_STATUS", 0x00820080),
    ("DISABLE_SW_OVERRIDE_STATUS", 0x00820084),
    ("EN_SW_OVERRIDE", 0x00820040),
    ("OPT_SECURE_FUSE_RESENSE_DISABLE", 0x00821058),
]


def main():
    fd = os.open("/dev/gpcprobe", os.O_RDWR)

    def rd(off, idx=0):
        buf = bytearray(struct.pack("<III", idx, off, 0))
        fcntl.ioctl(fd, gp.GP_IOC_READ32, buf)
        return struct.unpack_from("<I", buf, 8)[0]

    def wr(off, val, idx=0):
        buf = bytearray(struct.pack("<III", idx, off, val))
        fcntl.ioctl(fd, gp.GP_IOC_WRITE, buf)

    info = bytearray(4 + 16 * 4)
    fcntl.ioctl(fd, gp.GP_IOC_INFO, info, True)
    print("cards:", struct.unpack_from("<I", info)[0])

    print("\n=== status registers ===")
    for name, off in STATUS:
        print(f"  {name:32s} 0x{off:08X} = 0x{rd(off):08X}")

    # Fuse macro via RWL. RWL bit only re-routes the READ mux to the raw
    # sense latches; it is the NVIDIA-documented read path (ga100_rir
    # --dump-rir uses the same). No write to FUSEWDATA / CMD_WRITE.
    dc = rd(NV_FUSE_DEBUGCTRL)
    wr(NV_FUSE_DEBUGCTRL, dc | 2)
    try:
        print("\n=== RIR rows 0-7 (RWL raw view) ===")
        rir = []
        for r in range(8):
            wr(NV_FUSE_FUSEADDR, NUM_MAIN_ROWS + r)
            rir.append(rd(NV_FUSE_FUSERDATA))
            print(f"  Row {r}: 0x{rir[-1]:08X}")
        print("\n  decoded (scrambled TEF16 layout, addr[4:0]@[14:10],")
        print("  addr[6:5]@[9:8], addr[12:7]@[7:2], data@[1], en@[0]):")
        for r, v in enumerate(rir):
            for h in (0, 1):
                rec = (v >> 16) if h else (v & 0xFFFF)
                if rec == 0:
                    continue
                en, data = rec & 1, (rec >> 1) & 1
                addr = (((rec >> 10) & 0x1F) | (((rec >> 8) & 3) << 5)
                        | (((rec >> 2) & 0x3F) << 7))
                print(f"    slot {r*2+h}: 0x{rec:04X} scrambled-abs {addr}"
                      f" (row {addr//32}:{addr%32}) data={data} en={en}")
        print("\n=== main rows of interest (same RWL view) ===")
        for r in (0, 1, 18, 33, 40, 42):
            wr(NV_FUSE_FUSEADDR, r)
            print(f"  Row {r:3d}: 0x{rd(NV_FUSE_FUSERDATA):08X}")
    finally:
        wr(NV_FUSE_DEBUGCTRL, dc & ~2)

    print("\n=== health ===")
    for cmd in (["nvidia-smi", "--query-gpu=name,memory.total,pstate",
                 "--format=csv,noheader"],
                ["sh", "-c", "dmesg | grep -i xid | grep -v r8169 | tail -5 ; "
                 "echo dmesg-xid-lines-done"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True,
                                 timeout=20).stdout.strip()
            print(out if out else "  (clean)")
        except Exception as e:  # noqa: BLE001
            print("  health cmd failed:", e)
    os.close(fd)


if __name__ == "__main__":
    main()

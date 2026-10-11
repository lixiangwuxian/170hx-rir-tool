# GA100 RIR Writer

[English](README.md)

GA100 (Ampere) GPU 的 RIR (Redundancy Information Rows) 读写独立工具。
读取路径已在 CMP 170HX 上实测可用。RIR 写入能落地、
能跨冷启动保存。此前"gate 不变"是因为编码 bug（scrambled → 线性已修正），
正确载荷尚未实测。

> **⚠ 本项目几乎完全由 AI 辅助("vibe coding")完成——包括本 README。
> 代码的有效性仅限于文末汇总的实测结论。**
>
> **对硬件执行任何操作前,请先自行审查源码,尤其是写入路径
> (`cmd_write_rir`):RIR 烧写不可逆,一个 bug 可能让你损失一个
> slot 甚至整张卡。**

## 构建

```bash
make
# 或
gcc -O2 -o ga100_rir ga100_rir.c
```

## 前置条件

1. 已安装该 GPU 的 Linux 系统
2. root 权限 (sudo)

读取**不需要**解绑 NVIDIA 驱动(实测:`--status` / `--dump-rir` /
`--dump-fuses` 在驱动加载状态下均可用)。只有当命令报 DEBUGCTRL RWL
位无法置位(即驱动占着 fuse 块访问权)时才需要解绑:

```bash
lspci -D | grep NVIDIA
# 0000:41:00.0 3D controller: NVIDIA Corporation GA100 ...

echo 0000:41:00.0 > /sys/bus/pci/devices/0000:41:00.0/driver/unbind
```


## 快速上手

```bash
# 1. 查看当前状态(只读,安全)
sudo ./ga100_rir 0000:41:00.0

# 2. 易失性解锁(安全,重启后失效)——需要 gate=0:
sudo ./ga100_rir 0000:41:00.0 --sw-enable                  # 要求 gate 已开
sudo ./ga100_rir 0000:41:00.0 --sw-write gen3 0            # 解锁 PCIe Gen3/Gen4
sudo ./ga100_rir 0000:41:00.0 --sw-write nvlink 0          # 解锁 NVLink
sudo ./ga100_rir 0000:41:00.0 --sw-write ecc 1             # 启用 ECC

# 3. 重新绑定驱动并验证
echo 0000:41:00.0 > /sys/bus/pci/drivers/nvidia/bind
nvidia-smi
lspci -vvv -s 41:00.0 | grep -i speed
```

`--sw-enable` 只在 `disable_sw_override` gate 已经打开的卡上有效
(`--status` 显示 `fuse : 0`)。对 gate 锁死的卡,用
`--write-rir <row> 0x00950015` 通过 RIR 清除 gate fuse（不可逆）。

## 全部命令

```bash
# === 只读(安全) ===
sudo ./ga100_rir <bdf>                              # 完整状态 + RIR 状态
sudo ./ga100_rir <bdf> --status                     # 功能 / floorsweep 信息
sudo ./ga100_rir <bdf> --dump-rir                   # RIR 记录
sudo ./ga100_rir <bdf> --dump-fuses 128             # 原始 fuse 行

# === RIR 编码(不访问硬件) ===
sudo ./ga100_rir <bdf> --encode-rir 5               # 编码 bit 5 (disable_sw_override PRI)
sudo ./ga100_rir <bdf> --encode-rir 37              # 编码 bit 37 (disable_sw_override RED)

# === RIR 写入(不可逆) ===
sudo ./ga100_rir 0000:41:00.0 --write-rir 0 0x00950015     # disable_sw_override PRI+RED（线性编码）
# 实测 170HX 不需要任何外部硬件。写入能落地、能跨冷启动保存。
# 正确载荷尚未实测。

# === SW Override(易失,重启丢失) ===
sudo ./ga100_rir <bdf> --sw-enable                  # 激活 SW override
sudo ./ga100_rir <bdf> --sw-write gen3 0            # 解锁 PCIe Gen3
sudo ./ga100_rir <bdf> --sw-write nvlink 0          # 解锁 NVLink (0=全部启用)
sudo ./ga100_rir <bdf> --sw-write ecc 1             # 启用 ECC
sudo ./ga100_rir <bdf> --sw-write gpc <mask>        # GPC 禁用掩码
sudo ./ga100_rir <bdf> --sw-write fbp <mask>        # FBP 禁用掩码
sudo ./ga100_rir <bdf> --sw-write fbpa <mask>       # FBPA 禁用掩码
sudo ./ga100_rir <bdf> --sw-write fbio <mask>       # FBIO 禁用掩码
sudo ./ga100_rir <bdf> --sw-write l2 <mask>         # ROP/L2 禁用掩码
sudo ./ga100_rir <bdf> --sw-write pcie <mask>       # PCIe lane 禁用
sudo ./ga100_rir <bdf> --sw-disable                 # 关闭 SW override
```

## 关键 fuse 位(Row 0)

```
Bit | Fuse 名称               | 绝对位 (PRI/RED)
----|-------------------------|-------------------
  0 | enable_fuse_program     | 0 / 32
  1 | disable_fuse_program    | 1 / 33
  5 | disable_sw_override     | 5 / 37  ← 解锁目标
  6 | debug_feature_sw_ovr_en | 6 / 38
```

## 安全须知

- **RIR 写入是永久的** —— 只有 8 行 / 16 个 slot,无法撤销
- **正确编码的 RIR 写入尚未实测** —— 此前"gate 锁死"是编码 bug（scrambled vs 线性）导致；正确载荷 0x00950015 待验证
- **SW Override 是易失的** —— 测试安全,重启即恢复原状
- 每次 RIR 写入都有自动备份 + 两步确认
- 写入前先 `--dump-rir` 确认目标 slot 为空

## 实测结论(汇总)

- fuse 编程通路本身工作正常:写入落地、读回正确、断电冷启动后仍在。
- 此前 `DISABLE_SW_OVERRIDE_STATUS` 在写入后仍 = 1 —— 现已查明原因：
  旧的 scrambled 编码产生了 no-op（目标 row40:bit0 和 row42:bit0 本来就是 0）。
  详见 issue #1。
- 编码已修正为 GA100 线性布局。正确载荷 0x00950015 目标为已验证的
  fuse 位（Row0:5 PRI + Row1:5 RED），效果待实测。
- SW Override 路径（`--sw-enable` / `--sw-write`）尚未在实际卡上测试，
  因为目前没有 gate=0 的卡。
- RIR 规模实测:8 行 × 32 位(16 slot),不是 NVIDIA Maxwell 时代
  MODS 工具硬编码的 4 行。

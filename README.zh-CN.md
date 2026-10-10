# GA100 RIR Writer

[English](README.md)

GA100 (Ampere) GPU 的 RIR (Redundancy Information Rows) 读写独立工具。
读取路径与 SW Override 已在 CMP 170HX 上实测可用。**RIR 写入能落地、
能跨冷启动保存,但从未被证实会改变任何功能状态。**

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

# 2. 易失性解锁(安全,重启后失效)——这些是真有效的:
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
(`--status` 显示 `fuse : 0`)。对 gate 锁死的卡,下面那条历史 RIR
路线已被尝试过且**没能打开 gate**;仅作记录,不推荐执行。

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

# === RIR 写入(不可逆,对 gate 的效果未证实) ===
sudo ./ga100_rir 0000:41:00.0 --write-rir 0 0x15011401     # disable_sw_override PRI+RED 记录(但这并没有解开我这张 170HX 10G 的 fuse 😭)
# 实测 170HX 不需要任何外部硬件。写入能落地、能跨冷启动保存,
# 但从未打开过 gate。

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
- **RIR 写入的效果未证实** —— 记录能跨冷启动保存,但所有实测卡的
  gate 依旧锁死;不要为了"解锁"消耗 slot
- **SW Override 是易失的** —— 测试安全,重启即恢复原状
- 每次 RIR 写入都有自动备份 + 两步确认
- 写入前先 `--dump-rir` 确认目标 slot 为空

## 实测结论(汇总)

- fuse 编程通路本身工作正常:写入落地、读回正确、断电冷启动后仍在。
- 每张已烧卡冷启动后 `DISABLE_SW_OVERRIDE_STATUS` 仍 = 1,与未烧卡
  逐位一致。
- 两种解释均未被排除:(a) 记录布局/编码与硬件实际采用的不符;
  (b) GA100 根本不把 RIR 修正应用到 security latch 类 fuse。
- RIR 规模实测:8 行 × 32 位(16 slot),不是 NVIDIA Maxwell 时代
  MODS 工具硬编码的 4 行。

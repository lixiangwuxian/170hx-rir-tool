/*
 * ga100_rir - Standalone RIR / fuse inspection tool for GA100 (Ampere)
 *
 * Reads fuse state, HBM stack configuration, RIR records, and can
 * perform SW override or RIR writes on GA100-based GPUs (e.g. CMP 170HX).
 *
 * Register map and RIR record layout follow the GA100 fuse block and
 * TSMC TEF16 fuse-macro hardware definitions, cross-checked against
 * live-hardware reads. This is a standalone implementation; it
 * contains no NVIDIA source code.
 *
 * Build: gcc -O2 -o ga100_rir ga100_rir.c
 * Usage: sudo ./ga100_rir <pci_bdf> [command] [args...]
 *        Run without args for help.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

/* ========================================================================
 * Register map for GA100
 * ======================================================================== */

/* --- Fuse control (base 0x820xxx on GA100) --- */
#define NV_FUSE_FUSECTRL                    0x00820000
#define NV_FUSE_FUSECTRL_CMD_IDLE           0x0
#define NV_FUSE_FUSECTRL_CMD_READ           0x1
#define NV_FUSE_FUSECTRL_CMD_WRITE          0x2
#define NV_FUSE_FUSECTRL_STATE_MASK         (0x1Fu << 16)
#define NV_FUSE_FUSECTRL_STATE_IDLE         (4u << 16)

#define NV_FUSE_FUSEADDR                    0x00820004
#define NV_FUSE_FUSERDATA                   0x00820008
#define NV_FUSE_FUSEWDATA                   0x0082000C
#define NV_FUSE_PRIV2INTFC_START            0x00820028

#define NV_FUSE_DEBUGCTRL                   0x00820060
#define NV_FUSE_DEBUGCTRL_RWL_BIT           (1u << 1)

/* --- OPT fuse registers (base 0x820xxx, GA100 specific) --- */
/* SW Override control */
#define NV_FUSE_EN_SW_OVERRIDE              0x00820040
#define NV_FUSE_EN_SW_OVERRIDE_VAL_ENABLE   0x1
#define NV_FUSE_EN_SW_OVERRIDE_VAL_DISABLE  0x0
#define NV_FUSE_DISABLE_SW_OVERRIDE_STATUS  0x00820084

/* Floorsweep disable masks (HW fused values) */
#define NV_FUSE_OPT_GPC_DISABLE            0x00820350
#define NV_FUSE_OPT_FBP_DISABLE            0x00820364
#define NV_FUSE_OPT_FBPA_DISABLE           0x00820368
#define NV_FUSE_OPT_FBIO_DISABLE           0x0082036C
#define NV_FUSE_OPT_ROP_L2_DISABLE         0x008202C4

/* CP (copy/writable) versions of the same registers — used for SW override */
#define NV_FUSE_OPT_GPC_DISABLE_CP         0x0082022C
#define NV_FUSE_OPT_FBP_DISABLE_CP         0x00820234
#define NV_FUSE_OPT_FBPA_DISABLE_CP        0x00820454
#define NV_FUSE_OPT_FBIO_DISABLE_CP        0x00820460
#define NV_FUSE_OPT_ROP_L2_DISABLE_CP      0x008202E4

/* PCIe speed disable fuses */
#define NV_FUSE_OPT_DISABLE_GEN3_SPEED     0x00820250
#define NV_FUSE_OPT_PCIE_MAGIC_BITS_D      0x00820520  /* bit 25 = Gen4 speed disable */
#define NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT  25
#define NV_FUSE_OPT_PCIE_BOOT_GEN23_DIS   0x0082057C
#define NV_FUSE_OPT_PCIE_BOOT_GEN3_DIS    0x00820580
#define NV_FUSE_OPT_PCIE_LANE_DISABLE     0x00820394
#define NV_FUSE_OPT_PCIE_LANE_DISABLE_CP  0x00820248

/* NVLink */
#define NV_FUSE_OPT_NVLINK_DISABLE         0x00820684
#define NV_FUSE_OPT_NVLINK_DISABLE_CP      0x00820688
#define NV_FUSE_CTRL_OPT_NVLINK            0x008209B8
#define NV_FUSE_STATUS_OPT_NVLINK          0x00820DB8

/* ECC */
#define NV_FUSE_OPT_ECC_EN                 0x00820228
#define NV_FUSE_FEATURE_OVERRIDE_ECC       0x0082380C

/* IFF SW Fusing registers (GA100: single register with START 8:0, LAST 24:16) */
#define NV_FUSE_IFF_RECORD                  0x00820050
#define NV_FUSE_IFF_SW_FUSING               0x00820088
#define NV_FUSE_IFF_SW_FUSING_VAL_ENABLE    0x1
#define NV_FUSE_IFF_SW_FUSING_VAL_DISABLE   0x0

/* PMC */
#define NV_PMC_BOOT_0                       0x00000000

/* RIR constants
 * Row count: the community-reported GA100 RIR macro is 8 rows × 32-bit
 * (FUSEADDR mod 8, verified on their 20c2 cards and on our burned
 * 20c2 card: the burned slots decoded as 16-bit records at mod-8
 * addresses). NVIDIA's MODS gm20x ReadRIR hardcodes 4 rows, but that
 * is the Maxwell-era macro; GA100 has no host fuse engine (GSP owns
 * it) and no NVIDIA artifact caps GA100 at 4. Rows 4-7 read back as
 * plain 32-bit words through the same aperture. */
#define NUM_RIR_ROWS                        8
#define NUM_MAIN_FUSE_ROWS_GA100            512

/* GA100 HBM layout:
 * 6 HBM sites (A-F), each site has 2 FBPs = 12 FBPs max
 * Each FBP has 2 FBPAs, so 24 FBPAs max
 * Each FBP has 2 subpartitions (ROP/L2)
 */
#define GA100_MAX_FBP                       12
#define GA100_MAX_FBPA                      24
#define GA100_MAX_GPC                       8
#define GA100_HBM_SITES                     6

/* ========================================================================
 * MMIO helpers
 * ======================================================================== */
typedef struct {
    volatile uint32_t *bar0;
    size_t bar0_size;
    int fd;
    char bdf[48];
    uint32_t boot0;
    /* Backup state for undo */
    uint32_t rir_backup[NUM_RIR_ROWS];
    int rir_backup_valid;
} gpu_ctx_t;

static inline uint32_t rreg32(gpu_ctx_t *c, uint32_t off) {
    return c->bar0[off / 4];
}
static inline void wreg32(gpu_ctx_t *c, uint32_t off, uint32_t v) {
    c->bar0[off / 4] = v;
    __sync_synchronize();
}

/* ========================================================================
 * GPU open / close
 * ======================================================================== */
static int gpu_open(gpu_ctx_t *ctx, const char *bdf)
{
    char path[256];
    struct stat st;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    strncpy(ctx->bdf, bdf, sizeof(ctx->bdf) - 1);

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
    if (stat(path, &st) != 0) {
        fprintf(stderr, "ERROR: PCI device %s not found (%s)\n", bdf, strerror(errno));
        fprintf(stderr, "  Find with: lspci -D | grep -i nvidia\n");
        return -1;
    }

    ctx->fd = open(path, O_RDWR | O_SYNC);
    if (ctx->fd < 0) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "  1) Run as root\n");
        fprintf(stderr, "  2) Unbind driver:\n");
        fprintf(stderr, "     echo %s > /sys/bus/pci/devices/%s/driver/unbind\n", bdf, bdf);
        return -1;
    }

    ctx->bar0_size = st.st_size;
    ctx->bar0 = mmap(NULL, ctx->bar0_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, ctx->fd, 0);
    if (ctx->bar0 == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap failed: %s\n", strerror(errno));
        close(ctx->fd);
        return -1;
    }

    ctx->boot0 = rreg32(ctx, NV_PMC_BOOT_0);
    if (ctx->boot0 == 0xFFFFFFFF) {
        fprintf(stderr, "ERROR: GPU not responding (reads 0xFFFFFFFF)\n");
        munmap((void *)ctx->bar0, ctx->bar0_size);
        close(ctx->fd);
        return -1;
    }

    uint32_t arch = (ctx->boot0 >> 24) & 0x1F;
    uint32_t impl = (ctx->boot0 >> 20) & 0xF;
    printf("[INFO] GPU: %s  BOOT_0=0x%08X  arch=0x%X impl=0x%X\n",
           bdf, ctx->boot0, arch, impl);
    if (arch != 0x17)
        fprintf(stderr, "[WARN] Expected Ampere (arch 0x17), got 0x%X\n", arch);

    return 0;
}

static void gpu_close(gpu_ctx_t *ctx)
{
    if (ctx->bar0 && ctx->bar0 != MAP_FAILED)
        munmap((void *)ctx->bar0, ctx->bar0_size);
    if (ctx->fd >= 0)
        close(ctx->fd);
}

/* ========================================================================
 * Fuse hardware primitives
 * ======================================================================== */
static int poll_fuses_idle(gpu_ctx_t *ctx, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i++) {
        if ((rreg32(ctx, NV_FUSE_FUSECTRL) & NV_FUSE_FUSECTRL_STATE_MASK)
            == NV_FUSE_FUSECTRL_STATE_IDLE)
            return 0;
        usleep(1000);
    }
    fprintf(stderr, "ERROR: FUSECTRL timeout (%d ms), state=0x%08X\n",
            timeout_ms, rreg32(ctx, NV_FUSE_FUSECTRL));
    return -1;
}

static int fuse_read_row(gpu_ctx_t *ctx, uint32_t row, uint32_t *val)
{
    wreg32(ctx, NV_FUSE_FUSECTRL, 0);
    wreg32(ctx, NV_FUSE_FUSEADDR, row);
    wreg32(ctx, NV_FUSE_FUSECTRL, NV_FUSE_FUSECTRL_CMD_READ);
    if (poll_fuses_idle(ctx, 1000)) return -1;
    *val = rreg32(ctx, NV_FUSE_FUSERDATA);
    return 0;
}

static int countbits(uint32_t v) {
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/* ========================================================================
 * HBM / Floorsweep status
 * ======================================================================== */
static void cmd_status(gpu_ctx_t *ctx)
{
    uint32_t gpc_dis  = rreg32(ctx, NV_FUSE_OPT_GPC_DISABLE);
    uint32_t fbp_dis  = rreg32(ctx, NV_FUSE_OPT_FBP_DISABLE);
    uint32_t fbpa_dis = rreg32(ctx, NV_FUSE_OPT_FBPA_DISABLE);
    uint32_t fbio_dis = rreg32(ctx, NV_FUSE_OPT_FBIO_DISABLE);
    uint32_t l2_dis   = rreg32(ctx, NV_FUSE_OPT_ROP_L2_DISABLE);
    uint32_t sw_ovr_status = rreg32(ctx, NV_FUSE_DISABLE_SW_OVERRIDE_STATUS);
    uint32_t sw_ovr_en     = rreg32(ctx, NV_FUSE_EN_SW_OVERRIDE);

    printf("\n=== GA100 Floorsweep / HBM Status ===\n\n");

    printf("SW Override:\n");
    printf("  disable_sw_override fuse : %u (%s)\n",
           sw_ovr_status & 1,
           (sw_ovr_status & 1) ? "SW OVERRIDE PERMANENTLY DISABLED" : "SW override ALLOWED");
    printf("  en_sw_override register  : %u (%s)\n",
           sw_ovr_en & 1,
           (sw_ovr_en & 1) ? "ACTIVE" : "inactive");

    printf("\nOPT Disable Masks (from HW fuses + any active SW override):\n");
    printf("  OPT_GPC_DISABLE    = 0x%03X  (%d of %d GPC disabled)\n",
           gpc_dis & 0xFF, countbits(gpc_dis & 0xFF), GA100_MAX_GPC);
    printf("  OPT_FBP_DISABLE    = 0x%03X  (%d of %d FBP disabled)\n",
           fbp_dis & 0xFFF, countbits(fbp_dis & 0xFFF), GA100_MAX_FBP);
    printf("  OPT_FBPA_DISABLE   = 0x%06X  (%d of %d FBPA disabled)\n",
           fbpa_dis & 0xFFFFFF, countbits(fbpa_dis & 0xFFFFFF), GA100_MAX_FBPA);
    printf("  OPT_FBIO_DISABLE   = 0x%06X  (%d of %d FBIO disabled)\n",
           fbio_dis & 0xFFFFFF, countbits(fbio_dis & 0xFFFFFF), GA100_MAX_FBPA);
    printf("  OPT_ROP_L2_DISABLE = 0x%06X\n", l2_dis & 0xFFFFFF);

    /* GA100 HBM site mapping:
     * Site A = FBP 0,1   (FBPA 0-3)
     * Site B = FBP 2,3   (FBPA 4-7)
     * Site C = FBP 4,5   (FBPA 8-11)
     * Site D = FBP 6,7   (FBPA 12-15)
     * Site E = FBP 8,9   (FBPA 16-19)
     * Site F = FBP 10,11 (FBPA 20-23)
     */
    static const char *site_names[] = {"A", "B", "C", "D", "E", "F"};
    printf("\nHBM Stack / Site Status:\n");
    printf("  Site | FBPs    | FBP Mask   | FBPAs      | FBPA Mask  | Status\n");
    printf("  -----+---------+------------+------------+------------+--------\n");

    int total_enabled_fbp = 0;
    int total_enabled_fbpa = 0;

    for (int site = 0; site < GA100_HBM_SITES; site++) {
        int fbp_lo = site * 2;
        int fbp_hi = site * 2 + 1;
        int fbpa_base = site * 4;

        int fbp_lo_dis = (fbp_dis >> fbp_lo) & 1;
        int fbp_hi_dis = (fbp_dis >> fbp_hi) & 1;
        int fbps_enabled = (fbp_lo_dis ? 0 : 1) + (fbp_hi_dis ? 0 : 1);

        int fbpas_enabled = 0;
        for (int i = 0; i < 4; i++)
            if (!((fbpa_dis >> (fbpa_base + i)) & 1))
                fbpas_enabled++;

        total_enabled_fbp += fbps_enabled;
        total_enabled_fbpa += fbpas_enabled;

        const char *status;
        if (fbps_enabled == 2 && fbpas_enabled == 4)
            status = "FULL";
        else if (fbps_enabled == 0)
            status = "DISABLED";
        else if (fbps_enabled == 1 || fbpas_enabled < 4)
            status = "HALF";
        else
            status = "PARTIAL";

        printf("  %4s | %d,%d     | %s,%s        | %d,%d,%d,%d     | %s%s%s%s       | %s\n",
               site_names[site],
               fbp_lo, fbp_hi,
               fbp_lo_dis ? "X" : "O", fbp_hi_dis ? "X" : "O",
               fbpa_base, fbpa_base+1, fbpa_base+2, fbpa_base+3,
               (fbpa_dis >> (fbpa_base+0)) & 1 ? "X" : "O",
               (fbpa_dis >> (fbpa_base+1)) & 1 ? "X" : "O",
               (fbpa_dis >> (fbpa_base+2)) & 1 ? "X" : "O",
               (fbpa_dis >> (fbpa_base+3)) & 1 ? "X" : "O",
               status);
    }

    printf("\n  Total: %d/%d FBPs enabled, %d/%d FBPAs enabled\n",
           total_enabled_fbp, GA100_MAX_FBP,
           total_enabled_fbpa, GA100_MAX_FBPA);

    /* GB of HBM behind one live FBPA is SKU-dependent (measured on
     * CMP 170HX: 2 GB/FBPA on the 10 GB board, 4 GB/FBPA on the
     * 8 GB board). Default assumes the 10 GB board; override with
     * GB_PER_FBPA=n for other SKUs. */
    double gb_per_fbpa = 2.0;
    const char *gpf_env = getenv("GB_PER_FBPA");
    if (gpf_env) gb_per_fbpa = strtod(gpf_env, NULL);
    printf("  Estimated memory: ~%.0f GB (live FBPA x %.1f GB/FBPA;"
           " 10G SKU: 2, 8G SKU: 4; override GB_PER_FBPA)\n",
           total_enabled_fbpa * gb_per_fbpa, gb_per_fbpa);

    /* PCIe */
    uint32_t gen3_dis      = rreg32(ctx, NV_FUSE_OPT_DISABLE_GEN3_SPEED) & 1;
    uint32_t magic_d       = rreg32(ctx, NV_FUSE_OPT_PCIE_MAGIC_BITS_D);
    uint32_t gen4_dis      = (magic_d >> NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT) & 1;
    uint32_t boot_gen23_dis= rreg32(ctx, NV_FUSE_OPT_PCIE_BOOT_GEN23_DIS) & 1;
    uint32_t boot_gen3_dis = rreg32(ctx, NV_FUSE_OPT_PCIE_BOOT_GEN3_DIS) & 1;
    uint32_t pcie_lane_dis = rreg32(ctx, NV_FUSE_OPT_PCIE_LANE_DISABLE) & 0x3;

    printf("\nPCIe:\n");
    printf("  OPT_DISABLE_GEN3_SPEED      = %u  (%s)\n",
           gen3_dis, gen3_dis ? "Gen3 DISABLED" : "Gen3 allowed");
    printf("  MAGIC_BITS_D Gen4 disable   = %u  (%s)\n",
           gen4_dis, gen4_dis ? "Gen4 DISABLED" : "Gen4 allowed");
    printf("  OPT_PCIE_BOOT_GEN23_DISABLE = %u  (%s)\n",
           boot_gen23_dis, boot_gen23_dis ? "Gen2/3 boot DISABLED" : "Gen2/3 boot allowed");
    printf("  OPT_PCIE_BOOT_GEN3_DISABLE  = %u  (%s)\n",
           boot_gen3_dis, boot_gen3_dis ? "Gen3 boot DISABLED" : "Gen3 boot allowed");
    printf("  PCIE_MAGIC_BITS_D (raw)     = 0x%08X\n", magic_d);
    printf("  OPT_PCIE_LANE_DISABLE       = 0x%X (%d lanes disabled)\n",
           pcie_lane_dis, countbits(pcie_lane_dis));

    /* NVLink */
    uint32_t nvl_dis    = rreg32(ctx, NV_FUSE_OPT_NVLINK_DISABLE) & 0x7;
    uint32_t nvl_ctrl   = rreg32(ctx, NV_FUSE_CTRL_OPT_NVLINK) & 0xFFFF;
    uint32_t nvl_status = rreg32(ctx, NV_FUSE_STATUS_OPT_NVLINK) & 0xFFFF;

    printf("\nNVLink:\n");
    printf("  OPT_NVLINK_DISABLE  = 0x%X  (%s)\n",
           nvl_dis, nvl_dis == 0x7 ? "ALL NVLink DISABLED" :
                    nvl_dis == 0   ? "all NVLink enabled" : "partial");
    printf("  CTRL_OPT_NVLINK     = 0x%04X\n", nvl_ctrl);
    printf("  STATUS_OPT_NVLINK   = 0x%04X\n", nvl_status);

    /* ECC */
    uint32_t ecc_en     = rreg32(ctx, NV_FUSE_OPT_ECC_EN) & 1;
    uint32_t ecc_ovr    = rreg32(ctx, NV_FUSE_FEATURE_OVERRIDE_ECC);

    printf("\nECC:\n");
    printf("  OPT_ECC_EN              = %u  (%s)\n",
           ecc_en, ecc_en ? "ECC ENABLED" : "ECC disabled");
    printf("  FEATURE_OVERRIDE_ECC    = 0x%08X\n", ecc_ovr);
}

/* ========================================================================
 * RIR record encoding / decoding (from rirencoder.cpp)
 *
 * TSMC TEF16FGL fuse macro, 16-bit RIR record:
 *   [15]    Disable
 *   [14:10] Addr[4:0]
 *   [9:8]   Addr[6:5]
 *   [7:2]   Addr[12:7]
 *   [1]     Data (value the repaired bit reads as)
 *   [0]     Enable
 * ======================================================================== */
static uint16_t rir_encode(uint32_t abs_bit, int data_val)
{
    if (abs_bit > 0x3FFF) {
        fprintf(stderr, "ERROR: bit address 0x%X > 14-bit max (512 rows x 32)\n", abs_bit);
        return 0;
    }
    /* GA100 RIR record is LINEAR: [0]en [1]data [15:2]addr(14-bit).
     * Verified by three-card cross-reference (ga100-fuse-re + local 2082 dump):
     * factory record 0x0923 decodes to row18:bit8, matching observed
     * row18/row19 twin difference. See lixiangwuxian/170hx-rir-tool#1. */
    return (uint16_t)((abs_bit << 2) | ((data_val ? 1u : 0u) << 1) | 1u);
}

static void rir_decode(uint16_t rec, uint32_t *abs_bit, int *data,
                        int *enabled, int *disabled)
{
    *enabled  = rec & 1;
    *data     = (rec >> 1) & 1;
    *disabled = 0; /* GA100 has no disable bit; bit15 is addr[13] */
    *abs_bit  = (rec >> 2) & 0x3FFF;
}

/* ========================================================================
 * RIR dump
 * ======================================================================== */
static int rir_read_all(gpu_ctx_t *ctx, uint32_t rows[NUM_RIR_ROWS])
{
    wreg32(ctx, NV_FUSE_FUSECTRL, 0);
    uint32_t dc = rreg32(ctx, NV_FUSE_DEBUGCTRL);
    wreg32(ctx, NV_FUSE_DEBUGCTRL, dc | NV_FUSE_DEBUGCTRL_RWL_BIT);

    uint32_t dc_check = rreg32(ctx, NV_FUSE_DEBUGCTRL);
    if (!(dc_check & NV_FUSE_DEBUGCTRL_RWL_BIT)) {
        fprintf(stderr, "ABORT: DEBUGCTRL RWL bit did not latch (0x%08X). "
                        "Fuse controller access may be blocked.\n", dc_check);
        return -1;
    }

    for (int i = 0; i < NUM_RIR_ROWS; i++) {
        wreg32(ctx, NV_FUSE_FUSEADDR, i);
        wreg32(ctx, NV_FUSE_FUSECTRL, NV_FUSE_FUSECTRL_CMD_READ);
        if (poll_fuses_idle(ctx, 1000)) {
            wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);
            return -1;
        }
        rows[i] = rreg32(ctx, NV_FUSE_FUSERDATA);
    }

    wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);
    return 0;
}

static int cmd_dump_rir(gpu_ctx_t *ctx)
{
    uint32_t rows[NUM_RIR_ROWS];
    if (rir_read_all(ctx, rows)) return -1;

    printf("\n=== RIR State (%d rows, %d record slots) ===\n\n",
           NUM_RIR_ROWS, NUM_RIR_ROWS * 2);
    printf("Raw rows:\n");
    for (int i = 0; i < NUM_RIR_ROWS; i++)
        printf("  Row %d: 0x%08X\n", i, rows[i]);

    printf("\nDecoded:\n");
    printf("  Slot | Raw    | En | Dis | Data | AbsBit | Row:Bit    | Note\n");
    printf("  -----+--------+----+-----+------+--------+------------+-----\n");

    int used = 0;
    for (int r = 0; r < NUM_RIR_ROWS; r++) {
        for (int h = 0; h < 2; h++) {
            uint16_t rec = h ? (rows[r] >> 16) : (rows[r] & 0xFFFF);
            int slot = r * 2 + h;
            uint32_t ab; int data, en, dis;
            rir_decode(rec, &ab, &data, &en, &dis);
            const char *note = "";
            if (!en && rec == 0) note = "(empty)";
            else if (en && !dis) { note = "ACTIVE"; used++; }
            else if (dis) note = "(disabled)";
            printf("  %4d | 0x%04X | %2d | %3d | %4d | %6u | %3u:%-2u     | %s\n",
                   slot, rec, en, dis, data, ab, ab/32, ab%32, note);
        }
    }
    printf("\n  %d/%d slots in use, %d free.\n", used, NUM_RIR_ROWS, NUM_RIR_ROWS * 2 - used);
    return 0;
}

/* ========================================================================
 * Dump main fuse rows
 * ======================================================================== */
static int cmd_dump_fuses(gpu_ctx_t *ctx, int count)
{
    if (count > NUM_MAIN_FUSE_ROWS_GA100) count = NUM_MAIN_FUSE_ROWS_GA100;
    printf("\n=== Main Fuse Macro (showing non-zero rows, max %d) ===\n", count);

    /* Ensure RWL off */
    uint32_t dc = rreg32(ctx, NV_FUSE_DEBUGCTRL);
    wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);

    int nz = 0;
    for (int r = 0; r < count; r++) {
        uint32_t v;
        if (fuse_read_row(ctx, r, &v)) {
            printf("  Row %3d: READ ERROR\n", r);
            continue;
        }
        if (v) { printf("  Row %3d: 0x%08X\n", r, v); nz++; }
    }
    printf("\n  %d non-zero / %d rows.\n", nz, count);
    return 0;
}

/* ========================================================================
 * SW Override operations
 *
 * SW Override lets you change OPT fuse values via register writes.
 * Volatile — lost on GPU reset / power cycle. Safe for testing.
 *
 * Flow:
 *   1. Check disable_sw_override fuse == 0 (otherwise permanently locked)
 *   2. Write NV_FUSE_EN_SW_OVERRIDE = 1 (enable override mode)
 *   3. Write to _CP registers (e.g. OPT_FBP_DISABLE_CP)
 *   4. Values take effect immediately
 *   5. To revert: power cycle or write NV_FUSE_EN_SW_OVERRIDE = 0
 * ======================================================================== */

static int sw_override_check(gpu_ctx_t *ctx)
{
    uint32_t locked = rreg32(ctx, NV_FUSE_DISABLE_SW_OVERRIDE_STATUS) & 1;
    if (locked) {
        fprintf(stderr, "ERROR: SW override is permanently disabled by fuse.\n");
        fprintf(stderr, "       Cannot use SW override on this GPU.\n");
        return -1;
    }
    return 0;
}

static int cmd_sw_override_enable(gpu_ctx_t *ctx)
{
    if (sw_override_check(ctx)) return -1;

    printf("[INFO] Enabling SW Override...\n");
    wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_ENABLE);
    usleep(1000);

    uint32_t readback = rreg32(ctx, NV_FUSE_EN_SW_OVERRIDE);
    if (readback & 1) {
        printf("[OK] SW Override is now ACTIVE.\n");
        printf("     You can now write to _CP registers to override fuse values.\n");
        printf("     This is VOLATILE — lost on power cycle.\n");
        return 0;
    } else {
        fprintf(stderr, "[FAIL] SW Override did not enable (readback=0x%X)\n", readback);
        return -1;
    }
}

static int cmd_sw_override_disable(gpu_ctx_t *ctx)
{
    wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_DISABLE);
    printf("[OK] SW Override disabled.\n");
    return 0;
}

/* SW override: set a specific _CP register value */
static int cmd_sw_override_write(gpu_ctx_t *ctx, const char *name, uint32_t val)
{
    struct { const char *name; uint32_t addr; uint32_t ro_addr; int bits; const char *desc; } regs[] = {
        {"fbp",     NV_FUSE_OPT_FBP_DISABLE_CP,   NV_FUSE_OPT_FBP_DISABLE,       12, "FBP disable mask"},
        {"fbpa",    NV_FUSE_OPT_FBPA_DISABLE_CP,   NV_FUSE_OPT_FBPA_DISABLE,      24, "FBPA disable mask"},
        {"fbio",    NV_FUSE_OPT_FBIO_DISABLE_CP,   NV_FUSE_OPT_FBIO_DISABLE,      24, "FBIO disable mask"},
        {"gpc",     NV_FUSE_OPT_GPC_DISABLE_CP,    NV_FUSE_OPT_GPC_DISABLE,        8, "GPC disable mask"},
        {"l2",      NV_FUSE_OPT_ROP_L2_DISABLE_CP, NV_FUSE_OPT_ROP_L2_DISABLE,    24, "ROP/L2 disable mask"},
        {"nvlink",  NV_FUSE_OPT_NVLINK_DISABLE_CP, NV_FUSE_OPT_NVLINK_DISABLE,     3, "NVLink disable (0=all enabled, 7=all disabled)"},
        {"pcie",    NV_FUSE_OPT_PCIE_LANE_DISABLE_CP, NV_FUSE_OPT_PCIE_LANE_DISABLE, 2, "PCIe lane disable"},
        {"gen3",    NV_FUSE_OPT_DISABLE_GEN3_SPEED, NV_FUSE_OPT_DISABLE_GEN3_SPEED, 1, "Gen3 speed disable (0=Gen3 ok)"},
        {"ecc",     NV_FUSE_OPT_ECC_EN,            NV_FUSE_OPT_ECC_EN,              1, "ECC enable (1=ECC on)"},
        {NULL, 0, 0, 0, NULL}
    };

    /* Gen4 is special: bit 25 inside PCIE_MAGIC_BITS_D, needs read-modify-write */
    if (strcasecmp(name, "gen4") == 0) {
        if (sw_override_check(ctx)) return -1;
        uint32_t sw_en = rreg32(ctx, NV_FUSE_EN_SW_OVERRIDE) & 1;
        uint32_t cur = rreg32(ctx, NV_FUSE_OPT_PCIE_MAGIC_BITS_D);
        uint32_t cur_bit = (cur >> NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT) & 1;
        uint32_t new_bit = val & 1;

        printf("[INFO] gen4 (Gen4 speed disable, bit 25 in PCIE_MAGIC_BITS_D):\n");
        printf("  MAGIC_BITS_D raw  : 0x%08X\n", cur);
        printf("  Gen4 disable bit  : %u (%s)\n", cur_bit,
               cur_bit ? "Gen4 DISABLED" : "Gen4 allowed");
        printf("  New value         : %u (%s)\n", new_bit,
               new_bit ? "will DISABLE Gen4" : "will ALLOW Gen4");
        printf("  SW override       : %s\n", sw_en ? "ACTIVE" : "inactive");

        if (!sw_en) {
            printf("\n[INFO] SW Override not yet active. Enabling...\n");
            wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_ENABLE);
            usleep(1000);
        }

        printf("\nThis is VOLATILE — will be lost on power cycle.\n");
        printf("Proceed? (yes/no): ");
        fflush(stdout);
        char buf[16] = {0};
        if (!fgets(buf, sizeof(buf), stdin) || strncmp(buf, "yes", 3)) {
            printf("Aborted.\n");
            if (!sw_en) wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_DISABLE);
            return -1;
        }

        uint32_t new_val = cur;
        if (new_bit)
            new_val |= (1u << NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT);
        else
            new_val &= ~(1u << NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT);

        wreg32(ctx, NV_FUSE_OPT_PCIE_MAGIC_BITS_D, new_val);
        usleep(1000);
        uint32_t rb = rreg32(ctx, NV_FUSE_OPT_PCIE_MAGIC_BITS_D);
        uint32_t rb_bit = (rb >> NV_FUSE_OPT_PCIE_MAGIC_BITS_D_GEN4_BIT) & 1;
        printf("[VERIFY] Gen4 disable bit now = %u (%s)\n",
               rb_bit, rb_bit ? "Gen4 DISABLED" : "Gen4 allowed");
        return 0;
    }

    for (int i = 0; regs[i].name; i++) {
        if (strcasecmp(name, regs[i].name) == 0) {
            if (sw_override_check(ctx)) return -1;

            uint32_t mask = (1u << regs[i].bits) - 1;
            val &= mask;

            uint32_t cur_hw  = rreg32(ctx, regs[i].ro_addr) & mask;
            uint32_t sw_en   = rreg32(ctx, NV_FUSE_EN_SW_OVERRIDE) & 1;

            printf("[INFO] %s (%s):\n", regs[i].name, regs[i].desc);
            printf("  HW fuse value : 0x%0*X\n", regs[i].bits/4, cur_hw);
            printf("  SW override   : %s\n", sw_en ? "ACTIVE" : "inactive");
            printf("  New CP value  : 0x%0*X\n", regs[i].bits/4, val);
            printf("\n  Bits that will be NEWLY DISABLED: 0x%0*X\n",
                   regs[i].bits/4, val & ~cur_hw);
            printf("  Bits that will be NEWLY ENABLED:  0x%0*X\n",
                   regs[i].bits/4, cur_hw & ~val);

            if (!sw_en) {
                printf("\n[INFO] SW Override not yet active. Enabling...\n");
                wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_ENABLE);
                usleep(1000);
            }

            printf("\nThis is VOLATILE — will be lost on power cycle.\n");
            printf("Proceed? (yes/no): ");
            fflush(stdout);
            char buf[16] = {0};
            if (!fgets(buf, sizeof(buf), stdin) || strncmp(buf, "yes", 3)) {
                printf("Aborted.\n");
                if (!sw_en) wreg32(ctx, NV_FUSE_EN_SW_OVERRIDE, NV_FUSE_EN_SW_OVERRIDE_VAL_DISABLE);
                return -1;
            }

            wreg32(ctx, regs[i].addr, val);
            usleep(1000);
            uint32_t rb = rreg32(ctx, regs[i].ro_addr) & mask;
            printf("[VERIFY] OPT_%s_DISABLE now reads: 0x%0*X\n",
                   regs[i].name, regs[i].bits/4, rb);
            return 0;
        }
    }

    fprintf(stderr, "Unknown register name '%s'.\n"
                    "  Available: fbp, fbpa, fbio, gpc, l2, nvlink, pcie, gen3, gen4, ecc\n", name);
    return -1;
}

/* ========================================================================
 * RIR write (with safety protections)
 * ======================================================================== */
static void save_backup(gpu_ctx_t *ctx, const char *tag)
{
    char fname[128];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(fname, sizeof(fname),
             "rir_backup_%04d%02d%02d_%02d%02d%02d_%s.bin",
             tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tag);

    FILE *f = fopen(fname, "wb");
    if (!f) {
        fprintf(stderr, "[WARN] Could not create backup file %s\n", fname);
        return;
    }
    fwrite(ctx->rir_backup, sizeof(ctx->rir_backup), 1, f);
    fclose(f);
    printf("[BACKUP] Saved to %s\n", fname);
}

static int cmd_write_rir(gpu_ctx_t *ctx, uint32_t row, uint32_t value)
{
    if (row >= NUM_RIR_ROWS) {
        fprintf(stderr, "ERROR: row %u out of range (0-%d)\n", row, NUM_RIR_ROWS-1);
        return -1;
    }

    /* Step 1: Read current state and create backup */
    uint32_t rows[NUM_RIR_ROWS];
    if (rir_read_all(ctx, rows)) return -1;
    memcpy(ctx->rir_backup, rows, sizeof(rows));
    ctx->rir_backup_valid = 1;

    uint32_t current = rows[row];

    /* Step 1b: Validate slots are blank (CRITICAL safety check) */
    uint16_t cur_lo = current & 0xFFFF;
    uint16_t cur_hi = current >> 16;
    uint16_t req_lo = value & 0xFFFF;
    uint16_t req_hi = value >> 16;

    if ((cur_lo != 0 && cur_lo != req_lo) || (cur_hi != 0 && cur_hi != req_hi)) {
        fprintf(stderr, "ABORT: RIR row %u already has data (0x%08X).\n", row, current);
        fprintf(stderr, "  Slot %d: 0x%04X (must be 0x0000 or match 0x%04X)\n",
                row*2, cur_lo, req_lo);
        fprintf(stderr, "  Slot %d: 0x%04X (must be 0x0000 or match 0x%04X)\n",
                row*2+1, cur_hi, req_hi);
        fprintf(stderr, "  Merging into non-blank slots would create incorrect records.\n");
        fprintf(stderr, "  If you REALLY want to do this, comment out 713-722 lines in ga100_rir.c and rebuild to continue\n");
        return -1;
    }

    uint32_t new_bits = value & ~current;

    if (new_bits == 0) {
        printf("[INFO] No new bits to burn. Current=0x%08X contains all requested bits.\n", current);
        return 0;
    }

    uint32_t final_val = current | new_bits;

    /* Step 2: Show detailed change summary */
    printf("\n=== RIR WRITE PLAN ===\n\n");
    printf("  Target row : %u\n", row);
    printf("  Current    : 0x%08X\n", current);
    printf("  Requested  : 0x%08X\n", value);
    printf("  New bits   : 0x%08X (only these will be burned)\n", new_bits);
    printf("  Final      : 0x%08X\n", final_val);

    /* Decode what the new records will do */
    printf("\n  Records after write:\n");
    for (int h = 0; h < 2; h++) {
        uint16_t rec = h ? (final_val >> 16) : (final_val & 0xFFFF);
        uint32_t ab; int data, en, dis;
        rir_decode(rec, &ab, &data, &en, &dis);
        printf("    Slot %d: 0x%04X -> bit %u (row %u:%u) data=%d en=%d\n",
               row * 2 + h, rec, ab, ab/32, ab%32, data, en);
    }

    printf("\n  All current RIR rows:\n");
    for (int i = 0; i < NUM_RIR_ROWS; i++)
        printf("    Row %d: 0x%08X %s\n", i, rows[i], i == (int)row ? "<-- TARGET" : "");

    /* Step 3: Save backup file */
    save_backup(ctx, "pre_write");

    /* Step 4: Multi-stage confirmation */
    printf("\n*** THIS OPERATION IS IRREVERSIBLE ***\n");
    printf("*** Incorrect RIR values can BRICK the GPU ***\n\n");

    printf("Confirm step 1/2 - type the row number to write (%u): ", row);
    fflush(stdout);
    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), stdin) || strtoul(buf, NULL, 10) != row) {
        printf("Aborted (wrong row number).\n");
        return -1;
    }

    printf("Confirm step 2/2 - type 'BURN' to proceed: ");
    fflush(stdout);
    buf[0] = 0;
    if (!fgets(buf, sizeof(buf), stdin) || strncmp(buf, "BURN", 4) != 0) {
        printf("Aborted.\n");
        return -1;
    }

    /* Step 5: Execute write */
    printf("\n[EXEC] Writing RIR row %u...\n", row);

    uint32_t dc = rreg32(ctx, NV_FUSE_DEBUGCTRL);
    wreg32(ctx, NV_FUSE_FUSECTRL, 0);
    wreg32(ctx, NV_FUSE_DEBUGCTRL, dc | NV_FUSE_DEBUGCTRL_RWL_BIT);

    /* RWL read-back guard: abort if RWL didn't latch */
    uint32_t dc_check = rreg32(ctx, NV_FUSE_DEBUGCTRL);
    if (!(dc_check & NV_FUSE_DEBUGCTRL_RWL_BIT)) {
        fprintf(stderr, "ABORT: DEBUGCTRL RWL bit did not latch (0x%08X). "
                        "Fuse controller access may be blocked.\n", dc_check);
        return -1;
    }

    if (poll_fuses_idle(ctx, 2000)) {
        wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);
        return -1;
    }

    wreg32(ctx, NV_FUSE_FUSEADDR, row);
    wreg32(ctx, NV_FUSE_FUSEWDATA, new_bits);
    wreg32(ctx, NV_FUSE_FUSECTRL, NV_FUSE_FUSECTRL_CMD_WRITE);

    if (poll_fuses_idle(ctx, 5000)) {
        fprintf(stderr, "[ERROR] Write did not complete!\n");
        wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);
        return -1;
    }

    wreg32(ctx, NV_FUSE_DEBUGCTRL, dc & ~NV_FUSE_DEBUGCTRL_RWL_BIT);

    /* Step 6: Verify */
    uint32_t verify[NUM_RIR_ROWS];
    if (rir_read_all(ctx, verify)) {
        fprintf(stderr, "[ERROR] Verify read failed\n");
        return -1;
    }
    if (verify[row] != final_val) {
        fprintf(stderr, "[VERIFY FAIL] Expected 0x%08X, got 0x%08X\n",
                final_val, verify[row]);
        fprintf(stderr, "  RIR bits fuse 0->1 only; re-check the encoded\n"
                        "  value and retry (re-writing is harmless).\n");
        return -1;
    }
    printf("[VERIFY OK] Row %u = 0x%08X\n", row, verify[row]);
    /* post_write backup must capture the POST-write state; without this
     * copy save_backup() re-dumps the stale pre-write ctx->rir_backup. */
    memcpy(ctx->rir_backup, verify, sizeof(verify));
    save_backup(ctx, "post_write");
    return 0;
}

/* ========================================================================
 * RIR encode helper
 * ======================================================================== */
static void cmd_encode_rir(uint32_t abs_bit)
{
    printf("\n=== RIR Record Encoding ===\n");
    printf("Target: absolute bit %u  (fuse row %u, bit %u)\n",
           abs_bit, abs_bit / 32, abs_bit % 32);

    uint16_t r0 = rir_encode(abs_bit, 0);
    uint16_t r1 = rir_encode(abs_bit, 1);
    printf("  To force bit → 0 (repair/clear): record = 0x%04X\n", r0);
    printf("  To force bit → 1 (repair/set)  : record = 0x%04X\n", r1);

    /* Show placement options */
    printf("\nPlacement (pick an empty slot):\n");
    printf("  Slot 0 (row 0 low):  --write-rir 0 0x%08X\n", (uint32_t)r0);
    printf("  Slot 1 (row 0 high): --write-rir 0 0x%08X\n", (uint32_t)r0 << 16);
    printf("  Slot 2 (row 1 low):  --write-rir 1 0x%08X\n", (uint32_t)r0);
    printf("  ... (use OR to combine with existing row value)\n");
}

/* ========================================================================
 * Main
 * ======================================================================== */
static void usage(const char *prog)
{
    printf("ga100_rir — GA100 RIR / Fuse inspection tool\n\n");
    printf("Usage: sudo %s <pci_bdf> [command] [args...]\n\n", prog);
    printf("Commands:\n");
    printf("  (none)                     Show HBM/floorsweep status + RIR state\n");
    printf("  --status                   Show HBM stack / floorsweep status\n");
    printf("  --dump-rir                 Dump RIR records\n");
    printf("  --dump-fuses [N]           Dump main fuse rows (default 64)\n");
    printf("\n");
    printf("  --sw-enable                Enable SW override (volatile)\n");
    printf("  --sw-disable               Disable SW override\n");
    printf("  --sw-write <reg> <val>     Write SW override to _CP register\n");
    printf("                             reg: fbp, fbpa, fbio, gpc, l2,\n");
    printf("                                  nvlink, pcie, gen3, gen4, ecc\n");
    printf("\n");
    printf("  --encode-rir <abs_bit>     Encode RIR record for a fuse bit\n");
    printf("  --write-rir <row> <val>    Write RIR row (IRREVERSIBLE)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  sudo %s 0000:41:00.0                          # show status\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-enable              # enable SW override\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-write gen3 0        # unlock PCIe Gen3\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-write nvlink 0      # unlock NVLink\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-write ecc 1         # enable ECC\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-write gpc 0         # unlock all GPC/SM\n", prog);
    printf("  sudo %s 0000:41:00.0 --sw-write fbp 0         # unlock all HBM\n", prog);
    printf("  sudo %s 0000:41:00.0 --write-rir 0 0x00950015           # unlock SW override\n", prog);
    printf("\n");
    printf("Prerequisite: unbind NVIDIA driver first:\n");
    printf("  echo <bdf> > /sys/bus/pci/devices/<bdf>/driver/unbind\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    gpu_ctx_t ctx;
    if (gpu_open(&ctx, argv[1])) return 1;

    int ret = 0;

    if (argc == 2) {
        cmd_status(&ctx);
        ret = cmd_dump_rir(&ctx);
    }
    else if (!strcmp(argv[2], "--status"))
        cmd_status(&ctx);
    else if (!strcmp(argv[2], "--dump-rir"))
        ret = cmd_dump_rir(&ctx);
    else if (!strcmp(argv[2], "--dump-fuses"))
        ret = cmd_dump_fuses(&ctx, argc > 3 ? atoi(argv[3]) : 64);
    else if (!strcmp(argv[2], "--sw-enable"))
        ret = cmd_sw_override_enable(&ctx);
    else if (!strcmp(argv[2], "--sw-disable"))
        ret = cmd_sw_override_disable(&ctx);
    else if (!strcmp(argv[2], "--sw-write")) {
        if (argc < 5) { fprintf(stderr, "Need: --sw-write <reg> <val>\n"); ret = 1; }
        else ret = cmd_sw_override_write(&ctx, argv[3], strtoul(argv[4], NULL, 0));
    }
    else if (!strcmp(argv[2], "--encode-rir")) {
        if (argc < 4) { fprintf(stderr, "Need: --encode-rir <abs_bit>\n"); ret = 1; }
        else cmd_encode_rir(strtoul(argv[3], NULL, 0));
    }
    else if (!strcmp(argv[2], "--write-rir")) {
        if (argc < 5) { fprintf(stderr, "Need: --write-rir <row> <val>\n"); ret = 1; }
        else ret = cmd_write_rir(&ctx, strtoul(argv[3], NULL, 0),
                                  strtoul(argv[4], NULL, 0));
    }
    else { fprintf(stderr, "Unknown: %s\n", argv[2]); usage(argv[0]); ret = 1; }

    gpu_close(&ctx);
    return ret;
}

/*
 * elishpcie — подъём PCIe0 (QCA6390 Wi-Fi) на Xiaomi Pad 5 Pro (SM8250) без PEP Qualcomm.
 *
 * Не-PnP WDM-драйвер, запуск по требованию (sc start elishpcie), выгружаемый (sc stop).
 * Последовательность повторяет Linux: pcie-qcom.c (ops_1_9_0 / 2_7_0), phy-qcom-qmp-pcie.c
 * (sm8250 gen3x1), gcc-sm8250.c, pinctrl-sm8250.c и cnss2 (WLAN_EN).
 *
 * Services\elishpcie\Mode (DWORD, битовая маска):
 *   0x1  только чтение и лог (GCC, TLMM; PARF/PHY/DBI — только если клоки уже включены)
 *   0x2  питание: WLAN_EN (GPIO20), GDSC PCIE_0, RCG и ветви клоков
 *   0x4  сброс и инициализация PHY, PARF, PERST, iATU (ECAM как у DWC), LTSSM, ожидание линка
 *   0x8  не трогать WLAN_EN (если чип уже включён)
 *  0x10  проба ECAM: шина 0 слоты 1..31 и шина 1 через окно 0x60000000 (как будет читать pci.sys)
 * 0x8000 только чтение зарядного блока PM8150B (SID2 0x10..0x16) через SPMI observer
 * 0x4000 батарея: чтение двух bq27z561 (0x55) программным I2C на GPIO36/37 и GPIO28/29, только чтение
 * 0x2000 SMMU: атрибуты bypass для PCIe0 = Normal WB, inner shareable (когерентный DMA под _CCA=1)
 * 0x1000 перезапуск чипа без перезагрузки: PERST+WLAN_EN цикл, LinkUp (сброс RC), восстановление заголовка RC от pci.sys
 * 0x800  снять ASPM и L1SS на линке (чип, затем RC), без перезапуска драйвера Wi-Fi
 * 0x400  только чтение: GICD INTID 832..847 и MSI capability чипа
 * 0x200  перезапуск LTSSM (LTSSM_EN 1->0->1) без сброса RC
 * 0x100  цикл питания чипа: PERST#=0, WLAN_EN 0 -> 1, PERST#=1, ожидание переобучения линка (RC не трогает)
 *  0x80  только чтение PMIC через SPMI observer: SMPS/LDO sid0/sid4 (тип, статус, VSET, режим, EN)
 *  0x20  только чтение APPS SMMU (0x15000000): sCR0, ID, SMR/S2CR, CBAR — есть ли правило для SID PCIe0 0x1c00
 *  0x40  записать правило SMMU для PCIe0: SMR 0x1c00/0x7f -> S2CR -> последний context bank с SCTLR.M=0 (bypass, как
 *        qcom_smmu_cfg_probe в Linux), затем дамп (как 0x20)
 * Лог: \SystemRoot\elishpcie.log
 */
#include <ntddk.h>
#include <stdarg.h>

int __cdecl _vsnprintf(char *Buf, size_t Count, const char *Fmt, va_list Args);
int __cdecl _snprintf(char *Buf, size_t Count, const char *Fmt, ...);

#include "phytbl.h"

#define GCC_PHYS     0x00100000ull
#define GCC_SIZE     0x001F0000
#define TLMMN_PHYS   0x0F900000ull      /* тайл NORTH: GPIO20/21/79/80/81/124 */
#define TLMMN_SIZE   0x00080000
#define PARF_PHYS    0x01C00000ull
#define PARF_SIZE    0x00003000
#define PHY_PHYS     0x01C06000ull
#define PHY_SIZE     0x00001000
#define DBI_PHYS     0x60000000ull
#define DBI_SIZE     0x00002000          /* DBI 0x0..0xF1C, ELBI 0xF20, iATU 0x1000 */
#define CFG_PHYS     0x60100000ull       /* шина 1 через iATU CFG0 */
#define CFG_SIZE     0x00001000
#define MEM_BASE     0x60200000u         /* окно BAR из _CRS PCI0 */
#define MEM_LIMIT    0x61FEFFFFu

/* GCC */
#define GCC_PCIE_0_BCR        0x6b000
#define GCC_PCIE_0_GDSCR      0x6b004
#define GCC_PCIE_0_SLV_Q2A    0x6b010
#define GCC_PCIE_0_SLV_AXI    0x6b014
#define GCC_PCIE_0_MSTR_AXI   0x6b01c
#define GCC_PCIE_0_CFG_AHB    0x6b024
#define GCC_PCIE_0_AUX        0x6b028
#define GCC_PCIE_0_PIPE       0x6b02c
#define GCC_PCIE_0_AUX_RCG    0x6b038
#define GCC_PCIE_0_PHY_BCR    0x6c01c
#define GCC_PCIE_PHY_AUX      0x6f004
#define GCC_PCIE_PHY_REFGEN_RCG 0x6f014
#define GCC_PCIE0_PHY_REFGEN  0x6f02c
#define GCC_PCIE_WIFI_CLKREF  0x8c004
#define GCC_DDRSS_PCIE_SF_TBU 0x8d058
#define GCC_AGGRE_NOC_PCIE_TBU 0x9000c
#define GCC_APCS_CLK_VOTE     0x52008    /* биты 0..5: slv, mstr, cfg, aux, pipe, q2a */

/* PARF */
#define PARF_SYS_CTRL         0x000
#define PARF_PM_CTRL          0x020
#define PARF_PHY_CTRL         0x040
#define PARF_MHI_CLOCK_RESET_CTRL 0x174
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2 0x1a8
#define PARF_LTSSM            0x1b0
#define PARF_STATUS           0x230
#define PARF_DBI_BASE_ADDR_V2 0x350
#define PARF_DBI_BASE_ADDR_V2_HI 0x354
#define PARF_SLV_ADDR_SPACE_SIZE_V2 0x358
#define PARF_SLV_ADDR_SPACE_SIZE_V2_HI 0x35c
#define PARF_BLOCK_SLV_AXI_WR_BASE 0x360
#define PARF_ECAM_BASE        0x380
#define PARF_ECAM_BASE_HI     0x384
#define PARF_ATU_BASE_ADDR    0x634
#define PARF_ATU_BASE_ADDR_HI 0x638
#define PARF_DEVICE_TYPE      0x1000

/* DBI */
#define DBI_PORT_DEBUG1       0x72c
#define DBI_MISC_CONTROL_1    0x8bc
#define DBI_ATU               0x1000     /* unroll: регион i = 0x1000 + (i << 9) */

static PUCHAR gGcc, gTlmm, gParf, gPhy, gDbi, gCfg;
static UNICODE_STRING gRegPath;
static WCHAR gRegBuf[256];

/* ---------------- лог ---------------- */

static VOID
ULog(const char *Fmt, ...)
{
    char              Buf[512];
    int               N, Hdr;
    va_list           Ap;
    UNICODE_STRING    Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            H;
    LARGE_INTEGER     Now, Local;
    TIME_FIELDS       Tf;

    KeQuerySystemTime(&Now);
    ExSystemTimeToLocalTime(&Now, &Local);
    RtlTimeToTimeFields(&Local, &Tf);
    Hdr = _snprintf(Buf, sizeof(Buf), "%02u:%02u:%02u.%03u ", Tf.Hour, Tf.Minute, Tf.Second, Tf.Milliseconds);
    va_start(Ap, Fmt);
    N = _vsnprintf(Buf + Hdr, sizeof(Buf) - Hdr - 3, Fmt, Ap);
    va_end(Ap);
    if (N < 0) N = (int)(sizeof(Buf) - Hdr - 3);
    N += Hdr;
    Buf[N++] = '\r';
    Buf[N++] = '\n';

    RtlInitUnicodeString(&Name, L"\\SystemRoot\\elishpcie.log");
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&H, FILE_APPEND_DATA | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(H, NULL, NULL, NULL, &Iosb, Buf, (ULONG)N, NULL, NULL);
        ZwClose(H);
    }
}

/* ---------------- регистры ---------------- */

static __forceinline ULONG Rd(PUCHAR B, ULONG O) { return READ_REGISTER_ULONG((volatile ULONG *)(B + O)); }
static __forceinline VOID  Wr(PUCHAR B, ULONG O, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + O), V); }
static __forceinline VOID  Set(PUCHAR B, ULONG O, ULONG M) { Wr(B, O, Rd(B, O) | M); }
static __forceinline VOID  Clr(PUCHAR B, ULONG O, ULONG M) { Wr(B, O, Rd(B, O) & ~M); }

static VOID
SleepMs(ULONG Ms)
{
    LARGE_INTEGER D;
    D.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &D);
}

/* ждать (Rd & Mask) == Want, до TimeoutUs; TRUE если дождались */
static BOOLEAN
Poll(PUCHAR B, ULONG O, ULONG Mask, ULONG Want, ULONG TimeoutUs)
{
    ULONG T;
    for (T = 0; T < TimeoutUs; T += 10) {
        if ((Rd(B, O) & Mask) == Want) return TRUE;
        KeStallExecutionProcessor(10);
    }
    return (Rd(B, O) & Mask) == Want;
}

static PUCHAR
Map(ULONG64 Phys, SIZE_T Size)
{
    PHYSICAL_ADDRESS Pa;
    Pa.QuadPart = (LONGLONG)Phys;
    return (PUCHAR)MmMapIoSpaceEx(Pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

/* ---------------- GPIO (TLMM north) ---------------- */

#define GPIO_CFG(n) ((ULONG)(n) * 0x1000)
#define GPIO_IO(n)  ((ULONG)(n) * 0x1000 + 4)

static VOID
GpioOut(ULONG N, ULONG Func, ULONG Pull, BOOLEAN High)
{
    /* cfg: [1:0] pull, [5:2] func, [8:6] drive (0 = 2 мА), bit9 OE; io: bit1 out */
    ULONG Cfg = (Pull & 3) | ((Func & 0xF) << 2) | (1u << 9);

    /* WLAN_EN (GPIO20): Linux cnss_wlan_en_active — gpio, pull-up, drive 16 мА (0x3C3);
     * inactive остаётся 0x201 (pull-down, 2 мА), как и было. Другие GPIO не трогаем. */
    if (N == 20 && High) Cfg = 0x3C3u;

    Wr(gTlmm, GPIO_IO(N), High ? 2 : 0);
    Wr(gTlmm, GPIO_CFG(N), Cfg);
    Wr(gTlmm, GPIO_IO(N), High ? 2 : 0);
}

static VOID
GpioIn(ULONG N, ULONG Func, ULONG Pull)
{
    Wr(gTlmm, GPIO_CFG(N), (Pull & 3) | ((Func & 0xF) << 2));
}

/* ---------------- снимки ---------------- */

static BOOLEAN
ClocksOn(VOID)
{
    /* cfg_ahb, aux, slv/mstr axi — без них чтение PARF/DBI повесит шину */
    return !(Rd(gGcc, GCC_PCIE_0_CFG_AHB) >> 31) && !(Rd(gGcc, GCC_PCIE_0_AUX) >> 31) &&
           !(Rd(gGcc, GCC_PCIE_0_SLV_AXI) >> 31) && !(Rd(gGcc, GCC_PCIE_0_MSTR_AXI) >> 31) &&
           (Rd(gGcc, GCC_PCIE_0_GDSCR) >> 31);
}

static VOID
DumpGcc(const char *Tag)
{
    ULONG G[] = { GCC_PCIE_0_GDSCR, GCC_APCS_CLK_VOTE, GCC_PCIE_0_SLV_Q2A, GCC_PCIE_0_SLV_AXI, GCC_PCIE_0_MSTR_AXI,
                  GCC_PCIE_0_CFG_AHB, GCC_PCIE_0_AUX, GCC_PCIE_0_PIPE, GCC_PCIE_PHY_AUX, GCC_PCIE0_PHY_REFGEN,
                  GCC_PCIE_WIFI_CLKREF, GCC_DDRSS_PCIE_SF_TBU, GCC_AGGRE_NOC_PCIE_TBU, GCC_PCIE_0_AUX_RCG,
                  GCC_PCIE_0_AUX_RCG + 4, GCC_PCIE_PHY_REFGEN_RCG, GCC_PCIE_PHY_REFGEN_RCG + 4, GCC_PCIE_0_BCR,
                  GCC_PCIE_0_PHY_BCR };
    char  L[400];
    int   P = 0;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(G) && P < (int)sizeof(L) - 20; i++) {
        P += _snprintf(L + P, sizeof(L) - P, " %05x=%08x", G[i], Rd(gGcc, G[i]));
    }
    ULog("%s gcc:%s", Tag, L);
    ULog("%s gpio20 %08x/%08x 21 %08x/%08x 79 %08x/%08x 80 %08x/%08x 81 %08x/%08x 124 %08x/%08x", Tag,
         Rd(gTlmm, GPIO_CFG(20)), Rd(gTlmm, GPIO_IO(20)), Rd(gTlmm, GPIO_CFG(21)), Rd(gTlmm, GPIO_IO(21)),
         Rd(gTlmm, GPIO_CFG(79)), Rd(gTlmm, GPIO_IO(79)), Rd(gTlmm, GPIO_CFG(80)), Rd(gTlmm, GPIO_IO(80)),
         Rd(gTlmm, GPIO_CFG(81)), Rd(gTlmm, GPIO_IO(81)), Rd(gTlmm, GPIO_CFG(124)), Rd(gTlmm, GPIO_IO(124)));
}

static VOID
DumpCtl(const char *Tag)
{
    if (!ClocksOn()) {
        ULog("%s клоки PCIe0 выключены — PARF/PHY/DBI не читаю", Tag);
        return;
    }
    ULog("%s parf: sys %08x pm %08x phy %08x ltssm %08x status %08x dbi %08x atu %08x ecam %08x type %08x", Tag,
         Rd(gParf, PARF_SYS_CTRL), Rd(gParf, PARF_PM_CTRL), Rd(gParf, PARF_PHY_CTRL), Rd(gParf, PARF_LTSSM),
         Rd(gParf, PARF_STATUS), Rd(gParf, PARF_DBI_BASE_ADDR_V2), Rd(gParf, PARF_ATU_BASE_ADDR),
         Rd(gParf, PARF_ECAM_BASE), Rd(gParf, PARF_DEVICE_TYPE));
    ULog("%s phy: swrst %08x start %08x pwrdn %08x status1 %08x", Tag, Rd(gPhy, PHY_PCS_SW_RESET),
         Rd(gPhy, PHY_PCS_START_CONTROL), Rd(gPhy, PHY_PCS_POWER_DOWN_CONTROL), Rd(gPhy, PHY_PCS_PCS_STATUS1));
    if (Rd(gParf, PARF_DEVICE_TYPE) == 4 && !(Rd(gParf, PARF_PHY_CTRL) & 1)) {
        ULog("%s dbi: id %08x cmd %08x bus %08x lnkcap %08x lnksta %08x dbg1 %08x atu0 %08x/%08x atu1 %08x/%08x",
             Tag, Rd(gDbi, 0), Rd(gDbi, 4), Rd(gDbi, 0x18), Rd(gDbi, 0x7c), Rd(gDbi, 0x80), Rd(gDbi, DBI_PORT_DEBUG1),
             Rd(gDbi, DBI_ATU + 0), Rd(gDbi, DBI_ATU + 4), Rd(gDbi, DBI_ATU + 0x200), Rd(gDbi, DBI_ATU + 0x204));
    }
}

/* ---------------- подъём ---------------- */

static NTSTATUS
PowerUp(ULONG Mode)
{
    ULONG i;
    static const ULONG Branch[] = { GCC_PCIE_PHY_AUX, GCC_PCIE_WIFI_CLKREF, GCC_PCIE0_PHY_REFGEN,
                                    GCC_DDRSS_PCIE_SF_TBU, GCC_AGGRE_NOC_PCIE_TBU };

    /* PERST# (GPIO79) — держим в сбросе; CLKREQ (GPIO80) — функция pci_e0, pull-up; WAKE (81) — вход, pull-up */
    GpioOut(79, 0, 1, FALSE);
    GpioIn(80, 1, 3);
    GpioIn(81, 0, 3);

    if (!(Mode & 8)) {
        /* cnss2: регуляторы уже включены (S4/S5/S6, S8a), поднимаем WLAN_EN и ждём PMU чипа */
        GpioOut(20, 0, 1, TRUE);
        SleepMs(100);
        ULog("WLAN_EN=1, sw_ctrl(124) io %08x", Rd(gTlmm, GPIO_IO(124)));
    }

    /* GDSC PCIE_0: снять SW_COLLAPSE, дождаться PWR_ON */
    Clr(gGcc, GCC_PCIE_0_GDSCR, 1);
    if (!Poll(gGcc, GCC_PCIE_0_GDSCR, 0x80000000, 0x80000000, 5000)) {
        ULog("GDSC PCIE_0 не включился: %08x", Rd(gGcc, GCC_PCIE_0_GDSCR));
        return STATUS_DEVICE_POWER_FAILURE;
    }

    /* RCG aux = TCXO 19.2 МГц (src 0, div 1 -> поле 1), refgen = GPLL0 600/6 = 100 МГц (src 1, поле 11) */
    Wr(gGcc, GCC_PCIE_0_AUX_RCG + 4, (0u << 8) | 1);
    Wr(gGcc, GCC_PCIE_0_AUX_RCG + 8, 0);
    Wr(gGcc, GCC_PCIE_0_AUX_RCG + 12, 0);
    Wr(gGcc, GCC_PCIE_0_AUX_RCG + 16, 0);
    Set(gGcc, GCC_PCIE_0_AUX_RCG, 1);
    Poll(gGcc, GCC_PCIE_0_AUX_RCG, 1, 0, 1000);
    Wr(gGcc, GCC_PCIE_PHY_REFGEN_RCG + 4, (1u << 8) | 11);
    Set(gGcc, GCC_PCIE_PHY_REFGEN_RCG, 1);
    Poll(gGcc, GCC_PCIE_PHY_REFGEN_RCG, 1, 0, 1000);

    /* ветви: голосование APCS (slv, mstr, cfg, aux, pipe, q2a) + прямые */
    Set(gGcc, GCC_APCS_CLK_VOTE, 0x3F);
    for (i = 0; i < RTL_NUMBER_OF(Branch); i++) {
        Set(gGcc, Branch[i], 1);
    }
    KeStallExecutionProcessor(100);
    for (i = 0; i < RTL_NUMBER_OF(Branch); i++) {
        if (!Poll(gGcc, Branch[i], 0x80000000, 0, 2000)) {
            ULog("ветвь %05x не включилась: %08x", Branch[i], Rd(gGcc, Branch[i]));
        }
    }
    if (!Poll(gGcc, GCC_PCIE_0_CFG_AHB, 0x80000000, 0, 2000) || !Poll(gGcc, GCC_PCIE_0_AUX, 0x80000000, 0, 2000) ||
        !Poll(gGcc, GCC_PCIE_0_SLV_AXI, 0x80000000, 0, 2000) || !Poll(gGcc, GCC_PCIE_0_MSTR_AXI, 0x80000000, 0, 2000)) {
        ULog("клоки контроллера не включились");
        return STATUS_DEVICE_POWER_FAILURE;
    }
    ULog("питание и клоки PCIe0 включены");
    return STATUS_SUCCESS;
}

static VOID
Atu(ULONG Idx, ULONG Type, ULONG Base, ULONG Limit, ULONG Target, ULONG Ctrl2)
{
    ULONG R = DBI_ATU + (Idx << 9);
    Wr(gDbi, R + 0x08, Base);
    Wr(gDbi, R + 0x0C, 0);
    Wr(gDbi, R + 0x10, Limit);
    Wr(gDbi, R + 0x20, 0);
    Wr(gDbi, R + 0x14, Target);
    Wr(gDbi, R + 0x18, 0);
    Wr(gDbi, R + 0x00, Type);
    Wr(gDbi, R + 0x04, 0x80000000u | Ctrl2);
    Poll(gDbi, R + 0x04, 0x80000000u, 0x80000000u, 1000);
}

static NTSTATUS
LinkUp(VOID)
{
    ULONG i, V;

    /* --- init_2_7_0: сброс контроллера, PARF --- */
    Set(gGcc, GCC_PCIE_0_BCR, 1);
    SleepMs(2);
    Clr(gGcc, GCC_PCIE_0_BCR, 1);
    SleepMs(2);

    Wr(gParf, PARF_DEVICE_TYPE, 4);                          /* RC */
    Clr(gParf, PARF_PHY_CTRL, 1);                            /* PHY_TEST_PWR_DOWN */
    Wr(gParf, PARF_DBI_BASE_ADDR_V2, (ULONG)DBI_PHYS);
    Wr(gParf, PARF_DBI_BASE_ADDR_V2_HI, 0);
    Wr(gParf, PARF_ATU_BASE_ADDR, (ULONG)DBI_PHYS + DBI_ATU);
    Wr(gParf, PARF_ATU_BASE_ADDR_HI, 0);
    Wr(gParf, PARF_SLV_ADDR_SPACE_SIZE_V2, 0);
    Wr(gParf, PARF_SLV_ADDR_SPACE_SIZE_V2_HI, 0x80000000u);
    Clr(gParf, PARF_SYS_CTRL, 1u << 29);                     /* MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN */
    Set(gParf, PARF_MHI_CLOCK_RESET_CTRL, 1u << 4);          /* BYPASS */
    Clr(gParf, PARF_PM_CTRL, 1u << 5);                       /* REQ_NOT_ENTR_L1 */
    Set(gParf, PARF_AXI_MSTR_WR_ADDR_HALT_V2, 1u << 31);

    /* --- PHY: сброс BCR, таблицы, старт --- */
    Set(gGcc, GCC_PCIE_0_PHY_BCR, 1);
    SleepMs(1);
    Clr(gGcc, GCC_PCIE_0_PHY_BCR, 1);
    SleepMs(1);
    Set(gPhy, PHY_PCS_POWER_DOWN_CONTROL, 3);                /* SW_PWRDN | REFCLK_DRV_DSBL */
    for (i = 0; i < RTL_NUMBER_OF(kPhyTbl); i++) {
        Wr(gPhy, kPhyTbl[i][0], kPhyTbl[i][1]);
    }
    Clr(gPhy, PHY_PCS_SW_RESET, 1);
    Set(gPhy, PHY_PCS_START_CONTROL, 3);                     /* SERDES_START | PCS_START */
    SleepMs(2);
    if (!Poll(gPhy, PHY_PCS_PCS_STATUS1, 1u << 6, 0, 20000)) {
        ULog("PHY не стартовал: status1 %08x", Rd(gPhy, PHY_PCS_PCS_STATUS1));
        return STATUS_DEVICE_NOT_READY;
    }
    if (!Poll(gGcc, GCC_PCIE_0_PIPE, 0x80000000, 0, 2000)) {
        ULog("pipe clk не пошёл: %08x", Rd(gGcc, GCC_PCIE_0_PIPE));
    }
    ULog("PHY готов: status1 %08x", Rd(gPhy, PHY_PCS_PCS_STATUS1));

    /* --- post_init: ECAM = DBI, блокировщики выключены --- */
    Wr(gParf, PARF_ECAM_BASE, (ULONG)DBI_PHYS);
    Wr(gParf, PARF_ECAM_BASE_HI, 0);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x00, 0);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x04, 0);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x08, 0xFFFFFFFF);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x0C, 0xFFFFFFFF);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x10, 0);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x14, 0);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x18, 0xFFFFFFFF);
    Wr(gParf, PARF_BLOCK_SLV_AXI_WR_BASE + 0x1C, 0xFFFFFFFF);

    /* --- setup_rc (минимум): шины 0/1/1, iATU: CFG0 для шины 1 (shift mode), MEM 1:1 --- */
    Set(gDbi, DBI_MISC_CONTROL_1, 1);
    Wr(gDbi, 0x10, 0);                                       /* BAR0/1 корня не нужны */
    Wr(gDbi, 0x14, 0);
    Wr(gDbi, 0x18, (Rd(gDbi, 0x18) & 0xFF000000u) | 0x00010100u);
    Clr(gDbi, DBI_MISC_CONTROL_1, 1);
    Atu(0, 4, (ULONG)CFG_PHYS, (ULONG)CFG_PHYS + 0xFFFFF, 0, 1u << 28);
    Atu(1, 0, MEM_BASE, MEM_LIMIT, MEM_BASE, 0);
    Wr(gDbi, 4, Rd(gDbi, 4) | 0x6);                          /* MEM | BUS_MASTER */

    /* --- PERST# снять (T_PVPERL 100 мс), LTSSM --- */
    SleepMs(100);
    GpioOut(79, 0, 1, TRUE);
    SleepMs(1);
    Set(gParf, PARF_LTSSM, 1u << 8);

    for (i = 0; i < 100; i++) {
        V = Rd(gDbi, DBI_PORT_DEBUG1);
        if ((V & (1u << 4)) && !(V & (1u << 29))) break;
        SleepMs(10);
    }
    ULog("линк: dbg1 %08x ltssm(parf) %08x lnksta %08x после %u мс", V, Rd(gParf, PARF_LTSSM),
         Rd(gDbi, 0x80), i * 10);
    if (!(V & (1u << 4))) {
        return STATUS_LINK_FAILED;
    }
    SleepMs(100);
    ULog("шина 1, устройство 0: id %08x class %08x bar0 %08x", Rd(gCfg, 0), Rd(gCfg, 8), Rd(gCfg, 0x10));
    return STATUS_SUCCESS;
}

/* ---------------- программный I2C (open-drain через OE) для bq27z561, только чтение ---------------- */
typedef struct { volatile ULONG *Sda, *Scl; } BB_I2C;
#define BB_T 5   /* мкс полупериода, ~80-100 кГц */
static VOID   BbRel(volatile ULONG *P)  { P[0] = 3u | (2u << 6); KeStallExecutionProcessor(BB_T); }          /* вход, pull-up, OE=0 */
static VOID   BbLow(volatile ULONG *P)  { P[1] = 0; P[0] = 3u | (2u << 6) | (1u << 9); KeStallExecutionProcessor(BB_T); }
static ULONG  BbGet(volatile ULONG *P)  { return P[1] & 1; }
static BOOLEAN BbSclHigh(BB_I2C *B)
{
    ULONG i;
    BbRel(B->Scl);
    for (i = 0; i < 2000 && !BbGet(B->Scl); i++) KeStallExecutionProcessor(5);   /* clock stretching до 10 мс */
    return BbGet(B->Scl) != 0;
}
static VOID BbStart(BB_I2C *B) { BbRel(B->Sda); BbSclHigh(B); BbLow(B->Sda); BbLow(B->Scl); }
static VOID BbStop(BB_I2C *B)  { BbLow(B->Sda); BbSclHigh(B); BbRel(B->Sda); }
static BOOLEAN BbTx(BB_I2C *B, UCHAR V)
{
    int i; ULONG Ack;
    for (i = 7; i >= 0; i--) {
        if (V & (1u << i)) BbRel(B->Sda); else BbLow(B->Sda);
        BbSclHigh(B); BbLow(B->Scl);
    }
    BbRel(B->Sda); BbSclHigh(B); Ack = !BbGet(B->Sda); BbLow(B->Scl);
    return (BOOLEAN)Ack;
}
static UCHAR BbRx(BB_I2C *B, BOOLEAN Ack)
{
    int i; UCHAR V = 0;
    BbRel(B->Sda);
    for (i = 7; i >= 0; i--) { BbSclHigh(B); if (BbGet(B->Sda)) V |= (UCHAR)(1u << i); BbLow(B->Scl); }
    if (Ack) BbLow(B->Sda); else BbRel(B->Sda);
    BbSclHigh(B); BbLow(B->Scl); BbRel(B->Sda);
    return V;
}
static BOOLEAN BbReadWord(BB_I2C *B, UCHAR Addr, UCHAR Cmd, USHORT *Out)
{
    UCHAR Lo, Hi;
    BbStart(B);
    if (!BbTx(B, (UCHAR)(Addr << 1))) { BbStop(B); return FALSE; }
    if (!BbTx(B, Cmd)) { BbStop(B); return FALSE; }
    BbStart(B);
    if (!BbTx(B, (UCHAR)((Addr << 1) | 1))) { BbStop(B); return FALSE; }
    Lo = BbRx(B, TRUE); Hi = BbRx(B, FALSE);
    BbStop(B);
    *Out = (USHORT)(Lo | (Hi << 8));
    return TRUE;
}
static VOID
GaugeProbe(const char *Name, ULONG64 SdaPa, ULONG64 SclPa)
{
    static const UCHAR Cmd[] = { 0x2C, 0x08, 0x0C, 0x14, 0x06, 0x0A, 0x10, 0x12, 0x3C };
    static const char *Nm[]  = { "soc%", "mV", "mA", "avgmA", "0.1K", "flags", "rm_mAh", "fcc_mAh", "dc_mAh" };
    PUCHAR Ps = Map(SdaPa, 0x1000), Pc = Map(SclPa, 0x1000);
    BB_I2C B;
    ULONG SvS, SvC, k;
    USHORT V;
    KIRQL Old;
    if (!Ps || !Pc) { ULog("%s: map failed", Name); goto out; }
    B.Sda = (volatile ULONG *)Ps; B.Scl = (volatile ULONG *)Pc;
    SvS = B.Sda[0]; SvC = B.Scl[0];
    ULog("%s: cfg sda %08x scl %08x io %x/%x", Name, SvS, SvC, B.Sda[1], B.Scl[1]);
    BbRel(B.Sda); BbRel(B.Scl); KeStallExecutionProcessor(50);
    if (!BbGet(B.Sda) || !BbGet(B.Scl)) {
        ULog("%s: линия не в 1 в покое (sda %u scl %u) — не трогаю", Name, BbGet(B.Sda), BbGet(B.Scl));
    } else {
        for (k = 0; k < RTL_NUMBER_OF(Cmd); k++) {
            BOOLEAN Ok;
            KeRaiseIrql(DISPATCH_LEVEL, &Old);
            Ok = BbReadWord(&B, 0x55, Cmd[k], &V);
            KeLowerIrql(Old);
            if (Ok) ULog("%s: [%02x] %-8s = %u (0x%04x, s %d)", Name, Cmd[k], Nm[k], V, V, (SHORT)V);
            else    ULog("%s: [%02x] нет ACK", Name, Cmd[k]);
            SleepMs(2);
        }
    }
    B.Sda[0] = SvS; B.Scl[0] = SvC;
out:
    if (Ps) MmUnmapIoSpace(Ps, 0x1000);
    if (Pc) MmUnmapIoSpace(Pc, 0x1000);
}

/* ---------------- WDM ---------------- */

#define SMMU_PHYS 0x15000000ull
#define SMMU_SIZE 0x100000

static VOID
DumpSmmu(VOID)
{
    PUCHAR S = Map(SMMU_PHYS, SMMU_SIZE);
    ULONG  Id0, Id1, N, Ncb, I, Pg, Hits = 0;

    if (!S) { ULog("smmu: map failed"); return; }
    Id0 = Rd(S, 0x20); Id1 = Rd(S, 0x24);
    N   = Id0 & 0xFF;
    Ncb = Id1 & 0xFF;
    Pg  = (Id1 & (1u << 31)) ? 0x10000 : 0x1000;
    ULog("smmu: scr0 %08x sacr %08x idr0 %08x idr1 %08x idr2 %08x gfsr %08x gfsynr0 %08x gfsynr1 %08x nsmr %u ncb %u",
         Rd(S, 0x0), Rd(S, 0x10), Id0, Id1, Rd(S, 0x28), Rd(S, 0x48), Rd(S, 0x50), Rd(S, 0x54), N, Ncb);
    ULog("smmu: scr0 USFCFG(bit10)=%u CLIENTPD(bit0)=%u", (Rd(S, 0) >> 10) & 1, Rd(S, 0) & 1);
    for (I = 0; I < N && I < 256; I++) {
        ULONG Smr = Rd(S, 0x800 + 4 * I), S2 = Rd(S, 0xC00 + 4 * I);
        ULONG Sid = Smr & 0x7FFF, Msk = (Smr >> 16) & 0x7FFF;
        BOOLEAN Pcie = (Smr >> 31) && ((0x1c00 & ~Msk) == (Sid & ~Msk));
        if ((Smr >> 31) || S2) {
            ULog("smr[%3u] %08x (v%u id %04x mask %04x) s2cr %08x (type %u cb %u)%s", I, Smr, Smr >> 31, Sid, Msk,
                 S2, (S2 >> 16) & 3, S2 & 0xFF, Pcie ? "  <== PCIe0 0x1c00" : "");
            if (Pcie) Hits++;
        }
    }
    ULog("smmu: правил для 0x1c00: %u", Hits);
    if (Pg * 2 <= SMMU_SIZE) {
        for (I = 0; I < Ncb && I < 128; I++) {
            ULONG Cb = Rd(S, Pg + 4 * I);
            if (Cb) ULog("cbar[%u] %08x (type %u vmid %u)", I, Cb, (Cb >> 16) & 3, Cb & 0xFF);
        }
    }
    {
        ULONG Np = 1u << (((Id1 >> 28) & 7) + 1), CbBase = Np * Pg;
        for (I = 0; I < 6 && I < Ncb; I++) {
            PUCHAR C = S + CbBase + I * Pg;
            if (CbBase + (I + 1) * Pg > SMMU_SIZE) break;
            ULog("cb[%u] sctlr %08x tcr %08x ttbr0 %08x%08x fsr %08x cba2r %08x", I, Rd(C, 0), Rd(C, 0x30),
                 Rd(C, 0x24), Rd(C, 0x20), Rd(C, 0x58), Rd(S, Pg + 0x800 + 4 * I));
        }
    }
    MmUnmapIoSpace(S, SMMU_SIZE);
}

/* правило для PCIe0 (SID 0x1c00, маска 0x7f): S1-банк без трансляции = bypass для DMA QCA6390 */
static VOID
SmmuBypassPcie(VOID)
{
    PUCHAR S = Map(SMMU_PHYS, SMMU_SIZE);
    ULONG  Id0, Id1, N, Ncb, Pg, CbBase, I, Free = 0xFFFFFFFF, Cb, Ref = 0;

    if (!S) { ULog("smmu: map failed"); return; }
    Id0 = Rd(S, 0x20); Id1 = Rd(S, 0x24);
    N = Id0 & 0xFF; Ncb = Id1 & 0xFF;
    Pg = (Id1 & (1u << 31)) ? 0x10000 : 0x1000;
    CbBase = (1u << (((Id1 >> 28) & 7) + 1)) * Pg;
    Cb = Ncb - 1;
    for (I = 0; I < N; I++) {
        ULONG Smr = Rd(S, 0x800 + 4 * I);
        if ((Smr >> 31) && (Smr & 0x7FFF) == 0x1c00) { ULog("smmu: правило уже есть smr[%u] %08x", I, Smr); goto out; }
        if (!(Smr >> 31) && Free == 0xFFFFFFFF) Free = I;
    }
    if (Free == 0xFFFFFFFF || CbBase + (Cb + 1) * Pg > SMMU_SIZE || Rd(S, Pg + 4 * Cb) != 0) {
        ULog("smmu: нет места: free %x cb %u cbar %08x", Free, Cb, Rd(S, Pg + 4 * Cb));
        goto out;
    }
    for (I = 0; I < 6; I++) if (Rd(S, Pg + 4 * I)) { Ref = Rd(S, Pg + 4 * I); break; }
    if (!Ref) Ref = 0x0001f000;
    Wr(S + CbBase + Cb * Pg, 0x0, 0);                 /* SCTLR: M=0 — трансляции нет */
    Wr(S + CbBase + Cb * Pg, 0x58, 0xFFFFFFFF);       /* FSR: сбросить */
    Wr(S, Pg + 0x800 + 4 * Cb, 1);                    /* CBA2R: VA64 */
    Wr(S, Pg + 4 * Cb, Ref);                          /* CBAR как у банков UEFI (S1 + S2 bypass) */
    Wr(S, 0xC00 + 4 * Free, Cb);                      /* S2CR: TYPE=0 (translate) -> CB */
    Wr(S, 0x800 + 4 * Free, 0x80000000u | (0x7Fu << 16) | 0x1c00);
    Wr(S, 0x70, 0); (VOID)Rd(S, 0x74);                /* sTLBGSYNC + чтение sTLBGSTATUS */
    ULog("smmu: записано smr[%u] %08x s2cr %08x cb %u cbar %08x sctlr %08x", Free, Rd(S, 0x800 + 4 * Free),
         Rd(S, 0xC00 + 4 * Free), Cb, Rd(S, Pg + 4 * Cb), Rd(S + CbBase + Cb * Pg, 0));
out:
    MmUnmapIoSpace(S, SMMU_SIZE);
}

/* Атрибуты bypass-транзакций PCIe0 в контекст-банке правила 0x1c00: Normal WB, inner shareable, RA/WA —
 * DMA снупит кэши CPU, как требует _CCA=1 в DSDT (в Android: dma-coherent + coherent pagetable). */
static VOID
SmmuCoherentPcie(VOID)
{
    PUCHAR S = Map(SMMU_PHYS, SMMU_SIZE);
    ULONG  Id0, Id1, N, Pg, CbBase, I, Cb = 0xFFFFFFFF;
    const ULONG Attr = (2u << 26) | (2u << 24) | (2u << 22) | (1u << 20) | (0xFu << 16);

    if (!S) { ULog("smmu: map failed"); return; }
    Id0 = Rd(S, 0x20); Id1 = Rd(S, 0x24);
    N = Id0 & 0xFF;
    Pg = (Id1 & (1u << 31)) ? 0x10000 : 0x1000;
    CbBase = (1u << (((Id1 >> 28) & 7) + 1)) * Pg;
    for (I = 0; I < N; I++) {
        ULONG Smr = Rd(S, 0x800 + 4 * I), S2 = Rd(S, 0xC00 + 4 * I);
        if ((Smr >> 31) && (Smr & 0x7FFF) == 0x1c00 && ((S2 >> 16) & 3) == 0) { Cb = S2 & 0xFF; break; }
    }
    if (Cb == 0xFFFFFFFF || CbBase + (Cb + 1) * Pg > SMMU_SIZE) { ULog("smmu: правило 0x1c00 не найдено"); goto out; }
    {
        PUCHAR C = S + CbBase + Cb * Pg;
        ULONG Old = Rd(C, 0);
        if (Old & 1) { ULog("smmu: cb %u sctlr %08x — M=1, не трогаю", Cb, Old); goto out; }
        Wr(C, 0, (Old & ~0x0FFFC000u) | Attr);
        Wr(S, 0x70, 0); (VOID)Rd(S, 0x74);
        ULog("smmu: cb %u sctlr %08x -> %08x", Cb, Old, Rd(C, 0));
    }
out:
    MmUnmapIoSpace(S, SMMU_SIZE);
}


static ULONG
RegDword(PCWSTR Name, ULONG Def)
{
    OBJECT_ATTRIBUTES Oa;
    HANDLE            K;
    UNICODE_STRING    N;
    UCHAR             Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    ULONG             R, V = Def;

    InitializeObjectAttributes(&Oa, &gRegPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&K, KEY_READ, &Oa))) {
        RtlInitUnicodeString(&N, Name);
        if (NT_SUCCESS(ZwQueryValueKey(K, &N, KeyValuePartialInformation, Buf, sizeof(Buf), &R)) &&
            ((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD) {
            V = *(ULONG *)((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data;
        }
        ZwClose(K);
    }
    return V;
}

static VOID
Unmap(VOID)
{
    if (gGcc)  MmUnmapIoSpace(gGcc, GCC_SIZE);
    if (gTlmm) MmUnmapIoSpace(gTlmm, TLMMN_SIZE);
    if (gParf) MmUnmapIoSpace(gParf, PARF_SIZE);
    if (gPhy)  MmUnmapIoSpace(gPhy, PHY_SIZE);
    if (gDbi)  MmUnmapIoSpace(gDbi, DBI_SIZE);
    if (gCfg)  MmUnmapIoSpace(gCfg, CFG_SIZE);
    gGcc = gTlmm = gParf = gPhy = gDbi = gCfg = NULL;
}

static VOID
DriverUnload(PDRIVER_OBJECT Drv)
{
    (VOID)Drv;
    ULog("unload");
    Unmap();
}


/* ---------------- PMIC через SPMI (только чтение, observer EE0) ---------------- */

#define SPMI_CORE_PHYS 0x0C440000ull
#define SPMI_CORE_SIZE 0x1100
#define SPMI_OBS_PHYS  0x0E600000ull
#define SPMI_OBS_SIZE  0x100000

static PUCHAR gSpCore, gSpObs;

static int
SpmiRead(ULONG Apid, ULONG Addr, ULONG Bc, ULONG *Out)   /* Bc 1..4 байт */
{
    ULONG Off = 0x80 * Apid, St = 0, i;
    if (Off + 0x20 > SPMI_OBS_SIZE) return -1;
    Wr(gSpObs, Off + 0x00, (1u << 27) | ((Addr & 0xFF) << 4) | ((Bc - 1) & 7));   /* EXT_READL */
    for (i = 0; i < 1000; i++) {
        St = Rd(gSpObs, Off + 0x08);
        if (St & 1) break;
        KeStallExecutionProcessor(1);
    }
    if (!(St & 1) || (St & 0xE)) return -(int)(St | 0x100);
    *Out = Rd(gSpObs, Off + 0x18);
    return 0;
}

static VOID
DumpChg(VOID)   /* PM8150B SID2: CHGR 0x10, BATIF 0x12, USBIN 0x13, DCIN 0x14, TYPEC 0x15, MISC 0x16 — регистры 0x04..0x1F */
{
    ULONG A, Mp, Ppid, Off, V, Cnt = 0;
    char L[200]; int n;
    gSpCore = Map(SPMI_CORE_PHYS, SPMI_CORE_SIZE);
    gSpObs = Map(SPMI_OBS_PHYS, SPMI_OBS_SIZE);
    if (!gSpCore || !gSpObs) { ULog("spmi map failed"); goto out; }
    for (A = 0; 0x900 + 4 * A < SPMI_CORE_SIZE; A++) {
        Mp = Rd(gSpCore, 0x900 + 4 * A);
        Ppid = (Mp >> 8) & 0xFFF;
        if (!Mp || (Ppid >> 8) != 2 || (Ppid & 0xFF) < 0x10 || (Ppid & 0xFF) > 0x16) continue;
        n = _snprintf(L, sizeof(L), "sid2 %02x00:", Ppid & 0xFF);
        for (Off = 0x04; Off < 0x20; Off += 4) {
            if (SpmiRead(A, Off, 4, &V)) V = 0xEEEEEEEE;
            n += _snprintf(L + n, sizeof(L) - n, " %02x:%08x", Off, V);
        }
        ULog("%s", L);
        Cnt++;
    }
    ULog("chg: %u периферий", Cnt);
out:
    if (gSpCore) MmUnmapIoSpace(gSpCore, SPMI_CORE_SIZE);
    if (gSpObs) MmUnmapIoSpace(gSpObs, SPMI_OBS_SIZE);
    gSpCore = gSpObs = NULL;
}

static VOID
DumpPmic(VOID)
{
    ULONG Ver, A, Mp, Ppid, Sid, Per, T, V, M, E, St, Cnt = 0;
    char  L[200];
    gSpCore = Map(SPMI_CORE_PHYS, SPMI_CORE_SIZE);
    gSpObs = Map(SPMI_OBS_PHYS, SPMI_OBS_SIZE);
    if (!gSpCore || !gSpObs) { ULog("spmi map failed"); goto out; }
    Ver = Rd(gSpCore, 0);
    ULog("spmi arb version %08x", Ver);
    if ((Ver >> 28) != 5) { ULog("не v5 — выходим"); goto out; }
    for (A = 0; 0x900 + 4 * A < SPMI_CORE_SIZE; A++) {
        Mp = Rd(gSpCore, 0x900 + 4 * A);
        if (!Mp) continue;
        Ppid = (Mp >> 8) & 0xFFF;
        Sid = Ppid >> 8; Per = Ppid & 0xFF;
        if (Sid != 1 && Sid != 5) continue;
        if (SpmiRead(A, 0x04, 2, &T)) { ULog("apid %u ppid %03x: read err", A, Ppid); continue; }
        if (SpmiRead(A, 0x08, 1, &St)) St = 0xEE;
        if (SpmiRead(A, 0x40, 2, &V)) V = 0xEEEE;
        if (SpmiRead(A, 0x44, 4, &M)) M = 0xEEEEEEEE;
        if (SpmiRead(A, 0x46, 1, &E)) E = 0xEE;
        _snprintf(L, sizeof(L), "sid%u %02x00 apid %3u map %08x type %02x sub %02x st %02x vset %4u mV r44 %08x en %02x",
                  Sid, Per, A, Mp, T & 0xFF, (T >> 8) & 0xFF, St & 0xFF, V & 0xFFFF, M, E & 0xFF);
        ULog("%s", L);
        Cnt++;
    }
    ULog("pmic: %u периферий", Cnt);
out:
    if (gSpCore) MmUnmapIoSpace(gSpCore, SPMI_CORE_SIZE);
    if (gSpObs) MmUnmapIoSpace(gSpObs, SPMI_OBS_SIZE);
    gSpCore = gSpObs = NULL;
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegPath)
{
    ULONG    Mode;
    NTSTATUS St = STATUS_SUCCESS;

    gRegPath.Buffer = gRegBuf;
    gRegPath.MaximumLength = sizeof(gRegBuf);
    RtlCopyUnicodeString(&gRegPath, RegPath);
    Drv->DriverUnload = DriverUnload;

    Mode = RegDword(L"Mode", 1);
    ULog("elishpcie start, mode %x", Mode);

    if (Mode == 0x80) { DumpPmic(); ULog("done"); return STATUS_SUCCESS; }
    if (Mode == 0x8000) { DumpChg(); ULog("done"); return STATUS_SUCCESS; }
    if (Mode == 0x4000) {   /* только чтение bq27z561 x2 по программному I2C */
        GaugeProbe("master(se13 g36/37)", 0x0F500000ull + 36 * 0x1000, 0x0F500000ull + 37 * 0x1000);
        GaugeProbe("slave(se0 g28/29)",   0x0F900000ull + 28 * 0x1000, 0x0F900000ull + 29 * 0x1000);
        ULog("done");
        return STATUS_SUCCESS;
    }
    if (Mode == 0x2000) { SmmuCoherentPcie(); ULog("done"); return STATUS_SUCCESS; }
    if (Mode == 0x20 || Mode == 0x40) {   /* только SMMU, PCIe-блоки не трогаем */
        if (Mode == 0x40) SmmuBypassPcie();
        DumpSmmu();
        ULog("done");
        return STATUS_SUCCESS;
    }

    gGcc = Map(GCC_PHYS, GCC_SIZE);
    gTlmm = Map(TLMMN_PHYS, TLMMN_SIZE);
    gParf = Map(PARF_PHYS, PARF_SIZE);
    gPhy = Map(PHY_PHYS, PHY_SIZE);
    gDbi = Map(DBI_PHYS, DBI_SIZE);
    gCfg = Map(CFG_PHYS, CFG_SIZE);
    if (!gGcc || !gTlmm || !gParf || !gPhy || !gDbi || !gCfg) {
        ULog("map failed");
        Unmap();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DumpGcc("before");
    DumpCtl("before");

    if (Mode == 0x1000) {  /* перезапуск чипа без перезагрузки: цикл питания + полный сброс RC + возврат настроек pci.sys */
        static const ULONG Hdr[] = { 0x04, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x3C, 0x78, 0x80, 0x98 };
        ULONG Sv[RTL_NUMBER_OF(Hdr)], k;
        NTSTATUS Ls;
        if (!ClocksOn()) { ULog("клоки выключены"); Unmap(); return STATUS_SUCCESS; }
        for (k = 0; k < RTL_NUMBER_OF(Hdr); k++) Sv[k] = Rd(gDbi, Hdr[k]);
        ULog("rc saved: cmd %08x bar %08x/%08x bus %08x io %08x mem %08x pref %08x/%08x/%08x brctl %08x devctl %08x lnkctl %08x devctl2 %08x",
             Sv[0], Sv[2], Sv[3], Sv[4], Sv[5], Sv[6], Sv[7], Sv[8], Sv[9], Sv[11], Sv[12], Sv[13], Sv[14]);
        GpioOut(79, 0, 1, FALSE); SleepMs(10);
        GpioOut(20, 0, 1, FALSE); SleepMs(300);
        GpioOut(20, 0, 1, TRUE);  SleepMs(150);
        Ls = LinkUp();
        Set(gDbi, DBI_MISC_CONTROL_1, 1);
        for (k = 1; k < RTL_NUMBER_OF(Hdr); k++) {
            if (Hdr[k] == 0x80) Wr(gDbi, 0x80, Sv[k] & 0xFFFF);
            else if (Hdr[k] == 0x78) Wr(gDbi, 0x78, Sv[k] & 0xFFFF);
            else Wr(gDbi, Hdr[k], Sv[k]);
        }
        Clr(gDbi, DBI_MISC_CONTROL_1, 1);
        Wr(gDbi, 0x04, Sv[0] & 0xFFFF);
        ULog("restart: link %x; rc now cmd %08x bus %08x mem %08x lnkctl %08x; ep id %08x", Ls, Rd(gDbi, 4), Rd(gDbi, 0x18),
             Rd(gDbi, 0x20), Rd(gDbi, 0x80), NT_SUCCESS(Ls) ? Rd(gCfg, 0) : 0);
        Unmap();
        ULog("done");
        return STATUS_SUCCESS;
    }

    if (Mode == 0x800) {   /* снять ASPM L0s/L1 и L1SS: сначала чип, потом RC; прочее не трогаем */
        ULONG Pass, Ep = 0, Rc = 0, P;
        if (!ClocksOn() || !(Rd(gDbi, DBI_PORT_DEBUG1) & (1u << 4))) { ULog("link down or clocks off"); Unmap(); return STATUS_SUCCESS; }
        for (P = 0x100; P && P < 0x1000; P = Rd(gCfg, P) >> 20) { if ((Rd(gCfg, P) & 0xFFFF) == 0x1E) { Ep = P; break; } if (!(Rd(gCfg, P) >> 20)) break; }
        for (P = 0x100; P && P < 0x1000; P = Rd(gDbi, P) >> 20) { if ((Rd(gDbi, P) & 0xFFFF) == 0x1E) { Rc = P; break; } if (!(Rd(gDbi, P) >> 20)) break; }
        for (Pass = 0; Pass < 2; Pass++) {
            ULog("%s: ep lnkctl %08x l1ss@%03x cap %08x ctl1 %08x ctl2 %08x | rc lnkctl %08x l1ss@%03x cap %08x ctl1 %08x ctl2 %08x | parf pm %08x",
                 Pass ? "after" : "before", Rd(gCfg, 0x80), Ep, Ep ? Rd(gCfg, Ep + 4) : 0, Ep ? Rd(gCfg, Ep + 8) : 0, Ep ? Rd(gCfg, Ep + 12) : 0,
                 Rd(gDbi, 0x80), Rc, Rc ? Rd(gDbi, Rc + 4) : 0, Rc ? Rd(gDbi, Rc + 8) : 0, Rc ? Rd(gDbi, Rc + 12) : 0, Rd(gParf, PARF_PM_CTRL));
            if (Pass) break;
            if (Ep) Clr(gCfg, Ep + 8, 0xF);
            if (Rc) Clr(gDbi, Rc + 8, 0xF);
            Wr(gCfg, 0x80, (Rd(gCfg, 0x80) & 0xFFFF) & ~3u);
            Wr(gDbi, 0x80, (Rd(gDbi, 0x80) & 0xFFFF) & ~3u);
            SleepMs(5);
        }
        Unmap();
        ULog("done");
        return STATUS_SUCCESS;
    }

    if (Mode == 0x400) {   /* только чтение: GICD для INTID 832..847 и MSI capability чипа */
        PUCHAR G = Map(0x17A00000ull, 0x10000);
        ULONG Id, P, Cap = 0;
        if (G) {
            ULog("gicd ctlr %08x typer %08x", Rd(G, 0), Rd(G, 4));
            for (Id = 832; Id < 848; Id++) {
                ULONG Bit = 1u << (Id % 32), W = 4 * (Id / 32);
                ULog("intid %u: grp %u grpmod %u en %u pend %u act %u cfg %u prio %02x route %08x%08x", Id,
                     !!(Rd(G, 0x080 + W) & Bit), !!(Rd(G, 0xD00 + W) & Bit), !!(Rd(G, 0x100 + W) & Bit),
                     !!(Rd(G, 0x200 + W) & Bit), !!(Rd(G, 0x300 + W) & Bit),
                     (Rd(G, 0xC00 + 4 * (Id / 16)) >> (2 * (Id % 16))) & 3,
                     (Rd(G, 0x400 + (Id & ~3u)) >> (8 * (Id & 3))) & 0xFF,
                     Rd(G, 0x6000 + 8 * Id + 4), Rd(G, 0x6000 + 8 * Id));
            }
            MmUnmapIoSpace(G, 0x10000);
        }
        if (ClocksOn() && (Rd(gDbi, DBI_PORT_DEBUG1) & (1u << 4))) {
            ULog("bus1 id %08x cmd %08x", Rd(gCfg, 0), Rd(gCfg, 4));
            for (P = Rd(gCfg, 0x34) & 0xFC; P && P < 0x100; P = (Rd(gCfg, P) >> 8) & 0xFC) {
                ULONG H = Rd(gCfg, P);
                ULog("cap %02x id %02x: %08x %08x %08x %08x %08x %08x", P, H & 0xFF, H, Rd(gCfg, P + 4),
                     Rd(gCfg, P + 8), Rd(gCfg, P + 12), Rd(gCfg, P + 16), Rd(gCfg, P + 20));
                if ((H & 0xFF) == 5) Cap = P;
                if (++Id > 900) break;
            }
            if (Cap) {
                ULONG C = Rd(gCfg, Cap);
                ULog("MSI ctl %04x: enable %u mmc %u mme %u 64bit %u pvm %u", C >> 16, (C >> 16) & 1, (C >> 17) & 7,
                     (C >> 20) & 7, (C >> 23) & 1, (C >> 24) & 1);
            }
        } else ULog("link down or clocks off: bus1 not read");
        Unmap();
        ULog("done");
        return STATUS_SUCCESS;
    }

    if (Mode == 0x200) {   /* перезапуск LTSSM без сброса RC: LTSSM_EN 1 -> 0 -> 1 */
        ULONG V = 0, i;
        if (!ClocksOn()) { ULog("клоки PCIe0 выключены"); Unmap(); return STATUS_SUCCESS; }
        Clr(gParf, PARF_LTSSM, 1u << 8);
        SleepMs(20);
        Set(gParf, PARF_LTSSM, 1u << 8);
        for (i = 0; i < 100; i++) {
            V = Rd(gDbi, DBI_PORT_DEBUG1);
            if ((V & (1u << 4)) && !(V & (1u << 29))) break;
            SleepMs(10);
        }
        ULog("ltssm restart: dbg1 %08x ltssm %08x lnksta %08x through %u ms", V, Rd(gParf, PARF_LTSSM), Rd(gDbi, 0x80), i * 10);
        SleepMs(100);
        if (V & (1u << 4)) ULog("bus1 id %08x", Rd(gCfg, 0)); else ULog("link down, bus1 not read");
        DumpCtl("after");
        Unmap();
        ULog("done");
        return STATUS_SUCCESS;
    }

    if (Mode == 0x100) {   /* полный сброс чипа питанием, как cnss2: RC и iATU не трогаем */
        ULONG V = 0, i;
        if (!ClocksOn()) { ULog("клоки PCIe0 выключены — цикл не делаю"); Unmap(); return STATUS_SUCCESS; }
        GpioOut(79, 0, 1, FALSE);           /* PERST# */
        SleepMs(10);
        GpioOut(20, 0, 1, FALSE);           /* WLAN_EN=0 */
        SleepMs(300);
        GpioOut(20, 0, 1, TRUE);            /* WLAN_EN=1, ждём PMU чипа */
        SleepMs(150);
        GpioOut(79, 0, 1, TRUE);            /* PERST# снять */
        for (i = 0; i < 100; i++) {
            V = Rd(gDbi, DBI_PORT_DEBUG1);
            if ((V & (1u << 4)) && !(V & (1u << 29))) break;
            SleepMs(10);
        }
        ULog("цикл WLAN_EN: dbg1 %08x ltssm %08x lnksta %08x через %u мс", V, Rd(gParf, PARF_LTSSM),
             Rd(gDbi, 0x80), i * 10);
        SleepMs(100);
        if (V & (1u << 4)) ULog("шина 1: id %08x", Rd(gCfg, 0));
        else ULog("линк не поднялся — шину 1 не читаю");
        DumpCtl("after");
        Unmap();
        ULog("done");
        return STATUS_SUCCESS;
    }

    if (Mode & 2) {
        St = PowerUp(Mode);
    }
    if (NT_SUCCESS(St) && (Mode & 4)) {
        St = LinkUp();
    }
    if (Mode & 6) {
        DumpGcc("after");
        DumpCtl("after");
    }
    if ((Mode & 0x10) && ClocksOn() && (Rd(gDbi, DBI_PORT_DEBUG1) & (1u << 4))) {
        PUCHAR E = Map(DBI_PHYS, 0x200000);
        ULONG  Dv;
        if (E) {
            for (Dv = 0; Dv < 32; Dv++) {
                ULONG Id = Rd(E, Dv << 15);
                if (Dv < 4 || Id != 0xFFFFFFFF) ULog("ecam b0 d%u: %08x", Dv, Id);
            }
            ULog("ecam b1 d0: %08x d1: %08x", Rd(E, 0x100000), Rd(E, 0x100000 + (1u << 15)));
            MmUnmapIoSpace(E, 0x200000);
        }
    }
    ULog("done: %08x", St);
    /* остаёмся загруженными даже при ошибке — лог важнее; выгрузка через sc stop */
    return STATUS_SUCCESS;
}

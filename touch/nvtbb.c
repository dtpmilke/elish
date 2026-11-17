/*
  nvtbb: HID-тачскрин NT36523 Xiaomi Pad 5 Pro (elish) для Windows без SPB/PEP.

  Корневое устройство Root\NVTBB, сверху mshidkmdf (как vhidmini2 KMDF).
  Обмен с чипом программным SPI mode 0 на GPIO 8-11 (TLMM north, MmMapIoSpaceEx):
  GPIO8 MISO, GPIO9 MOSI, GPIO10 CLK, GPIO11 CS. Прошивки встроены (fwz.h, выбор — значение Panel) и
  загружается как nvt_download_firmware_hw_crc драйвера Xiaomi (elish-r-oss).
  INT (GPIO39) не используется: системный поток опрашивает буфер событий каждые 8 мс.
  Лог: %SystemRoot%\nvtbb.log.
*/

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <ntstrsafe.h>

#include "fwz.h"

#define TLMM_GPIO8_PA  0x0F908000ULL
#define TLMM_MAP_LEN   0x4000

#define PIN_MISO  8
#define PIN_MOSI  9
#define PIN_CLK   10
#define PIN_CS    11

#define NVT_ENG_RST_ADDR         0x7FFF80
#define NVT_SWRST_N8_ADDR        0x3F0FE
#define NVT_SPI_RD_FAST_ADDR     0x3F310
#define NVT_CHIP_VER_TRIM_ADDR   0x3F004
#define EVENT_BUF_ADDR           0x2FE00
#define BOOT_RDY_ADDR            0x3F10D
#define TX_AUTO_COPY_EN          0x3F7E8
#define SPI_DMA_TX_INFO          0x3F7F1
#define ILM_LENGTH_ADDR          0x3F118
#define DLM_LENGTH_ADDR          0x3F130
#define ILM_DES_ADDR             0x3F128
#define DLM_DES_ADDR             0x3F12C
#define G_ILM_CHECKSUM_ADDR      0x3F100
#define G_DLM_CHECKSUM_ADDR      0x3F104
#define BLD_CRC_EN_ADDR          0x3F30E
#define BLD_ILM_DLM_CRC_ADDR     0x3F133
#define EVENT_MAP_HOST_CMD       0x50
#define EVENT_MAP_RESET_COMPLETE 0x60
#define EVENT_MAP_FWINFO         0x78
#define RESET_STATE_INIT         0xA0
#define RESET_STATE_MAX          0xAF
#define NVT_TRANSFER_LEN         (63 * 1024)
#define POINT_DATA_LEN           65
#define MAX_FINGERS              10

/*
  Экспериментальные варианты против WDT прошивки в простое (см. README в elish-touch-test):
    -DNVT_FIX_ATOMIC   каждая SPI-транзакция выполняется на DISPATCH_LEVEL (не вытесняется планировщиком),
                       заливка прошивки кусками NVT_UP_CHUNK, поток опроса привязан к одному ядру и с высоким
                       приоритетом, перезаливка при >= NVT_WDT_FRAMES подряд кадров FD/FE
    -DNVT_FIX_IDLEPOLL после NVT_IDLE_AFTER_MS без касаний опрос раз в NVT_IDLE_POLL_MS, при касании снова 8 мс
    -DNVT_FIX_D0       EvtDeviceD0Exit паркует поток опроса и снимает ExSetTimerResolution (таймер 1 мс мешает DRIPS),
                       EvtDeviceD0Entry возвращает; независим от ATOMIC/IDLEPOLL
    -DNVT_FIX_DISP     (требует NVT_FIX_D0) PoRegisterPowerSettingCallback на GUID_CONSOLE_DISPLAY_STATE: экран off (0)
                       паркует поток как D0Exit (но callback ждёт парковку не дольше 200 мс), on/dim (1/2) возобновляет
                       как D0Entry; поток работает только при D0Active && DispOn (единый gate под StateLock)
  Без этих define код ведёт себя как rel12.
*/
#if defined(NVT_FIX_ATOMIC) || defined(NVT_FIX_IDLEPOLL)
#define NVT_WDT_STATS 1
#endif

#ifndef NVT_UP_CHUNK
#define NVT_UP_CHUNK      128      /* байт прошивки за одну атомарную транзакцию: ~1 мс при ~1 Мбит/с */
#endif
#ifndef NVT_IDLE_AFTER_MS
#define NVT_IDLE_AFTER_MS 500
#endif
#ifndef NVT_IDLE_POLL_MS
#define NVT_IDLE_POLL_MS  60
#endif
#ifndef NVT_WDT_FRAMES
#ifdef NVT_FIX_ATOMIC
#define NVT_WDT_FRAMES   2         /* 2 подряд кадра FD/FE: рваный кадр исключён атомарностью */
#else
#define NVT_WDT_FRAMES   11        /* как rel12: WdtRun > 10 */
#endif
#endif

#ifdef NVT_FIX_ATOMIC
/* Вложенный подъём IRQL допустим (new >= current). Внутри нельзя: Sleep/Wait, лог в файл, pageable-код. */
#define ATOMIC_BEGIN()  KIRQL AtomicIrql_; KeRaiseIrql(DISPATCH_LEVEL, &AtomicIrql_)
#define ATOMIC_END()    KeLowerIrql(AtomicIrql_)
#else
#define ATOMIC_BEGIN()  do { } while (0)
#define ATOMIC_END()    do { } while (0)
#endif

#define REPORT_ID_TOUCH    1
#define REPORT_ID_MAXCOUNT 2

/* ---------------- лог ---------------- */

static VOID
NvtLog(_In_z_ PCSTR Fmt, ...)
{
    CHAR              Line[256];
    va_list           Args;
    UNICODE_STRING    Path = RTL_CONSTANT_STRING(L"\\SystemRoot\\nvtbb.log");
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    LARGE_INTEGER     Now;
    TIME_FIELDS       Tf;
    size_t            Len, Pre;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    KeQuerySystemTime(&Now);
    ExSystemTimeToLocalTime(&Now, &Now);
    RtlTimeToTimeFields(&Now, &Tf);
    RtlStringCbPrintfA(Line, sizeof(Line), "%02d:%02d:%02d.%03d ", Tf.Hour, Tf.Minute, Tf.Second, Tf.Milliseconds);
    RtlStringCbLengthA(Line, sizeof(Line), &Pre);
    va_start(Args, Fmt);
    RtlStringCbVPrintfA(Line + Pre, sizeof(Line) - Pre, Fmt, Args);
    va_end(Args);
    RtlStringCbLengthA(Line, sizeof(Line), &Len);

    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&File, FILE_APPEND_DATA | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(File, NULL, NULL, NULL, &Iosb, Line, (ULONG)Len, NULL, NULL);
        ZwClose(File);
    }
}

/* Метки в ключе службы (Services\nvtbb): видны офлайн, даже если лог-файл не пишется */
static WCHAR          gRegPathBuf[256];
static UNICODE_STRING gRegPath;

static VOID
RegMark(_In_z_ PCWSTR Name, ULONG Val)
{
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING    N;
    HANDLE            Key;

    if (gRegPath.Length == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    InitializeObjectAttributes(&Oa, &gRegPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_SET_VALUE, &Oa))) {
        RtlInitUnicodeString(&N, Name);
        ZwSetValueKey(Key, &N, 0, REG_DWORD, &Val, sizeof(Val));
        ZwFlushKey(Key);
        ZwClose(Key);
    }
}

static ULONG
RegReadDword(_In_z_ PCWSTR Name, ULONG Default)
{
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING    N;
    HANDLE            Key;
    UCHAR             Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buf;
    ULONG             Len, Val = Default;

    if (gRegPath.Length == 0) {
        return Default;
    }

    InitializeObjectAttributes(&Oa, &gRegPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_QUERY_VALUE, &Oa))) {
        RtlInitUnicodeString(&N, Name);
        if (NT_SUCCESS(ZwQueryValueKey(Key, &N, KeyValuePartialInformation, Info, sizeof(Buf), &Len)) &&
            Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG)) {
            Val = *(ULONG *)Info->Data;
        }

        ZwClose(Key);
    }

    return Val;
}

/* ---------------- программный SPI ---------------- */

static volatile ULONG *gTlmm;   /* VA GPIO8 ctl; шаг 0x1000 байт на пин */
static BOOLEAN         gMosi;   /* текущий уровень MOSI */
static BOOLEAN         gSlow;   /* запасной медленный режим (обратное чтение после каждой записи) */
static volatile LONG   gMmioOff; /* 1 = D0Exit: MMIO/SPI запрещён — примитивы не трогают железо */

#define PIN_CTL(n) ((PULONG)(gTlmm + ((n) - 8) * 0x400))
#define PIN_IO(n)  ((PULONG)(gTlmm + ((n) - 8) * 0x400 + 1))

static __forceinline VOID
PinOut(ULONG Pin, BOOLEAN High)
{
    if (gMmioOff) return;
    WRITE_REGISTER_ULONG(PIN_IO(Pin), High ? 2 : 0);
    (VOID)READ_REGISTER_ULONG(PIN_IO(Pin));
}

static VOID
BbSetup(VOID)
{
    if (gMmioOff) return;
    /* выходы: func 0, 2 мА, без подтяжки, OE (bit9); MISO: вход с подтяжкой вниз */
    PinOut(PIN_CS, TRUE);
    PinOut(PIN_CLK, FALSE);
    PinOut(PIN_MOSI, FALSE);
    gMosi = FALSE;
    WRITE_REGISTER_ULONG(PIN_CTL(PIN_CS), 1 << 9);
    WRITE_REGISTER_ULONG(PIN_CTL(PIN_CLK), 1 << 9);
    WRITE_REGISTER_ULONG(PIN_CTL(PIN_MOSI), 1 << 9);
    WRITE_REGISTER_ULONG(PIN_CTL(PIN_MISO), 1);
    KeStallExecutionProcessor(100);
}

static VOID
BbRestore(VOID)
{
    ULONG n;

    if (gMmioOff) return;
    PinOut(PIN_CS, TRUE);
    for (n = PIN_MISO; n <= PIN_CS; n++) {
        WRITE_REGISTER_ULONG(PIN_CTL(n), (1 << 2) | (2 << 6));
    }
}

/*
  Быстрый обмен: записи в TLMM (Device-nGnRE) идут по порядку без ожидания, обратное
  чтение не нужно. Mode 0: MISO выставлен с предыдущего спада, читаем его до фронта.
  Медленный SPI (~190 кГц) ронял прошивку по WDT в режиме простоя (кадры FD/FE).
*/

static __forceinline VOID
BbMosi(BOOLEAN High)
{
    if (gMmioOff) return;
    if (High != gMosi) {
        WRITE_REGISTER_ULONG(PIN_IO(PIN_MOSI), High ? 2 : 0);
        gMosi = High;
    }
}

static UCHAR
BbByteSlow(UCHAR Out)
{
    ULONG b;
    UCHAR In = 0;

    for (b = 0; b < 8; b++) {
        if (gMmioOff) return 0;
        PinOut(PIN_MOSI, (Out & (0x80 >> b)) != 0);
        PinOut(PIN_CLK, TRUE);
        In = (UCHAR)((In << 1) | (READ_REGISTER_ULONG(PIN_IO(PIN_MISO)) & 1));
        PinOut(PIN_CLK, FALSE);
    }

    gMosi = (Out & 1) != 0;
    return In;
}

static UCHAR
BbByte(UCHAR Out)
{
    ULONG b;
    UCHAR In = 0;

    if (gSlow) {
        return BbByteSlow(Out);
    }

    for (b = 0; b < 8; b++) {
        if (gMmioOff) return 0;
        BbMosi((Out & (0x80 >> b)) != 0);
        In = (UCHAR)((In << 1) | (READ_REGISTER_ULONG(PIN_IO(PIN_MISO)) & 1));
        WRITE_REGISTER_ULONG(PIN_IO(PIN_CLK), 2);
        WRITE_REGISTER_ULONG(PIN_IO(PIN_CLK), 0);
    }

    return In;
}

static VOID
BbByteOut(UCHAR Out)
{
    ULONG b;

    if (gSlow) {
        BbByteSlow(Out);
        return;
    }

    for (b = 0; b < 8; b++) {
        if (gMmioOff) return;
        BbMosi((Out & (0x80 >> b)) != 0);
        WRITE_REGISTER_ULONG(PIN_IO(PIN_CLK), 2);
        WRITE_REGISTER_ULONG(PIN_IO(PIN_CLK), 0);
    }
}

static VOID
NvtWriteRaw(UCHAR First, const UCHAR *Data, ULONG Len)
{
    ULONG i;
    if (gMmioOff) return;
    ATOMIC_BEGIN();

    PinOut(PIN_CS, FALSE);
    BbByteOut(First | 0x80);
    for (i = 0; i < Len; i++) {
        BbByteOut(Data[i]);
    }

    PinOut(PIN_CS, TRUE);
    ATOMIC_END();
}

static VOID
NvtReadRaw(UCHAR Addr, UCHAR *Out, ULONG Len)
{
    ULONG i;
    if (gMmioOff) return;
    ATOMIC_BEGIN();

    PinOut(PIN_CS, FALSE);
    BbByteOut(Addr & 0x7F);
    BbByteOut(0);
    for (i = 0; i < Len; i++) {
        Out[i] = BbByte(0);
    }

    PinOut(PIN_CS, TRUE);
    ATOMIC_END();
}

static VOID
NvtSetPage(ULONG Addr)
{
    UCHAR B[2] = { (UCHAR)(Addr >> 15), (UCHAR)(Addr >> 7) };

    NvtWriteRaw(0xFF, B, 2);
}

static VOID
NvtWriteAddr(ULONG Addr, UCHAR Val)
{
    ATOMIC_BEGIN();
    NvtSetPage(Addr);
    NvtWriteRaw((UCHAR)(Addr & 0x7F), &Val, 1);
    ATOMIC_END();
}

static VOID
NvtReadAt(ULONG Addr, UCHAR *Out, ULONG Len)
{
    ATOMIC_BEGIN();
    NvtSetPage(Addr);
    NvtReadRaw((UCHAR)(Addr & 0x7F), Out, Len);
    ATOMIC_END();
}

static VOID
SleepMs(ULONG Ms)
{
    LARGE_INTEGER T;

    T.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &T);
}

static ULONG
Le32(const UCHAR *P)
{
    return P[0] | (P[1] << 8) | (P[2] << 16) | ((ULONG)P[3] << 24);
}

/* ---------------- NT36523: опознание и загрузка прошивки ---------------- */

static VOID
NvtBootloaderReset(VOID)
{
    NvtWriteAddr(NVT_SWRST_N8_ADDR, 0x69);
    KeStallExecutionProcessor(5000);
    NvtWriteAddr(NVT_SPI_RD_FAST_ADDR, 0x00);
}

static BOOLEAN
NvtCheckChip(VOID)
{
    UCHAR Id[6];
    ULONG Retry;

    NvtWriteAddr(NVT_ENG_RST_ADDR, 0x5A);
    SleepMs(10);
    for (Retry = 0; Retry < 5; Retry++) {
        if (gMmioOff) return FALSE;
        NvtBootloaderReset();
        NvtReadAt(NVT_CHIP_VER_TRIM_ADDR, Id, 6);
        NvtLog("chip ver trim: %02x %02x %02x %02x %02x %02x\r\n", Id[0], Id[1], Id[2], Id[3], Id[4], Id[5]);
        if (Id[3] == 0x23 && Id[4] == 0x65 && Id[5] == 0x03) {
            return TRUE;
        }

        SleepMs(10);
    }

    return FALSE;
}

typedef struct {
    ULONG BinAddr, SramAddr, Size, Crc;
} BIN_MAP;

#define MAX_PART 32
static BIN_MAP gMap[MAX_PART];
static ULONG   gParts;

/* nvt_bin_header_parser, только вариант hw_crc + cascade (как у k81a_fw01) */
static BOOLEAN
ParseHeader(const UCHAR *Fw, ULONG FwSize)
{
    ULONG   End = Le32(Fw), Pos, Info = 0, Ovly, List;
    BOOLEAN Cascade = (Fw[0x20] & 0x02) != 0, FoundHdr = FALSE;

    for (Pos = 0x30; Pos < (Cascade ? End / 2 : End); Pos += 0x10) {
        Info++;
    }

    if (Cascade) {
        Info++;
    }

    Ovly   = (Fw[0x28] & 0x10) ? (Fw[0x28] & 0x0F) : 0;
    gParts = 2 + Ovly + Info;
    if (!Cascade || gParts > MAX_PART) {
        NvtLog("unexpected fw header: cascade %d parts %u\r\n", Cascade, gParts);
        return FALSE;
    }

    for (List = 0; List < gParts; List++) {
        if (List < 2) {
            gMap[List].BinAddr  = Le32(&Fw[List * 12]);
            gMap[List].SramAddr = Le32(&Fw[4 + List * 12]);
            gMap[List].Size     = Le32(&Fw[8 + List * 12]);
            gMap[List].Crc      = Le32(&Fw[0x18 + List * 4]);
        } else {
            if (List < 2 + Info) {
                Pos = FoundHdr ? End - 0x10 : 0x30 + 0x10 * (List - 2);
            } else {
                Pos = gMap[1].BinAddr + 0x10 * (List - 2 - Info);
            }

            gMap[List].SramAddr = Le32(&Fw[Pos]);
            gMap[List].Size     = Le32(&Fw[Pos + 4]);
            gMap[List].BinAddr  = Le32(&Fw[Pos + 8]);
            gMap[List].Crc      = Le32(&Fw[Pos + 12]);
            if (List < 2 + Info && gMap[List].BinAddr < End && gMap[List].Size != 0) {
                FoundHdr = TRUE;
            }
        }

        if (gMap[List].BinAddr + gMap[List].Size > FwSize) {
            NvtLog("partition %u out of file\r\n", List);
            return FALSE;
        }
    }

    return TRUE;
}

static VOID
SetBldCrcBank(ULONG DesAddr, ULONG SramAddr, ULONG LenAddr, ULONG Size, ULONG GAddr, ULONG Crc)
{
    UCHAR B[4];

    NvtSetPage(DesAddr);
    B[0] = (UCHAR)SramAddr; B[1] = (UCHAR)(SramAddr >> 8); B[2] = (UCHAR)(SramAddr >> 16);
    NvtWriteRaw((UCHAR)(DesAddr & 0x7F), B, 3);
    B[0] = (UCHAR)Size; B[1] = (UCHAR)(Size >> 8); B[2] = (UCHAR)((Size >> 16) & 1);
    NvtWriteRaw((UCHAR)(LenAddr & 0x7F), B, 3);
    B[0] = (UCHAR)Crc; B[1] = (UCHAR)(Crc >> 8); B[2] = (UCHAR)(Crc >> 16); B[3] = (UCHAR)(Crc >> 24);
    NvtWriteRaw((UCHAR)(GAddr & 0x7F), B, 4);
}

static BOOLEAN
NvtDownloadOnce(const UCHAR *Fw)
{
    UCHAR B[8];
    ULONG List, Sram, Bin, Left, Len, i;

    if (gMmioOff) return FALSE;
    NvtBootloaderReset();
    SetBldCrcBank(ILM_DES_ADDR, gMap[0].SramAddr, ILM_LENGTH_ADDR, gMap[0].Size, G_ILM_CHECKSUM_ADDR, gMap[0].Crc);
    SetBldCrcBank(DLM_DES_ADDR, gMap[1].SramAddr, DLM_LENGTH_ADDR, gMap[1].Size, G_DLM_CHECKSUM_ADDR, gMap[1].Crc);
    NvtWriteAddr(TX_AUTO_COPY_EN, 0x69);

    for (List = 0; List < gParts; List++) {
        if (gMap[List].Size == 0) {
            continue;
        }

        Sram = gMap[List].SramAddr;
        Bin  = gMap[List].BinAddr;
        Left = gMap[List].Size + 1;
        while (Left) {
#ifdef NVT_FIX_ATOMIC
            Len = min(Left, NVT_UP_CHUNK);
#else
            Len = min(Left, NVT_TRANSFER_LEN);
#endif
            {
                ATOMIC_BEGIN();
                NvtSetPage(Sram);
                NvtWriteRaw((UCHAR)(Sram & 0x7F), Fw + Bin, Len);
                ATOMIC_END();
            }
            Sram += Len;
            Bin  += Len;
            Left -= Len;
        }
    }

    for (i = 0; i < 200; i++) {
        if (gMmioOff) return FALSE;
        NvtReadAt(SPI_DMA_TX_INFO, B, 1);
        if (B[0] == 0) {
            break;
        }

        SleepMs(1);
    }

    /* nvt_bld_crc_enable, nvt_fw_crc_enable, nvt_boot_ready */
    NvtReadAt(BLD_CRC_EN_ADDR, B, 1);
    B[0] |= 0x80;
    NvtWriteRaw((UCHAR)(BLD_CRC_EN_ADDR & 0x7F), B, 1);
    NvtSetPage(EVENT_BUF_ADDR);
    B[0] = 0;
    NvtWriteRaw(EVENT_MAP_RESET_COMPLETE, B, 1);
    B[0] = 0xAE;
    NvtWriteRaw(EVENT_MAP_HOST_CMD, B, 1);
    NvtWriteAddr(BOOT_RDY_ADDR, 1);
    SleepMs(5);

    NvtSetPage(EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE);
    for (i = 0; i <= 50; i++) {
        if (gMmioOff) return FALSE;
        NvtReadRaw(EVENT_MAP_RESET_COMPLETE, B, 5);
        if (B[0] >= RESET_STATE_INIT && B[0] <= RESET_STATE_MAX) {
            NvtLog("fw reset state %02x after %u0 ms\r\n", B[0], i);
            return TRUE;
        }

        SleepMs(10);
    }

    NvtReadAt(BLD_ILM_DLM_CRC_ADDR, B, 1);
    NvtLog("fw did not start: crc flags %02x\r\n", B[0]);
    return FALSE;
}

/* ---------------- HID ---------------- */

#define FINGER_COLLECTION                                                   \
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,                                     \
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,                                     \
    0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,             \
    0x05, 0x01, 0x75, 0x10, 0x95, 0x01, 0x55, 0x0E, 0x65, 0x11,             \
    0x09, 0x30, 0x26, 0x3F, 0x06, 0x35, 0x00, 0x46, 0xC9, 0x05, 0x81, 0x02, \
    0x09, 0x31, 0x26, 0xFF, 0x09, 0x46, 0x41, 0x09, 0x81, 0x02,             \
    0x45, 0x00, 0x55, 0x00, 0x65, 0x00,                                     \
    0xC0

/* 1600x2560, ~148.1x236.9 мм (единицы 0.1 мм: cm, exponent -2) */
static const UCHAR gReportDesc[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, REPORT_ID_TOUCH,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    0x05, 0x0D,
    0x55, 0x0C, 0x66, 0x01, 0x10, 0x47, 0xFF, 0xFF, 0x00, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02,             /* Scan Time, 100 мкс */
    0x45, 0x00, 0x55, 0x00, 0x65, 0x00,
    0x09, 0x54, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, /* Contact Count */
    0x85, REPORT_ID_MAXCOUNT,
    0x09, 0x55, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, /* Contact Count Maximum */
    0xC0
};

#pragma pack(push, 1)
typedef struct {
    UCHAR  Tip;
    UCHAR  Id;
    USHORT X;
    USHORT Y;
} HID_CONTACT;

typedef struct {
    UCHAR       ReportId;
    HID_CONTACT C[MAX_FINGERS];
    USHORT      ScanTime;
    UCHAR       Count;
} TOUCH_REPORT;
#pragma pack(pop)

static const HID_DESCRIPTOR gHidDesc = {
    0x09, 0x21, 0x0100, 0x00, 0x01, { 0x22, sizeof(gReportDesc) }
};

typedef struct {
    WDFQUEUE  ReadQueue;
    PVOID     Thread;
    KEVENT    StopEvent;
    PVOID     TlmmVa;
    volatile LONG TimerRes;    /* 1 = ExSetTimerResolution(10000,TRUE) выставлен; парность On/Off через Interlocked* */
#ifdef NVT_FIX_D0
    KEVENT    PauseEvent;     /* notification: D0Exit установил, поток должен припарковаться */
    KEVENT    ResumeEvent;    /* notification: D0Entry установил */
    KEVENT    ParkedEvent;    /* notification: поток припарковался */
#endif
#ifdef NVT_FIX_DISP
    KSPIN_LOCK StateLock;     /* один замок на всё состояние gate D0/экран; без лога и ожидания внутри */
    PVOID      PowerCb;       /* handle PoRegisterPowerSettingCallback */
    LONG       D0Active;      /* под замком: 1 = устройство в D0 */
    LONG       DispOn;        /* под замком: 1 = экран on/dim */
    LONG       GateUp;        /* под замком: 1 = поток опроса должен работать (D0Active && DispOn) */
    volatile LONG PollArmed;  /* 1 = поток в цикле опроса (только тогда gate ставит gMmioOff) */
    volatile LONG CbLive;     /* 1 = callback дисплея зарегистрирован */
#endif
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetCtx)

static NTSTATUS
CopyToRequest(WDFREQUEST Request, const VOID *Src, size_t Len)
{
    WDFMEMORY Mem;
    size_t    OutLen;
    NTSTATUS  Status;

    Status = WdfRequestRetrieveOutputMemory(Request, &Mem);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WdfMemoryGetBuffer(Mem, &OutLen);
    if (OutLen < Len) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    Status = WdfMemoryCopyFromBuffer(Mem, 0, (PVOID)Src, Len);
    if (NT_SUCCESS(Status)) {
        WdfRequestSetInformation(Request, Len);
    }

    return Status;
}

static volatile LONG gReadsIn, gSent, gDropped, gSendErr;

static VOID
SendReport(PDEVICE_CONTEXT Ctx, const TOUCH_REPORT *Rep)
{
    WDFREQUEST Request;
    NTSTATUS   Status;

    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->ReadQueue, &Request))) {
        Status = CopyToRequest(Request, Rep, sizeof(*Rep));
        if (NT_SUCCESS(Status)) {
            gSent++;
        } else if (gSendErr++ < 3) {
            NvtLog("report copy failed %08x\r\n", Status);
        }

        WdfRequestComplete(Request, Status);
    } else {
        gDropped++;
    }
}

/* ---------------- поток опроса ---------------- */

static UCHAR *gNvtFw;
static ULONG  gNvtFwSize;

/* Прошивка по типу панели (значение Panel в ключе службы): 1 — производитель 0x42 (k81a_fw01),
   2 — 0x08/0x35 (k81a_fw02); как novatek,cfg_N в device tree elish */
static BOOLEAN
NvtUnpackFw(ULONG Index)
{
    const UCHAR *P = gNvtFws[Index].Z, *End = P + gNvtFws[Index].ZLen;
    ULONG        Pos = 0, Lit, Zero, Size = gNvtFws[Index].Size;

    gNvtFwSize = Size;
    gNvtFw = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, 'bbtN');
    if (gNvtFw == NULL) {
        NvtLog("fw alloc failed\r\n");
        return FALSE;
    }

    while (P + 8 <= End) {
        Lit = Le32(P);
        P += 4;
        if (Lit > (ULONG)(End - P) || Pos + Lit > Size) {
            break;
        }

        RtlCopyMemory(gNvtFw + Pos, P, Lit);
        P += Lit;
        Pos += Lit;
        Zero = Le32(P);
        P += 4;
        if (Pos + Zero > Size) {
            break;
        }

        RtlZeroMemory(gNvtFw + Pos, Zero);
        Pos += Zero;
    }

    if (Pos != Size || P != End) {
        NvtLog("fw unpack failed at %u\r\n", Pos);
        return FALSE;
    }

    return TRUE;
}

#ifndef NVT_RELEASE
static VOID
NvtDump(PCSTR Tag, const UCHAR *P, ULONG Len)
{
    CHAR  Hex[200];
    ULONG i, o = 0;

    for (i = 0; i < Len && o + 3 < sizeof(Hex); i++) {
        Hex[o++] = "0123456789abcdef"[P[i] >> 4];
        Hex[o++] = "0123456789abcdef"[P[i] & 15];
        if ((i & 3) == 3) Hex[o++] = ' ';
    }

    Hex[o] = 0;
    NvtLog("%s: %s\r\n", Tag, Hex);
}
#endif

/* Состояние шины и чипа: регистры пинов, trim (жива ли шина), reset state и версия fw */
static VOID
NvtDiag(PCSTR Tag)
{
    UCHAR T[8], R[2], V[2];

    if (gMmioOff) return;
    NvtReadAt(NVT_CHIP_VER_TRIM_ADDR, T, 6);
    NvtReadAt(EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE, R, 1);
    NvtReadAt(EVENT_BUF_ADDR | EVENT_MAP_FWINFO, V, 2);
    NvtLog("%s: ctl %03x %03x %03x %03x io %x %x %x %x | trim %02x %02x %02x %02x %02x %02x | reset %02x fw %02x/%02x\r\n", Tag,
           READ_REGISTER_ULONG(PIN_CTL(8)), READ_REGISTER_ULONG(PIN_CTL(9)),
           READ_REGISTER_ULONG(PIN_CTL(10)), READ_REGISTER_ULONG(PIN_CTL(11)),
           READ_REGISTER_ULONG(PIN_IO(8)), READ_REGISTER_ULONG(PIN_IO(9)),
           READ_REGISTER_ULONG(PIN_IO(10)), READ_REGISTER_ULONG(PIN_IO(11)),
           T[0], T[1], T[2], T[3], T[4], T[5], R[0], V[0], V[1]);
}

/* ExSetTimerResolution — глобальный счётчик запросов: каждому (10000,TRUE) ровно один (0,FALSE).
   CAS-флаг делает парность конструктивной (кто бы ни вызвал — On/Off ровно по одному разу). */
static VOID
NvtTimerResOn(PDEVICE_CONTEXT Ctx)
{
    if (InterlockedCompareExchange(&Ctx->TimerRes, 1, 0) == 0) {
        ExSetTimerResolution(10000, TRUE);
    }
}

static VOID
NvtTimerResOff(PDEVICE_CONTEXT Ctx)
{
    if (InterlockedCompareExchange(&Ctx->TimerRes, 0, 1) == 1) {
        ExSetTimerResolution(0, FALSE);
    }
}

#ifdef NVT_FIX_DISP
/*
  Совместный gate D0/экран: поток опроса работает, только когда D0Active && DispOn.
  Источники — EvtD0Exit/EvtD0Entry и PoRegisterPowerSettingCallback(GUID_CONSOLE_DISPLAY_STATE).
  Изменения состояния и событий — под StateLock, без лога и ожидания внутри; ожидание
  ParkedEvent, снятие таймера и лог — после отпускания замка. ExSetTimerResolution и ожидание
  с таймаутом требуют IRQL <= APC_LEVEL (см. wdm.h), поэтому выше APC_LEVEL callback
  меняет только состояние.
*/

/* {6FE69556-704A-47A0-8F24-C28D936FDA47} GUID_CONSOLE_DISPLAY_STATE; в wdm.h он через
   DEFINE_GUID (extern без INITGUID) — здесь свой экземпляр */
static const GUID NvtGuidConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

/*
  NvtGateSet: вызывающий выставляет D0Active/DispOn (-1 = не менять), функция пересчитывает gate.
  Возврат: 0 = без изменений, 1 = перешли в парковку (вызывающему: NvtTimerResOff + ожидание
  ParkedEvent + лог), 2 = перешли в работу (gMmioOff=0 и события уже сделаны).
*/
static LONG
NvtGateSet(PDEVICE_CONTEXT Ctx, LONG D0Active, LONG DispOn)
{
    KIRQL   Old;
    LONG    Action = 0;
    BOOLEAN WantUp;

    KeAcquireSpinLock(&Ctx->StateLock, &Old);
    if (D0Active >= 0) {
        Ctx->D0Active = D0Active;
    }

    if (DispOn >= 0) {
        Ctx->DispOn = DispOn;
    }

    WantUp = Ctx->D0Active != 0 && Ctx->DispOn != 0;
    if (WantUp && !Ctx->GateUp) {
        Ctx->GateUp = TRUE;
        InterlockedExchange(&gMmioOff, 0);       /* сначала разрешить MMIO, затем будить */
        KeClearEvent(&Ctx->PauseEvent);
        KeSetEvent(&Ctx->ResumeEvent, IO_NO_INCREMENT, FALSE);
        Action = 2;
    } else if (!WantUp && Ctx->GateUp) {
        Ctx->GateUp = FALSE;
        if (Ctx->PollArmed) {
            InterlockedExchange(&gMmioOff, 1);   /* поток в цикле: SPI гасим сразу, перезаливка не начнётся */
        }

        KeClearEvent(&Ctx->ParkedEvent);
        KeClearEvent(&Ctx->ResumeEvent);
        KeSetEvent(&Ctx->PauseEvent, IO_NO_INCREMENT, FALSE);
        Action = 1;
    }

    KeReleaseSpinLock(&Ctx->StateLock, Old);
    return Action;
}

static NTSTATUS
NvtDispCallback(LPCGUID SettingGuid, PVOID Value, ULONG ValueLength, PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER   W;
    ULONG           State;
    LONG            Action;
    KIRQL           Old;
    BOOLEAN         Live;

    UNREFERENCED_PARAMETER(SettingGuid);

    KeAcquireSpinLock(&Ctx->StateLock, &Old);
    Live = Ctx->CbLive != 0;
    KeReleaseSpinLock(&Ctx->StateLock, Old);
    if (!Live) {
        return STATUS_SUCCESS;   /* ReleaseHardware снимает регистрацию — не трогаем состояние */
    }

    if (Value == NULL || ValueLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    State = *(const ULONG *)Value;   /* GUID_CONSOLE_DISPLAY_STATE: 0 = off, 1 = on, 2 = dim */
    if (State > 2) {
        return STATUS_INVALID_PARAMETER;
    }

    /* По документации POWER_SETTING_CALLBACK вызывается на PASSIVE_LEVEL; выше APC_LEVEL
       недопустимы ожидание с таймаутом и ExSetTimerResolution — только состояние под замком,
       без лога (NvtLog пишет лишь на PASSIVE) */
    if (KeGetCurrentIrql() > APC_LEVEL) {
        (VOID)NvtGateSet(Ctx, -1, State == 0 ? 0 : 1);
        return STATUS_SUCCESS;
    }

    if (State == 0) {
        Action = NvtGateSet(Ctx, -1, 0);
        if (Action == 1) {
            NvtTimerResOff(Ctx);
            if (Ctx->Thread != NULL) {
                W.QuadPart = -10 * 1000 * 200;   /* до 200 мс: callback не должен блокироваться надолго */
                if (KeWaitForSingleObject(&Ctx->ParkedEvent, Executive, KernelMode, FALSE, &W) == STATUS_TIMEOUT) {
                    NvtLog("disp: off -> park timeout\r\n");
                } else {
                    NvtLog("disp: off -> parked\r\n");
                }
            } else {
                NvtLog("disp: off -> park pending (no thread)\r\n");
            }
        }
    } else {
        Action = NvtGateSet(Ctx, -1, 1);
        if (Action == 2) {
            NvtLog("disp: on -> resumed\r\n");
        }
    }

    return STATUS_SUCCESS;
}
#endif

#ifdef NVT_FIX_D0
/* Ожидание следующего витка опроса. TRUE = продолжать, FALSE = остановка. В паузе (D0Exit) таймер ядра отпущен,
   поток спит без таймаута и не трогает SPI. */
static BOOLEAN
NvtWaitTick(PDEVICE_CONTEXT Ctx, PLARGE_INTEGER Timeout)
{
    PVOID    Obj[2];
    NTSTATUS St;

    Obj[0] = &Ctx->StopEvent; Obj[1] = &Ctx->PauseEvent;
    St = KeWaitForMultipleObjects(2, Obj, WaitAny, Executive, KernelMode, FALSE, Timeout, NULL);
    if (St == STATUS_TIMEOUT) {
        return TRUE;
    }
    if (St == STATUS_WAIT_0) {
        return FALSE;
    }
    /* пауза */
    NvtTimerResOff(Ctx);
    NvtLog("d0: poll parked\r\n");
    KeSetEvent(&Ctx->ParkedEvent, IO_NO_INCREMENT, FALSE);
    Obj[1] = &Ctx->ResumeEvent;
    St = KeWaitForMultipleObjects(2, Obj, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
    if (St == STATUS_WAIT_0) {
        return FALSE;
    }
    if (Timeout->QuadPart == -80000) {   /* активный темп (8 мс): 1 мс снова нужен; idle включится в витке */
        NvtTimerResOn(Ctx);
    }
    BbSetup();   /* транзакция могла быть прервана посреди байта: CS/пины в известное состояние */
    NvtLog("d0: poll resumed\r\n");
    return TRUE;
}
#endif

static VOID
PollThread(PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    UCHAR           P[POINT_DATA_LEN], B[40], Sum;
    BOOLEAN         Down[MAX_FINGERS] = { 0 }, Now[MAX_FINGERS];
    USHORT          X[MAX_FINGERS] = { 0 }, Y[MAX_FINGERS] = { 0 };
    TOUCH_REPORT    Rep;
    LARGE_INTEGER   Timeout, T0, Freq;
    ULONG           i, f, n, Try, Reports = 0, BadFrames = 0, Frames = 0, Idle = 0, Valid = 0, Tick = 0;
    ULONG           BadRun = 0, WdtRun = 0, Dumps = 0, Reloads = 0, Panel, Changes = 0;
    UCHAR           Kind, LastKind = 0xFF;
    UCHAR           Id, St;
#ifdef NVT_FIX_ATOMIC
    KAFFINITY       OldAff, NewAff;
    ULONG           Cpu;
#endif
#ifdef NVT_WDT_STATS
    LONGLONG        NowMs, StartMs, NextStatMs;
    ULONG           Polls = 0, SlowPolls = 0, TouchFrames = 0;
#endif
#ifdef NVT_FIX_IDLEPOLL
    LONGLONG        LastActiveMs;
#endif

#ifdef NVT_FIX_ATOMIC
    /* одно ядро (старшее из активных: CPU0 перегружен прерываниями) и высокий приоритет */
    NewAff = KeQueryActiveProcessors();
    for (Cpu = 63; Cpu > 0 && !(NewAff & ((KAFFINITY)1 << Cpu)); Cpu--) {
    }

    OldAff = KeSetSystemAffinityThreadEx((KAFFINITY)1 << Cpu);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    NvtLog("atomic: poll thread pinned to cpu %u, priority %d, up chunk %u, wdt frames %u\r\n", Cpu, LOW_REALTIME_PRIORITY,
           NVT_UP_CHUNK, NVT_WDT_FRAMES);
#endif

    RegMark(L"Stage", 5);
    NvtLog("nvtbb start\r\n");
    BbSetup();
    if (!NvtCheckChip()) {
        NvtLog("fast spi: chip not detected, falling back to slow spi\r\n");
        gSlow = TRUE;
        BbSetup();
    }

    if (!NvtCheckChip()) {
        RegMark(L"Stage", 0x50);
        NvtLog("NT36523 not detected\r\n");
        goto out;
    }

    Panel = RegReadDword(L"Panel", 1);
    if (Panel < 1 || Panel > RTL_NUMBER_OF(gNvtFws)) {
        Panel = 1;
    }

    NvtLog("panel %u: %s\r\n", Panel, gNvtFws[Panel - 1].Name);
    if (!NvtUnpackFw(Panel - 1) || !ParseHeader(gNvtFw, gNvtFwSize)) {
        goto out;
    }

    T0 = KeQueryPerformanceCounter(&Freq);
    for (Try = 0; Try < 3; Try++) {
        if (NvtDownloadOnce(gNvtFw)) {
            break;
        }

        if (Try == 1 && !gSlow) {
            NvtLog("fast spi: download failed, falling back to slow spi\r\n");
            gSlow = TRUE;
            BbSetup();
        }
    }

    NvtLog("download %s in %u ms (try %u, %s spi)\r\n", Try < 3 ? "ok" : "FAILED",
           (ULONG)((KeQueryPerformanceCounter(NULL).QuadPart - T0.QuadPart) * 1000 / Freq.QuadPart), Try, gSlow ? "slow" : "fast");
    RegMark(L"Download", Try);
    if (Try == 3) {
        goto out;
    }

    RegMark(L"Stage", 6);

    SleepMs(100);
    NvtReadAt(EVENT_BUF_ADDR | EVENT_MAP_FWINFO, B, 38);
    NvtLog("fw ver %02x/%02x x_num %u y_num %u abs %u x %u\r\n", B[0], B[1], B[2], B[3],
           (B[4] << 8) | B[5], (B[6] << 8) | B[7]);
    NvtDiag("after fw");

    NvtTimerResOn(Ctx);
    Timeout.QuadPart = -80000;   /* 8 мс */
#ifdef NVT_WDT_STATS
    StartMs = KeQueryPerformanceCounter(NULL).QuadPart * 1000 / Freq.QuadPart;
    NextStatMs = StartMs + 60000;
#ifdef NVT_FIX_IDLEPOLL
    LastActiveMs = StartMs;
    NvtLog("idlepoll: after %u ms idle poll every %u ms\r\n", NVT_IDLE_AFTER_MS, NVT_IDLE_POLL_MS);
#endif
#endif
#ifdef NVT_FIX_DISP
    InterlockedExchange(&Ctx->PollArmed, 1);   /* gate может гасить gMmioOff и парковать нас */
#endif
#ifdef NVT_FIX_D0
    while (NvtWaitTick(Ctx, &Timeout)) {
#else
    while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT) {
#endif
#ifdef NVT_WDT_STATS
        Polls++;
        NowMs = KeQueryPerformanceCounter(NULL).QuadPart * 1000 / Freq.QuadPart;
        if (NowMs >= NextStatMs) {
            NextStatMs = NowMs + 60000;
            NvtLog("wdtstat: up %u s polls %u (idle-rate %u) frames %u touch %u bad %u | wdt resets %u\r\n",
                   (ULONG)((NowMs - StartMs) / 1000), Polls, SlowPolls, Frames, TouchFrames, BadFrames, Reloads);
        }
#endif
#ifdef NVT_FIX_IDLEPOLL
        /* темп следующего ожидания; решение на входе витка, чтобы работало и на «мусорных» кадрах (continue) */
        if (WdtRun == 0 && NowMs - LastActiveMs >= NVT_IDLE_AFTER_MS) {
            Timeout.QuadPart = -(LONGLONG)NVT_IDLE_POLL_MS * 10000;
            NvtTimerResOff(Ctx);   /* 60 мс квантуется и так: 1 мс в простое держит DRIPS=0 */
            SlowPolls++;
        } else {
            Timeout.QuadPart = -80000;
            NvtTimerResOn(Ctx);    /* активность —1 мс снова нужен опросу */
        }
#endif
        /* статистика: каждые ~2 с первые 2 минуты, потом каждые ~20 с */
        Tick++;
#ifndef NVT_RELEASE
        if (Tick % (Tick < 15000 ? 250u : 2500u) == 0) {
            NvtLog("stat: frames %u valid %u idle %u bad %u | reports %u sent %d dropped %d reads %d | %02x %02x %02x %02x %02x %02x %02x\r\n",
                   Frames, Valid, Idle, BadFrames, Reports, gSent, gDropped, gReadsIn,
                   P[0], P[1], P[2], P[3], P[4], P[5], P[64]);
            NvtDiag("stat");
        }
#endif

        /*
          Прошивка сброшена WDT (как nvt_wdt_fw_recovery Xiaomi: >10 кадров FD/FE подряд) —
          перезаливаем. Постоянный «мусор» до первого касания после загрузки прошивки — норма.
        */
        if (WdtRun >= NVT_WDT_FRAMES) {
            if (gMmioOff) continue;   /* D0Exit/disp-off: перезаливку не начинаем, идём парковаться */
            Reloads++;
#ifdef NVT_WDT_STATS
            if (Reloads <= 2000 || Reloads % 100 == 0) {
#else
            if (Reloads <= 5 || Reloads % 100 == 0) {
#endif
                NvtDiag("fw wdt reset");
            }

            BadRun = 0;
            WdtRun = 0;
#ifdef NVT_FIX_IDLEPOLL
            LastActiveMs = KeQueryPerformanceCounter(NULL).QuadPart * 1000 / Freq.QuadPart;
            Timeout.QuadPart = -80000;
            NvtTimerResOn(Ctx);    /* перезаливка = активность */
#endif
            BbSetup();
            for (Try = 0; Try < 3 && !NvtDownloadOnce(gNvtFw); Try++) {
            }

            SleepMs(100);
            NvtReadAt(EVENT_BUF_ADDR | EVENT_MAP_FWINFO, B, 8);
#ifdef NVT_WDT_STATS
            if (Reloads <= 2000 || Reloads % 100 == 0) {
#else
            if (Reloads <= 5 || Reloads % 100 == 0) {
#endif
                NvtLog("reload %u: %s, fw ver %02x/%02x abs %u x %u\r\n", Reloads, Try < 3 ? "ok" : "FAILED",
                       B[0], B[1], (B[4] << 8) | B[5], (B[6] << 8) | B[7]);
            }
            continue;
        }

        {
            ATOMIC_BEGIN();     /* SetPage + чтение кадра — одно неделимое окно (~0.6 мс) */
            NvtSetPage(EVENT_BUF_ADDR);
            NvtReadRaw(0x00, P, POINT_DATA_LEN);
            ATOMIC_END();
        }
        if (gMmioOff) continue;   /* D0Exit: не разбираем кадр, идём парковаться в NvtWaitTick */

        Frames++;
        for (i = 0; i < 6 && (P[i] == 0xFD || P[i] == 0xFE); i++) {
        }

        WdtRun = i == 6 ? WdtRun + 1 : 0;

        /* смена типа кадров: 0 касание, 1 простой (FF), 2 сброс (FD/FE), 3 прочий мусор */
        Sum = 0;
        for (f = 0; f < POINT_DATA_LEN - 1; f++) {
            Sum += P[f];
        }

        Kind = i == 6 ? 2 : (UCHAR)(~Sum + 1) != P[POINT_DATA_LEN - 1] ? 3 : (P[0] == 0xFF && P[1] == 0xFF) ? 1 : 0;
        if (Kind != LastKind) {
            if (Changes++ < 200) {
                NvtLog("frames: %s\r\n", Kind == 0 ? "touch" : Kind == 1 ? "idle" : Kind == 2 ? "FD/FE (fw reset)" : "garbage");
            }

            LastKind = Kind;
        }

        RtlZeroMemory(Now, sizeof(Now));
        Sum = 0;
        for (i = 0; i < POINT_DATA_LEN - 1; i++) {
            Sum += P[i];
        }

        if ((UCHAR)(~Sum + 1) == P[POINT_DATA_LEN - 1]) {
            BadRun = 0;
            if (P[0] == 0xFF && P[1] == 0xFF) {
                Idle++;
            } else {
                Valid++;
            }

            for (f = 0; f < MAX_FINGERS; f++) {
                Id = P[6 * f] >> 3;
                St = P[6 * f] & 7;
                if (Id == 0 || Id > MAX_FINGERS || (St != 1 && St != 2)) {
                    continue;
                }

                Now[Id - 1] = TRUE;
                X[Id - 1]   = (USHORT)((P[6 * f + 1] << 4) | (P[6 * f + 3] >> 4));
                Y[Id - 1]   = (USHORT)((P[6 * f + 2] << 4) | (P[6 * f + 3] & 0x0F));
                if (X[Id - 1] > 1599) X[Id - 1] = 1599;
                if (Y[Id - 1] > 2559) Y[Id - 1] = 2559;
            }
        } else if (P[0] != 0xFF || P[1] != 0xFF) {
            BadFrames++;    /* рваный кадр: пропускаем, состояние не меняем */
            BadRun++;
#ifdef NVT_RELEASE
            UNREFERENCED_PARAMETER(Dumps);
#else
            if (Dumps < 6 && (BadFrames == 1 || BadFrames == 2 || BadFrames == 50 || BadFrames == 300 || BadFrames == 1000 || BadFrames == 3000)) {
                Dumps++;
                NvtDump("bad", P, POINT_DATA_LEN);
            }
#endif

            continue;
        }

        RtlZeroMemory(&Rep, sizeof(Rep));
        Rep.ReportId = REPORT_ID_TOUCH;
        n = 0;
        for (i = 0; i < MAX_FINGERS; i++) {
            if (Now[i] || Down[i]) {
                Rep.C[n].Tip = Now[i] ? 1 : 0;
                Rep.C[n].Id  = (UCHAR)i;
                Rep.C[n].X   = X[i];
                Rep.C[n].Y   = Y[i];
                n++;
            }

            Down[i] = Now[i];
        }

#ifdef NVT_WDT_STATS
        if (Kind == 0) {
            TouchFrames++;
        }
#endif

#ifdef NVT_FIX_IDLEPOLL
        /* активность = валидный кадр с касанием (мусор ff90.. и FF-простой не считаются) или палец ещё прижат */
        if (n || Kind == 0) {
            LastActiveMs = NowMs;
        }
#endif

        if (n) {
            Rep.Count    = (UCHAR)n;
            Rep.ScanTime = (USHORT)(KeQueryPerformanceCounter(NULL).QuadPart * 10000 / Freq.QuadPart);
            SendReport(Ctx, &Rep);
            if (Reports++ == 0) {
                RegMark(L"Stage", 7);
            }

            if (Reports <= 40) {
                NvtLog("report %u: n %u [%u:%u %u,%u] [%u:%u %u,%u] raw %02x %02x %02x %02x\r\n", Reports, n,
                       Rep.C[0].Id, Rep.C[0].Tip, Rep.C[0].X, Rep.C[0].Y,
                       Rep.C[1].Id, Rep.C[1].Tip, Rep.C[1].X, Rep.C[1].Y, P[0], P[1], P[2], P[3]);
            }
        }
    }

#ifdef NVT_FIX_DISP
    InterlockedExchange(&Ctx->PollArmed, 0);
#endif
    NvtTimerResOff(Ctx);
    NvtLog("poll stop: reports %u, bad frames %u\r\n", Reports, BadFrames);

out:
    BbRestore();
#ifdef NVT_FIX_ATOMIC
    KeRevertToUserAffinityThreadEx(OldAff);
#endif
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------- WDF ---------------- */

static VOID
EvtIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    PDEVICE_CONTEXT       Ctx = GetCtx(WdfIoQueueGetDevice(Queue));
    NTSTATUS              Status = STATUS_SUCCESS;
    HID_DEVICE_ATTRIBUTES Attr;
    PHID_XFER_PACKET      Packet;

    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    if (Code == IOCTL_HID_READ_REPORT) {
        gReadsIn++;
    } else {
        NvtLog("ioctl %08x out %u in %u\r\n", Code, (ULONG)OutLen, (ULONG)InLen);
    }

    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        Status = CopyToRequest(Request, &gHidDesc, sizeof(gHidDesc));
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        RtlZeroMemory(&Attr, sizeof(Attr));
        Attr.Size          = sizeof(Attr);
        Attr.VendorID      = 0x0603;
        Attr.ProductID     = 0x3652;
        Attr.VersionNumber = 0x0001;
        Status = CopyToRequest(Request, &Attr, sizeof(Attr));
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        Status = CopyToRequest(Request, gReportDesc, sizeof(gReportDesc));
        break;

    case IOCTL_HID_READ_REPORT:
        Status = WdfRequestForwardToIoQueue(Request, Ctx->ReadQueue);
        if (NT_SUCCESS(Status)) {
            return;
        }

        break;

    case IOCTL_HID_GET_FEATURE:
        Packet = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
        NvtLog("get feature packet %p id %u len %u\r\n", Packet, Packet ? Packet->reportId : 0, Packet ? Packet->reportBufferLen : 0);
        if (Packet == NULL || Packet->reportBufferLen < 2 || Packet->reportId != REPORT_ID_MAXCOUNT) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        Packet->reportBuffer[0] = REPORT_ID_MAXCOUNT;
        Packet->reportBuffer[1] = MAX_FINGERS;
        WdfRequestSetInformation(Request, 2);
        break;

    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestComplete(Request, Status);
}

static NTSTATUS
EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT   Ctx = GetCtx(Device);
    PHYSICAL_ADDRESS  Pa;
    HANDLE            Thread;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS          Status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    RegMark(L"Stage", 4);
    NvtLog("PrepareHardware\r\n");
    Pa.QuadPart = TLMM_GPIO8_PA;
    Ctx->TlmmVa = MmMapIoSpaceEx(Pa, TLMM_MAP_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->TlmmVa == NULL) {
        NvtLog("MmMapIoSpaceEx failed\r\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    gTlmm = (volatile ULONG *)Ctx->TlmmVa;
    KeInitializeEvent(&Ctx->StopEvent, NotificationEvent, FALSE);
#ifdef NVT_FIX_D0
    KeInitializeEvent(&Ctx->PauseEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Ctx->ResumeEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Ctx->ParkedEvent, NotificationEvent, FALSE);
#endif
    InitializeObjectAttributes(&Oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &Oa, NULL, NULL, PollThread, Ctx);
    if (!NT_SUCCESS(Status)) {
        NvtLog("PsCreateSystemThread failed %08x\r\n", Status);
        return Status;
    }

    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &Ctx->Thread, NULL);
    NvtLog("PrepareHardware: tlmm va %p, thread started\r\n", Ctx->TlmmVa);
    ZwClose(Thread);
#ifdef NVT_FIX_DISP
    /* Первый вызов callback приходит сразу при регистрации (м.б. внутри PoRegister): к этому
       моменту контекст/события/поток готовы, CbLive уже выставлен. */
    Ctx->CbLive = 1;
    Status = PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device), &NvtGuidConsoleDisplayState,
                                            NvtDispCallback, Ctx, &Ctx->PowerCb);
    if (!NT_SUCCESS(Status)) {
        Ctx->PowerCb = NULL;
        Ctx->CbLive  = 0;
        NvtLog("disp: PoRegisterPowerSettingCallback failed %08x\r\n", Status);
    } else {
        NvtLog("disp: display power callback registered\r\n");
    }

#endif
    return STATUS_SUCCESS;
}

#ifdef NVT_FIX_D0
static NTSTATUS
EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT Ctx = GetCtx(Device);
    LARGE_INTEGER   W;

    UNREFERENCED_PARAMETER(TargetState);
    if (Ctx->Thread != NULL) {
        InterlockedExchange(&gMmioOff, 1);   /* MMIO/SPI запрещены ДО паузы: поток допишет текущий байт и остановится */
#ifdef NVT_FIX_DISP
        if (NvtGateSet(Ctx, 0, -1) == 0) {
            return STATUS_SUCCESS;   /* уже припаркованы по экрану — повторно не ждём */
        }
#endif
        NvtTimerResOff(Ctx);                 /* 1 мс снять уже здесь — даже если поток не успел припарковаться */
#ifndef NVT_FIX_DISP
        KeClearEvent(&Ctx->ParkedEvent);
        KeClearEvent(&Ctx->ResumeEvent);
        KeSetEvent(&Ctx->PauseEvent, IO_NO_INCREMENT, FALSE);
#endif
        W.QuadPart = -10 * 1000 * 500;   /* до 500 мс: поток может быть посреди SPI-транзакции */
        if (KeWaitForSingleObject(&Ctx->ParkedEvent, Executive, KernelMode, FALSE, &W) == STATUS_TIMEOUT) {
            NvtLog("d0: park timeout, retry\r\n");
            W.QuadPart = -10 * 1000 * 1500;   /* повторное ожидание: поток мог быть в SleepMs заливки/перезаливки */
            if (KeWaitForSingleObject(&Ctx->ParkedEvent, Executive, KernelMode, FALSE, &W) == STATUS_TIMEOUT) {
                NvtLog("d0: park timeout (hard); mmio flag keeps thread off hardware\r\n");
            }
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT Ctx = GetCtx(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (Ctx->Thread != NULL) {
#ifdef NVT_FIX_DISP
        /* возобновляем только при включённом экране (гейт D0Active && DispOn) */
        (VOID)NvtGateSet(Ctx, 1, -1);
#else
        InterlockedExchange(&gMmioOff, 0);   /* сначала разрешить MMIO, затем будить */
        KeClearEvent(&Ctx->PauseEvent);
        KeSetEvent(&Ctx->ResumeEvent, IO_NO_INCREMENT, FALSE);
#endif
    }
    return STATUS_SUCCESS;
}
#endif

static NTSTATUS
EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT Ctx = GetCtx(Device);

    UNREFERENCED_PARAMETER(Translated);

#ifdef NVT_FIX_DISP
    /* Снять callback ДО остановки потока: новых событий экрана не будет. CbLive=0 под замком
       отсекает опоздавшие вызовы; PoUnregisterPowerSettingCallback выполняется на PASSIVE. */
    {
        KIRQL Old;
        PVOID Handle;

        KeAcquireSpinLock(&Ctx->StateLock, &Old);
        Ctx->CbLive = 0;
        Handle = Ctx->PowerCb;
        Ctx->PowerCb = NULL;
        KeReleaseSpinLock(&Ctx->StateLock, Old);
        if (Handle != NULL) {
            PoUnregisterPowerSettingCallback(Handle);
        }
    }
#endif
    if (Ctx->Thread != NULL) {
        KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }

    if (Ctx->TlmmVa != NULL) {
        gTlmm = NULL;
        MmUnmapIoSpace(Ctx->TlmmVa, TLMM_MAP_LEN);
        Ctx->TlmmVa = NULL;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_IO_QUEUE_CONFIG          Qc;
    WDFDEVICE                    Device;
    WDFQUEUE                     Queue;
    PDEVICE_CONTEXT              Ctx;
    NTSTATUS                     Status;

    UNREFERENCED_PARAMETER(Driver);
    RegMark(L"Stage", 3);
    NvtLog("DeviceAdd\r\n");

    /* над нами mshidkmdf: он FDO для hidclass, мы фильтр без power policy */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
#ifdef NVT_FIX_D0
    Pnp.EvtDeviceD0Entry = EvtD0Entry;
    Pnp.EvtDeviceD0Exit  = EvtD0Exit;
#endif
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = GetCtx(Device);
#ifdef NVT_FIX_DISP
    KeInitializeSpinLock(&Ctx->StateLock);
    Ctx->D0Active  = 1;   /* уточняется EvtD0Entry/EvtD0Exit */
    Ctx->DispOn    = 1;   /* уточняется первым же callback сразу при регистрации */
    Ctx->GateUp    = 1;
    Ctx->PollArmed = 0;
    Ctx->CbLive    = 0;
    Ctx->PowerCb   = NULL;
#endif

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Qc, WdfIoQueueDispatchParallel);
    Qc.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    Status = WdfIoQueueCreate(Device, &Qc, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&Qc, WdfIoQueueDispatchManual);
    Qc.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &Qc, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->ReadQueue);
    return Status;
}

NTSTATUS FxDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);

/* Точка входа образа: метка до привязки KMDF, затем штатный FxDriverEntry */
NTSTATUS
NvtImageEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;

    (VOID)gSizePad[0];
    gRegPath.Buffer        = gRegPathBuf;
    gRegPath.MaximumLength = sizeof(gRegPathBuf);
    RtlCopyUnicodeString(&gRegPath, RegistryPath);
    RegMark(L"Stage", 1);
    Status = FxDriverEntry(DriverObject, RegistryPath);
    RegMark(L"FxEntryStatus", (ULONG)Status);
    return Status;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS          Status;

    RegMark(L"Stage", 2);
    NvtLog("DriverEntry\r\n");
    WDF_DRIVER_CONFIG_INIT(&Config, EvtDeviceAdd);
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    RegMark(L"DriverCreateStatus", (ULONG)Status);
    NvtLog("WdfDriverCreate %08x\r\n", Status);
    return Status;
}

/*
 * elishotg.sys — one-shot DWC3/QSCRATCH Host-mode helper for Xiaomi Pad 5 Pro (elish, SM8250).
 *
 * On SM8250 (elish), the USB-C port is physically wired as USB 2.0 only (no SuperSpeed/DP lanes).
 * When switching ACPI\QCOM0597\0 (urssynopsys) to Host mode (RoleSwitchMode = 1), standard Windows
 * usbxhci.sys starts and sees the port, but USB 2.0 enumeration hangs in DeviceEnumerating (status 9)
 * until two QSCRATCH (0x0A6F8800) hardware workarounds from Linux dwc3-qcom.c are applied:
 *   1. Clear Device-mode VBUS override bits in QSCRATCH_HS_PHY_CTRL (0x10) & QSCRATCH_SS_PHY_CTRL (0x30).
 *   2. Select UTMI clock as PIPE clock in QSCRATCH_GENERAL_CFG (0x08):
 *      PIPE_UTMI_CLK_DIS -> PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW -> clear PIPE_UTMI_CLK_DIS.
 *
 * Design: strictly one-shot in DriverEntry (no background threads, no polling -> zero NoC hang risk),
 * immediate MmUnmapIoSpace, clean DriverUnload (sc stop elishotg).
 */
#include <ntddk.h>
#include <ntstrsafe.h>

#define DWC3_PA      0x0A600000ULL
#define DWC3_LEN     0x10000
#define QSCRATCH_PA  0x0A6F8800ULL

#define GCTL         0xC110
#define QS_GEN_CFG   0x08
#define QS_HS_CTRL   0x10
#define QS_SS_CTRL   0x30

#define HS_VBUS_BITS       ((1u << 20) | (1u << 28))
#define SS_PWR_PRESENT     (1u << 24)
#define PIPE_UTMI_CLK_SEL  (1u << 0)
#define PIPE3_PHYSTATUS_SW (1u << 3)
#define PIPE_UTMI_CLK_DIS  (1u << 8)

static ULONG Rd(volatile UCHAR *Base, ULONG Off) {
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}
static VOID Wr(volatile UCHAR *Base, ULONG Off, ULONG Val) {
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

static VOID LogLine(_In_z_ PCSTR Fmt, ...) {
    CHAR              Line[256];
    va_list           Args;
    UNICODE_STRING    Path = RTL_CONSTANT_STRING(L"\\SystemRoot\\elishotg.log");
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    size_t            Len;

    va_start(Args, Fmt);
    RtlStringCbVPrintfA(Line, sizeof(Line), Fmt, Args);
    va_end(Args);
    RtlStringCbLengthA(Line, sizeof(Line), &Len);

    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&File, FILE_APPEND_DATA | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(File, NULL, NULL, NULL, &Iosb, Line, (ULONG)Len, NULL, NULL);
        ZwClose(File);
    }
}

static VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    LogLine("elishotg: unloaded cleanly\r\n");
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    PHYSICAL_ADDRESS Pa;
    volatile UCHAR *Dwc = NULL, *QsPage = NULL, *Qs = NULL;

    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = DriverUnload;

    Pa.QuadPart = DWC3_PA;
    Dwc = (volatile UCHAR *)MmMapIoSpaceEx(Pa, DWC3_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = QSCRATCH_PA & ~0xFFFULL;
    QsPage = (volatile UCHAR *)MmMapIoSpaceEx(Pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Dwc || !QsPage) {
        LogLine("elishotg: MmMapIoSpaceEx failed\r\n");
        if (Dwc) MmUnmapIoSpace((PVOID)Dwc, DWC3_LEN);
        if (QsPage) MmUnmapIoSpace((PVOID)QsPage, 0x1000);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Qs = QsPage + (QSCRATCH_PA & 0xFFF);

    ULONG gctl   = Rd(Dwc, GCTL);
    ULONG prtcap = (gctl >> 12) & 3;
    ULONG qgen   = Rd(Qs, QS_GEN_CFG);
    ULONG qhs    = Rd(Qs, QS_HS_CTRL);
    ULONG qss    = Rd(Qs, QS_SS_CTRL);
    LogLine("DWC3: prtcap=%u qs gen=%08x hs=%08x ss=%08x\r\n", prtcap, qgen, qhs, qss);

    if (prtcap == 1) {
        if ((qhs & HS_VBUS_BITS) || (qss & SS_PWR_PRESENT)) {
            Wr(Qs, QS_SS_CTRL, qss & ~SS_PWR_PRESENT);
            Wr(Qs, QS_HS_CTRL, qhs & ~HS_VBUS_BITS);
        }
        if ((qgen & (PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW)) != (PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW)) {
            ULONG v = qgen | PIPE_UTMI_CLK_DIS;
            Wr(Qs, QS_GEN_CFG, v); (void)Rd(Qs, QS_GEN_CFG); KeStallExecutionProcessor(500);
            v |= (PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
            Wr(Qs, QS_GEN_CFG, v); (void)Rd(Qs, QS_GEN_CFG); KeStallExecutionProcessor(500);
            v &= ~PIPE_UTMI_CLK_DIS;
            Wr(Qs, QS_GEN_CFG, v); (void)Rd(Qs, QS_GEN_CFG); KeStallExecutionProcessor(500);
            LogLine("DWC3: UTMI_CLK selected -> gen=%08x\r\n", Rd(Qs, QS_GEN_CFG));
        }
    }

    MmUnmapIoSpace((PVOID)Dwc, DWC3_LEN);
    MmUnmapIoSpace((PVOID)QsPage, 0x1000);
    return STATUS_SUCCESS;
}

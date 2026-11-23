/*
 * elishtsens — температура TSENS Xiaomi Pad 5 Pro (elish) для Windows ARM64 (WDM, ROOT\ELISHTSENS).
 *
 * Два контроллера TSENS v2.4 (qcom,tsens24xx, live.dts:8002-8021):
 *   tsens0: SROT 0xC222000, TM 0xC263000 (gpuss-0 = ch 0x0f);
 *   tsens1: SROT 0xC223000, TM 0xC265000 (gpuss-1 = ch 0x08, ddr = ch 0x03).
 * Только чтение: никаких записей в регистры TSENS, прерываний и SCM (reinit делает TZ,
 * work\mimo\src-tsens.report.md). Формат: Sn_STATUS [11:0] — знаковый код 0.1 °C,
 * милли°C = code*100 (tsens2xxx.c:55-56, 73-83); бит21 VALID; TRDY бит3 — первый обход готов.
 * Значения пишутся в HKLM\SYSTEM\CurrentControlSet\Services\elishtsens\Readings:
 * DWORD на включённый сенсор (милли°C, со знаком), ValidMask (бит n = канал n: 0-15 tsens0,
 * 16-31 tsens1), Status ("ok"/"no-valid"/"not-ready"/"not-initialized").
 * Опрос раз в 2 с (work item на PASSIVE). Параметры службы: OneShot=1 — «шаг 0» (один опрос
 * и стоп), Guard=1 — защита от цикла падений (как elishbat).
 */
#include <ntddk.h>

#define TAG_POOL      'stse'
#define POLL_SEC      2
#define MAP_SZ        0x1000
#define NCH           16

#define TSENS0_SROT   0xC222000ull
#define TM0_BASE      0xC263000ull
#define TSENS1_SROT   0xC223000ull
#define TM1_BASE      0xC265000ull

/* регистры (tsens2xxx.c:15-72 = mainline tsens-v2.c:11-27) */
#define REG_CTRL      0x4       /* SROT: бит0 TSENS_EN, биты 3..18 маска сенсоров */
#define REG_TRDY      0xe4      /* TM: бит3 «first round complete» (Xiaomi) */
#define REG_SN        0xa0      /* TM+0xa0+4n: Sn_STATUS */
#define TSENS_EN_BIT  (1u << 0)
#define SENSOR_SHIFT  3
#define SENSOR_MASK   0xffffu
#define TRDY_BIT      (1u << 3)
#define SN_VALID_BIT  (1u << 21)
#define SN_CODE_MASK  0xfffu
#define SN_SIGN_BIT   0x800u

/* имена каналов — thermal-zones live.dts:430-1010 (без -usr): tsens0 = phandle 0x19, tsens1 = 0x1a */
static const WCHAR *const kName[2][NCH] = {
    { L"aoss0", L"cpu-0-0", L"cpu-0-1", L"cpu-0-2", L"cpu-0-3", L"cpuss-0", L"cpuss-1",
      L"cpu-1-0", L"cpu-1-1", L"cpu-1-2", L"cpu-1-3", L"cpu-1-4", L"cpu-1-5", L"cpu-1-6",
      L"cpu-1-7", L"gpuss-0" },
    { L"aoss-1", L"cwlan", L"video", L"ddr", L"q6-hvx", L"camera", L"cmpss", L"npu",
      L"gpuss-1", L"ts1-ch09", L"ts1-ch0a", L"ts1-ch0b", L"ts1-ch0c", L"ts1-ch0d",
      L"ts1-ch0e", L"ts1-ch0f" }
};

typedef struct {
    PUCHAR  Srot, Tm;
    BOOLEAN Online;   /* TSENS_EN = 1 */
    BOOLEAN Ready;    /* TRDY бит3 */
    ULONG   Mask;     /* включённые сенсоры: бит n = канал n */
} TCTRL;

typedef struct {
    PDEVICE_OBJECT Self, Lower, Pdo;
    TCTRL          Tc[2];
    HANDLE         RdKey;
    KTIMER         Timer;
    KDPC           Dpc;
    PIO_WORKITEM   Work;
    volatile LONG  WorkBusy;
    ULONG          GoodPolls;
    BOOLEAN        Started, OneShot;
} EXT, *PEXT;

DRIVER_INITIALIZE DriverEntry;
static UNICODE_STRING gReg;

/* ---------------- реестр ---------------- */
static ULONG RegGetDword(const WCHAR *Name)   /* запрос DWORD из ключа службы */
{
    RTL_QUERY_REGISTRY_TABLE Q[2];
    ULONG V = 0, Def = 0;
    RtlZeroMemory(Q, sizeof(Q));
    Q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q[0].Name = (PWSTR)Name; Q[0].EntryContext = &V;
    Q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Q[0].DefaultData = &Def; Q[0].DefaultLength = sizeof(ULONG);
    if (!gReg.Buffer || !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, gReg.Buffer, Q, NULL, NULL))) return 0;
    return V;
}

/* Защита от цикла падений: Guard=1 пишется до чтения железа и снимается после 3 удачных опросов.
   Если при старте Guard уже 1 — прошлый запуск уронил систему, устройство не стартует (сброс: Guard=0). */
static VOID GuardSet(ULONG V)
{
    OBJECT_ATTRIBUTES Oa;
    HANDLE H;
    UNICODE_STRING N = RTL_CONSTANT_STRING(L"Guard");
    if (!gReg.Buffer) return;
    InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&H, KEY_SET_VALUE, &Oa))) return;
    ZwSetValueKey(H, &N, 0, REG_DWORD, &V, sizeof(V));
    ZwFlushKey(H);
    ZwClose(H);
}

static VOID WriteDword(HANDLE H, const WCHAR *Name, ULONG V)
{
    UNICODE_STRING N;
    RtlInitUnicodeString(&N, Name);
    ZwSetValueKey(H, &N, 0, REG_DWORD, &V, sizeof(V));
}
static VOID WriteSz(HANDLE H, const WCHAR *Name, const WCHAR *Val)
{
    UNICODE_STRING N, S;
    RtlInitUnicodeString(&N, Name);
    RtlInitUnicodeString(&S, Val);
    ZwSetValueKey(H, &N, 0, REG_SZ, S.Buffer, S.Length + sizeof(WCHAR));
}

/* ---------------- железо (только чтение) ---------------- */
static PUCHAR MapWin(ULONG64 Pa)
{
    PHYSICAL_ADDRESS P;
    P.QuadPart = (LONGLONG)Pa;
    return (PUCHAR)MmMapIoSpaceEx(P, MAP_SZ, PAGE_READONLY | PAGE_NOCACHE);
}
static ULONG Rd32(PUCHAR Base, ULONG Off)
{
    return *(volatile ULONG *)(Base + Off);
}
static LONG CodeToMilli(ULONG Code)   /* 12 бит со знаком (0x800), деци°C → милли°C */
{
    LONG C = (LONG)(Code & SN_CODE_MASK);
    if (C & SN_SIGN_BIT) C |= ~(LONG)SN_CODE_MASK;
    return C * 100;
}
static VOID HwFree(PEXT E)
{
    int t;
    for (t = 0; t < 2; t++) {
        if (E->Tc[t].Srot) { MmUnmapIoSpace(E->Tc[t].Srot, MAP_SZ); E->Tc[t].Srot = NULL; }
        if (E->Tc[t].Tm)   { MmUnmapIoSpace(E->Tc[t].Tm, MAP_SZ);   E->Tc[t].Tm = NULL; }
    }
}

/* ---------------- опрос ---------------- */
/* Один проход: DWORD на каждый включённый сенсор + ValidMask + Status (всё в Readings).
   Возвращает AnyValid (хотя бы один сенсор с VALID) — для Guard-счётчика. */
static BOOLEAN DoPoll(PEXT E)
{
    ULONG t, ch, n, code, vm = 0;
    BOOLEAN AnyValid = FALSE, AnyReady = FALSE;
    for (t = 0; t < 2; t++) {
        TCTRL *T = &E->Tc[t];
        if (!T->Online) continue;
        T->Ready = (Rd32(T->Tm, REG_TRDY) & TRDY_BIT) != 0;   /* Xiaomi: TRDY перед чтением */
        if (!T->Ready) continue;
        AnyReady = TRUE;
        for (ch = 0; ch < NCH; ch++) {
            BOOLEAN Valid = FALSE;
            if (!(T->Mask & (1u << ch))) continue;            /* только включённые CTRL */
            code = 0;
            for (n = 0; n < 3; n++) {                          /* до 3 перечитываний */
                code = Rd32(T->Tm, REG_SN + 4 * ch);
                if (code & SN_VALID_BIT) { Valid = TRUE; break; }
            }
            WriteDword(E->RdKey, kName[t][ch], (ULONG)CodeToMilli(code));
            if (Valid) { vm |= 1u << (ch + NCH * t); AnyValid = TRUE; }
        }
    }
    WriteDword(E->RdKey, L"ValidMask", vm);
    WriteSz(E->RdKey, L"Status", AnyValid ? L"ok" : (AnyReady ? L"no-valid" : L"not-ready"));
    return AnyValid;
}

static VOID PollWork(PDEVICE_OBJECT Dev, PVOID Ctx)
{
    PEXT E = (PEXT)Ctx;
    UNREFERENCED_PARAMETER(Dev);
    if (E->Started) {                                        /* остановлен — железо не трогаем */
        if (DoPoll(E) && E->GoodPolls < 3 && ++E->GoodPolls == 3) GuardSet(0);
    }
    InterlockedExchange(&E->WorkBusy, 0);
}
static VOID PollDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    PEXT E = (PEXT)Ctx;
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    if (InterlockedCompareExchange(&E->WorkBusy, 1, 0) != 0) return;   /* work уже идёт */
    if (!E->Started) { InterlockedExchange(&E->WorkBusy, 0); return; } /* Stop: не ставим work */
    IoQueueWorkItem(E->Work, PollWork, DelayedWorkQueue, E);
}

/* ---------------- старт/стоп ---------------- */
static NTSTATUS Start(PEXT E)
{
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING Rd = RTL_CONSTANT_STRING(L"Readings");
    LARGE_INTEGER Due;
    HANDLE Sv = NULL;
    NTSTATUS St;
    int i;

    if (RegGetDword(L"Guard")) return STATUS_DEVICE_CONFIGURATION_ERROR;
    GuardSet(1);
    E->Tc[0].Srot = MapWin(TSENS0_SROT); E->Tc[0].Tm = MapWin(TM0_BASE);
    E->Tc[1].Srot = MapWin(TSENS1_SROT); E->Tc[1].Tm = MapWin(TM1_BASE);
    if (!E->Tc[0].Srot || !E->Tc[0].Tm || !E->Tc[1].Srot || !E->Tc[1].Tm) {
        HwFree(E); GuardSet(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* при старте: SROT+0x4 (TSENS_EN, маска сенсоров) и TM+0xe4 TRDY; TM читаем только при Online */
    for (i = 0; i < 2; i++) {
        ULONG Ctl = Rd32(E->Tc[i].Srot, REG_CTRL);
        E->Tc[i].Online = (Ctl & TSENS_EN_BIT) != 0;
        E->Tc[i].Mask = (Ctl >> SENSOR_SHIFT) & SENSOR_MASK;
        E->Tc[i].Ready = E->Tc[i].Online && (Rd32(E->Tc[i].Tm, REG_TRDY) & TRDY_BIT) != 0;
    }

    /* подключный ключ Readings */
    if (!gReg.Buffer) { HwFree(E); GuardSet(0); return STATUS_INVALID_PARAMETER; }
    InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    St = ZwOpenKey(&Sv, KEY_CREATE_SUB_KEY, &Oa);
    if (NT_SUCCESS(St)) {
        InitializeObjectAttributes(&Oa, &Rd, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Sv, NULL);
        St = ZwCreateKey(&E->RdKey, KEY_SET_VALUE, &Oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
        ZwClose(Sv);
    }
    if (!NT_SUCCESS(St)) { HwFree(E); GuardSet(0); return St; }

    E->OneShot = RegGetDword(L"OneShot") != 0;

    if (!E->Tc[0].Online && !E->Tc[1].Online) {
        /* TSENS_EN=0: без чтения TM, без таймера — честно «not-initialized» (SCM-reinit не делаем) */
        WriteDword(E->RdKey, L"ValidMask", 0);
        WriteSz(E->RdKey, L"Status", L"not-initialized");
        ZwClose(E->RdKey); E->RdKey = NULL;
        HwFree(E);
        GuardSet(0);
        return STATUS_SUCCESS;
    }

    DoPoll(E);
    if (E->OneShot) {
        /* «шаг 0»: один опрос, таймер не запускаем */
        GuardSet(0);
        E->Started = TRUE;
        return STATUS_SUCCESS;
    }
    E->Started = TRUE;
    Due.QuadPart = -10000000LL * POLL_SEC;
    KeSetTimerEx(&E->Timer, Due, POLL_SEC * 1000, &E->Dpc);
    return STATUS_SUCCESS;
}
static VOID Stop(PEXT E)
{
    LARGE_INTEGER W;
    if (!E->Started) return;
    E->Started = FALSE;
    KeCancelTimer(&E->Timer);
    KeFlushQueuedDpcs();
    W.QuadPart = -10 * 1000 * 20;
    while (E->WorkBusy) KeDelayExecutionThread(KernelMode, FALSE, &W);
    HwFree(E);
    if (E->RdKey) { ZwClose(E->RdKey); E->RdKey = NULL; }
}

/* ---------------- WDM ---------------- */
static DRIVER_ADD_DEVICE AddDevice;
static DRIVER_UNLOAD Unload;
static DRIVER_DISPATCH DispPnp, DispPower, DispIoctl, DispCreateClose, DispPass;

static NTSTATUS AddDevice(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT Dev;
    PEXT E;
    NTSTATUS St = IoCreateDevice(Drv, sizeof(EXT), NULL, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &Dev);
    if (!NT_SUCCESS(St)) return St;
    E = (PEXT)Dev->DeviceExtension;
    RtlZeroMemory(E, sizeof(*E));
    E->Self = Dev; E->Pdo = Pdo;
    KeInitializeTimerEx(&E->Timer, NotificationTimer);
    KeInitializeDpc(&E->Dpc, PollDpc, E);
    E->Work = IoAllocateWorkItem(Dev);
    E->Lower = IoAttachDeviceToDeviceStack(Dev, Pdo);
    if (!E->Lower || !E->Work) {
        if (E->Work) IoFreeWorkItem(E->Work);
        if (E->Lower) IoDetachDevice(E->Lower);
        IoDeleteDevice(Dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    Dev->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
static NTSTATUS Complete(PIRP Irp, NTSTATUS St) { Irp->IoStatus.Status = St; IoCompleteRequest(Irp, IO_NO_INCREMENT); return St; }
static NTSTATUS DispPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PEXT E = (PEXT)Dev->DeviceExtension;
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS St;
    switch (Sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        St = IoForwardIrpSynchronously(E->Lower, Irp) ? Irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(St)) St = Start(E);
        return Complete(Irp, St);
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_STOP_DEVICE:
        Stop(E);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        Stop(E);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        St = IoCallDriver(E->Lower, Irp);
        IoFreeWorkItem(E->Work);
        IoDetachDevice(E->Lower);
        IoDeleteDevice(Dev);
        return St;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(E->Lower, Irp);
}
static NTSTATUS DispPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PEXT E = (PEXT)Dev->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(E->Lower, Irp);
}
static NTSTATUS DispPass(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PEXT E = (PEXT)Dev->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(E->Lower, Irp);
}
static NTSTATUS DispIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);   /* IOCTL нет — только реестр */
}
static NTSTATUS DispCreateClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    Irp->IoStatus.Information = 0;
    return Complete(Irp, STATUS_SUCCESS);
}
static VOID Unload(PDRIVER_OBJECT Drv) { UNREFERENCED_PARAMETER(Drv); if (gReg.Buffer) ExFreePoolWithTag(gReg.Buffer, TAG_POOL); gReg.Buffer = NULL; }

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Reg)
{
    gReg.MaximumLength = Reg->Length + sizeof(WCHAR);
    gReg.Buffer = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, gReg.MaximumLength, TAG_POOL);
    if (gReg.Buffer) RtlCopyUnicodeString(&gReg, Reg);
    Drv->DriverExtension->AddDevice = AddDevice;
    Drv->DriverUnload = Unload;
    Drv->MajorFunction[IRP_MJ_PNP] = DispPnp;
    Drv->MajorFunction[IRP_MJ_POWER] = DispPower;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispIoctl;
    Drv->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = DispPass;
    Drv->MajorFunction[IRP_MJ_CREATE] = DispCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = DispCreateClose;
    return STATUS_SUCCESS;
}

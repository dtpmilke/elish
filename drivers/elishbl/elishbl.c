/*
 * elishbl — яркость подсветки Xiaomi Pad 5 Pro (elish) в Windows ARM64 (не-PnP служба, автозапуск).
 *
 * Подсветка: KTZ8866 @0x11 на шине QUP1 SE11 (GPIO60 SDA / GPIO61 SCL, тайл SOUTH), программный I2C.
 * Панель и KTZ8866 включает загрузчик Xiaomi; драйвер пишет ТОЛЬКО яркость (11 бит: 0x04 = биты 2:0,
 * 0x05 = биты 10:3) и ничего больше — питание панели (ENP/ENN, HWEN) и BL_EN не трогает.
 *
 * Яркость задаётся в процентах: HKLM\SOFTWARE\elish\Brightness (DWORD 0..100, пишут обычные пользователи —
 * ACL ставит установщик). Драйвер следит за ключом и плавно меняет яркость. Лог: %SystemRoot%\elishbl.log
 * Services\elishbl\Mode: 0 — работа, 1 — только прочитать регистры KTZ8866 в лог (без записи).
 */
#include <ntddk.h>
#include <stdarg.h>

#define TAG_POOL   'lbse'
#define TLMM_S     0x0F500000ull
#define KTZ_ADDR   0x11
#define BRT_MAX    2047
#define BRT_MIN    4

static volatile ULONG *gSda, *gScl;
static ULONG   gSvSda, gSvScl;
static PVOID   gMapSda, gMapScl;
static PETHREAD gThread;
static KEVENT  gStop;
static ULONG   gCur = 0xFFFFFFFF;
static KEVENT  gLock;             /* замок на SynchronizationEvent: под ним остаёмся на PASSIVE_LEVEL с APC,
                                     в отличие от FAST_MUTEX (APC_LEVEL, APC заблокированы) */
static KEVENT  gWake;             /* сигнал из \Callback\PowerState — восстановление яркости делает Worker */
static KEVENT  gQcEv;             /* сигнал из \Callback\QCDXDL_BACKLIGHT_CHANGE */
static volatile LONG gQcPct = -1; /* последний процент от qcdx (0..100), -1 если нет */
static volatile LONG gQcIdx, gQcRaw;
static BOOLEAN gQcGot;            /* TRUE после первого события от qcdx */
static volatile LONG gDispOff;    /* !=0: консоль погашена — в железо яркость не пишем */
static PVOID   gPwrReg1, gPwrReg2;
static PCALLBACK_OBJECT gPsObj;   /* \Callback\PowerState */
static PVOID   gPsReg;
static PCALLBACK_OBJECT gQcBlObj; /* \Callback\QCDXDL_BACKLIGHT_CHANGE */
static PVOID   gQcBlReg;
static PCALLBACK_OBJECT gQcEvtObj;/* \Callback\QCDXDL_EVT_REGISTER */
static UNICODE_STRING gSvc;

NTSYSAPI NTSTATUS NTAPI ZwCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN);
NTSYSAPI NTSTATUS NTAPI ZwNotifyChangeKey(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
int __cdecl _snprintf(char *, size_t, const char *, ...);

static VOID ULog(const char *Fmt, ...)
{
    char Buf[512]; int N, Hdr; va_list Ap;
    UNICODE_STRING Name; OBJECT_ATTRIBUTES Oa; IO_STATUS_BLOCK Iosb; HANDLE H;
    LARGE_INTEGER Now, Local; TIME_FIELDS Tf;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* ZwCreateFile/ZwWriteFile/ZwClose требуют PASSIVE_LEVEL */
    KeQuerySystemTime(&Now); ExSystemTimeToLocalTime(&Now, &Local); RtlTimeToTimeFields(&Local, &Tf);
    Hdr = _snprintf(Buf, sizeof(Buf), "%02u:%02u:%02u.%03u ", Tf.Hour, Tf.Minute, Tf.Second, Tf.Milliseconds);
    va_start(Ap, Fmt); N = _vsnprintf(Buf + Hdr, sizeof(Buf) - Hdr - 3, Fmt, Ap); va_end(Ap);
    if (N < 0) N = (int)(sizeof(Buf) - Hdr - 3);
    N += Hdr; Buf[N++] = '\r'; Buf[N++] = '\n';
    RtlInitUnicodeString(&Name, L"\\SystemRoot\\elishbl.log");
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&H, FILE_APPEND_DATA | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(H, NULL, NULL, NULL, &Iosb, Buf, (ULONG)N, NULL, NULL);
        ZwClose(H);
    }
}

/* ---------------- программный I2C (open-drain через OE) ---------------- */
#define BB_T 3
static VOID  BbRel(volatile ULONG *P) { P[0] = 3u | (2u << 6); KeStallExecutionProcessor(BB_T); }
static VOID  BbLow(volatile ULONG *P) { P[1] = 0; P[0] = 3u | (2u << 6) | (1u << 9); KeStallExecutionProcessor(BB_T); }
static ULONG BbGet(volatile ULONG *P) { return P[1] & 1; }
static VOID  BbSclHigh(VOID)
{
    ULONG i;
    BbRel(gScl);
    for (i = 0; i < 2000 && !BbGet(gScl); i++) KeStallExecutionProcessor(5);
}
static VOID BbStart(VOID) { BbRel(gSda); BbSclHigh(); BbLow(gSda); BbLow(gScl); }
static VOID BbStop(VOID)  { BbLow(gSda); BbSclHigh(); BbRel(gSda); }
static BOOLEAN BbTx(UCHAR V)
{
    int i; ULONG Ack;
    for (i = 7; i >= 0; i--) { if (V & (1u << i)) BbRel(gSda); else BbLow(gSda); BbSclHigh(); BbLow(gScl); }
    BbRel(gSda); BbSclHigh(); Ack = !BbGet(gSda); BbLow(gScl);
    return (BOOLEAN)Ack;
}
static UCHAR BbRx(BOOLEAN Ack)
{
    int i; UCHAR V = 0;
    BbRel(gSda);
    for (i = 7; i >= 0; i--) { BbSclHigh(); if (BbGet(gSda)) V |= (UCHAR)(1u << i); BbLow(gScl); }
    if (Ack) BbLow(gSda); else BbRel(gSda);
    BbSclHigh(); BbLow(gScl); BbRel(gSda);
    return V;
}
static VOID BbRecover(VOID)
{
    int i;
    BbRel(gSda); BbRel(gScl);
    for (i = 0; i < 9 && !BbGet(gSda); i++) { BbLow(gScl); BbSclHigh(); }
    BbStop();
}
/* одна транзакция: пины на время передачи переводятся в GPIO, затем возвращаются как были (qup11) */
static BOOLEAN Xfer(UCHAR Reg, BOOLEAN Write, UCHAR *Val)
{
    KIRQL Old; BOOLEAN Ok = FALSE; int Try;
    ULONG SvA = gSda[0], SvC = gScl[0];
    for (Try = 0; Try < 3 && !Ok; Try++) {
        KeRaiseIrql(DISPATCH_LEVEL, &Old);
        BbRel(gSda); BbRel(gScl);
        if (!BbGet(gSda)) BbRecover();
        BbStart();
        if (BbTx(KTZ_ADDR << 1) && BbTx(Reg)) {
            if (Write) Ok = BbTx(*Val);
            else { BbStart(); if (BbTx((KTZ_ADDR << 1) | 1)) { *Val = BbRx(FALSE); Ok = TRUE; } }
        }
        BbStop();
        gSda[0] = SvA; gScl[0] = SvC;
        KeLowerIrql(Old);
    }
    return Ok;
}
static BOOLEAN RegRd(UCHAR R, UCHAR *V) { return Xfer(R, FALSE, V); }
static BOOLEAN RegWr(UCHAR R, UCHAR V)  { return Xfer(R, TRUE, &V); }

static BOOLEAN RawGet(ULONG *Raw)
{
    UCHAR Lo, Hi;
    if (!RegRd(0x04, &Lo) || !RegRd(0x05, &Hi)) return FALSE;
    *Raw = ((ULONG)Hi << 3) | (Lo & 7);
    return TRUE;
}
static BOOLEAN RawSet(ULONG Raw)
{
    return RegWr(0x04, (UCHAR)(Raw & 7)) && RegWr(0x05, (UCHAR)(Raw >> 3));
}

/* проценты -> 11 бит: сбалансированная кривая (50% линейная + 50% квадратичная) */
static ULONG PctToRaw(ULONG P) { if (P > 100) P = 100; return BRT_MIN + (BRT_MAX - BRT_MIN) * (P * 100 + P * P) / 20000; }
static ULONG RawToPct(ULONG R)
{
    ULONG P;
    for (P = 0; P < 100 && PctToRaw(P + 1) <= R; P++) ;
    return P;
}

/* GUID_CONSOLE_DISPLAY_STATE (0 off, 1 on, 2 dimmed) и GUID_MONITOR_POWER_ON (0/1) —
   как в elishusb.c:51-52; callback POWER_SETTING_CALLBACK вызывается на PASSIVE_LEVEL. */
static const GUID gGuidDisp = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
static const GUID gGuidMon  = { 0x02731015, 0x4510, 0x4526, { 0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea } };

static NTSTATUS PwrSettingCb(LPCGUID Setting, PVOID Value, ULONG Len, PVOID Ctx)
{
    ULONG Prev; int Msg = 0;                 /* 1 off ok, 2 off fail, 3 on ok, 4 on fail */
    UNREFERENCED_PARAMETER(Setting); UNREFERENCED_PARAMETER(Ctx);
    if (Value == NULL || Len < sizeof(ULONG)) return STATUS_SUCCESS;
    KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
    Prev = gCur;
    if (*(PULONG)Value == 0) {                     /* display off */
        if (!gDispOff) {
            gDispOff = 1;
            Msg = RawSet(0) ? 1 : 2;
        }
    } else {                                       /* on / dimmed */
        if (gDispOff) {
            gDispOff = 0;
            if (gCur != 0xFFFFFFFF) Msg = RawSet(gCur) ? 3 : 4;
        }
    }
    KeSetEvent(&gLock, 0, FALSE);
    /* лог ПОСЛЕ снятия замка: ULog (Zw*) нельзя вызывать под gLock */
    if (Msg == 1) ULog("display off -> подсветка 0 (было %u)", Prev);
    else if (Msg == 2) ULog("display off: яркость 0 не записалась");
    else if (Msg == 3) ULog("display on -> яркость %u", Prev);
    else if (Msg == 4) ULog("display on: восстановление %u не прошло", Prev);
    return STATUS_SUCCESS;
}

/* \Callback\PowerState (elishusb.c:73-82): Arg1 == PO_CB_SYSTEM_STATE_LOCK,
   Arg2: 0 — система покидает S0, 1 — вернулась в S0.
   ExNotifyCallback зовёт обработчик на IRQL до DISPATCH_LEVEL (wdm.h: CALLBACK_FUNCTION _IRQL_requires_same_,
   ExNotifyCallback _IRQL_requires_max_(DISPATCH_LEVEL)) — здесь только KeSetEvent; яркость пишет Worker. */
static VOID PsCb(PVOID Ctx, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Ctx);
    if ((ULONG_PTR)Arg1 == PO_CB_SYSTEM_STATE_LOCK && (ULONG_PTR)Arg2 == 1)
        KeSetEvent(&gWake, 0, FALSE);
}

/* \Callback\QCDXDL_BACKLIGHT_CHANGE: qcdx шлёт Arg1 = &{4, idx, percent, val}.
   Вызывается на IRQL до DISPATCH_LEVEL — только сохраняем поля и будим Worker. */
static VOID QcBlCb(PVOID Ctx, PVOID Arg1, PVOID Arg2)
{
    const ULONG *E = (const ULONG *)Arg1;
    ULONG Pct;
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Arg2);
    if (!E || E[0] != 4) return;
    Pct = E[2];
    if (Pct > 100) Pct = 100;
    InterlockedExchange(&gQcIdx, (LONG)E[1]);
    InterlockedExchange(&gQcRaw, (LONG)E[3]);
    InterlockedExchange(&gQcPct, (LONG)Pct);
    KeSetEvent(&gQcEv, 0, FALSE);
}

static VOID QcRegisterMask(VOID)
{
    ULONG Idx, Mask = 4;
    if (!gQcEvtObj) return;
    for (Idx = 1; Idx <= 2; Idx++) {
        ULONG I = Idx;
        ExNotifyCallback(gQcEvtObj, &I, &Mask);
    }
}
static VOID Ramp(ULONG To)
{
    LARGE_INTEGER W; int i, Msg = 0; LONG From; ULONG Cur; BOOLEAN Forced = FALSE;
    KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
    if (gDispOff) { gDispOff = 0; Forced = TRUE; }  /* яркость просили при погашенной консоли: пользователь
                                                       крутит яркость — значит экран включён, пишем в железо */
    Cur = gCur;
    if (Forced || Cur == 0xFFFFFFFF || Cur == To) {
        if (Forced || Cur != To) Msg = RawSet(To) ? 1 : 2;
        gCur = To;
        KeSetEvent(&gLock, 0, FALSE);
        if (Forced) ULog("ramp при gDispOff: экран считаем включённым, пишу %u", To);
        if (Msg == 2) ULog("запись яркости не прошла");
        return;
    }
    KeSetEvent(&gLock, 0, FALSE);
    From = (LONG)Cur;
    W.QuadPart = -10 * 1000 * 15;
    for (i = 1; i <= 10; i++) {
        ULONG V = (ULONG)(From + ((LONG)To - From) * i / 10);
        BOOLEAN Ok = FALSE, Stop = FALSE;
        KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        if (gDispOff) { Stop = TRUE; gCur = To; }   /* консоль погасла во время перехода: цель запомним */
        else Ok = RawSet(V);
        KeSetEvent(&gLock, 0, FALSE);
        if (Stop) { ULog("ramp прерван display off, цель %u запомнена", To); return; }
        if (!Ok) { ULog("запись яркости не прошла (%u)", V); break; }
        KeDelayExecutionThread(KernelMode, FALSE, &W);   /* пауза БЕЗ замка — callback не ждёт весь ramp */
    }
    KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
    gCur = To;
    KeSetEvent(&gLock, 0, FALSE);
}

/* ---------------- реестр ---------------- */
static BOOLEAN QueryDword(PCWSTR Path, PCWSTR Name, ULONG *Out)
{
    RTL_QUERY_REGISTRY_TABLE Q[2]; ULONG V = 0xFFFFFFFF, Def = 0xFFFFFFFF;
    RtlZeroMemory(Q, sizeof(Q));
    Q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q[0].Name = (PWSTR)Name; Q[0].EntryContext = &V;
    Q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Q[0].DefaultData = &Def; Q[0].DefaultLength = sizeof(ULONG);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, (PWSTR)Path, Q, NULL, NULL)) || V == 0xFFFFFFFF) return FALSE;
    *Out = V;
    return TRUE;
}
#define USER_KEY L"\\Registry\\Machine\\SOFTWARE\\elish"

static VOID Worker(PVOID Ctx)
{
    OBJECT_ATTRIBUTES Oa; UNICODE_STRING K = RTL_CONSTANT_STRING(USER_KEY);
    HANDLE H = NULL, Ev = NULL; PKEVENT EvObj = NULL; IO_STATUS_BLOCK Iosb;
    PVOID Wait[4]; KWAIT_BLOCK WaitBlocks[4]; NTSTATUS St; ULONG P; LARGE_INTEGER Tmo;
    UNREFERENCED_PARAMETER(Ctx);
    Tmo.QuadPart = -5LL * 1000 * 10000;
    InitializeObjectAttributes(&Oa, &K, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(St = ZwCreateKey(&H, KEY_NOTIFY | KEY_QUERY_VALUE | KEY_SET_VALUE, &Oa, 0, NULL, 0, NULL))) { ULog("ключ SOFTWARE\\elish: %08x", St); goto out; }
    if (!QueryDword(USER_KEY, L"Brightness", &P)) {          /* первый запуск: берём яркость загрузчика */
        UNICODE_STRING N = RTL_CONSTANT_STRING(L"Brightness");
        P = RawToPct(gCur);
        ZwSetValueKey(H, &N, 0, REG_DWORD, &P, sizeof(P));
        ULog("Brightness не задана, сохраняю текущую: %u%%", P);
    }
    InitializeObjectAttributes(&Oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateEvent(&Ev, EVENT_ALL_ACCESS, &Oa, SynchronizationEvent, FALSE))) goto out;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(Ev, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode, (PVOID *)&EvObj, NULL))) goto out;
    Wait[0] = &gStop; Wait[1] = EvObj; Wait[2] = &gWake; Wait[3] = &gQcEv;
    QcRegisterMask();
    for (;;) {
        if (QueryDword(USER_KEY, L"Brightness", &P)) {
            ULONG R = PctToRaw(P);
            /* gDispOff в условии: событие реестра при погашенной консоли — признак включённого экрана
               (страховка от «тёмного экрана навсегда»): Ramp снимет gDispOff и применит яркость */
            if (R != gCur || gDispOff) { Ramp(R); ULog("яркость %u%% -> %u", P > 100 ? 100 : P, R); }
        }
        St = ZwNotifyChangeKey(H, Ev, NULL, NULL, &Iosb, REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
        if (!NT_SUCCESS(St)) { ULog("ZwNotifyChangeKey %08x", St); break; }
        for (;;) {
            St = KeWaitForMultipleObjects(4, Wait, WaitAny, Executive, KernelMode, FALSE, gQcGot ? NULL : &Tmo, WaitBlocks);
            if (St == STATUS_WAIT_0) goto out;
            if (St == STATUS_WAIT_1) break;                     /* реестр: перечитать и переармировать */
            if (St == STATUS_TIMEOUT) {
                QcRegisterMask();
                continue;
            }
            if (St == STATUS_WAIT_2) {                          /* \Callback\PowerState: вернулись в S0 */
                int Msg = 0; ULONG Cur = 0;
                KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
                if (gDispOff) {
                    gDispOff = 0;
                    Cur = gCur;
                    Msg = (Cur == 0xFFFFFFFF) ? 0 : (RawSet(Cur) ? 1 : 2);
                }
                KeSetEvent(&gLock, 0, FALSE);
                QcRegisterMask();
                if (Msg == 1) ULog("wake: яркость восстановлена %u", Cur);
                else if (Msg == 2) ULog("wake: восстановление %u не прошло", Cur);
            }
            if (St == STATUS_WAIT_3) {                          /* \Callback\QCDXDL_BACKLIGHT_CHANGE */
                LONG Pct = InterlockedExchange(&gQcPct, -1);
                if (Pct >= 0) {
                    ULONG Idx = (ULONG)InterlockedCompareExchange(&gQcIdx, 0, 0);
                    ULONG Raw = (ULONG)InterlockedCompareExchange(&gQcRaw, 0, 0);
                    ULONG Upd = (ULONG)Pct, R = PctToRaw(Upd);
                    if (!gQcGot) {
                        gQcGot = TRUE;
                        ULog("qcdx bl подключён: idx=%u pct=%u val=%u", Idx, Upd, Raw);
                    }
                    if (R != gCur || gDispOff) {
                        UNICODE_STRING N = RTL_CONSTANT_STRING(L"Brightness");
                        Ramp(R);
                        ULog("qcdx яркость %u%% -> %u", Upd, R);
                        ZwSetValueKey(H, &N, 0, REG_DWORD, &Upd, sizeof(Upd));
                    }
                }
            }
        }
    }
out:
    if (EvObj) ObDereferenceObject(EvObj);
    if (Ev) ZwClose(Ev);
    if (H) ZwClose(H);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------- загрузка ---------------- */
static VOID Unmap(VOID)
{
    if (gMapSda) { gSda[0] = gSvSda; MmUnmapIoSpace(gMapSda, 0x1000); gMapSda = NULL; }
    if (gMapScl) { gScl[0] = gSvScl; MmUnmapIoSpace(gMapScl, 0x1000); gMapScl = NULL; }
}
static VOID DriverUnload(PDRIVER_OBJECT Drv)
{
    UNREFERENCED_PARAMETER(Drv);
    if (gPwrReg1) { PoUnregisterPowerSettingCallback(gPwrReg1); gPwrReg1 = NULL; }
    if (gPwrReg2) { PoUnregisterPowerSettingCallback(gPwrReg2); gPwrReg2 = NULL; }
    if (gQcBlReg) { ExUnregisterCallback(gQcBlReg); gQcBlReg = NULL; }
    if (gQcBlObj) { ObDereferenceObject(gQcBlObj); gQcBlObj = NULL; }
    if (gQcEvtObj) { ObDereferenceObject(gQcEvtObj); gQcEvtObj = NULL; }
    if (gPsReg) { ExUnregisterCallback(gPsReg); gPsReg = NULL; }
    if (gPsObj) { ObDereferenceObject(gPsObj); gPsObj = NULL; }
    if (gThread) {
        KeSetEvent(&gStop, 0, FALSE);
        KeWaitForSingleObject(gThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(gThread);
        gThread = NULL;
    }
    Unmap();
    if (gSvc.Buffer) ExFreePoolWithTag(gSvc.Buffer, TAG_POOL);
    ULog("выгружен");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Reg)
{
    PHYSICAL_ADDRESS Pa; HANDLE Th; NTSTATUS St; ULONG Mode = 0, i; UCHAR V; char L[200]; int n;

    Drv->DriverUnload = DriverUnload;
    gSvc.MaximumLength = Reg->Length + sizeof(WCHAR);
    gSvc.Buffer = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, gSvc.MaximumLength, TAG_POOL);
    if (gSvc.Buffer) { RtlCopyUnicodeString(&gSvc, Reg); QueryDword(gSvc.Buffer, L"Mode", &Mode); }

    Pa.QuadPart = (LONGLONG)(TLMM_S + 60 * 0x1000); gMapSda = MmMapIoSpaceEx(Pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)(TLMM_S + 61 * 0x1000); gMapScl = MmMapIoSpaceEx(Pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!gMapSda || !gMapScl) { ULog("не отобразил TLMM"); DriverUnload(Drv); return STATUS_INSUFFICIENT_RESOURCES; }
    gSda = (volatile ULONG *)gMapSda; gScl = (volatile ULONG *)gMapScl;
    gSvSda = gSda[0]; gSvScl = gScl[0];
    ULog("старт, mode %u; gpio60 cfg %08x io %08x, gpio61 cfg %08x io %08x", Mode, gSvSda, gSda[1], gSvScl, gScl[1]);

    n = _snprintf(L, sizeof(L), "ktz8866:");
    for (i = 0; i <= 0x15; i++) {
        if (RegRd((UCHAR)i, &V)) n += _snprintf(L + n, sizeof(L) - n, " %02x", V);
        else n += _snprintf(L + n, sizeof(L) - n, " --");
    }
    ULog("%s", L);
    if (!RawGet(&gCur)) { ULog("KTZ8866 не отвечает — ничего не пишу"); DriverUnload(Drv); return STATUS_DEVICE_DOES_NOT_EXIST; }
    ULog("текущая яркость %u (%u%%)", gCur, RawToPct(gCur));
    if (Mode == 1) { DriverUnload(Drv); return STATUS_UNSUCCESSFUL; }   /* только чтение: не остаёмся в памяти */

    KeInitializeEvent(&gLock, SynchronizationEvent, TRUE);   /* TRUE = замок свободен */
    KeInitializeEvent(&gWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&gQcEv, SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(St = PoRegisterPowerSettingCallback(NULL, &gGuidDisp, PwrSettingCb, NULL, &gPwrReg1)))
        ULog("PoRegister(disp) %08x", St);
    if (!NT_SUCCESS(St = PoRegisterPowerSettingCallback(NULL, &gGuidMon, PwrSettingCb, NULL, &gPwrReg2)))
        ULog("PoRegister(mon) %08x", St);
    {
        OBJECT_ATTRIBUTES Oa;
        UNICODE_STRING Cn = RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
        UNICODE_STRING QcBl = RTL_CONSTANT_STRING(L"\\Callback\\QCDXDL_BACKLIGHT_CHANGE");
        UNICODE_STRING QcEvt = RTL_CONSTANT_STRING(L"\\Callback\\QCDXDL_EVT_REGISTER");
        InitializeObjectAttributes(&Oa, &Cn, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        if (NT_SUCCESS(ExCreateCallback(&gPsObj, &Oa, FALSE, TRUE)))
            gPsReg = ExRegisterCallback(gPsObj, PsCb, NULL);
        if (!gPsReg) ULog("ExRegisterCallback(PowerState) не зарегистрировался");

        InitializeObjectAttributes(&Oa, &QcBl, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        if (NT_SUCCESS(ExCreateCallback(&gQcBlObj, &Oa, TRUE, TRUE)))
            gQcBlReg = ExRegisterCallback(gQcBlObj, QcBlCb, NULL);
        if (!gQcBlReg) ULog("ExRegisterCallback(QCDXDL_BACKLIGHT_CHANGE) не зарегистрировался");

        InitializeObjectAttributes(&Oa, &QcEvt, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        if (!NT_SUCCESS(ExCreateCallback(&gQcEvtObj, &Oa, TRUE, TRUE)))
            ULog("ExCreateCallback(QCDXDL_EVT_REGISTER) не создался");
    }

    KeInitializeEvent(&gStop, NotificationEvent, FALSE);
    St = PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, Worker, NULL);
    /* при ошибке DriverEntry DriverUnload не вызовется: callback'и уже зарегистрированы — снимаем сами */
    if (!NT_SUCCESS(St)) { ULog("PsCreateSystemThread %08x", St); DriverUnload(Drv); return St; }
    ObReferenceObjectByHandle(Th, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&gThread, NULL);
    ZwClose(Th);
    return STATUS_SUCCESS;
}

/*
 * elishbat — батарея Xiaomi Pad 5 Pro (elish) для Windows ARM64 (battery miniclass, WDM, ROOT\ELISHBAT).
 *
 * Две ячейки, у каждой свой TI bq27z561 (I2C 0x55): «master» на GPIO36 SDA / GPIO37 SCL (QUP1 SE13),
 * «slave» на GPIO28 SDA / GPIO29 SCL (QUP0 SE0). QUP без PEP не работает, поэтому I2C программный
 * (open-drain через OE, подтяжки на плате). Из счётчиков только читаем стандартные регистры.
 * Зарядка: PM8150B (SID2) через SPMI — чтение observer, запись chnls (EXT_WRITEL).
 * USBIN INT_RT_STS (0x1310) бит 4 = кабель, APSD_RESULT (0x1308) = тип, CHGR STATUS_1 (0x1006) = фаза.
 * Управление ICL 0x1370 по типу зарядника + watchdog pet 0x1643 + температурный контроль.
 * Windows видит одну батарею: ёмкости в мВт·ч при номинале 3,87 В на ячейку, мощность — сумма I*V ячеек.
 *
 * chg-ctl5: правки по вердикту answer-verify-chg-ctl4.md (разделы 4 и 6):
 *   1) G10: restore 0x1042=1 при ChgTempCtl=1 выполняется ДО early-return по
 *      ChgCtlErr, если TEMP_STOP уже не активен; state 4 не залипает;
 *   2) Start/Stop: Guard не остаётся 1 после неудачного Start; GuardSet(0)
 *      вынесен из Stop в PnP-путь (PASSIVE), в Stop больше нет Zw*;
 *   3) Stop: REF_STOP ставится до ожиданий; безтаймаутные вызовы описаны;
 *   4) ChgCtlErr=4/5: ничего не пишем, но температуру оцениваем и state отражаем;
 *   5) E->Lock — KMUTEX вместо FAST_MUTEX: длинный I2C/SPMI не под fast mutex;
 *   6) state 4 на выходе из TEMP_STOP; отчёт обновлён.
 *
 * chg-ctl6: точечные правки по вердикту verify-chg-ctl5 (разделы 1, 5, 6):
 *   1) HwLock: STATUS_ABANDONED — успешный захват KMUTEX (владение уже передано);
 *   2) ChgControl: после ChgCtlErr перед ChgRestore сначала подтверждённый
 *      SetIclSafe(0x0A); не подтвердили — 0x1042=1 не пишем, ChgEnOff=1, повтор;
 *   3) BatteryClassUnload: NTSTATUS проверяется; при ошибке HwFree не делаем
 *      (Stop/Start возвращают ошибку, ресурсы остаются — утечка вместо UAF);
 *   4) restore до проверки кабеля оставлен как в ctl5, разобран в REPORT.
 *
 * chg-ctl4: правки по разбору answer-verify-chg-ctl3.md (пп.2 (4)-(7), 3):
 *   1) Stop/Start/PnP без бесконечных ожиданий: GuardSet без ZwFlushKey, WorkBusy с таймаутом,
 *      квиесценция Ref перед MmUnmapIoSpace;
 *   2) SetIclSafe с read-back;
 *   3) TempBad учитывает ошибки 0x3C/0x2A;
 *   4) при ChgCtlErr=5 ChgControl не начинает работу, pet не пишется;
 *   5) state 4 при выходе из TEMP_STOP сбрасывается, но неудачный restore снова ставит 4;
 *   6) G10: после ошибки разрешены только restore 1, ICL 0x0A и аварийный 0x1042=0 в TEMP_STOP;
 *   7) ChgTempTestOffset (тестовый ключ, 0.1 C).
 */
#include <ntddk.h>
#include <batclass.h>

#define TAG_POOL      'tbse'
#define POLL_SEC      15
#define HW_LOCK_TIMEOUT_MS 2000   /* chg-ctl5: не ждать KMUTEX дольше 2 с */
#define CHG_DUMP_MAX  12          /* проба зарядника: не более 12 проходов после старта */
#define CHG_DUMP_GAP  5           /* проба: не чаще одного прохода в 5 с */
#define NOMINAL_MV    3870
#define TLMM_N        0x0F900000ull
#define TLMM_S        0x0F500000ull
#define SPMI_CORE     0x0C440000ull
#define SPMI_CORE_SZ  0x1100
#define SPMI_CHNLS    0x0C600000ull
#define SPMI_WIN_SZ   0x1000        /* п.3: окно канала RW (0x10000*Apid), не вся область 32 МБ */
#define SPMI_OBS      0x0E600000ull
#define SPMI_OBS_SZ   0x100000
#define SPMI_CNFG     0x0C40A000ull /* G12: cnfg по live.dts:4111 (reg-names ...,"cnfg"), размер 0x26000 */
#define SPMI_CNFG_SZ  0x1000        /* G12: ownership table 0x700 + 4*apid, apid<512 -> максимум 0xF00 */

/* SMB5 регистры (PM8150B SID2, адреса внутри PMIC) */
#define REG_APSD_RESULT   0x1308   /* APSD_RESULT_STATUS_REG: тип зарядника */
#define REG_APSD_STATUS   0x1307   /* APSD_STATUS_REG: bit0 = APSD_DONE */
#define REG_USBIN_RT      0x1310   /* USBIN INT_RT_STS: bit4 = plugin */
#define REG_ICL_CFG       0x1370   /* USBIN_CURRENT_LIMIT_CFG_REG */
#define REG_CHG_EN        0x1042   /* CHARGING_ENABLE_CMD_REG: bit0 = enable */
#define REG_CHG_STATUS    0x1006   /* BATTERY_CHARGER_STATUS_1_REG */
#define REG_WD_PET        0x1643   /* BARK_BITE_WDOG_PET_REG: bit0 = pet */
/* Коды ошибок ChgCtlErr */
#define CHG_ERR_NONE      0
#define CHG_ERR_SPMI      1   /* ошибка SPMI-записи */
#define CHG_ERR_VERIFY    2   /* чтение назад не совпало */
#define CHG_ERR_NOOWNER   4   /* G12: нет APID с owner==EE0 — записывать нельзя */
#define CHG_ERR_NOMAP     5   /* G15: SpCore/SpObs (или cnfg/RW-окно) не смаплены — управление не начинать */
/* Температурные пороги (дека-градусы C, из fuel gauge: TempReg - 2731) */
#define TEMP_ICL_TH       430    /* >=43.0 C -> ICL 0x0A */
#define TEMP_STOP_TH      470    /* >=47.0 C -> стоп заряда */
#define TEMP_COLD_TH      20     /* <2.0 C -> стоп заряда (по минимуму ячеек) */
#define TEMP_HYST         30     /* гистерезис 3 C */
#define TEMP_ERR          0xFFFFFFFF  /* TempCheck: температура не прочитана */

/* chg-ctl4: квиесценция перед HwFree. Старший бит Ref = Stop начался,
   младшие 31 бит — число входов в PollWork/Refresh. */
#define REF_STOP   ((LONG)0x80000000)
#define REF_MASK   ((LONG)0x7FFFFFFF)

typedef struct { volatile ULONG *Sda, *Scl; ULONG SvSda, SvScl; } BUS;

typedef struct {
    BOOLEAN Ok;
    BOOLEAN TempBad;  /* G5/G6: любой сбой I2C ячейки в текущем проходе или Temp==0 */
    UCHAR   Fail;
    USHORT  Soc, Mv, Rm, Fcc, Dc, Temp, Cycles;
    SHORT   Ma, AvgMa;
} CELL;

typedef struct {
    PDEVICE_OBJECT Self, Lower, Pdo;
    PVOID          Class;
    ULONG          Tag;
    BUS            Bus[2];
    PUCHAR         Map[4];
    PUCHAR         SpCore, SpObs;
    PUCHAR         SpCnfg;                      /* G12: cnfg (read-only) — ownership table */
    PUCHAR         SpWrUsb, SpWrChg, SpWrMisc;  /* п.3: RW-окна каналов (0x10000*Apid, 0x1000) */
    ULONG          ApidChg, ApidUsb, ApidMisc;
    KMUTEX         Lock;        /* chg-ctl5: пассивный мьютекс для длинных I2C/SPMI */
    CELL           Cell[2];
    BOOLEAN        Online;
    ULONG          ChgState;
    LARGE_INTEGER  Stamp;
    ULONG          LastState, LastCap;
    KTIMER         Timer;
    KDPC           Dpc;
    PIO_WORKITEM   Work;
    volatile LONG  WorkBusy;
    volatile LONG  Ref;         /* chg-ctl4: REF_STOP | счётчик пользователей железа */
    ULONG          GoodPolls;
    BOOLEAN        Started;
    BOOLEAN        HwInited;    /* chg-ctl4: HwInit выполнен, HwFree ещё не было */
    BOOLEAN        StopFailed;  /* chg-ctl4: Stop не дождался WorkBusy/Ref в этой загрузке */
    ULONG          ChgDump;     /* 1 = проба зарядника: чтение SID2 в таймере опроса */
    ULONG          ChgDumpN;    /* сделано проходов пробы */
    LARGE_INTEGER  ChgDumpLast; /* interrupt time прохода — пауза CHG_DUMP_GAP с */
    /* chg-ctl: управление зарядкой */
    ULONG          ChgCtl;      /* 0 = только чтение, 1 = управление */
    ULONG          ChgIclMax;   /* макс. ICL, ед. 50 мА */
    ULONG          ChgIclForce; /* !=0 — принудительное значение ICL */
    ULONG          ChgTempCtl;  /* 0 = выкл, 1 = температурный ограничитель */
    ULONG          ChgCtlErr;   /* код ошибки (CHG_ERR_*) */
    ULONG          ChgCtlIcl;   /* последнее записанное значение ICL */
    ULONG          ChgCtlType;  /* последний определённый тип зарядника */
    ULONG          ChgCtlState; /* 0=IDLE, 1=ACTIVE, 2=ERROR, 3=TEMP_STOP, 4=NOT_APPLIED (стоп/restore не применён) */
    ULONG          ChgCtlWrites;/* счётчик записей ICL (0x1370) в PMIC */
    ULONG          TempState;   /* 0=OK, 1=ICL_LIMIT, 2=STOP */
    BOOLEAN        LastPlugin;  /* предыдущее состояние USBIN plugin */
    BOOLEAN        NeedInit;    /* нужно инициализировать ICL при новом подключении */
    ULONG          ChgTempErr;  /* п.2: ошибки чтения температуры (I2C / T<=0 K) */
    ULONG          ChgReadErr;  /* п.5: ошибки чтения 0x1310 (plugin) */
    ULONG          ChgPetErr;   /* п.6: ошибки pet watchdog */
    ULONG          ChgEnErr;    /* п.6: ошибки записи 0x1042 */
    BOOLEAN        ChgEnOff;    /* п.1: драйвер запретил заряд (0x1042=0), вернуть при !ChgTempCtl */
    LONG           TempTestOffset; /* chg-ctl4: тестовый сдвиг, 0.1 C (ключ ChgTempTestOffset) */
} EXT, *PEXT;

DRIVER_INITIALIZE DriverEntry;
static UNICODE_STRING gReg;

/* Защита от цикла падений: Guard=1 пишется до запуска железа и снимается после 3 удачных опросов.
   Если при старте Guard уже 1 — прошлый запуск уронил систему, устройство не стартует (сброс: Guard=0). */
static ULONG GuardGet(VOID)
{
    RTL_QUERY_REGISTRY_TABLE Q[2];
    ULONG V = 0, Def = 0;
    RtlZeroMemory(Q, sizeof(Q));
    Q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q[0].Name = L"Guard"; Q[0].EntryContext = &V;
    Q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Q[0].DefaultData = &Def; Q[0].DefaultLength = sizeof(ULONG);
    if (!gReg.Buffer || !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, gReg.Buffer, Q, NULL, NULL))) return 0;
    return V;
}
/* chg-ctl5 п.3: GuardSet содержит Zw* и поэтому вызывается только на PASSIVE_LEVEL
   (Start, PollWork, DispPnp). В Stop его больше нет. */
static VOID GuardSet(ULONG V)
{
    OBJECT_ATTRIBUTES Oa;
    HANDLE H;
    UNICODE_STRING N = RTL_CONSTANT_STRING(L"Guard");
    if (!gReg.Buffer) return;
    InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&H, KEY_SET_VALUE, &Oa))) return;
    ZwSetValueKey(H, &N, 0, REG_DWORD, &V, sizeof(V));
    /* chg-ctl4: без ZwFlushKey. HvWriteLogFile зависал (см. RegSetVal); Guard — не тот
       ключ, ради которого можно блокировать Start/Stop. Значение уйдёт при обычном flush. */
    ZwClose(H);
}

/* Значения диагностики (ChgDump/ChgDumpN/ChgDumpT/ChgR_*) — в тот же ключ, что Guard.
   RegGetEx отличается от RegGet тем, что сообщает об ошибке RtlQueryRegistryValues:
   G3 требует при ошибке чтения ChgTempCtl сохранять прошлое значение, а не считать
   ключ выключенным (Def=0). */
static BOOLEAN RegGetEx(PCWSTR Name, ULONG Def, ULONG *Out)
{
    RTL_QUERY_REGISTRY_TABLE Q[2];
    ULONG V = 0;
    RtlZeroMemory(Q, sizeof(Q));
    Q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q[0].Name = (PWSTR)Name; Q[0].EntryContext = &V;
    Q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Q[0].DefaultData = &Def; Q[0].DefaultLength = sizeof(ULONG);
    if (!gReg.Buffer || !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, gReg.Buffer, Q, NULL, NULL))) return FALSE;
    *Out = V;
    return TRUE;
}
static ULONG RegGet(PCWSTR Name, ULONG Def)
{
    ULONG V = Def;
    RegGetEx(Name, Def, &V);
    return V;
}
static VOID RegSetVal(PCWSTR Name, ULONG Type, PVOID Data, ULONG Size)
{
    OBJECT_ATTRIBUTES Oa;
    HANDLE H;
    UNICODE_STRING N;
    if (!gReg.Buffer) return;
    RtlInitUnicodeString(&N, Name);
    InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&H, KEY_SET_VALUE, &Oa))) return;
    ZwSetValueKey(H, &N, 0, Type, Data, Size);
    ZwClose(H);   /* без ZwFlushKey: флаш не нужен, а его HvWriteLogFile зависал */
}

/* chg-ctl4: вход в код, который трогает железо (PollWork/Refresh). Если Stop уже
   выставил REF_STOP — не входим. Счётчик не даёт HwFree размапить IO, пока
   work item или battery-callback ещё внутри. */
static BOOLEAN HwEnter(PEXT E)
{
    LONG V;
    for (;;) {
        V = E->Ref;
        if (V & REF_STOP) return FALSE;
        if (InterlockedCompareExchange(&E->Ref, V + 1, V) == V) return TRUE;
    }
}
static VOID HwLeave(PEXT E) { InterlockedDecrement(&E->Ref); }

/* chg-ctl4: остановить новые входы и подождать до Ms. Возвращает TRUE, если
   активных пользователей не осталось. Вызывается только из Stop (PASSIVE_LEVEL). */
static BOOLEAN HwQuiesce(PEXT E, ULONG Ms)
{
    ULONG i;
    LARGE_INTEGER W;
    InterlockedOr(&E->Ref, REF_STOP);
    W.QuadPart = -10 * 1000 * 10;   /* 10 мс */
    for (i = 0; i < Ms / 10; i++) {
        if ((E->Ref & REF_MASK) == 0) return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &W);
    }
    return (E->Ref & REF_MASK) == 0;
}

/* chg-ctl5 п.5: вместо FAST_MUTEX — KMUTEX. Длинные I2C (bit-bang) и SPMI-опросы
   остаются сериализованными, но ожидание мьютекса идёт на PASSIVE_LEVEL, без
   busy-wait/APC-spin fast mutex. Ожидание ограничено HW_LOCK_TIMEOUT_MS: если
   мьютекс занят дольше, проход пропускается (FALSE), а не блокируется навсегда.
   Все вызывающие (Refresh, ChgDumpPass, ChgControl) работают на PASSIVE_LEVEL;
   Stop этот мьютекс не берёт. */
static BOOLEAN HwLock(PEXT E)
{
    LARGE_INTEGER T;
    NTSTATUS St;
    T.QuadPart = -10 * 1000 * (LONGLONG)HW_LOCK_TIMEOUT_MS;
    St = KeWaitForSingleObject(&E->Lock, Executive, KernelMode, FALSE, &T);
    /* chg-ctl6 п.1: для KMUTEX STATUS_ABANDONED означает, что владение уже
       передано вызывающему. Это успешный захват: вызывающий обязан сделать
       HwUnlock. Неуспех — только STATUS_TIMEOUT и прочие статусы. */
    return St == STATUS_SUCCESS || St == STATUS_ABANDONED;
}
static VOID HwUnlock(PEXT E) { KeReleaseMutex(&E->Lock, FALSE); }

static DRIVER_ADD_DEVICE AddDevice;
static DRIVER_UNLOAD Unload;
static DRIVER_DISPATCH DispPnp, DispPower, DispIoctl, DispCreateClose, DispPass;

/* ---------------- программный I2C ---------------- */
#define BB_T 5
static VOID  BbRel(volatile ULONG *P) { P[0] = 3u | (2u << 6); KeStallExecutionProcessor(BB_T); }
static VOID  BbLow(volatile ULONG *P) { P[1] = 0; P[0] = 3u | (2u << 6) | (1u << 9); KeStallExecutionProcessor(BB_T); }
static ULONG BbGet(volatile ULONG *P) { return P[1] & 1; }
static BOOLEAN BbSclHigh(BUS *B)
{
    ULONG i;
    BbRel(B->Scl);
    for (i = 0; i < 2000 && !BbGet(B->Scl); i++) KeStallExecutionProcessor(5);
    return BbGet(B->Scl) != 0;
}
static VOID BbStart(BUS *B) { BbRel(B->Sda); BbSclHigh(B); BbLow(B->Sda); BbLow(B->Scl); }
static VOID BbStop(BUS *B)  { BbLow(B->Sda); BbSclHigh(B); BbRel(B->Sda); }
static BOOLEAN BbTx(BUS *B, UCHAR V)
{
    int i; ULONG Ack;
    for (i = 7; i >= 0; i--) {
        if (V & (1u << i)) BbRel(B->Sda); else BbLow(B->Sda);
        BbSclHigh(B); BbLow(B->Scl);
    }
    BbRel(B->Sda); BbSclHigh(B); Ack = !BbGet(B->Sda); BbLow(B->Scl);
    return (BOOLEAN)Ack;
}
static UCHAR BbRx(BUS *B, BOOLEAN Ack)
{
    int i; UCHAR V = 0;
    BbRel(B->Sda);
    for (i = 7; i >= 0; i--) { BbSclHigh(B); if (BbGet(B->Sda)) V |= (UCHAR)(1u << i); BbLow(B->Scl); }
    if (Ack) BbLow(B->Sda); else BbRel(B->Sda);
    BbSclHigh(B); BbLow(B->Scl); BbRel(B->Sda);
    return V;
}
static VOID BbRecover(BUS *B)   /* SDA залипла в 0: до 9 тактов SCL, затем STOP */
{
    int i;
    BbRel(B->Sda); BbRel(B->Scl);
    for (i = 0; i < 9 && !BbGet(B->Sda); i++) { BbLow(B->Scl); BbSclHigh(B); }
    BbStop(B);
}
static BOOLEAN ReadWord(BUS *B, UCHAR Cmd, USHORT *Out)
{
    UCHAR Lo, Hi;
    KIRQL Old;
    BOOLEAN Ok = FALSE;
    int Try;
    for (Try = 0; Try < 3 && !Ok; Try++) {
        KeRaiseIrql(DISPATCH_LEVEL, &Old);
        if (!BbGet(B->Sda)) BbRecover(B);
        BbStart(B);
        if (BbTx(B, 0x55 << 1) && BbTx(B, Cmd)) {
            BbStart(B);
            if (BbTx(B, (0x55 << 1) | 1)) {
                Lo = BbRx(B, TRUE); Hi = BbRx(B, FALSE);
                *Out = (USHORT)(Lo | (Hi << 8));
                Ok = TRUE;
            }
        }
        BbStop(B);
        KeLowerIrql(Old);
    }
    return Ok;
}

/* ---------------- SPMI (чтение + запись) ---------------- */
/* Чтение: EXT_READL opcode=1, observer-канал (SpObs), окно 0x80*APID.
   Запись: EXT_WRITEL opcode=0, RW-канал, окно 0x10000*APID (п.3: смаплено только окно 0x1000).
   Формат команды v5 (fmt_cmd_v2): (opc<<27)|((addr&0xFF)<<4)|(bc&0x7).
   Доказательство: spmi-pmic-arb.c:1028 pmic_arb_fmt_cmd_v2, :69 PMIC_ARB_OP_EXT_WRITEL=0,
   :1007 offset_v5 CHANNEL_RW = 0x10000*apid. Data -> WDATA0 (offset 0x10) до команды. */
static int SpmiReadSt(PEXT E, ULONG Apid, ULONG Addr, ULONG *Out, ULONG *StOut)
{
    ULONG Off, St = 0, i;
    volatile ULONG *O;
    if (!E->SpObs || Apid == 0xFFFFFFFF) return -1;
    Off = 0x80 * Apid;
    if (Off + 0x20 > SPMI_OBS_SZ) return -1;
    O = (volatile ULONG *)(E->SpObs + Off);
    O[0] = (1u << 27) | ((Addr & 0xFF) << 4);         /* EXT_READL, 1 байт */
    for (i = 0; i < 1000; i++) { St = O[2]; if (St & 1) break; KeStallExecutionProcessor(1); }
    if (StOut) *StOut = St;
    if (!(St & 1) || (St & 0xE)) return -1;
    *Out = O[6] & 0xFF;
    return 0;
}
static int SpmiRead(PEXT E, ULONG Apid, ULONG Addr, ULONG *Out)
{
    return SpmiReadSt(E, Apid, Addr, Out, NULL);
}
/* Запись 1 байта через RW-канал SPMI PMIC-арбитра (v5).
   Win — окно канала (SPMI_CHNLS + 0x10000*Apid, смаплено SPMI_WIN_SZ в HwInit).
   WDATA0 = +0x10, CMD = +0x00, STATUS = +0x08. */
static int SpmiWriteSt(PUCHAR Win, ULONG Addr, ULONG Val, ULONG *StOut)
{
    ULONG St = 0, i;
    volatile ULONG *W;
    if (!Win) return -1;
    W = (volatile ULONG *)Win;
    W[4] = Val & 0xFF;                                /* WDATA0: данные */
    KeMemoryBarrier();                                /* G14: WDATA0 должен быть виден до CMD */
    W[0] = ((Addr & 0xFF) << 4);                      /* EXT_WRITEL opcode=0, 1 байт */
    for (i = 0; i < 1000; i++) { St = W[2]; if (St & 1) break; KeStallExecutionProcessor(1); }
    if (StOut) *StOut = St;
    if (!(St & 1) || (St & 0xE)) return -1;
    return 0;
}
static int SpmiWrite(PUCHAR Win, ULONG Addr, ULONG Val)
{
    return SpmiWriteSt(Win, Addr, Val, NULL);
}
/* Чтение APID по PPID: первый APID, у которого поле PPID совпало (для observer-чтения).
   Это НЕ выбор владельца: в v5 на один PPID бывает несколько APID (по одному на EE),
   а право записи определяется ownership-таблицей (см. FindApidOwned). */
static ULONG FindApid(PEXT E, ULONG Ppid)
{
    ULONG A, Mp;
    for (A = 0; 0x900 + 4 * A < SPMI_CORE_SZ; A++) {
        Mp = *(volatile ULONG *)(E->SpCore + 0x900 + 4 * A);
        if (Mp && ((Mp >> 8) & 0xFFF) == Ppid) return A;
    }
    return 0xFFFFFFFF;
}
/* G12: APID для записи — только с владельцем EE0.
   Доказательства: core mapping table v5 — pmic_arb_apid_map_offset_v5,
   spmi-pmic-arb.c:1116-1118 (0x900 + 4*apid), поле PPID = (reg>>8)&0xFFF (:870/:920);
   ownership — cnfg+0x700+4*apid, owner = bits 2:0 (spmi-pmic-arb.c:52-54
   SPMI_OWNERSHIP_TABLE_REG/PERIPH2OWNER, :923-925); cnfg = 0x0C40A000 и qcom,ee = 0 —
   live.dts:4111-4115. EE0 = 0.
   Возвращает 0xFFFFFFFF, если APID с owner==0 нет. */
static ULONG FindApidOwned(PEXT E, ULONG Ppid)
{
    ULONG A, Mp, Own, Found = 0xFFFFFFFF;
    if (!E->SpCore || !E->SpCnfg) return 0xFFFFFFFF;
    for (A = 0; 0x900 + 4 * A < SPMI_CORE_SZ && 0x700 + 4 * A < SPMI_CNFG_SZ; A++) {
        Mp = *(volatile ULONG *)(E->SpCore + 0x900 + 4 * A);
        if (!Mp || ((Mp >> 8) & 0xFFF) != Ppid) continue;
        Own = *(volatile ULONG *)(E->SpCnfg + 0x700 + 4 * A) & 0x7;
        if (Own == 0) Found = A;   /* как Linux ppid_to_apid_v5: последний APID этого EE */
    }
    return Found;
}
/* ---------------- опрос ---------------- */
static VOID ReadCell(BUS *B, CELL *C)
{
    USHORT V;
    CELL N = *C;
    BOOLEAN Ok = TRUE, Tok, DcOk = TRUE, CyOk = TRUE;
    Ok &= ReadWord(B, 0x2C, &N.Soc);
    Ok &= ReadWord(B, 0x08, &N.Mv);
    if (ReadWord(B, 0x0C, &V)) N.Ma = (SHORT)V; else Ok = FALSE;
    if (ReadWord(B, 0x14, &V)) N.AvgMa = (SHORT)V; else Ok = FALSE;
    Ok &= ReadWord(B, 0x10, &N.Rm);
    Ok &= ReadWord(B, 0x12, &N.Fcc);
    Tok = ReadWord(B, 0x06, &N.Temp);
    Ok &= Tok;
    if (!C->Ok || !C->Dc) { DcOk = ReadWord(B, 0x3C, &V); if (DcOk) N.Dc = V; }
    CyOk = ReadWord(B, 0x2A, &V); if (CyOk) N.Cycles = V;
    /* G5/G6 + chg-ctl4 п.3: любой сбой I2C в текущем проходе (включая 0x3C/0x2A)
       или Temp==0 — температура недостоверна. Флаг ставится сразу, не после 4 неудач;
       прошлые значения ячейки при этом не теряются. */
    N.TempBad = (!Ok || !DcOk || !CyOk || N.Temp == 0);
    C->TempBad = N.TempBad;
    if (Ok && N.Soc <= 100 && N.Mv > 2500 && N.Mv < 5000 && N.Fcc) { N.Ok = TRUE; N.Fail = 0; *C = N; }
    else if (++C->Fail >= 4) C->Ok = FALSE;   /* разовый сбой шины — держим прошлые значения */
}
static VOID Refresh(PEXT E, BOOLEAN Force)
{
    LARGE_INTEGER Now;
    ULONG V;
    if (!HwEnter(E)) return;   /* chg-ctl4: Stop уже начался — железо не трогаем */
    Now.QuadPart = (LONGLONG)KeQueryInterruptTime();   /* не системное время: NTP его двигает */
    if (!HwLock(E)) { HwLeave(E); return; }            /* chg-ctl5: не ждём дольше 2 с */
    if (Force || Now.QuadPart - E->Stamp.QuadPart > 2 * 10000000LL) {
        ReadCell(&E->Bus[0], &E->Cell[0]);
        ReadCell(&E->Bus[1], &E->Cell[1]);
        if (!SpmiRead(E, E->ApidUsb, 0x10, &V)) E->Online = (V & 0x10) != 0;
        if (!SpmiRead(E, E->ApidChg, 0x06, &V)) E->ChgState = V & 7;
        E->Stamp = Now;
    }
    HwUnlock(E);
    HwLeave(E);
}
static ULONG MwH(ULONG MaH) { return MaH * NOMINAL_MV / 1000; }

static VOID Totals(PEXT E, ULONG *Design, ULONG *Full, ULONG *Rem, ULONG *Mv, LONG *Mw, LONG *AvgMw, ULONG *TempK10, ULONG *Cycles)
{
    int i, n = 0;
    LONG P = 0, Pa = 0;
    ULONG D = 0, F = 0, R = 0, M = 0, T = 0, Cy = 0;
    for (i = 0; i < 2; i++) {
        CELL *C = &E->Cell[i];
        if (!C->Ok) continue;
        n++;
        D += MwH(C->Dc ? C->Dc : 4300); F += MwH(C->Fcc); R += MwH(C->Rm);
        M += C->Mv; T += C->Temp; Cy = max(Cy, (ULONG)C->Cycles);
        P += (LONG)C->Ma * (LONG)C->Mv / 1000;
        Pa += (LONG)C->AvgMa * (LONG)C->Mv / 1000;
    }
    if (n == 1) { D *= 2; F *= 2; R *= 2; P *= 2; Pa *= 2; }   /* вторая ячейка не ответила — считаем симметрично */
    *Design = D; *Full = F; *Rem = min(R, F); *Mv = n ? M / n : 0; *Mw = P; *AvgMw = Pa; *TempK10 = n ? T / n : 0; *Cycles = Cy;
}
static ULONG PowerState(PEXT E, LONG Mw, ULONG Rem, ULONG Full)
{
    ULONG S = 0;
    if (E->Online) S |= BATTERY_POWER_ON_LINE;
    /* п.3: при ChgCtl=0 — старое условие chg-probe3 (Mw > 0), при ChgCtl=1 — по статусу заряда */
    if (E->Online && E->ChgState >= 1 && E->ChgState <= 4 && (E->ChgCtl || Mw > 0)) S |= BATTERY_CHARGING;
    else if (!E->Online || Mw < -50) S |= BATTERY_DISCHARGING;
    if (!E->Online && Full && Rem * 100 < Full * 3) S |= BATTERY_CRITICAL;
    return S;
}

/* ---------------- колбэки battc ---------------- */
static NTSTATUS QueryTag(PVOID Ctx, PULONG Tag)
{
    PEXT E = (PEXT)Ctx;
    Refresh(E, FALSE);
    if (!E->Cell[0].Ok && !E->Cell[1].Ok) return STATUS_NO_SUCH_DEVICE;
    *Tag = E->Tag;
    return STATUS_SUCCESS;
}
static NTSTATUS CopyOut(PVOID Buf, ULONG Len, PULONG Ret, const VOID *Src, ULONG Size)
{
    *Ret = Size;
    if (Len < Size) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buf, Src, Size);
    return STATUS_SUCCESS;
}
static NTSTATUS QueryInformation(PVOID Ctx, ULONG Tag, BATTERY_QUERY_INFORMATION_LEVEL Level, LONG AtRate,
                                 PVOID Buf, ULONG Len, PULONG Ret)
{
    PEXT E = (PEXT)Ctx;
    ULONG D, F, R, Mv, T, Cy, V;
    LONG Mw, Amw;
    UNREFERENCED_PARAMETER(AtRate);
    if (Tag != E->Tag) return STATUS_NO_SUCH_DEVICE;
    Refresh(E, FALSE);
    Totals(E, &D, &F, &R, &Mv, &Mw, &Amw, &T, &Cy);
    switch (Level) {
    case BatteryInformation: {
        BATTERY_INFORMATION I;
        RtlZeroMemory(&I, sizeof(I));
        I.Capabilities = BATTERY_SYSTEM_BATTERY;
        I.Technology = 1;
        RtlCopyMemory(I.Chemistry, "LION", 4);
        I.DesignedCapacity = D;
        I.FullChargedCapacity = F;
        I.DefaultAlert1 = F * 7 / 100;
        I.DefaultAlert2 = F * 4 / 100;
        I.CycleCount = Cy;
        return CopyOut(Buf, Len, Ret, &I, sizeof(I));
    }
    case BatteryTemperature:
        return CopyOut(Buf, Len, Ret, &T, sizeof(T));
    case BatteryEstimatedTime:
        V = (Amw < -50 && !E->Online) ? (ULONG)((ULONGLONG)R * 3600 / (ULONG)(-Amw)) : BATTERY_UNKNOWN_TIME;
        return CopyOut(Buf, Len, Ret, &V, sizeof(V));
    case BatteryDeviceName:
        return CopyOut(Buf, Len, Ret, L"elish dual-cell (bq27z561 x2)", sizeof(L"elish dual-cell (bq27z561 x2)"));
    case BatteryManufactureName:
        return CopyOut(Buf, Len, Ret, L"Xiaomi", sizeof(L"Xiaomi"));
    case BatteryUniqueID:
        return CopyOut(Buf, Len, Ret, L"elish-bat0", sizeof(L"elish-bat0"));
    case BatterySerialNumber:
        return CopyOut(Buf, Len, Ret, L"0", sizeof(L"0"));
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}
static NTSTATUS QueryStatus(PVOID Ctx, ULONG Tag, PBATTERY_STATUS St)
{
    PEXT E = (PEXT)Ctx;
    ULONG D, F, R, Mv, T, Cy;
    LONG Mw, Amw;
    if (Tag != E->Tag) return STATUS_NO_SUCH_DEVICE;
    Refresh(E, FALSE);
    Totals(E, &D, &F, &R, &Mv, &Mw, &Amw, &T, &Cy);
    St->PowerState = PowerState(E, Mw, R, F);
    St->Capacity = R;
    St->Voltage = Mv ? Mv : BATTERY_UNKNOWN_VOLTAGE;
    St->Rate = Mw;
    return STATUS_SUCCESS;
}
static NTSTATUS SetStatusNotify(PVOID Ctx, ULONG Tag, PBATTERY_NOTIFY N)
{
    PEXT E = (PEXT)Ctx;
    UNREFERENCED_PARAMETER(N);
    return Tag == E->Tag ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}
static NTSTATUS DisableStatusNotify(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); return STATUS_SUCCESS; }
static NTSTATUS SetInformation(PVOID Ctx, ULONG Tag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buf)
{
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Tag); UNREFERENCED_PARAMETER(Level); UNREFERENCED_PARAMETER(Buf);
    return STATUS_NOT_SUPPORTED;
}

/* ---------------- проба зарядника (ChgDump, только чтение SID2) ---------------- */
/* Список адресов SMB5 (PM8150B, SID2): CHGR 0x10xx, DCDC 0x11xx, USBIN 0x13xx, TYPEC 0x15xx, MISC 0x16xx.
   PPID = 0x200 | периферия (0x210/0x211/0x213/0x215/0x216); APID — индекс, найденный FindApid.
   APID не найден → 0xFFFF (не читаем); ошибка статуса → 0xFFFF0000|St; иначе байт в DWORD. */
static VOID ChgName(WCHAR *N, USHORT A)
{
    static const WCHAR Hx[] = L"0123456789ABCDEF";
    N[0] = L'C'; N[1] = L'h'; N[2] = L'g'; N[3] = L'R'; N[4] = L'_';
    N[5] = Hx[(A >> 12) & 15]; N[6] = Hx[(A >> 8) & 15]; N[7] = Hx[(A >> 4) & 15]; N[8] = Hx[A & 15];
    N[9] = 0;
}
static VOID ChgDumpPass(PEXT E)
{
    static const USHORT Reg[] = {
        0x1006, 0x1007, 0x100D, 0x1042, 0x1043, 0x1051, 0x1061, 0x1070, 0x1090,
        0x1106, 0x1108, 0x110A, 0x110B,
        0x1307, 0x1308, 0x1310, 0x1340, 0x1342, 0x1362, 0x1365, 0x1366, 0x1370, 0x1380,
        0x1506, 0x150B, 0x1651, 0x1653
    };
    WCHAR N[12];
    UNICODE_STRING U;
    OBJECT_ATTRIBUTES Oa;
    LARGE_INTEGER T;
    HANDLE H;
    ULONG Val[sizeof(Reg) / sizeof(Reg[0])];
    ULONG i, Apid, V, St, n = sizeof(Reg) / sizeof(Reg[0]);
    if (!E->SpCore || !E->SpObs) n = 0;   /* SPMI не смаплен — пишем только N+T */
    /* 1. чтение SPMI под замком — быстро, без реестра */
    if (n) {
        if (!HwLock(E)) return;   /* chg-ctl5: занят дольше 2 с — пропускаем проход */
        for (i = 0; i < n; i++) {
            Apid = FindApid(E, 0x200 + (Reg[i] >> 8));
            V = 0xFFFF;
            if (Apid != 0xFFFFFFFF) {
                St = 0;
                if (!SpmiReadSt(E, Apid, Reg[i], &V, &St)) V &= 0xFF;
                else V = 0xFFFF0000 | (St & 0xFFFF);
            }
            Val[i] = V;
        }
        HwUnlock(E);
    }
    /* 2. запись в реестр БЕЗ замка и без ZwFlushKey: один сеанс на проход (27 значений + N + T) */
    E->ChgDumpN++;
    KeQuerySystemTime(&T);
    if (!gReg.Buffer) return;
    InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&H, KEY_SET_VALUE, &Oa))) return;
    for (i = 0; i < n; i++) {
        ChgName(N, Reg[i]);
        RtlInitUnicodeString(&U, N);
        ZwSetValueKey(H, &U, 0, REG_DWORD, &Val[i], sizeof(Val[i]));
    }
    RtlInitUnicodeString(&U, L"ChgDumpN");
    ZwSetValueKey(H, &U, 0, REG_DWORD, &E->ChgDumpN, sizeof(E->ChgDumpN));
    RtlInitUnicodeString(&U, L"ChgDumpT");
    ZwSetValueKey(H, &U, 0, REG_QWORD, &T, sizeof(T));
    ZwClose(H);
}
static VOID ChgDumpPoll(PEXT E)
{
    LARGE_INTEGER Now;
    if (!E->ChgDump || E->ChgDumpN >= CHG_DUMP_MAX) return;
    Now.QuadPart = (LONGLONG)KeQueryInterruptTime();
    if (E->ChgDumpLast.QuadPart && Now.QuadPart - E->ChgDumpLast.QuadPart < (LONGLONG)CHG_DUMP_GAP * 10000000LL) return;
    ChgDumpPass(E);
    E->ChgDumpLast = Now;
    if (E->ChgDumpN >= CHG_DUMP_MAX) { E->ChgDump = 0; RegSetVal(L"ChgDump", REG_DWORD, &E->ChgDump, sizeof(E->ChgDump)); }
}
/* ---------------- управление зарядкой (chg-ctl) ---------------- */
/* Определяет ICL по типу зарядника из APSD_RESULT_STATUS (0x1308 & 0x7F).
   SDP=bit0 -> 0x0A (500 мА); CDP=bit2, DCP=bit3, QC2=bit5, QC3=bit6 -> 0x1E (1500 мА);
   OCP=bit1, FLOAT=bit4, 0, несколько бит -> 0x0A. Верхний предел ChgIclMax. */
static ULONG IclByType(ULONG Type, ULONG IclMax)
{
    ULONG Icl;
    Type &= 0x7F;
    if (Type == 0x01) Icl = 0x0A;                          /* SDP */
    else if (Type == 0x04 || Type == 0x08 || Type == 0x20 || Type == 0x40) Icl = 0x1E; /* CDP/DCP/QC2/QC3 */
    else Icl = 0x0A;                                       /* OCP/FLOAT/0/несколько */
    if (Icl > IclMax) Icl = IclMax;
    return Icl;
}
/* Записывает ICL в 0x1370 с проверкой чтением назад. Ошибка -> ChgCtlErr, стоп записей. */
static BOOLEAN SetIcl(PEXT E, ULONG Icl)
{
    ULONG Rd, St, Rb = 0;
    if (!E->SpWrUsb) { E->ChgCtlErr = CHG_ERR_SPMI; E->ChgCtlState = 2; return FALSE; }
    if (SpmiWriteSt(E->SpWrUsb, REG_ICL_CFG, Icl, &St)) { E->ChgCtlErr = CHG_ERR_SPMI; E->ChgCtlState = 2; return FALSE; }
    E->ChgCtlWrites++;
    if (SpmiRead(E, E->ApidUsb, REG_ICL_CFG, &Rd) || (Rd & 0xFF) != (Icl & 0xFF)) {
        /* несовпадение или ошибка чтения — откат на безопасные 0x0A и стоп.
           G10: ChgCtlIcl=0x0A ставим только после подтверждённого read-back отката,
           иначе SetIclSafe посчитает 0x0A уже записанным и не повторит его. */
        if (!SpmiWriteSt(E->SpWrUsb, REG_ICL_CFG, 0x0A, NULL)) {
            E->ChgCtlWrites++;
            if (!SpmiRead(E, E->ApidUsb, REG_ICL_CFG, &Rb) && (Rb & 0xFF) == 0x0A)
                E->ChgCtlIcl = 0x0A;
        }
        E->ChgCtlErr = CHG_ERR_VERIFY;
        E->ChgCtlState = 2;
        return FALSE;
    }
    E->ChgCtlIcl = Icl;
    E->ChgCtlErr = CHG_ERR_NONE;
    return TRUE;
}
/* G10: после ChgCtlErr единственная разрешённая запись ICL — безопасные 0x0A.
   chg-ctl4 п.2: в отличие от прежней версии делает read-back, как SetIcl; ChgCtlIcl=0x0A
   ставится только после подтверждения. ChgCtlErr/ChgCtlState не сбрасываются: иначе
   управление само бы «вылечилось» после ошибки.
   chg-ctl6 п.2: возвращает TRUE только если 0x0A подтверждён (уже подтверждён
   прошлой записью или read-back совпал). FALSE — запись не прошла/не подтверждена;
   ChgControl в этом проходе не имеет права включать заряд 0x1042=1. */
static BOOLEAN SetIclSafe(PEXT E)
{
    ULONG Rd;
    if (E->ChgCtlIcl == 0x0A) return TRUE;   /* 0x0A уже подтверждён чтением */
    if (!E->SpWrUsb) return FALSE;           /* окна нет — подтвердить нечем */
    if (SpmiWrite(E->SpWrUsb, REG_ICL_CFG, 0x0A)) return FALSE;   /* запись не прошла — повторим */
    E->ChgCtlWrites++;
    if (SpmiRead(E, E->ApidUsb, REG_ICL_CFG, &Rd) || (Rd & 0xFF) != 0x0A) {
        /* read-back не подтвердил 0x0A: не врём ChgCtlIcl, повторим на следующем проходе */
        return FALSE;
    }
    E->ChgCtlIcl = 0x0A;
    return TRUE;
}
/* Pet watchdog (0x1643=1). Нужен: WD_CFG 0x1651=0x62 включает BARK_WDOG_INT_EN (bit6)
   и WDOG_TIMER_EN_ON_PLUGIN (bit1); bit5=1 — ГИПОТЕЗА: WDOG_TIMER_EN (загрузчик).
   BARK timeout: 0x1653 bark=3 -> 128 с (qpnp-smb5.c:4150, ilog2(bark/16)), pet раз в 15 с — с запасом.
   Linux кормит собаку в wdog_bark_irq_handler (smb5-lib.c:10339). Ошибка pet — ChgPetErr (п.6). */
static VOID WdogPet(PEXT E)
{
    if (!E->SpWrMisc || SpmiWrite(E->SpWrMisc, REG_WD_PET, 1)) E->ChgPetErr++;
}
/* Температурный ограничитель: T из fuel gauge (TempReg 0x06, дека-Кельвины).
   Для жары — МАКСИМУМ по ячейкам, для холода — МИНИМУМ (п.2), не среднее.
   Hot: T>=47C (470) -> STOP, T>=43C (430) -> ICL 0x0A; возврат <44C / <40C (гистерезис 3C).
   Cold: Tmin<2C (20) -> STOP (п.2), возврат Tmin>=5C (50). Пороги строже DT (too-hot=58C, too-cold=-10C).
   G5/G6: ошибка I2C любой ячейки в текущем проходе (TempBad), включая 0x3C/0x2A, или Temp==0 — TEMP_ERR:
   состояние не менять, ChgTempErr++, решение о стопе/ICL 0x0A принимает ChgControl.
   chg-ctl4 п.7: ChgTempTestOffset (0.1 C) прибавляется к Tmax/Tmin ТОЛЬКО здесь, т.е. при ChgTempCtl=1. */
static ULONG TempCheck(PEXT E, ULONG *LimitIcl)
{
    ULONG i, n = 0, T, Tmax = 0, Tmin = 0xFFFFFFFF;
    LONG TCmax, TCmin;
    for (i = 0; i < 2; i++) {
        if (E->Cell[i].TempBad) { E->ChgTempErr++; return TEMP_ERR; }   /* G5/G6: любой сбой — неизвестно */
        if (!E->Cell[i].Ok || E->Cell[i].Temp == 0) continue;           /* нет валидной ячейки */
        T = E->Cell[i].Temp;
        if (T > Tmax) Tmax = T;
        if (T < Tmin) Tmin = T;
        n++;
    }
    if (!n) { E->ChgTempErr++; return TEMP_ERR; }   /* температура не прочитана */
    TCmax = (LONG)Tmax - 2731;   /* дека-Кельвины -> дека-Цельсии (знак сохраняется) */
    TCmin = (LONG)Tmin - 2731;
    TCmax += E->TempTestOffset;  /* chg-ctl4 п.7: тестовый сдвиг, по умолчанию 0 */
    TCmin += E->TempTestOffset;
    *LimitIcl = 0x1E; /* без ограничения */
    if (E->TempState == 0) {
        if (TCmax >= TEMP_STOP_TH || TCmin < TEMP_COLD_TH) { E->TempState = 2; return 2; }
        if (TCmax >= TEMP_ICL_TH) { E->TempState = 1; *LimitIcl = 0x0A; return 1; }
    } else if (E->TempState == 1) {
        if (TCmax >= TEMP_STOP_TH || TCmin < TEMP_COLD_TH) { E->TempState = 2; return 2; }
        if (TCmax < TEMP_ICL_TH - TEMP_HYST) E->TempState = 0;
        else { *LimitIcl = 0x0A; return 1; }
    } else { /* TEMP_STOP: выход — не жарко (<44C) и не холодно (>=5C) */
        if (TCmax < TEMP_STOP_TH - TEMP_HYST && TCmin >= TEMP_COLD_TH + TEMP_HYST) {
            E->TempState = (TCmax >= TEMP_ICL_TH) ? 1 : 0;
            if (E->TempState == 1) { *LimitIcl = 0x0A; return 1; }
        }
        return 2;
    }
    return 0;
}
/* Включает/выключает зарядку (0x1042 bit0). Ошибка — ChgEnErr++, не фатально (п.6). */
static BOOLEAN SetChgEnable(PEXT E, BOOLEAN On)
{
    if (!E->SpWrChg || SpmiWrite(E->SpWrChg, REG_CHG_EN, On ? 1 : 0)) { E->ChgEnErr++; return FALSE; }
    return TRUE;
}
/* G1/G2/G9: чтение фактического 0x1042 bit0 (1 = заряд разрешён).
   Если чтение не удалось — подтверждения нет, вызывающий повторяет действие. */
static BOOLEAN ChgReadEnable(PEXT E, ULONG *V)
{
    if (!E->SpWrChg) return FALSE;
    return SpmiRead(E, E->ApidChg, REG_CHG_EN, V) == 0;
}
static BOOLEAN ChgIsOff(PEXT E)
{
    ULONG V;
    return ChgReadEnable(E, &V) && (V & 1) == 0;
}
static BOOLEAN ChgIsOn(PEXT E)
{
    ULONG V;
    return ChgReadEnable(E, &V) && (V & 1) != 0;
}
/* G9/G10: включить заряд и подтвердить чтением 0x1042. Ошибка/неподтверждение —
   FALSE, вызывающий ставит ChgEnOff=1 и повторяет на следующем проходе. */
static BOOLEAN ChgRestore(PEXT E)
{
    if (!SetChgEnable(E, TRUE)) return FALSE;
    return ChgIsOn(E);
}
/* Полный блок управления: вызывается из PollWork при ChgCtl=1.
   Все SPMI-операции под замком E->Lock (вызывается из-под него или берёт его).
   Никаких Zw* под замком. */
static VOID ChgControl(PEXT E)
{
    ULONG ApidUsb = E->ApidUsb, V, Type = 0, Icl, IclMax, LimitIcl = 0x1E, TempSt = 0, PrevTemp = E->TempState;
    BOOLEAN Plugin = FALSE;
    /* G15/G12: RW-окна не смаплены или нет APID с owner==EE0 — писать нельзя,
       управление не начинаем. chg-ctl5 п.4: температуру всё равно оцениваем
       (только чтение I2C-данных, уже снятых Refresh), чтобы не потерять тепловой
       стоп молча. Если стоп/restore требуется — state 4 «не применено», иначе
       state 2 «управление недоступно». Никаких записей, включая pet: аппаратный
       BARK (0x1653) остаётся последней защитой, см. REPORT. */
    if (E->ChgCtlErr == CHG_ERR_NOMAP || E->ChgCtlErr == CHG_ERR_NOOWNER) {
        if (E->ChgTempCtl) {
            (VOID)TempCheck(E, &LimitIcl);
            E->ChgCtlState = (E->TempState == 2 || E->ChgEnOff) ? 4 : 2;
        } else {
            E->ChgCtlState = E->ChgEnOff ? 4 : 2;
        }
        return;
    }
    /* G2/G3/G9/G10: при выключенном ChgTempCtl возвращаем 0x1042=1, если драйвер был в стопе
       (ChgEnOff или TempState==2). Restore выполняется ДО проверки ChgCtlErr: после ошибки это
       единственная разрешённая запись, кроме ICL 0x0A и аварийного 0 в TEMP_STOP. При неудаче/неподтверждении restore
       ChgEnOff=1 и state 4 — повтор на следующем проходе; TempState сбрасывается только после
       подтверждённого restore. */
    if (!E->ChgTempCtl) {
        if (E->ChgEnOff || E->TempState == 2) {
            /* chg-ctl6 п.2: после ошибки ICL сначала подтверждаем безопасные 0x0A,
               и только потом включаем заряд. Если 0x0A не подтверждён — 0x1042=1
               в этом проходе не пишем, ChgEnOff остаётся 1, повтор на следующем. */
            if (E->ChgCtlErr != CHG_ERR_NONE && !SetIclSafe(E)) {
                E->ChgEnOff = TRUE;
                E->ChgCtlState = 4;
                return;
            }
            if (ChgRestore(E)) {
                E->ChgEnOff = FALSE;
                E->TempState = 0;
                E->ChgCtlState = (E->ChgCtlErr == CHG_ERR_NONE) ? 0 : 2;
            } else {
                /* restore 0x1042=1 не удался/не подтвердился: не выдавать ACTIVE,
                   ChgEnOff=1 заставляет повторить restore на следующем проходе */
                E->ChgEnOff = TRUE;
                E->ChgCtlState = 4;
                if (E->ChgCtlErr != CHG_ERR_NONE) SetIclSafe(E);
                return;
            }
        } else {
            E->TempState = 0;
            if (E->ChgCtlState == 3 || E->ChgCtlState == 4)
                E->ChgCtlState = (E->ChgCtlErr == CHG_ERR_NONE) ? 0 : 2;
        }
    }
    /* G1/G5/G6: температуру проверяем ДО early-return по ChgCtlErr. Ошибка ICL/watchdog
       не должна мешать аварийному 0x1042=0: при нагреве после ошибки стоп всё равно
       применяется (это read-only опрос датчиков + запись 0 и, при необходимости, ICL 0x0A).
       G5/G6: при TEMP_ERR в стопе остаёмся в стопе, иначе ICL 0x0A и состояние не повышаем. */
    if (E->ChgTempCtl) {
        PrevTemp = E->TempState;
        TempSt = TempCheck(E, &LimitIcl);
        /* chg-ctl5 п.1: state 4 — только «требуемый стоп/restore не применён».
           Если температура уже не STOP и restore не требуется, 4 не должен
           залипать. Если restore нужен (ChgEnOff), 4 снимется/подтвердится ниже. */
        if (E->ChgCtlState == 4 && !E->ChgEnOff && E->TempState != 2 && TempSt != 2)
            E->ChgCtlState = (E->ChgCtlErr == CHG_ERR_NONE) ? 0 : 2;
        if (TempSt == TEMP_ERR) {
            if (E->TempState == 2 || E->ChgEnOff) {
                if (!E->ChgEnOff || !ChgIsOff(E)) {
                    if (SetChgEnable(E, FALSE) && ChgIsOff(E)) E->ChgEnOff = TRUE;
                    else E->ChgEnOff = FALSE;
                }
                /* G7: 0x1370 только если отличается; после ChgCtlErr — только SetIclSafe
                   (0x0A с read-back и без сброса ошибки), никаких обычных записей. */
                if (E->ChgCtlErr == CHG_ERR_NONE) {
                    if (E->ChgCtlIcl != 0x0A) SetIcl(E, 0x0A);
                } else {
                    SetIclSafe(E);
                }
                E->ChgCtlState = E->ChgEnOff ? 3 : 4;
                return;
            }
            if (E->ChgCtlErr == CHG_ERR_NONE) {
                if (E->ChgCtlIcl != 0x0A && !SetIcl(E, 0x0A)) return;
            } else {
                E->ChgCtlState = 2;   /* chg-ctl5: ошибка управления, стопа нет */
                SetIclSafe(E);
            }
            return;
        }
        if (TempSt == 2) {
            /* G1/G2/G7/G8: в TEMP_STOP 0x1042=0 повторяем каждый проход, пока ChgEnOff=0
               или ChgIsOff не подтвердил 0; 0x1370 пишем только если отличается;
               ChgCtlState=3 только при подтверждённом 0, иначе 4 «стоп не применён».
               chg-ctl4 п.6: это осознанное исключение из G10 — после ChgCtlErr запись
               0x1042=0 разрешена, потому что тепловой стоп важнее запрета записей. */
            if (!E->ChgEnOff || !ChgIsOff(E)) {
                if (SetChgEnable(E, FALSE) && ChgIsOff(E)) E->ChgEnOff = TRUE;
                else E->ChgEnOff = FALSE;
            }
            if (E->ChgCtlErr == CHG_ERR_NONE) {
                if (E->ChgCtlIcl != 0x0A) SetIcl(E, 0x0A);
            } else {
                SetIclSafe(E);
            }
            E->ChgCtlState = E->ChgEnOff ? 3 : 4;
            return;
        }
        /* chg-ctl5 п.1 (G10): сюда дошли, значит TEMP_STOP сейчас НЕ активен
           (TempSt 0/1). Выход из TEMP_STOP и restore 0x1042=1 обязаны произойти
           ДО early-return по ChgCtlErr — иначе после ошибки ICL заряд остаётся
           выключенным навсегда. Если restore не подтверждён — ChgEnOff=1,
           state 4 и повтор на следующем проходе.
           chg-ctl6 п.2: после ошибки сначала SetIclSafe(0x0A) с read-back и только
           при подтверждении — ChgRestore(0x1042=1). Иначе заряд включился бы с
           неизвестным/старым ICL. При активном TEMP_STOP сюда не попадаем:
           выше (TempSt==2) 0x1042=0 сохраняется. */
        if (PrevTemp == 2 || E->ChgEnOff) {
            if (E->ChgCtlErr != CHG_ERR_NONE && !SetIclSafe(E)) {
                E->ChgEnOff = TRUE;
                E->ChgCtlState = 4;
                return;
            }
            if (ChgRestore(E)) {
                E->ChgEnOff = FALSE;
                E->ChgCtlState = (E->ChgCtlErr == CHG_ERR_NONE) ? 0 : 2;
            } else {
                E->ChgEnOff = TRUE;
                E->ChgCtlState = 4;
                if (E->ChgCtlErr != CHG_ERR_NONE) SetIclSafe(E);
                return;
            }
        }
    }
    /* G10 (chg-ctl5 п.1): после ошибки разрешены только restore 0x1042=1 (выше),
       ICL 0x0A (SetIclSafe) и аварийный 0x1042=0 в TEMP_STOP/TEMP_ERR. Сюда дошли,
       значит TEMP_STOP нет — обычных записей не будет. */
    if (E->ChgCtlErr != CHG_ERR_NONE) {
        E->ChgCtlState = 2;
        SetIclSafe(E);
        return;
    }
    /* 1. Читаем USBIN plugin и APSD (п.5: ошибка чтения 0x1310 — не отключение) */
    if (ApidUsb == 0xFFFFFFFF) { E->ChgReadErr++; return; }
    if (SpmiRead(E, ApidUsb, REG_USBIN_RT, &V)) { E->ChgReadErr++; return; }
    Plugin = (V & 0x10) != 0;
    if (!Plugin) {
        /* кабель отключён: вернуть ICL на безопасные 0x0A (п.1: 0x1042 не пишем) */
        if (E->LastPlugin) {
            SetIcl(E, 0x0A);
            E->NeedInit = TRUE;
            E->LastPlugin = FALSE;
            E->ChgCtlState = 0;
        }
        return;
    }
    /* 2. Подключение: ждём APSD done (0x1307 bit0) при новом кабеле */
    if (E->NeedInit || !E->LastPlugin) {
        if (SpmiRead(E, ApidUsb, REG_APSD_STATUS, &V) || !(V & 1)) return; /* APSD не готов */
        E->NeedInit = FALSE;
        E->LastPlugin = TRUE;
    }
    /* 3. Читаем тип зарядника */
    if (SpmiRead(E, ApidUsb, REG_APSD_RESULT, &V)) return;
    Type = V & 0x7F;
    E->ChgCtlType = Type;
    /* 4. Определяем ICL (п.4: не ниже 0x0A) */
    IclMax = (E->ChgIclMax > 0x1E) ? 0x1E : E->ChgIclMax;
    if (IclMax < 0x0A) IclMax = 0x0A;
    if (E->ChgIclForce) {
        Icl = E->ChgIclForce;
        if (Type == 0x01 && Icl > 0x10) Icl = 0x10;  /* SDP: не больше 800 мА */
        if (Icl > IclMax) Icl = IclMax;
    } else {
        Icl = IclByType(Type, IclMax);
    }
    if (Icl < 0x0A) Icl = 0x0A;
    /* 5. Температурный ограничитель: только ограничение ICL при TempSt==1.
       Выход из TEMP_STOP и restore уже сделаны выше — до early-return по ChgCtlErr. */
    if (E->ChgTempCtl) {
        if (TempSt == 1) {
            if (Icl > LimitIcl) Icl = LimitIcl;
        }
    }
    /* 6. Пишем ICL если отличается от текущего */
    if (Icl != E->ChgCtlIcl) {
        if (!SetIcl(E, Icl)) return;
    }
    /* п.1: 0x1042 здесь не пишем — только по переходам TEMP_STOP / при !ChgTempCtl */
    E->ChgCtlState = 1;
}
/* ---------------- периодический опрос ---------------- */
static VOID PollWork(PDEVICE_OBJECT Dev, PVOID Ctx)
{
    PEXT E = (PEXT)Ctx;
    ULONG D, F, R, Mv, T, Cy, S, Tc;
    LONG Mw, Amw;
    UNREFERENCED_PARAMETER(Dev);
    /* chg-ctl4: Stop мог начаться до запуска work item — тогда железо уже не трогаем.
       Счётчик Ref держится до самого конца, чтобы HwFree не размапил IO под нами. */
    if (!HwEnter(E)) { InterlockedExchange(&E->WorkBusy, 0); return; }
    Refresh(E, TRUE);
    ChgDumpPoll(E);   /* проба зарядника: чтение SID2 (ChgDump=1) */
    if (E->ChgCtl) {
        /* G3: ошибка RtlQueryRegistryValues — сохраняем прошлое значение, а не считаем
           «пользователь выключил». Успешное чтение (в т.ч. отсутствующий ключ -> Def=0)
           применяем на следующем проходе. */
        if (RegGetEx(L"ChgTempCtl", 0, &Tc)) E->ChgTempCtl = Tc;
        /* chg-ctl4 п.7: тестовый сдвиг читается вне замка, применяется только в TempCheck
           (TempCheck вызывается только при ChgTempCtl=1). */
        E->TempTestOffset = (LONG)RegGet(L"ChgTempTestOffset", 0);
        if (HwLock(E)) {
            ChgControl(E);  /* управление ICL/watchdog/температурой — SPMI под замком */
            /* п.6: pet при ChgCtlErr==0. G1 safety: если TEMP_STOP не применён (state 4)
               и ограничитель включён — не кормим watchdog, чтобы bark снял заряд. */
            if (E->ChgCtlErr == CHG_ERR_NONE && !(E->ChgCtlState == 4 && E->ChgTempCtl)) WdogPet(E);
            HwUnlock(E);
        }
        /* chg-ctl5: мьютекс занят дольше HW_LOCK_TIMEOUT_MS — пропускаем проход
           управления; следующий PollWork повторит. Stop это не блокирует. */
    }
    Totals(E, &D, &F, &R, &Mv, &Mw, &Amw, &T, &Cy);
    S = PowerState(E, Mw, R, F);
    if (E->GoodPolls < 3 && ++E->GoodPolls == 3) GuardSet(0);
    if (E->Class && (S != E->LastState || (F && (R * 100 / F) != E->LastCap))) {
        E->LastState = S; E->LastCap = F ? R * 100 / F : 0;
        BatteryClassStatusNotify(E->Class);
    }
    /* журнал в реестр (раз за проход, вне замка): + счётчики пп.2,5,6 и флаг п.1 */
    if (E->ChgCtl) {
        static const WCHAR *Names[] = { L"ChgCtlIcl", L"ChgCtlType", L"ChgCtlState", L"ChgCtlWrites", L"ChgCtlErr",
                                        L"ChgTempErr", L"ChgReadErr", L"ChgPetErr", L"ChgEnErr", L"ChgEnOff" };
        ULONG Vals[10];
        OBJECT_ATTRIBUTES Oa;
        HANDLE H;
        UNICODE_STRING U;
        ULONG i;
        Vals[0] = E->ChgCtlIcl; Vals[1] = E->ChgCtlType; Vals[2] = E->ChgCtlState;
        Vals[3] = E->ChgCtlWrites; Vals[4] = E->ChgCtlErr;
        Vals[5] = E->ChgTempErr; Vals[6] = E->ChgReadErr; Vals[7] = E->ChgPetErr;
        Vals[8] = E->ChgEnErr; Vals[9] = E->ChgEnOff;
        if (gReg.Buffer) {
            InitializeObjectAttributes(&Oa, &gReg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
            if (NT_SUCCESS(ZwOpenKey(&H, KEY_SET_VALUE, &Oa))) {
                for (i = 0; i < 10; i++) {
                    RtlInitUnicodeString(&U, Names[i]);
                    ZwSetValueKey(H, &U, 0, REG_DWORD, &Vals[i], sizeof(Vals[i]));
                }
                ZwClose(H);
            }
        }
    }
    InterlockedExchange(&E->WorkBusy, 0);
    HwLeave(E);
}
static VOID PollDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    PEXT E = (PEXT)Ctx;
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    if (E->Started && !InterlockedExchange(&E->WorkBusy, 1)) IoQueueWorkItem(E->Work, PollWork, DelayedWorkQueue, E);
}

/* ---------------- железо ---------------- */
static PUCHAR MapPin(ULONG64 Pa)
{
    PHYSICAL_ADDRESS P; P.QuadPart = (LONGLONG)Pa;
    return (PUCHAR)MmMapIoSpaceEx(P, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
}
/* п.3: RW-окно канала SPMI: 0x10000*Apid, размер 0x1000 (не вся область 32 МБ) */
static PUCHAR MapWrWin(ULONG Apid)
{
    PHYSICAL_ADDRESS P;
    P.QuadPart = (LONGLONG)(SPMI_CHNLS + 0x10000ull * Apid);
    return (PUCHAR)MmMapIoSpaceEx(P, SPMI_WIN_SZ, PAGE_READWRITE | PAGE_NOCACHE);
}
static NTSTATUS HwInit(PEXT E)
{
    static const ULONG64 Pin[4] = { TLMM_S + 36 * 0x1000, TLMM_S + 37 * 0x1000, TLMM_N + 28 * 0x1000, TLMM_N + 29 * 0x1000 };
    PHYSICAL_ADDRESS P;
    int i;
    for (i = 0; i < 4; i++) if (!(E->Map[i] = MapPin(Pin[i]))) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < 2; i++) {
        BUS *B = &E->Bus[i];
        B->Sda = (volatile ULONG *)E->Map[2 * i]; B->Scl = (volatile ULONG *)E->Map[2 * i + 1];
        B->SvSda = B->Sda[0]; B->SvScl = B->Scl[0];
        BbRel(B->Sda); BbRel(B->Scl);
    }
    P.QuadPart = SPMI_CORE; E->SpCore = (PUCHAR)MmMapIoSpaceEx(P, SPMI_CORE_SZ, PAGE_READWRITE | PAGE_NOCACHE);
    P.QuadPart = SPMI_OBS;  E->SpObs  = (PUCHAR)MmMapIoSpaceEx(P, SPMI_OBS_SZ, PAGE_READWRITE | PAGE_NOCACHE);
    E->ApidChg = E->ApidUsb = E->ApidMisc = 0xFFFFFFFF;
    if (E->SpCore && E->SpObs) {
        E->ApidChg = FindApid(E, 0x210);
        E->ApidUsb = FindApid(E, 0x213);
    }
    /* G12/G15: при ChgCtl=1 сначала проверяем, что SPMI смаплен и у каждого нужного
       PPID есть APID с владельцем EE0 (ownership). Только после этого маппим RW-окна.
       Нет core/obs (или cnfg/RW не смаппились) -> ChgCtlErr=5, управление не запускаем;
       нет APID с owner==0 -> ChgCtlErr=4, никаких записей. */
    if (E->ChgCtl) {
        if (!E->SpCore || !E->SpObs) {
            E->ChgCtlErr = CHG_ERR_NOMAP;
        } else {
            P.QuadPart = SPMI_CNFG;
            E->SpCnfg = (PUCHAR)MmMapIoSpaceEx(P, SPMI_CNFG_SZ, PAGE_READONLY | PAGE_NOCACHE);
            if (!E->SpCnfg) {
                E->ChgCtlErr = CHG_ERR_NOMAP;
            } else {
                ULONG A;
                A = FindApidOwned(E, 0x213);   /* USBIN: 0x1370 */
                if (A != 0xFFFFFFFF) E->ApidUsb = A; else E->ChgCtlErr = CHG_ERR_NOOWNER;
                A = FindApidOwned(E, 0x210);   /* CHGR: 0x1042 */
                if (A != 0xFFFFFFFF) E->ApidChg = A; else E->ChgCtlErr = CHG_ERR_NOOWNER;
                A = FindApidOwned(E, 0x216);   /* MISC: 0x1643 */
                if (A != 0xFFFFFFFF) E->ApidMisc = A; else E->ChgCtlErr = CHG_ERR_NOOWNER;
                if (E->ChgCtlErr == CHG_ERR_NONE) {
                    E->SpWrUsb  = MapWrWin(E->ApidUsb);
                    E->SpWrChg  = MapWrWin(E->ApidChg);
                    E->SpWrMisc = MapWrWin(E->ApidMisc);
                    if (!E->SpWrUsb || !E->SpWrChg || !E->SpWrMisc) E->ChgCtlErr = CHG_ERR_NOMAP;
                }
            }
        }
    }
    E->HwInited = TRUE;
    /* Возвращаем STATUS_SUCCESS даже при ChgCtlErr=4/5: батарея должна работать
       в read-only, а управление/ pet сами проверяют ChgCtlErr. */
    return STATUS_SUCCESS;
}
static VOID HwFree(PEXT E)
{
    int i;
    for (i = 0; i < 2; i++) if (E->Bus[i].Sda && E->Bus[i].Scl) { E->Bus[i].Sda[0] = E->Bus[i].SvSda; E->Bus[i].Scl[0] = E->Bus[i].SvScl; }
    for (i = 0; i < 4; i++) if (E->Map[i]) { MmUnmapIoSpace(E->Map[i], 0x1000); E->Map[i] = NULL; }
    if (E->SpCore) { MmUnmapIoSpace(E->SpCore, SPMI_CORE_SZ); E->SpCore = NULL; }
    if (E->SpWrUsb)  { MmUnmapIoSpace(E->SpWrUsb,  SPMI_WIN_SZ); E->SpWrUsb  = NULL; }
    if (E->SpWrChg)  { MmUnmapIoSpace(E->SpWrChg,  SPMI_WIN_SZ); E->SpWrChg  = NULL; }
    if (E->SpWrMisc) { MmUnmapIoSpace(E->SpWrMisc, SPMI_WIN_SZ); E->SpWrMisc = NULL; }
    if (E->SpCnfg)   { MmUnmapIoSpace(E->SpCnfg,   SPMI_CNFG_SZ); E->SpCnfg  = NULL; }
    if (E->SpObs)  { MmUnmapIoSpace(E->SpObs, SPMI_OBS_SZ); E->SpObs = NULL; }
    E->Bus[0].Sda = E->Bus[0].Scl = E->Bus[1].Sda = E->Bus[1].Scl = (volatile ULONG *)NULL;
    E->HwInited = FALSE;
}
static NTSTATUS Start(PEXT E)
{
    BATTERY_MINIPORT_INFO Mi;
    LARGE_INTEGER Due;
    NTSTATUS St;
    if (GuardGet()) {
        /* Guard=1 после таймаута Stop — это не crash-loop, а признак, что work item
           не успел завершиться. Флаг живёт только в этой загрузке: после реального
           bugcheck новый EXT его не помнит, и Guard снова защищает. */
        if (!E->StopFailed) return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    GuardSet(1);
    E->ChgDump = RegGet(L"ChgDump", 0);   /* 0 = как раньше, проба выключена */
    E->ChgDumpN = 0;
    E->ChgDumpLast.QuadPart = 0;
    /* chg-ctl: ключи управления зарядкой */
    E->ChgCtl = RegGet(L"ChgCtl", 0);
    E->ChgIclMax = RegGet(L"ChgIclMax", 0x1E);
    if (E->ChgIclMax > 0x1E) E->ChgIclMax = 0x1E;
    if (E->ChgIclMax < 0x0A) E->ChgIclMax = 0x0A;   /* п.4: ICL никогда не ниже 0x0A */
    E->ChgIclForce = RegGet(L"ChgIclForce", 0);
    if (E->ChgIclForce && E->ChgIclForce < 0x0A) E->ChgIclForce = 0x0A;   /* п.4: 0 = не задано */
    E->ChgTempCtl = RegGet(L"ChgTempCtl", 0);
    E->ChgCtlIcl = 0;
    E->ChgCtlType = 0;
    E->ChgCtlState = 0;
    E->ChgCtlWrites = 0;
    E->TempState = 0;
    E->LastPlugin = FALSE;
    E->NeedInit = TRUE;
    E->ChgTempErr = E->ChgReadErr = E->ChgPetErr = E->ChgEnErr = 0;   /* пп.2,5,6 */
    E->ChgEnOff = RegGet(L"ChgEnOff", 0) != 0;   /* п.1: драйвер ранее запретил заряд */
    E->TempTestOffset = 0;                        /* chg-ctl4 п.7: до первого PollWork */
    /* chg-ctl4: если прошлый Stop не дождался work item/callback, второй экземпляр
       работы на том же EXT запускать нельзя. WorkBusy проверяем отдельно: DPC мог
       уже поставить work item, который ещё не вошёл в HwEnter. */
    if ((E->Ref & REF_MASK) || E->WorkBusy) {
        E->StopFailed = TRUE;
        /* chg-ctl5 п.2: неудачный Start не должен оставлять Guard=1 — иначе
           следующая загрузка (StopFailed уже не помнит) откажет по Guard. */
        GuardSet(0);
        return STATUS_DEVICE_BUSY;
    }
    /* Прошлый Stop мог не освободить IO/class (таймаут). Пока REF_STOP ещё стоит,
       добиваем quiesce, выгружаем class и только потом освобождаем IO. Это закрывает
       гонку с battery-callback, который иначе мог бы войти в Refresh во время HwFree. */
    if (E->HwInited || E->Class) {
        if (!HwQuiesce(E, 5000)) {
            E->StopFailed = TRUE;
            GuardSet(0);   /* chg-ctl5 п.2: то же для cleanup-ветки Start */
            return STATUS_DEVICE_BUSY;
        }
        if (E->Class) {
            /* chg-ctl6 п.3: статус unload проверяем. При ошибке class может быть
               ещё жив (callback возможен) — HwFree/MmUnmapIoSpace не делаем,
               оставляем ресурсы и повторяем на следующем Start: утечка вместо UAF. */
            St = BatteryClassUnload(E->Class);
            if (!NT_SUCCESS(St)) {
                DbgPrint("elishbat: BatteryClassUnload failed 0x%08X (Start cleanup), keep IO mapped\n", (ULONG)St);
                E->StopFailed = TRUE;
                GuardSet(0);
                return STATUS_DEVICE_BUSY;
            }
            E->Class = NULL;
        }
        HwFree(E);
    }
    InterlockedExchange(&E->Ref, 0);   /* снять REF_STOP; активных входов уже нет */
    E->ChgCtlErr = CHG_ERR_NONE;
    St = HwInit(E);
    if (!NT_SUCCESS(St)) { HwFree(E); GuardSet(0); return St; }
    E->GoodPolls = 0;
    Refresh(E, TRUE);
    RtlZeroMemory(&Mi, sizeof(Mi));
    Mi.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    Mi.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    Mi.Context = E;
    Mi.QueryTag = QueryTag;
    Mi.QueryInformation = QueryInformation;
    Mi.SetInformation = SetInformation;
    Mi.QueryStatus = QueryStatus;
    Mi.SetStatusNotify = SetStatusNotify;
    Mi.DisableStatusNotify = DisableStatusNotify;
    Mi.Pdo = E->Pdo;
    if (!E->Class) {
        St = BatteryClassInitializeDevice(&Mi, &E->Class);
        if (!NT_SUCCESS(St)) { E->Class = NULL; HwFree(E); GuardSet(0); return St; }
    }
    E->Started = TRUE;
    E->StopFailed = FALSE;
    Due.QuadPart = -10000000LL * POLL_SEC;
    KeSetTimerEx(&E->Timer, Due, POLL_SEC * 1000, &E->Dpc);
    return STATUS_SUCCESS;
}
/* chg-ctl4: ограниченное ожидание WorkBusy. Возвращает FALSE по таймауту. */
static BOOLEAN WaitWorkBusy(PEXT E, ULONG Ms)
{
    ULONG i;
    LARGE_INTEGER W;
    W.QuadPart = -10 * 1000 * 10;   /* 10 мс */
    for (i = 0; i < Ms / 10; i++) {
        if (!E->WorkBusy) return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &W);
    }
    return !E->WorkBusy;
}
/* chg-ctl5: остановка без бесконечных ожиданий и без Zw*.
   1) Started=0, REF_STOP (новые входы в железо запрещены), таймер снят, DPC слиты;
   2) WorkBusy ждём не более 5 с;
   3) затем HwQuiesce (Ref) не более 5 с — на случай battery-callback;
   4) только после этого BatteryClassUnload + HwFree (MmUnmapIoSpace).
   E->Lock здесь не берём: под ним длинные I2C/SPMI, и ожидание мьютекса снова стало бы
   бесконечным. Вместо него — Ref, который считает все входы в железо.
   Если work item/callback не закончился — НЕ размапливаем и НЕ удаляем device:
   лучше утечка, чем use-after-unmap. Возвращает TRUE, если ресурсы освобождены.
   GuardSet(0) сюда НЕ входит (в Stop нет Zw*): его делает DispPnp после возврата Stop. */
static BOOLEAN Stop(PEXT E)
{
    NTSTATUS St;
    if (!E->Started && !E->HwInited && !E->Class && !E->WorkBusy && !(E->Ref & REF_MASK)) return TRUE;
    E->Started = FALSE;
    /* chg-ctl5 п.3: REF_STOP до ожиданий. Work item, ещё не вошедший в HwEnter,
       сам сбросит WorkBusy; уже вошедший добивается через Ref. */
    InterlockedOr(&E->Ref, REF_STOP);
    KeCancelTimer(&E->Timer);
    /* Без таймаута: ждёт завершения поставленных DPC. DPC тут короткий
       (Started + IoQueueWorkItem), поэтому риск ограничен; см. REPORT. */
    KeFlushQueuedDpcs();
    if (!WaitWorkBusy(E, 5000)) {
        E->StopFailed = TRUE;
        return FALSE;
    }
    if (!HwQuiesce(E, 5000)) {
        E->StopFailed = TRUE;
        return FALSE;
    }
    /* BatteryClassUnload и MmUnmapIoSpace (в HwFree) тоже без таймаута — это
       внешние вызовы. Порядок не меняем: сначала quiesce, потом unload/unmap,
       иначе возможен callback на отмапленную память.
       chg-ctl6 п.3: если unload не успешен, class может быть ещё жив — HwFree/
       MmUnmapIoSpace не вызываем, возвращаем FALSE (PnP оставит ресурсы, утечка
       вместо UAF), повтор — на следующем Start. */
    if (E->Class) {
        St = BatteryClassUnload(E->Class);
        if (!NT_SUCCESS(St)) {
            DbgPrint("elishbat: BatteryClassUnload failed 0x%08X (Stop), keep IO mapped\n", (ULONG)St);
            E->StopFailed = TRUE;
            return FALSE;
        }
        E->Class = NULL;
    }
    HwFree(E);
    E->StopFailed = FALSE;
    return TRUE;
}

/* ---------------- WDM ---------------- */
static NTSTATUS AddDevice(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT Dev;
    PEXT E;
    NTSTATUS St = IoCreateDevice(Drv, sizeof(EXT), NULL, FILE_DEVICE_BATTERY, FILE_DEVICE_SECURE_OPEN, FALSE, &Dev);
    if (!NT_SUCCESS(St)) return St;
    E = (PEXT)Dev->DeviceExtension;
    RtlZeroMemory(E, sizeof(*E));
    E->Self = Dev; E->Pdo = Pdo; E->Tag = 1;
    KeInitializeMutex(&E->Lock, 0);   /* chg-ctl5: KMUTEX для длинных I2C/SPMI */
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
    BOOLEAN Freed;
    switch (Sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        St = IoForwardIrpSynchronously(E->Lower, Irp) ? Irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(St)) St = Start(E);
        return Complete(Irp, St);
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_STOP_DEVICE:
        Stop(E);
        /* chg-ctl5 п.3: единственное место снятия Guard на пути Stop — PnP
           (PASSIVE_LEVEL). В самом Stop Zw* больше нет. Снимаем и при Stop()==FALSE:
           это не crash-loop, а таймаут; иначе после перезагрузки Guard=1 навсегда
           заблокирует загрузку. Если Stop зависнет, сюда не дойдём — Guard останется 1. */
        GuardSet(0);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        Freed = Stop(E);
        GuardSet(0);   /* chg-ctl5 п.2/3: снятие Guard вне Stop, на PASSIVE_LEVEL */
        if (!Freed) {
            /* Work item/callback ещё жив: device extension, work item и Lower нельзя
               освобождать. Завершаем remove с утечкой — это безопаснее UAF/BSOD. */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(E->Lower, Irp);
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        St = IoCallDriver(E->Lower, Irp);
        IoFreeWorkItem(E->Work);
        E->Work = NULL;
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
    PEXT E = (PEXT)Dev->DeviceExtension;
    NTSTATUS St = STATUS_NOT_SUPPORTED;
    if (E->Class) St = BatteryClassIoctl(E->Class, Irp);
    if (St == STATUS_NOT_SUPPORTED) return Complete(Irp, St);
    return St;
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



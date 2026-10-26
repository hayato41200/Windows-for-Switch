/*
 * Nintendo Switch battery miniclass driver (battc), KMDF.
 *
 * ACPI device BAT0 (ACPI\SWOA0301) has two I2cSerialBus connections on I2C1:
 *   0: MAX17050 fuel gauge (0x36): RepSOC 0x06 (1/256 %), VCELL 0x09
 *      (bits 15:3 x 0.625 mV), Current 0x0A (signed, 1.5625 uV / 5 mOhm =
 *      0.3125 mA, + = charging), Temp 0x08 (signed 1/256 C)
 *   1: BQ24193 charger (0x6B): SYS_STAT 0x08 (VBUS_STAT 7:6: 1 USB host,
 *      2 adapter, 3 OTG; CHRG_STAT 5:4: 1 pre, 2 fast, 3 done; PG bit 2)
 * Capacity is reported relative (0-100 %), like the gauge's RepSOC. Values
 * and register meanings match what UEFI R363+ prints on the device.
 * The structure follows Microsoft's SimBatt sample.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <batclass.h>
#include <spb.h>

#define GAUGE_REPSOC   0x06
#define GAUGE_TEMP     0x08
#define GAUGE_VCELL    0x09
#define GAUGE_CURRENT  0x0A
#define CHARGER_SYSSTAT 0x08
#define BATTERY_TAG    1
#define POLL_MS        10000

typedef struct _SWBAT_DEVICE {
    WDFIOTARGET Gauge;
    WDFIOTARGET Charger;
    LARGE_INTEGER GaugeId, ChargerId;
    ULONG Connections;
    WDFWAITLOCK Lock;
    PVOID ClassHandle;
    WDFTIMER Timer;
    volatile BOOLEAN Stopping;
    volatile LONG TimerRuns;
    BATTERY_STATUS Last;
} SWBAT_DEVICE, *PSWBAT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBAT_DEVICE, SwBatGetDevice)

typedef struct _SWBAT_TIMER {
    WDFDEVICE Device;
} SWBAT_TIMER, *PSWBAT_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBAT_TIMER, SwBatGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwBatEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwBatEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwBatEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwBatEvtSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBatEvtIoDeviceControl;
EVT_WDF_TIMER SwBatEvtTimer;
BCLASS_QUERY_TAG_CALLBACK SwBatQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK SwBatQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK SwBatSetInformation;
BCLASS_QUERY_STATUS_CALLBACK SwBatQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK SwBatSetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK SwBatDisableStatusNotify;

/* ------------------------------------------------------------ diagnostics */

static WCHAR SwBatParamsPath[256];

static VOID SwBatTrace(PCWSTR Name, ULONG Value)
{
    if (SwBatParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBatParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwBatOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

/* Write the register number, then read Length bytes (repeated start). */
static NTSTATUS SwBatReadRegs(WDFIOTARGET Target, UCHAR Register, PUCHAR Data, ULONG Length)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 1 + Length) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwBatRead16(WDFIOTARGET Target, UCHAR Register, PUSHORT Value)
{
    UCHAR Buffer[2] = { 0, 0 };
    NTSTATUS Status = SwBatReadRegs(Target, Register, Buffer, 2);
    *Value = (USHORT)(Buffer[0] | (Buffer[1] << 8));
    return Status;
}

/* Read gauge + charger into a BATTERY_STATUS. Caller holds Dev->Lock. */
static NTSTATUS SwBatReadStatus(PSWBAT_DEVICE Dev, PBATTERY_STATUS Out)
{
    USHORT Soc = 0, Vcell = 0;
    UCHAR Sys = 0;
    ULONG Percent, VbusStat, ChrgStat;
    BOOLEAN Online;
    NTSTATUS Status;

    Status = SwBatRead16(Dev->Gauge, GAUGE_REPSOC, &Soc);
    if (NT_SUCCESS(Status)) {
        Status = SwBatRead16(Dev->Gauge, GAUGE_VCELL, &Vcell);
    }
    if (!NT_SUCCESS(Status)) {
        SwBatTrace(L"LastGaugeError", (ULONG)Status);
        return Status;
    }
    Percent = Soc >> 8;
    if (Percent > 100) {
        Percent = 100;
    }
    if (!NT_SUCCESS(SwBatReadRegs(Dev->Charger, CHARGER_SYSSTAT, &Sys, 1))) {
        Sys = 0;                     /* unknown: treat as on battery */
    }
    VbusStat = (Sys >> 6) & 3;
    ChrgStat = (Sys >> 4) & 3;
    Online = (VbusStat == 1 || VbusStat == 2) && (Sys & 0x04) != 0;   /* OTG (3) = Switch supplies VBUS */

    Out->PowerState = 0;
    if (Online) {
        Out->PowerState |= BATTERY_POWER_ON_LINE;
        if (ChrgStat == 1 || ChrgStat == 2) {
            Out->PowerState |= BATTERY_CHARGING;
        }
    } else {
        Out->PowerState |= BATTERY_DISCHARGING;
    }
    Out->Capacity = Percent;
    Out->Voltage = ((ULONG)(Vcell >> 3) * 625u) / 1000u;
    Out->Rate = (LONG)BATTERY_UNKNOWN_RATE;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------- battery class */

NTSTATUS SwBatQueryTag(PVOID Context, PULONG BatteryTag)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    *BatteryTag = (Dev->Gauge != NULL) ? BATTERY_TAG : BATTERY_TAG_INVALID;
    return (Dev->Gauge != NULL) ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS SwBatReturn(PVOID Source, ULONG Size, PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    *ReturnedLength = Size;
    if (BufferLength < Size) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Buffer, Source, Size);
    return STATUS_SUCCESS;
}

NTSTATUS SwBatQueryInformation(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level, LONG AtRate,
                               PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    UNREFERENCED_PARAMETER(AtRate);

    *ReturnedLength = 0;
    if (BatteryTag != BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    switch (Level) {
    case BatteryInformation: {
        BATTERY_INFORMATION Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
        Info.Technology = 1;                       /* rechargeable */
        Info.Chemistry[0] = 'L'; Info.Chemistry[1] = 'I'; Info.Chemistry[2] = 'O'; Info.Chemistry[3] = 'N';
        Info.DesignedCapacity = 100;
        Info.FullChargedCapacity = 100;
        Info.DefaultAlert1 = 5;
        Info.DefaultAlert2 = 10;
        Info.CriticalBias = 0;
        Info.CycleCount = 0;
        return SwBatReturn(&Info, sizeof(Info), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryGranularityInformation: {
        BATTERY_REPORTING_SCALE Scale = { 1, 100 };
        return SwBatReturn(&Scale, sizeof(Scale), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryTemperature: {
        USHORT Raw = 0;
        ULONG Kelvin10;
        NTSTATUS Status;
        WdfWaitLockAcquire(Dev->Lock, NULL);
        Status = SwBatRead16(Dev->Gauge, GAUGE_TEMP, &Raw);
        WdfWaitLockRelease(Dev->Lock);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Kelvin10 = (ULONG)((((LONG)(SHORT)Raw) * 10) / 256 + 2732);
        return SwBatReturn(&Kelvin10, sizeof(Kelvin10), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryEstimatedTime: {
        ULONG Time = BATTERY_UNKNOWN_TIME;
        return SwBatReturn(&Time, sizeof(Time), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryDeviceName: {
        static const WCHAR Name[] = L"Nintendo Switch Battery";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryManufactureName: {
        static const WCHAR Name[] = L"Nintendo";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryUniqueID: {
        static const WCHAR Name[] = L"SwitchBAT0";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS SwBatSetInformation(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS SwBatQueryStatus(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS BatteryStatus)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    NTSTATUS Status;
    if (BatteryTag != BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    WdfWaitLockAcquire(Dev->Lock, NULL);
    Status = SwBatReadStatus(Dev, BatteryStatus);
    if (NT_SUCCESS(Status)) {
        Dev->Last = *BatteryStatus;
    }
    WdfWaitLockRelease(Dev->Lock);
    return Status;
}

/* Status changes are found by the poll timer, which calls BatteryClassStatusNotify. */
NTSTATUS SwBatSetStatusNotify(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY BatteryNotify)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryNotify);
    return (BatteryTag == BATTERY_TAG) ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

NTSTATUS SwBatDisableStatusNotify(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

VOID SwBatEvtTimer(WDFTIMER Timer)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(SwBatGetTimer(Timer)->Device);
    BATTERY_STATUS Now;
    BOOLEAN Changed = FALSE;

    WdfWaitLockAcquire(Dev->Lock, NULL);
    if (NT_SUCCESS(SwBatReadStatus(Dev, &Now))) {
        Changed = (Now.PowerState != Dev->Last.PowerState) || (Now.Capacity != Dev->Last.Capacity);
        SwBatTrace(L"LastPowerState", Now.PowerState);
        SwBatTrace(L"LastCapacity", Now.Capacity);
        SwBatTrace(L"LastVoltage", Now.Voltage);
    }
    WdfWaitLockRelease(Dev->Lock);
    SwBatTrace(L"TimerRuns", (ULONG)InterlockedIncrement(&Dev->TimerRuns));
    if (Changed && Dev->ClassHandle != NULL) {
        (VOID)BatteryClassStatusNotify(Dev->ClassHandle);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot timer, re-armed */
    }
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwBatEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    Dev->Connections = 0;
    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER Id;
            Id.LowPart = Desc->u.Connection.IdLowPart;
            Id.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            if (Dev->Connections == 0) {
                Dev->GaugeId = Id;
            } else if (Dev->Connections == 1) {
                Dev->ChargerId = Id;
            }
            Dev->Connections++;
        }
    }
    SwBatTrace(L"I2cConnections", Dev->Connections);
    SwBatTrace(L"GaugeIdLow", Dev->GaugeId.LowPart);
    SwBatTrace(L"GaugeIdHigh", (ULONG)Dev->GaugeId.HighPart);
    SwBatTrace(L"ChargerIdLow", Dev->ChargerId.LowPart);
    return (Dev->Connections >= 2) ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS SwBatEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    BATTERY_MINIPORT_INFO Info;
    BATTERY_STATUS First;
    NTSTATUS Status;

    Status = SwBatOpenTarget(Device, Dev->GaugeId, &Dev->Gauge);
    SwBatTrace(L"OpenGaugeStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = SwBatOpenTarget(Device, Dev->ChargerId, &Dev->Charger);
    SwBatTrace(L"OpenChargerStatus", (ULONG)Status);    /* not fatal: unknown = on battery */

    WdfWaitLockAcquire(Dev->Lock, NULL);
    Status = SwBatReadStatus(Dev, &First);
    if (NT_SUCCESS(Status)) {
        Dev->Last = First;
    }
    WdfWaitLockRelease(Dev->Lock);
    SwBatTrace(L"FirstReadStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        SwBatTrace(L"FirstCapacity", First.Capacity);
        SwBatTrace(L"FirstVoltage", First.Voltage);
        SwBatTrace(L"FirstPowerState", First.PowerState);
    }

    RtlZeroMemory(&Info, sizeof(Info));
    Info.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    Info.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    Info.Context = Dev;
    Info.QueryTag = SwBatQueryTag;
    Info.QueryInformation = SwBatQueryInformation;
    Info.SetInformation = SwBatSetInformation;
    Info.QueryStatus = SwBatQueryStatus;
    Info.SetStatusNotify = SwBatSetStatusNotify;
    Info.DisableStatusNotify = SwBatDisableStatusNotify;
    Info.Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    Info.DeviceName = NULL;
    Status = BatteryClassInitializeDevice(&Info, &Dev->ClassHandle);
    SwBatTrace(L"ClassInitStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        Dev->ClassHandle = NULL;
        return Status;
    }

    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        /* v3: v2 used a periodic passive-level timer, which never ran (no
         * LastPowerState trace, AC icon stuck after unplugging). KMDF does
         * not allow periodic timers at passive level, so use a one-shot
         * timer that the callback re-arms. */
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwBatEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBAT_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwBatTrace(L"TimerCreateStatus", (ULONG)Status);
        if (NT_SUCCESS(Status)) {
            SwBatGetTimer(Dev->Timer)->Device = Device;
        } else {
            Dev->Timer = NULL;
        }
    }
    Dev->Stopping = FALSE;
    if (Dev->Timer != NULL) {
        WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    }
    return STATUS_SUCCESS;
}

VOID SwBatEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->ClassHandle != NULL) {
        (VOID)BatteryClassUnload(Dev->ClassHandle);
        Dev->ClassHandle = NULL;
    }
}

VOID SwBatEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG IoControlCode)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    if (Dev->ClassHandle != NULL) {
        /* battc completes the IRP itself unless it does not handle it. */
        Status = BatteryClassIoctl(Dev->ClassHandle, WdfRequestWdmGetIrp(Request));
    }
    if (Status == STATUS_NOT_SUPPORTED) {
        WdfRequestComplete(Request, Status);
    }
}

NTSTATUS SwBatEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFDEVICE Device;
    PSWBAT_DEVICE Dev;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwBatEvtPrepareHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwBatEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwBatEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBAT_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwBatGetDevice(Device);
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = SwBatEvtIoDeviceControl;
    return WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwBatParamsPath)) {
        RtlCopyMemory(SwBatParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwBatParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwBatParamsPath);
    }
    SwBatTrace(L"DriverVersion", 3);

    WDF_DRIVER_CONFIG_INIT(&Config, SwBatEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}

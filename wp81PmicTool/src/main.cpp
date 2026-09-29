// wp81PmicTool - sends IOCTLs to the Qualcomm PMIC driver qcpmic8930.sys (\\.\QCOMPMIC)
// and prints the result.
//
// Usage: wp81PmicTool [options] <ioctl> [value ...]   send one IOCTL
//        wp81PmicTool [-p pmic] auto                   send every read-only IOCTL
//        wp81PmicTool list [filter]                    print the known IOCTLs
//
// Every IOCTL of the driver is METHOD_BUFFERED / FILE_ANY_ACCESS and the driver checks
// both buffer lengths for an exact match against its descriptor table, so the tool
// always sends the required sizes unless -i / -o say otherwise.

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CreateFileW, DeviceIoControl, GetModuleHandleW and GetProcAddress are exported by
// kernelbase (mincore.lib) on Windows Phone 8.1, but the SDK headers only declare some
// of them for the desktop partition.
extern "C" {
WINBASEAPI HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
                                     DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                     DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                     HANDLE hTemplateFile);
WINBASEAPI BOOL WINAPI DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode,
                                       LPVOID lpInBuffer, DWORD nInBufferSize,
                                       LPVOID lpOutBuffer, DWORD nOutBufferSize,
                                       LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
WINBASEAPI HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName);
WINBASEAPI FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
}

// NtDeviceIoControlFile gives the exact NTSTATUS, which tells apart the five
// STATUS_INVALID_PARAMETER_n codes of the driver's validation (all ERROR_INVALID_PARAMETER
// for DeviceIoControl).
struct IoStatusBlock {
    union {
        LONG Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};
typedef LONG (NTAPI *NtDeviceIoControlFileFn)(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                                              PVOID ApcContext, IoStatusBlock *IoStatus,
                                              ULONG IoControlCode, PVOID InputBuffer,
                                              ULONG InputBufferLength, PVOID OutputBuffer,
                                              ULONG OutputBufferLength);

#define DEFAULT_DEVICE_PATH L"\\\\.\\QCOMPMIC"
#define MAX_BUFFER_SIZE     4096
#define MAX_FIELDS          32
#define MAX_GPIO_LIST       64
#define STATUS_PENDING_VALUE 0x00000103L

// Class of an IOCTL
#define CLS_READ       'R'  // read-only, sent by "auto"
#define CLS_READ_MANUAL 'r' // read-only but not sent by "auto" (see the note)
#define CLS_WRITE      'W'  // changes the state of the PMIC or of the driver: needs -f
#define CLS_DANGEROUS  'D'  // may crash the phone: needs -f
#define CLS_UNHANDLED  'U'  // passes the validation, then STATUS_NOT_SUPPORTED
#define CLS_STUB       'S'  // handled, but always fails
#define CLS_NO_DEVICE  'X'  // device type not handled: STATUS_INVALID_DEVICE_REQUEST

#define SPECIAL_NONE          0
#define SPECIAL_GPIO_CONFIG   1
#define SPECIAL_PMICS_INFO    2  // hides the slots past the number of PMICs
#define SPECIAL_VREG_SETTINGS 3  // detects the empty structure of a missing resource
#define SPECIAL_BMS_OUTPUT    4  // value width and sign depend on the selector

struct IoctlInfo {
    DWORD Code;
    const char *Name;
    WORD InputSize;     // exact size required by the driver
    WORD OutputSize;    // exact size required by the driver
    char Class;
    BYTE Special;
    // Field layouts: "off type name|off type name|...", off in hex. Types:
    //   d u32   i s32   h u32 hex   b u8   w u16   q u64   Q u64 bit mask   p pointer
    //   e PMIC error   m PMIC model   r VREG resource   k VREG kind   v voltage in uV
    //   t RTC seconds   a RTC alarm time   c ADC AMUX channel   s BMS output selector
    //   f kernel function pointer (Thumb-2)   l vibrator level (100 mV units)
    //   x<n> n raw bytes
    const char *InputSpec;
    const char *OutputSpec;
    const char *Note;
};

// Common layouts
#define IN_P    "00 d pmic"
#define OUT_E   "00 e error"
#define OUT_VAL(name) "00 d " name "|04 e error"
#define VREG_SETTINGS "00 d client ? (0 when read)|04 r resource|08 k kind|0C v voltage|" \
                      "10 d LVS: 1 when on (enable ?)|14 d LVS: 1 when on ?|18 d setting[3] ?|" \
                      "1C d setting[4] ?|20 d setting[5] ?|24 d LDO: 1 on most >=1.8V LDOs ?|" \
                      "28 d setting[7] ?|2C d setting[8] ?|30 d setting[9] ?|" \
                      "34 d setting[10] ?|38 d setting[11] ?"

static const IoctlInfo g_Ioctls[] = {
    // 0x8001 Test
    { 0x80010FA0, "IOCTL_PM_TEST_VREG_VOLT_CURR_FREQ", 4, 16, CLS_STUB, 0, "00 d value ?", NULL,
      "not implemented: always STATUS_UNSUCCESSFUL" },
    { 0x80010FA4, "IOCTL_PM_TEST_MPP_GET_STATUS", 8, 12, CLS_STUB, 0, "00 d value1 ?|04 d value2 ?", NULL,
      "not implemented: always STATUS_UNSUCCESSFUL" },

    // 0x8002 ADC / BTM / CCADC
    { 0x80020FA0, "IOCTL_PM_ADC_READ_DATA", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("data (raw ADC code)"),
      "on a Lumia 520 it returned the same code as the BMS VBATT_AVG register" },
    { 0x80020FA4, "IOCTL_PM_ADC_CONFIG_MUX", 8, 4, CLS_WRITE, 0, IN_P "|04 d mux", OUT_E, NULL },
    { 0x80020FA8, "IOCTL_PM_ADC_SET_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80020FAC, "IOCTL_PM_ADC_REQUEST_CONVERSION", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x80020FB0, "IOCTL_PM_ADC_GET_CONVERSION_STATUS", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("status"), NULL },
    { 0x80020FB4, "IOCTL_PM_ADC_SET_INPUT", 8, 4, CLS_WRITE, 0, IN_P "|04 d input", OUT_E, NULL },
    { 0x80020FB8, "IOCTL_PM_ADC_SET_DECIMATION_RATIO", 8, 4, CLS_WRITE, 0, IN_P "|04 d ratio", OUT_E, NULL },
    { 0x80020FBC, "IOCTL_PM_ADC_SET_CONVERSION_RATE", 8, 4, CLS_WRITE, 0, IN_P "|04 d rate", OUT_E, NULL },
    { 0x80020FC0, "IOCTL_PM_ADC_GET_PRESCALAR", 8, 12, CLS_READ, 0, IN_P "|04 c AMUX channel",
      "00 d numerator|04 d denominator|08 e error", "channels 0-14; 15 fails" },
    { 0x80020FC4, "IOCTL_PM_ADC_CONFIG_PREMUX", 8, 4, CLS_WRITE, 0, IN_P "|04 d premux", OUT_E, NULL },
    { 0x80020FC8, "IOCTL_PM_ADC_CONFIG_CONVERSION_SEQUENCER", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3", OUT_E, "applied as 3 calls: value2, value1, value3" },
    { 0x80020FCC, "IOCTL_PM_ADC_ENABLE_CONVERSION_SEQUENCER", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80020FD0, "IOCTL_PM_ADC_BTM_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 b value1|05 b value2", OUT_E, NULL },
    { 0x80020FD4, "IOCTL_PM_ADC_BTM_DISABLE", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x80020FD8, "IOCTL_PM_ADC_BTM_READ_DATA", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("data"), NULL },
    { 0x80020FDC, "IOCTL_PM_ADC_BTM_SET_WARM_THRESHOLD", 8, 4, CLS_WRITE, 0, IN_P "|04 d threshold", OUT_E, NULL },
    { 0x80020FE0, "IOCTL_PM_ADC_BTM_SET_COOL_THRESHOLD", 8, 4, CLS_WRITE, 0, IN_P "|04 d threshold", OUT_E, NULL },
    { 0x80020FE4, "IOCTL_PM_ADC_BTM_GET_CONVERSION_STATUS", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("status"), NULL },
    { 0x80020FE8, "IOCTL_PM_ADC_BTM_REQUEST_CONVERSION", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x80020FEC, "IOCTL_PM_ADC_SET_PREMUX_OUTPUT", 8, 4, CLS_WRITE, 0, IN_P "|04 d output", OUT_E, NULL },
    { 0x80020FF0, "IOCTL_PM_ADC_BTM_CONFIG_MUX", 8, 4, CLS_WRITE, 0, IN_P "|04 d mux", OUT_E, NULL },
    { 0x80020FF4, "IOCTL_PM_ADC_BTM_SET_INPUT", 8, 4, CLS_WRITE, 0, IN_P "|04 d input", OUT_E, NULL },
    { 0x80020FF8, "IOCTL_PM_READ_SEQUENCER_TIMEOUT_FLAG", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("flag"), NULL },
    { 0x80020FFC, "IOCTL_PM_READ_SEQUENCER_FIFO_FLAG", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("flag"), NULL },
    { 0x80021000, "IOCTL_PM_CCADC_READ_DATA", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("data"),
      "the driver reports 4 bytes returned, so the error field is not returned" },
    { 0x80021004, "IOCTL_PM_CCADC_SET_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80021008, "IOCTL_PM_CCADC_REQUEST_CONVERSION", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x8002100C, "IOCTL_PM_CCADC_SET_DECIMATION_RATIO", 8, 4, CLS_WRITE, 0, IN_P "|04 d ratio", OUT_E, NULL },
    { 0x80021010, "IOCTL_PM_CCADC_SET_CONVERSION_RATE", 8, 4, CLS_WRITE, 0, IN_P "|04 d rate", OUT_E, NULL },
    { 0x80021014, "IOCTL_PM_CCADC_CONNECT_RSENSE", 8, 4, CLS_WRITE, 0, IN_P "|04 d connect", OUT_E, NULL },
    { 0x80021018, "IOCTL_PM_CCADC_CONFIGURE_OFFSET", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x8002101C, "IOCTL_PM_CCADC_CONFIGURE_GAIN", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x80021020, "IOCTL_PM_CCADC_SET_OFFSET_TRIM", 8, 4, CLS_WRITE, 0, IN_P "|04 d trim", OUT_E, NULL },
    { 0x80021024, "IOCTL_PM_CCADC_GET_CONVERSION_STATUS", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("status"), NULL },
    { 0x80021028, "IOCTL_PM_BTM_CONFIG_PREMUX", 8, 4, CLS_WRITE, 0, IN_P "|04 d premux", OUT_E, NULL },
    { 0x8002102C, "IOCTL_PM_BTM_SET_PREMUX_OUTPUT", 8, 4, CLS_WRITE, 0, IN_P "|04 d output", OUT_E, NULL },
    { 0x80021030, "IOCTL_PM_CCADC_SET_SEL_SHIFT", 8, 4, CLS_WRITE, 0, IN_P "|04 d shift", OUT_E, NULL },

    // 0x8003 GPIO
    { 0x80030FA0, "IOCTL_PM_GPIO_CONFIG_BIAS_VOLTAGE", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d gpio|08 d voltage source", OUT_E, NULL },
    { 0x80030FA4, "IOCTL_PM_GPIO_CONFIG_DIGITAL_INPUT", 20, 4, CLS_WRITE, 0,
      IN_P "|04 d gpio|08 d pull|0C d voltage source|10 d source", OUT_E, NULL },
    { 0x80030FA8, "IOCTL_PM_GPIO_CONFIG_DIGITAL_OUTPUT", 24, 4, CLS_WRITE, 0,
      IN_P "|04 d gpio|08 d buffer ?|0C d voltage source ?|10 d source ?|14 d strength ?", OUT_E, NULL },
    { 0x80030FAC, "IOCTL_PM_GPIO_SET_INVERSION_CONFIGURATION", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d gpio|08 d invert", OUT_E, NULL },
    { 0x80030FB0, "IOCTL_PM_GPIO_SET_EXT_PIN_CONFIG", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d gpio|08 d config", OUT_E, NULL },
    { 0x80030FB4, "IOCTL_PM_GPIO_GET_GPIO_STATE", 12, 8, CLS_READ, 0,
      IN_P "|04 d gpio|08 d source (0 = interrupt real-time status)", "00 e error|04 d state",
      "source 0 reads the real-time status of interrupt 192 + gpio (same bits as IRQ block 3, "
      "type 1); on a PM8038, source 1 returns 0 for every GPIO" },
    { 0x80030FB8, "IOCTL_PM_GPIO_REGISTER", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80030FBC, "IOCTL_PM_GPIO_UNREGISTER", 0, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80030FC0, "IOCTL_PM_GPIO_GET_NUMBER_OF_GPIOS", 4, 12, CLS_READ, 0, IN_P,
      "00 d pmic|04 d number of GPIOs|08 e error", NULL },
    { 0x80030FC4, "IOCTL_PM_GPIO_GET_GPIO_CONFIG", 12, 2816, CLS_DANGEROUS, SPECIAL_GPIO_CONFIG,
      "00 d pmic|04 p GPIO list pointer|08 d count", NULL,
      "the driver dereferences a raw user pointer: a bugcheck is possible if the request is "
      "not handled in this process's context" },

    // 0x8004 Keypad / power key
    { 0x80040FA0, "IOCTL_PM_KEYPAD_CONFIG", 36, 4, CLS_WRITE, 0,
      IN_P "|04 d config[0]|08 d config[1]|0C d config[2]|10 d config[3]|14 d config[4]|"
      "18 d config[5]|1C d config[6]|20 d config[7]", OUT_E, NULL },
    { 0x80040FA4, "IOCTL_PM_KEYPAD_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80040FA8, "IOCTL_PM_KEYPAD_GET_MATRIX", 8, 76, CLS_READ_MANUAL, 0, IN_P "|04 d unknown ?",
      "00 e error|04 d unknown ?|08 d count ?|0C x32 matrix|2C x32 unknown ?",
      "not in auto: the meaning of the input value is unknown" },
    { 0x80040FAC, "IOCTL_PM_KEYPAD_POLL_RECENT_MATRIX", 4, 40, CLS_READ_MANUAL, 0, IN_P,
      "00 e error|04 d count ?|08 x32 matrix", "not in auto: polling may consume keypad events" },
    { 0x80040FB0, "IOCTL_PM_KEYPAD_GET_POWER_KEY", 4, 8, CLS_READ, 0, IN_P,
      "00 e error|04 d power-key state",
      "fails with STATUS_UNSUCCESSFUL on a PM8038 (power-key function probably missing for model 3)" },
    { 0x80040FB4, "IOCTL_PM_KEYPAD_CONFIG_POWER_KEY", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2", OUT_E, NULL },
    { 0x80040FB8, "IOCTL_PM_KEYPAD_CONFIG_HARD_RESET", 24, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3|10 d value4|14 d value5", OUT_E,
      "applied as 3 calls: (value1), (value2), (value4, value5, value3)" },

    // 0x8005 MPP
    { 0x80050FA0, "IOCTL_PM_MPP_CONFIG_DIGITAL_INPUT", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d level|0C d detection ?", OUT_E, NULL },
    { 0x80050FA4, "IOCTL_PM_MPP_CONFIG_DIGITAL_OUTPUT", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d level/control ?", OUT_E, NULL },
    { 0x80050FA8, "IOCTL_PM_MPP_CONFIG_DIGITAL_INOUT", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d value1|0C d value2", OUT_E, NULL },
    { 0x80050FAC, "IOCTL_PM_MPP_CONFIG_ANALOG_INPUT", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d channel", OUT_E, NULL },
    { 0x80050FB0, "IOCTL_PM_MPP_CONFIG_ANALOG_OUTPUT", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d level|0C d on/off ?", OUT_E, NULL },
    { 0x80050FB4, "IOCTL_PM_MPP_CONFIG_I_SINK", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d current level|0C d switch ?", OUT_E, NULL },
    { 0x80050FB8, "IOCTL_PM_MPP_CONFIG_ATEST", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d channel", OUT_E, NULL },
    { 0x80050FBC, "IOCTL_PM_MPP_GET_INPUT_STATE", 12, 8, CLS_READ, 0,
      IN_P "|04 d mpp|08 d source (0 = interrupt real-time status)", "00 e error|04 d state",
      "source 0 reads the real-time status of interrupt 128 + mpp, without checking the MPP number; "
      "on a PM8038, source 1 fails for every MPP" },
    { 0x80050FC0, "IOCTL_PM_MPP_SET_OUTPUT_STATE", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d mpp|08 d state", OUT_E, NULL },
    { 0x80050FC4, "IOCTL_PM_MPP_REGISTER", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80050FC8, "IOCTL_PM_MPP_UNREGISTER", 0, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },

    // 0x8006 Voltage regulators
    { 0x80060FA0, "IOCTL_PM_BEGIN_SEQUENCE", 0, 4, CLS_WRITE, 0, NULL, OUT_E,
      "starts a group of votes" },
    { 0x80060FA4, "IOCTL_PM_END_SEQUENCE", 0, 4, CLS_WRITE, 0, NULL, OUT_E,
      "ends a group of votes" },
    { 0x80060FA8, "IOCTL_PM_VREG_SMPS_CLOCK_SEL", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FAC, "IOCTL_PM_VREG_SMPS_TCXO_DIV_SEL", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FB0, "IOCTL_PM_VREG_BUCK_CONFIG_COMP", 8, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FB4, "IOCTL_PM_VREG_LDO_BYPASS_SET", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FB8, "IOCTL_PM_VREG_LDO_BYPASS_CLEAR", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FBC, "IOCTL_PM_VREG_LDO_CURRENT_LIMIT_ENABLE", 8, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FC0, "IOCTL_PM_VREG_SMPS_SWITCH_SIZE_SET", 8, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FC4, "IOCTL_PM_VREG_SMPS_CONFIG", 12, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FC8, "IOCTL_PM_VREG_SMPS_SWITCH_DRIVER_SIZE_SET", 8, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FCC, "IOCTL_PM_VREG_SMPS_PULSE_SKIPPING_ENABLE", 8, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FD0, "IOCTL_PM_VREG_SMPS_SET_STEPPER_CONFIG", 16, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FD4, "IOCTL_PM_VREG_VOTE_FOR_POWER_SETTINGS", 60, 4, CLS_DANGEROUS, 0, VREG_SETTINGS, OUT_E,
      "a wrong vote can switch off a regulator the phone needs; the vote compares 10 dwords "
      "(00-27) for an LDO, 13 (00-33) for an SMPS, 8 for an LVS/NCP" },
    { 0x80060FD8, "IOCTL_PM_VREG_REGISTER", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FDC, "IOCTL_PM_VREG_UNREGISTER", 0, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80060FE0, "IOCTL_PM_VREG_INITIALIZE", 0, 4, CLS_WRITE, 0, NULL, OUT_E,
      "initialises the vote layer if needed" },
    { 0x80060FE4, "IOCTL_PM_VREG_GET_POWER_SETTINGS", 4, 60, CLS_READ, SPECIAL_VREG_SETTINGS, "00 r resource",
      VREG_SETTINGS,
      "the resource can be given by name (SMPS1, LDO5, CXO_CLOCK, ...)" },

    // 0x8007 Interrupts
    { 0x80070FA0, "IOCTL_PM_IRQ_CONFIG_INTERRUPT", 24, 4, CLS_WRITE, 0,
      IN_P "|04 d block|08 d bit|14 d trigger", OUT_E, "interrupt id = block * 64 + bit" },
    { 0x80070FA4, "IOCTL_PM_IRQ_CLEAR_ACTIVE_INTERRUPTS", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d block|08 Q mask", OUT_E, NULL },
    { 0x80070FA8, "IOCTL_PM_IRQ_GET_INTERRUPTS_STATUS", 24, 16, CLS_READ, 0,
      IN_P "|04 d block|08 Q mask|10 d type (0 = latched, 1 = real-time)", "00 Q status|08 e error",
      "interrupt id = block * 64 + bit; type 0 reads IT_STATUS (0x1C1), type 1 RT_STATUS (0x1C3); "
      "bytes 0C-0F are reported as returned but not written by the driver" },
    { 0x80070FAC, "IOCTL_PM_IRQ_MASK_INTERRUPTS", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d block|08 Q mask", OUT_E, NULL },
    { 0x80070FB0, "IOCTL_PM_IRQ_UNMASK_INTERRUPT", 24, 4, CLS_WRITE, 0,
      IN_P "|04 d block|08 d bit|10 d trigger", OUT_E, NULL },
    { 0x80070FB4, "IOCTL_PM_IRQ_TRIGGER_INTERRUPT", 8, 4, CLS_WRITE, 0,
      IN_P "|04 d interrupt id", OUT_E, "reprograms the trigger of the interrupt so that it fires" },

    // 0x8008 Charger
    { 0x80080FA0, "IOCTL_PM_CHG_DEVICE_BOOT_DONE", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x80080FA4, "IOCTL_PM_CHG_USB_ENUM_TIMER_STOP", 8, 4, CLS_WRITE, 0, IN_P "|04 d ignored", OUT_E, NULL },
    { 0x80080FA8, "IOCTL_PM_CHG_PARAMS", 28, 4, CLS_WRITE, 0,
      IN_P "|04 d value1 (entry 4, VMAX ?)|08 d value2 (entry 3, IMAX ?)|0C d value3 (entry 5, VBATDET ?)|"
      "10 d value4 (entry 9, ITERM ?)|14 d value5 (entry 7)|18 d value6 (entry 8)", OUT_E, NULL },
    { 0x80080FAC, "IOCTL_PM_CHG_GET_STATE", 4, 16, CLS_READ, 0, IN_P,
      "00 d zero|04 d charger state|08 b byte8 ?|09 b byte9 ?|0C e error", NULL },
    { 0x80080FB0, "IOCTL_PM_CHG_FSM_ENABLE", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1 (byte)|08 d value2|0C d value3 (byte)", OUT_E,
      "applied as 3 calls: value2, value3, value1" },
    { 0x80080FB4, "IOCTL_PM_CHG_PET_WDOG", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x80080FB8, "IOCTL_PM_CHG_CHGPATH_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80080FBC, "IOCTL_PM_CHG_BATTSAFE", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1|08 d value2", OUT_E,
      "always applied to PMIC 0, whatever the pmic value" },
    { 0x80080FC0, "IOCTL_PM_CHG_VIN_MIN", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1 (byte)|08 d value2", OUT_E, NULL },
    { 0x80080FC4, "IOCTL_PM_CHG_BATT_TEMP_CONFIG", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3", OUT_E, NULL },
    { 0x80080FC8, "IOCTL_PM_CHG_BATT_TEMP_CNTRL", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x80080FCC, "IOCTL_PM_CHG_VREF_BATT_THERM_CNTRL", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x80080FD0, "IOCTL_PM_CHG_GET_BATTSAFE", 4, 12, CLS_READ, 0, IN_P,
      "00 d VMAX_SAFE (mV) ?|04 d IMAX_SAFE (mA) ?|08 e error", NULL },
    { 0x80080FD4, "IOCTL_PM_CHG_GET_REGULATION_LOOP", 4, 8, CLS_READ, 0, IN_P,
      "00 b flag0|01 b flag1|02 b flag2|03 b flag3|04 e error", NULL },
    { 0x80080FD8, "IOCTL_PM_CHG_FORCE_OVPFET_ON_OFF", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2 (byte)", OUT_E, NULL },
    { 0x80080FDC, "IOCTL_PM_CHG_OVP_SET_DEBOUNCE_TIME", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2", OUT_E, NULL },
    { 0x80080FE0, "IOCTL_PM_CHG_CLOCK_KICKSTART_CRITICAL", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x80080FE4, "IOCTL_PM_CHG_READ_CHARGING_PARAMTERS", 4, 28, CLS_READ, 0, IN_P,
      "00 d input current limit (mA) ?|04 d battery current (mA) ?|08 d VBATDET delta (mV) ?|"
      "0C d ITERM (mA) ?|10 d VMAX (mV) ?|14 d VIN_MIN (mV) ?|18 e error",
      "written in the order 04, 00, 10, 08, 0C, 14" },

    // 0x800A RTC
    { 0x800A0FA0, "IOCTL_PM_RTC_START", 8, 4, CLS_WRITE, 0, IN_P "|04 t start time", OUT_E, NULL },
    { 0x800A0FA4, "IOCTL_PM_RTC_STOP", 4, 4, CLS_WRITE, 0, IN_P, OUT_E, NULL },
    { 0x800A0FA8, "IOCTL_PM_RTC_GET_TIME", 4, 8, CLS_READ, 0, IN_P, "00 t time|04 e error", NULL },
    { 0x800A0FAC, "IOCTL_PM_RTC_GET_TIME_ADJUST", 4, 8, CLS_READ, 0, IN_P, OUT_VAL("adjust"), NULL },
    { 0x800A0FB0, "IOCTL_PM_RTC_SET_TIME_ADJUST", 8, 4, CLS_WRITE, 0, IN_P "|04 d adjust", OUT_E, NULL },
    { 0x800A0FB4, "IOCTL_PM_RTC_ENABLE_ALARM", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d alarm id|08 d delay (s, relative to the RTC time)", OUT_E, NULL },
    { 0x800A0FB8, "IOCTL_PM_RTC_DISABLE_ALARM", 8, 4, CLS_WRITE, 0, IN_P "|04 d alarm id", OUT_E, NULL },
    { 0x800A0FBC, "IOCTL_PM_RTC_GET_ALARM_TIME", 8, 8, CLS_READ, 0, IN_P "|04 d alarm id",
      "00 a alarm time|04 e error", NULL },
    { 0x800A0FC0, "IOCTL_PM_RTC_GET_ALARM_STATUS", 4, 8, CLS_READ, 0, IN_P, "00 b status|04 e error", NULL },

    // 0x800D Audio
    { 0x800D0FA0, "IOCTL_PM_AUDIO_HSED_CONFIG_CONTROL_REGISTER", 32, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3|10 d value4|14 d value5|18 d value6|1C d value7", OUT_E, NULL },
    { 0x800D0FA4, "IOCTL_PM_AUDIO_HSED_ENABLE_CONTROL_REGISTER", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d HSED id ?|08 d enable ?", OUT_E, NULL },

    // 0x800E PWM / LPG
    { 0x800E0FA0, "IOCTL_PM_PWM_SELECT_LPG_PARAMETERS", 40, 4, CLS_WRITE, 0, IN_P, OUT_E,
      "layout unknown after the pmic index: use -r or -x" },
    { 0x800E0FA4, "IOCTL_PM_SET_PWM_VALUE", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1 ?|08 d value2 ?", OUT_E, NULL },
    { 0x800E0FA8, "IOCTL_PM_CONFIGURE_LUT", 40, 4, CLS_WRITE, 0, IN_P, OUT_E,
      "layout unknown after the pmic index: use -r or -x" },
    { 0x800E0FAC, "IOCTL_PM_START_LUT", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C b value3", OUT_E, NULL },

    // 0x800F LED
    { 0x800F0FA0, "IOCTL_PM_LED_CONFIG", 16, 4, CLS_WRITE, 0,
      "00 d value1 ?|04 d value2 ?|08 d value3 ?|0C d value4 ?", OUT_E, NULL },
    { 0x800F0FA4, "IOCTL_GET_LED_INTERFACE", 0, 8, CLS_READ, 0, NULL,
      "00 f PM_LED_CONFIG address|04 f PM_LED_CONFIG_SLEEP address",
      "kernel addresses of two driver functions" },
    { 0x800F0FA8, "IOCTL_PM_LED_SLEEP_CONFIG", 20, 4, CLS_WRITE, 0,
      "00 d value1 ?|04 d value2 ?|08 d value3 ?|0C d value4 ?|10 d value5 ?", OUT_E, NULL },

    // 0x8011 PSI
    { 0x80110FA0, "IOCTL_PM_PSI_SET_MODE", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1|08 d value2", OUT_E, NULL },
    { 0x80110FA4, "IOCTL_PM_PSI_SEND", 912, 4, CLS_WRITE, 0, IN_P "|04 x900 data|388 d length", OUT_E,
      "the driver's descriptor carries 0x80110FA0, so the validation probably rejects this code" },
    { 0x80110FA8, "IOCTL_PM_PSI_SEND_RECEIVE", 912, 908, CLS_WRITE, 0,
      IN_P "|04 x900 data|388 d length|38C d unknown ?", "00 x900 received data|384 d length ?|388 e error", NULL },

    // 0x8012 Loudspeaker
    { 0x80120FA0, "IOCTL_PM_SPEAKER_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80120FA4, "IOCTL_PM_SPEAKER_MUTE", 8, 4, CLS_WRITE, 0, IN_P "|04 d mute", OUT_E, NULL },
    { 0x80120FA8, "IOCTL_PM_SPEAKER_BYPASS_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80120FAC, "IOCTL_PM_SPEAKER_ADDMODE_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80120FB0, "IOCTL_PM_SPEAKER_CD_GAIN", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1|08 d value2 (byte)", OUT_E, NULL },
    { 0x80120FB4, "IOCTL_PM_SPEAKER_NOISE_PARAMETERS", 36, 4, CLS_WRITE, 0, IN_P, OUT_E,
      "layout unknown after the pmic index (byte at 14, values at 18, 1C...): use -r or -x" },

    // 0x8013 RGB LED
    { 0x80130FA0, "IOCTL_PM_RGB_LED_ENABLE", 12, 4, CLS_WRITE, 0,
      IN_P "|04 b colour mask ?|05 b enable ?|08 d source ?", OUT_E, NULL },

    // 0x8014 WLED
    { 0x80140FA0, "IOCTL_PM_WLED_CONFIG", 56, 4, CLS_WRITE, 0, IN_P, OUT_E,
      "mixed bytes/dwords at 04, 08, 09, 0C, 14, 15, 17, 18, 1C, 24-34: use -r or -x" },
    { 0x80140FA4, "IOCTL_PM_WLED_ENABLE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80140FA8, "IOCTL_PM_WLED_DIMMING_CONFIG", 12, 4, CLS_WRITE, 0, IN_P "|04 d value1|08 d value2", OUT_E, NULL },
    { 0x80140FAC, "IOCTL_PM_WLED_CONFIG_ADDITIONAL_PARAM", 56, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2 (byte)|0C d value3|10 d value4|14 d value5|18 d value6|1C d value7",
      OUT_E, NULL },

    // 0x8015 Hardware info
    { 0x80150FA0, "IOCTL_PM_HARDWARE_REGISTER", 4, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80150FA4, "IOCTL_PM_HARDWARE_UNREGISTER", 0, 4, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80150FA8, "IOCTL_PM_HARDWARE_GET_NUMBER_OF_PMICS", 0, 4, CLS_READ, 0, NULL,
      "00 d number of PMICs", NULL },
    { 0x80150FAC, "IOCTL_PM_HARDWARE_GET_PMICS_INFO", 0, 32, CLS_READ, SPECIAL_PMICS_INFO, NULL,
      "00 d number of PMICs|04 d PMIC 0 index|08 m PMIC 0 model|0C d PMIC 0 revision|"
      "10 d PMIC 1 index|14 m PMIC 1 model|18 d PMIC 1 revision|1C e error", NULL },

    // 0x8019 BMS
    { 0x80190FA0, "IOCTL_PM_GAUGE_BMS_OUTPUT_REG_STOP_UPDATE_BMS", 8, 4, CLS_WRITE, 0, IN_P "|04 d stop", OUT_E, NULL },
    { 0x80190FA4, "IOCTL_PM_GAUGE_READ_BMS_OUTPUT_REG_BMS", 8, 8, CLS_READ, SPECIAL_BMS_OUTPUT,
      IN_P "|04 s selector", OUT_VAL("value"),
      "selects bits 4:2 of BMS_CONTROL (0x224), then reads BMS_OUTPUT0/1; values are raw ADC codes" },
    { 0x80190FA8, "IOCTL_PM_GAUGE_CALIBRATE_BMS", 8, 4, CLS_WRITE, 0, IN_P "|04 d value", OUT_E, NULL },
    { 0x80190FAC, "IOCTL_PM_GAUGE_ENABLE_BMS", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80190FB0, "IOCTL_PM_GAUGE_BMS_OVERRIDE_TRIGGER_BMS", 4, 4, CLS_STUB, 0, IN_P, OUT_E,
      "stub: always PMIC error 19" },
    { 0x80190FB4, "IOCTL_PM_GAUGE_BMS_OVERRIDE_READ_BMS", 4, 4, CLS_STUB, 0, IN_P, OUT_E,
      "stub: always PMIC error 19" },
    { 0x80190FB8, "IOCTL_PM_GAUGE_BMS_SET_VSENSE_THR_BMS", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d value1 (state ?)|08 d value2 (threshold ?)", OUT_E, NULL },
    { 0x80190FBC, "IOCTL_PM_GAUGE_BMS_SET_SAMPLE_AVG_BMS", 12, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2", OUT_E, NULL },
    { 0x80190FC0, "IOCTL_PM_GAUGE_BMS_GET_BMS_STATE", 8, 32, CLS_UNHANDLED, 0, NULL, NULL, NULL },
    { 0x80190FC4, "IOCTL_PM_GAUGE_BMS_OVERRIDE_VBAT_MODE", 8, 4, CLS_WRITE, 0,
      IN_P "|04 b value1|05 b value2", OUT_E, NULL },
    { 0x80190FC8, "IOCTL_PM_GAUGE_BMS_SET_CHARGING_STATE", 8, 4, CLS_WRITE, 0, IN_P "|04 d state", OUT_E, NULL },
    { 0x80190FCC, "IOCTL_PM_GAUGE_BMS_ENABLE_OCV_UPDATE", 8, 4, CLS_WRITE, 0, IN_P "|04 d enable", OUT_E, NULL },
    { 0x80190FD0, "IOCTL_PM_GAUGE_BMS_CONFIGURE", 36, 4, CLS_WRITE, 0,
      IN_P "|04 d mode|08 d param1|0C d param2|10 d param3|14 d threshold1|18 d threshold2|"
      "1C d threshold3|20 d threshold4", OUT_E, "-1 = unchanged" },

    // 0x801A Battery alarm
    { 0x801A0FA0, "IOCTL_PM_BATALRM_CONFIG_CONTROL_REGISTER", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3", OUT_E, NULL },
    { 0x801A0FA4, "IOCTL_PM_BATALRM_ENABLE_CONTROL_REGISTER", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d value1|08 d value2|0C d value3", OUT_E, NULL },
    { 0x801A0FA8, "IOCTL_PM_BATALRM_READ_ALARM_STATUS", 4, 12, CLS_READ, 0, IN_P,
      "00 d status1 ?|04 d status2 ?|08 e error ?", NULL },

    // 0x801B Coin-cell charger
    { 0x801B0FA0, "IOCTL_PM_COIN_CHG_CONFIGURE", 16, 4, CLS_WRITE, 0,
      IN_P "|04 d enable|08 d voltage ?|0C d resistor ?", OUT_E, NULL },

    // 0x801D Vibrator
    { 0x801D0FA0, "IOCTL_PM_VIB_CONTROL_REGISTER", 8, 4, CLS_WRITE, 0, IN_P "|04 l level", OUT_E,
      "level = drive voltage in 100 mV units: 0 = off, 12-31 = 1.2-3.1 V, anything else fails "
      "(PMIC error 10); no timer: the motor runs until 0 is sent" },

    // 0x801E Clocks
    { 0x801E0FA0, "IOCTL_PM_CLK_SET_MP3_1_GPIO_DIV_REGISTER", 8, 4, CLS_WRITE, 0, IN_P "|04 d divider", OUT_E, NULL },
    { 0x801E0FA4, "IOCTL_PM_CLK_SET_MP3_2_GPIO_DIV_REGISTER", 8, 4, CLS_WRITE, 0, IN_P "|04 d divider", OUT_E, NULL },

    // Descriptors of device types the driver does not handle (names only)
    { 0x80090FA0, "IOCTL_PM_3P_GAUGE_GET_BATT_PERC", 0, 1, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FA4, "IOCTL_PM_3P_GAUGE_GET_TEMP", 0, 2, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FA8, "IOCTL_PM_3P_GAUGE_GET_CAPACITY_PARAMETERS", 0, 10, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FAC, "IOCTL_PM_3P_GAUGE_GET_CURRENT_PARAMETERS", 0, 8, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FB0, "IOCTL_PM_3P_GAUGE_GET_TIME_PARAMETERS", 0, 8, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FB4, "IOCTL_PM_3P_GAUGE_GET_POWER_PARAMETERS", 0, 12, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80090FB8, "IOCTL_PM_3P_GAUGE_GET_CONTROL_DATA", 0, 16, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FA4, "IOCTL_BATT_MNGR_GET_BATTERY_ID", 0, 4, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FA8, "IOCTL_BATT_MNGR_GET_CHARGER_STATUS", 4, 16, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FAC, "IOCTL_BATT_MNGR_GET_BATTERY_INFO", 12, 512, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FB0, "IOCTL_BATT_MNGR_CONTROL_CHARGING", 16, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FB4, "IOCTL_BATT_MNGR_SET_STATUS_NOTIFICATION_CRITERIA", 16, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FB8, "IOCTL_BATT_MNGR_DISABLE_STATUS_NOTIFICATION", 0, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FBC, "IOCTL_BATT_MNGR_SET_OPERATIONAL_MODE", 4, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FC0, "IOCTL_BATT_MNGR_GET_CURRENT_STATE", 0, 4, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FC4, "IOCTL_BATT_MNGR_SET_CHARGE_RATE", 4, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800B0FC8, "IOCTL_BATT_MNGR_NOTIFY_MINICLASS_REMOVAL", 0, 0, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x800C0FA0, "IOCTL_PMIC_BATT_MINI_TEST", 0, 0, CLS_NO_DEVICE, 0, NULL, NULL,
      "same code as IOCTL_PMIC_BATT_MINI_NOTIFY_STATUS" },
    { 0x80100FA0, "IOCTL_PM_ABD_GET_PMIC_RTC_DATA", 4, 16, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80100FA4, "IOCTL_PM_ABD_SET_PMIC_RTC_DATA", 16, 4, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80160FA0, "IOCTL_PM_3P_CHG_CHARGE_BATTERY", 4, 4, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80160FA4, "IOCTL_PM_3P_CHG_STOP_CHARGING", 0, 4, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
    { 0x80160FA8, "IOCTL_PM_3P_CHG_GET_CHARGER_STATUS", 0, 8, CLS_NO_DEVICE, 0, NULL, NULL, NULL },
};

#define IOCTL_COUNT (sizeof(g_Ioctls) / sizeof(g_Ioctls[0]))

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool ContainsNoCase(const char *text, const char *pattern)
{
    size_t n = strlen(pattern);
    for (; *text != '\0'; text++) {
        if (_strnicmp(text, pattern, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool EndsWithNoCase(const char *text, const char *suffix)
{
    size_t t = strlen(text);
    size_t s = strlen(suffix);
    return s <= t && _stricmp(text + t - s, suffix) == 0 && (s == t || text[t - s - 1] == '_');
}

static const IoctlInfo *FindIoctlByCode(DWORD code)
{
    for (size_t i = 0; i < IOCTL_COUNT; i++) {
        if (g_Ioctls[i].Code == code) {
            return &g_Ioctls[i];
        }
    }
    return NULL;
}

// Accepts the full name, the name without "IOCTL_" or "IOCTL_PM_", or any unique part of it.
static const IoctlInfo *FindIoctlByName(const char *name, bool *ambiguous)
{
    *ambiguous = false;
    for (size_t i = 0; i < IOCTL_COUNT; i++) {
        const char *full = g_Ioctls[i].Name;
        if (_stricmp(full, name) == 0 ||
            (_strnicmp(full, "IOCTL_", 6) == 0 && _stricmp(full + 6, name) == 0) ||
            (_strnicmp(full, "IOCTL_PM_", 9) == 0 && _stricmp(full + 9, name) == 0)) {
            return &g_Ioctls[i];
        }
    }

    const IoctlInfo *match = NULL;
    const IoctlInfo *suffixMatch = NULL;
    int matches = 0;
    int suffixMatches = 0;
    for (size_t i = 0; i < IOCTL_COUNT; i++) {
        if (ContainsNoCase(g_Ioctls[i].Name, name)) {
            match = &g_Ioctls[i];
            matches++;
            if (EndsWithNoCase(g_Ioctls[i].Name, name)) {
                suffixMatch = &g_Ioctls[i];
                suffixMatches++;
            }
        }
    }
    if (matches == 1) {
        return match;
    }
    if (suffixMatches == 1) {
        return suffixMatch;
    }
    if (matches > 1) {
        *ambiguous = true;
        printf("Ambiguous IOCTL name '%s', candidates:\n", name);
        for (size_t i = 0; i < IOCTL_COUNT; i++) {
            if (ContainsNoCase(g_Ioctls[i].Name, name)) {
                printf("  0x%08lX %s\n", g_Ioctls[i].Code, g_Ioctls[i].Name);
            }
        }
    }
    return NULL;
}

// Parses a signed or unsigned 64-bit value: decimal, 0x-hex or negative.
static bool ParseInt64(const char *text, __int64 *value)
{
    char *end = NULL;
    if (text[0] == '-') {
        *value = _strtoi64(text, &end, 0);
    } else {
        *value = (__int64)_strtoui64(text, &end, 0);
    }
    return end != text && *end == '\0';
}

static bool ParseDword(const char *text, DWORD *value)
{
    __int64 v;
    if (!ParseInt64(text, &v) || v < -0x80000000LL || v > 0xFFFFFFFFLL) {
        return false;
    }
    *value = (DWORD)v;
    return true;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses "0102ab", "01:02:ab" or "01 02 ab" (separators ':', '-', '.', ' ' are ignored).
static bool ParseHexBytes(const char *text, BYTE *out, DWORD max, DWORD *length)
{
    DWORD n = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    while (*text != '\0') {
        if (*text == ':' || *text == '-' || *text == '.' || *text == ' ') {
            text++;
            continue;
        }
        int hi = HexDigit(text[0]);
        int lo = text[1] != '\0' ? HexDigit(text[1]) : -1;
        if (hi < 0 || lo < 0 || n >= max) {
            return false;
        }
        out[n++] = (BYTE)(hi * 16 + lo);
        text += 2;
    }
    *length = n;
    return true;
}

static DWORD ReadDword(const BYTE *p)
{
    DWORD v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void PrintHexDump(const BYTE *data, DWORD length, const char *indent)
{
    bool skipping = false;
    for (DWORD offset = 0; offset < length; offset += 16) {
        DWORD lineLength = length - offset < 16 ? length - offset : 16;
        // Collapse lines identical to the previous one (long zero-filled buffers)
        if (offset >= 16 && offset + 16 < length && memcmp(data + offset, data + offset - 16, 16) == 0) {
            if (!skipping) {
                printf("%s*\n", indent);
                skipping = true;
            }
            continue;
        }
        skipping = false;
        printf("%s%04lX:", indent, offset);
        for (DWORD i = 0; i < 16; i++) {
            if (i < lineLength) {
                printf(" %02X", data[offset + i]);
            } else {
                printf("   ");
            }
        }
        printf("  ");
        for (DWORD i = 0; i < lineLength; i++) {
            BYTE c = data[offset + i];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Value decoders
// ---------------------------------------------------------------------------

static const char *PmicErrorText(DWORD error)
{
    switch (error) {
    case 0:   return "success";
    case 9:   return "PMIC index out of range";
    case 10:  return "parameter out of range (e.g. vibrator level)";
    case 19:  return "function not available on this PMIC model";
    case 36:  return "invalid VREG resource kind";
    case 89:  return "VREG interface not initialised";
    case 92:  return "no PMIC information";
    case 93:  return "VREG interface not initialised";
    case 95:  return "invalid state or parameter";
    case 105: return "NULL pointer";
    default:  return "?";
    }
}

static const char *PmicModelText(DWORD model)
{
    switch (model) {
    case 0:  return "REV[7:4] = 0xE, unknown";
    case 1:  return "REV2 = 0x06, PM8921 ?";
    case 2:  return "REV2 = 0x0B, unknown";
    case 3:  return "REV2 = 0x09, PM8038 ? (Lumia 520)";
    case 4:  return "unknown model, PMIC skipped";
    default: return "?";
    }
}

static const char *VregKindText(DWORD kind)
{
    switch (kind) {
    case 0:  return "LDO";
    case 1:  return "SMPS";
    case 2:  return "MVS ?";
    case 3:  return "LVS";
    case 4:  return "NCP ?";
    case 5:  return "CXO buffers";
    case 6:  return "CXO clock";
    case 7:  return "VDDCX corner ?";
    case 8:  return "discrete ?";
    default: return "invalid";
    }
}

#define VREG_RESOURCE_COUNT 55

// Resource ids, in the order of the driver's PM_VREG_RESOURCE_ID_* string table.
static void VregResourceName(DWORD id, char *buffer, size_t size)
{
    if (id <= 7) {
        _snprintf(buffer, size, "SMPS%lu", id + 1);
    } else if (id <= 36) {
        _snprintf(buffer, size, "LDO%lu", id - 7);
    } else if (id <= 43) {
        _snprintf(buffer, size, "LVS%lu", id - 36);
    } else if (id <= 45) {
        _snprintf(buffer, size, "MVS%lu", id - 43);
    } else if (id == 46) {
        _snprintf(buffer, size, "NCP");
    } else if (id == 47) {
        _snprintf(buffer, size, "CXO_BUFFERS");
    } else if (id == 48) {
        _snprintf(buffer, size, "CXO_CLOCK");
    } else if (id == 49) {
        _snprintf(buffer, size, "VDDCX_CORNER");
    } else if (id <= 54) {
        _snprintf(buffer, size, "DV%lu", id - 49);
    } else {
        _snprintf(buffer, size, "invalid");
    }
    buffer[size - 1] = '\0';
}

static bool ParseVregResource(const char *text, DWORD *id)
{
    char name[32];
    for (DWORD i = 0; i < VREG_RESOURCE_COUNT; i++) {
        VregResourceName(i, name, sizeof(name));
        if (_stricmp(name, text) == 0) {
            *id = i;
            return true;
        }
    }
    return ParseDword(text, id);
}

// Prints seconds as a duration. The PMIC RTC is not counted from a calendar epoch
// (it reads about a week on a Lumia 520), so no date is derived from it.
static void PrintSeconds(DWORD seconds)
{
    DWORD days = seconds / 86400;
    DWORD rest = seconds % 86400;
    printf("%lu s (%lud %02lu:%02lu:%02lu)", seconds, days, rest / 3600, (rest / 60) % 60, rest % 60);
}

// XOADC AMUX channels, from the PM8921/PM8038 channel table (names of the Linux pm8xxx-adc driver).
static const char *AdcChannelText(DWORD channel)
{
    static const char *const names[] = {
        "VCOIN", "VBAT", "DCIN", "ICHG", "VPH_PWR", "IBAT", "MPP_1", "MPP_2", "BATT_THERM",
        "BATT_ID", "USBIN", "DIE_TEMP", "0.625 V reference", "1.25 V reference", "CHG_TEMP",
        "MUXOFF ?",
    };
    return channel < sizeof(names) / sizeof(names[0]) ? names[channel] : "invalid";
}

#define BMS_SELECTOR_COUNT 7

// Selectors of IOCTL_PM_GAUGE_READ_BMS_OUTPUT_REG_BMS (hardware names of the Linux pm8921-bms driver).
static const char *BmsSelectorText(DWORD selector)
{
    static const char *const names[BMS_SELECTOR_COUNT] = {
        "OCV_FOR_RBATT", "VSENSE_FOR_RBATT", "VBATT_FOR_RBATT", "coulomb counter",
        "LAST_GOOD_OCV", "VSENSE_AVG", "VBATT_AVG",
    };
    return selector < BMS_SELECTOR_COUNT ? names[selector] : "none, rereads the previous selection";
}

// ---------------------------------------------------------------------------
// Field layouts
// ---------------------------------------------------------------------------

struct Field {
    DWORD Offset;
    char Type;
    DWORD Size;
    char Name[64];
};

static DWORD TypeSize(char type)
{
    switch (type) {
    case 'b': return 1;
    case 'w': return 2;
    case 'q':
    case 'Q': return 8;
    default:  return 4;
    }
}

static int ParseSpec(const char *spec, Field *fields, int max)
{
    int count = 0;
    const char *p = spec;
    while (p != NULL && *p != '\0' && count < max) {
        Field *f = &fields[count];
        char *end;
        f->Offset = strtoul(p, &end, 16);
        p = end;
        while (*p == ' ') p++;
        f->Type = *p++;
        if (f->Type == 'x') {
            f->Size = strtoul(p, &end, 10);
            p = end;
        } else {
            f->Size = TypeSize(f->Type);
        }
        while (*p == ' ') p++;
        const char *sep = strchr(p, '|');
        size_t len = sep != NULL ? (size_t)(sep - p) : strlen(p);
        if (len >= sizeof(f->Name)) {
            len = sizeof(f->Name) - 1;
        }
        memcpy(f->Name, p, len);
        f->Name[len] = '\0';
        p = sep != NULL ? sep + 1 : NULL;
        count++;
    }
    return count;
}

static void PrintFieldValue(const Field *f, const BYTE *data)
{
    DWORD v = 0;
    if (f->Size == 4) {
        v = ReadDword(data);
    }
    switch (f->Type) {
    case 'b':
        printf("%u (0x%02X)", data[0], data[0]);
        break;
    case 'w': {
        WORD w;
        memcpy(&w, data, sizeof(w));
        printf("%u (0x%04X)", w, w);
        break;
    }
    case 'q':
    case 'Q': {
        unsigned __int64 q;
        memcpy(&q, data, sizeof(q));
        printf("0x%016I64X", q);
        if (f->Type == 'Q') {
            int bits = 0;
            for (int bit = 0; bit < 64; bit++) {
                bits += (q >> bit) & 1 ? 1 : 0;
            }
            if (bits == 0) {
                printf(" (no bit set)");
            } else if (bits > 16) {
                printf(" (%d bits set)", bits);
            } else {
                printf(" (bits");
                for (int bit = 0; bit < 64; bit++) {
                    if (q & (1ULL << bit)) {
                        printf(" %d", bit);
                    }
                }
                printf(")");
            }
        }
        break;
    }
    case 'i':
        printf("%ld (0x%08lX)", (LONG)v, v);
        break;
    case 'h':
    case 'p':
        printf("0x%08lX", v);
        break;
    case 'f':
        printf("0x%08lX", v);
        if (v & 1) {
            printf(" (Thumb-2 entry point, code at 0x%08lX)", v & ~1UL);
        }
        break;
    case 'e':
        printf("%lu (%s)", v, PmicErrorText(v));
        break;
    case 'm':
        printf("%lu (%s)", v, PmicModelText(v));
        break;
    case 'k':
        printf("%lu (%s)", v, VregKindText(v));
        break;
    case 'r': {
        char name[32];
        VregResourceName(v, name, sizeof(name));
        printf("%lu (%s)", v, name);
        break;
    }
    case 't':
        PrintSeconds(v);
        break;
    case 'a':
        if (v == 0xFFFFFFFF) {
            printf("0xFFFFFFFF (no alarm set)");
        } else {
            PrintSeconds(v);
        }
        break;
    case 'v':
        printf("%lu uV (%lu.%06lu V)", v, v / 1000000, v % 1000000);
        break;
    case 'c':
        printf("%lu (%s)", v, AdcChannelText(v));
        break;
    case 'l':
        if (v == 0) {
            printf("0 (motor off)");
        } else if (v >= 12 && v <= 31) {
            printf("%lu (%lu.%lu V)", v, v / 10, v % 10);
        } else {
            printf("%lu (out of range: only 0 or 12-31 are accepted)", v);
        }
        break;
    case 's':
        printf("%lu (%s)", v, BmsSelectorText(v));
        break;
    case 'x':
        if (f->Size <= 16) {
            for (DWORD i = 0; i < f->Size; i++) {
                printf("%02X%s", data[i], i + 1 < f->Size ? " " : "");
            }
        } else {
            printf("%lu bytes\n", f->Size);
            PrintHexDump(data, f->Size, "      ");
            return;
        }
        break;
    default: // 'd'
        printf("%lu (0x%08lX)", v, v);
        if ((LONG)v < 0) {
            printf(", signed %ld", (LONG)v);
        }
        break;
    }
    printf("\n");
}

// Prints the fields of a layout that lie inside the first `length` bytes of `data`.
static void PrintFields(const char *spec, const BYTE *data, DWORD length)
{
    Field fields[MAX_FIELDS];
    int count = ParseSpec(spec, fields, MAX_FIELDS);
    for (int i = 0; i < count; i++) {
        const Field *f = &fields[i];
        printf("    %-28s = ", f->Name);
        if (f->Offset + f->Size > length) {
            printf("(not returned)\n");
            continue;
        }
        PrintFieldValue(f, data + f->Offset);
    }
}

// Writes one command-line value into the field it belongs to.
static bool WriteFieldValue(const Field *f, const char *text, BYTE *buffer)
{
    BYTE *p = buffer + f->Offset;
    __int64 v;
    switch (f->Type) {
    case 'x': {
        DWORD length;
        if (!ParseHexBytes(text, p, f->Size, &length)) {
            printf("Invalid hex bytes for '%s' (max %lu bytes): %s\n", f->Name, f->Size, text);
            return false;
        }
        return true;
    }
    case 'r': {
        DWORD id;
        if (!ParseVregResource(text, &id)) {
            printf("Invalid resource for '%s': %s\n", f->Name, text);
            return false;
        }
        memcpy(p, &id, sizeof(id));
        return true;
    }
    case 'q':
    case 'Q':
        if (!ParseInt64(text, &v)) {
            printf("Invalid value for '%s': %s\n", f->Name, text);
            return false;
        }
        memcpy(p, &v, 8);
        return true;
    case 'b':
        if (!ParseInt64(text, &v) || v < -128 || v > 0xFF) {
            printf("Invalid byte value for '%s': %s\n", f->Name, text);
            return false;
        }
        *p = (BYTE)v;
        return true;
    case 'w':
        if (!ParseInt64(text, &v) || v < -32768 || v > 0xFFFF) {
            printf("Invalid 16-bit value for '%s': %s\n", f->Name, text);
            return false;
        }
        {
            WORD w = (WORD)v;
            memcpy(p, &w, sizeof(w));
        }
        return true;
    default: {
        DWORD d;
        if (!ParseDword(text, &d)) {
            printf("Invalid 32-bit value for '%s': %s\n", f->Name, text);
            return false;
        }
        memcpy(p, &d, sizeof(d));
        return true;
    }
    }
}

// Builds "<pmic> <gpio> ..." from an input layout.
static void PrintArgsUsage(const IoctlInfo *info)
{
    if (info->Special == SPECIAL_GPIO_CONFIG) {
        printf("<pmic> <gpio> [gpio ...]");
        return;
    }
    if (info->InputSpec == NULL) {
        printf(info->InputSize == 0 ? "(none)" : "(unknown layout: use -r or -x)");
        return;
    }
    Field fields[MAX_FIELDS];
    int count = ParseSpec(info->InputSpec, fields, MAX_FIELDS);
    for (int i = 0; i < count; i++) {
        if (fields[i].Type == 'x') {
            printf("%s<%s: %lu hex bytes>", i ? " " : "", fields[i].Name, fields[i].Size);
        } else {
            printf("%s<%s>", i ? " " : "", fields[i].Name);
        }
    }
}

static const char *ClassText(char cls)
{
    switch (cls) {
    case CLS_READ:        return "read-only";
    case CLS_READ_MANUAL: return "read-only, not sent by auto";
    case CLS_WRITE:       return "changes the state of the phone";
    case CLS_DANGEROUS:   return "may crash the phone";
    case CLS_UNHANDLED:   return "valid, but not handled by the driver (STATUS_NOT_SUPPORTED)";
    case CLS_STUB:        return "stub, always fails";
    case CLS_NO_DEVICE:   return "device type not handled (STATUS_INVALID_DEVICE_REQUEST)";
    default:              return "?";
    }
}

// ---------------------------------------------------------------------------
// Status names
// ---------------------------------------------------------------------------

static const char *NtStatusText(LONG status)
{
    switch ((DWORD)status) {
    case 0x00000000: return "STATUS_SUCCESS";
    case 0x80000005: return "STATUS_BUFFER_OVERFLOW";
    case 0xC0000001: return "STATUS_UNSUCCESSFUL (the PMIC library reported an error)";
    case 0xC0000005: return "STATUS_ACCESS_VIOLATION";
    case 0xC000000D: return "STATUS_INVALID_PARAMETER";
    case 0xC0000010: return "STATUS_INVALID_DEVICE_REQUEST (device type not handled)";
    case 0xC0000022: return "STATUS_ACCESS_DENIED";
    case 0xC0000023: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC00000BB: return "STATUS_NOT_SUPPORTED (valid IOCTL without a handler)";
    case 0xC00000EF: return "STATUS_INVALID_PARAMETER_1 (no descriptor for this code)";
    case 0xC00000F0: return "STATUS_INVALID_PARAMETER_2 (input buffer missing)";
    case 0xC00000F1: return "STATUS_INVALID_PARAMETER_3 (input length differs from the descriptor)";
    case 0xC00000F2: return "STATUS_INVALID_PARAMETER_4 (output buffer missing or wrong length)";
    case 0xC00000F3: return "STATUS_INVALID_PARAMETER_5 (output length differs from the descriptor)";
    default:         return "";
    }
}

static const char *Win32ErrorText(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS:              return "ERROR_SUCCESS";
    case ERROR_FILE_NOT_FOUND:       return "ERROR_FILE_NOT_FOUND (driver not loaded or wrong device path)";
    case ERROR_ACCESS_DENIED:        return "ERROR_ACCESS_DENIED (run with higher privileges)";
    case ERROR_INVALID_FUNCTION:     return "ERROR_INVALID_FUNCTION (STATUS_INVALID_DEVICE_REQUEST)";
    case ERROR_INVALID_PARAMETER:    return "ERROR_INVALID_PARAMETER (usually a buffer size that does not match)";
    case ERROR_GEN_FAILURE:          return "ERROR_GEN_FAILURE (STATUS_UNSUCCESSFUL)";
    case ERROR_NOT_SUPPORTED:        return "ERROR_NOT_SUPPORTED";
    case ERROR_INSUFFICIENT_BUFFER:  return "ERROR_INSUFFICIENT_BUFFER";
    case ERROR_MORE_DATA:            return "ERROR_MORE_DATA (STATUS_BUFFER_OVERFLOW)";
    default:                         return "";
    }
}

// ---------------------------------------------------------------------------
// Sending an IOCTL
// ---------------------------------------------------------------------------

static HANDLE g_Device = INVALID_HANDLE_VALUE;
static NtDeviceIoControlFileFn g_NtDeviceIoControlFile = NULL;
static BYTE g_InputBuffer[MAX_BUFFER_SIZE];
static BYTE g_OutputBuffer[MAX_BUFFER_SIZE];
static int g_Sent = 0;
static int g_Succeeded = 0;

struct CallResult {
    bool Ok;
    bool HaveNtStatus;
    LONG NtStatus;
    DWORD Win32Error;
    DWORD Returned;
};

static bool OpenDevice(const wchar_t *path)
{
    g_Device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (g_Device == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        // The IOCTLs are FILE_ANY_ACCESS, so a handle without read/write access is enough.
        g_Device = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (g_Device == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        printf("CreateFile(%ls) failed: %lu %s\n", path, error, Win32ErrorText(error));
        return false;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL) {
        g_NtDeviceIoControlFile = (NtDeviceIoControlFileFn)GetProcAddress(ntdll, "NtDeviceIoControlFile");
    }
    return true;
}

static CallResult SendIoctl(DWORD code, BYTE *input, DWORD inputLength, BYTE *output, DWORD outputLength)
{
    CallResult r;
    memset(&r, 0, sizeof(r));
    memset(output, 0, outputLength);

    if (g_NtDeviceIoControlFile != NULL) {
        IoStatusBlock iosb;
        memset(&iosb, 0, sizeof(iosb));
        LONG status = g_NtDeviceIoControlFile(g_Device, NULL, NULL, NULL, &iosb, code,
                                              inputLength > 0 ? input : NULL, inputLength,
                                              outputLength > 0 ? output : NULL, outputLength);
        if (status == STATUS_PENDING_VALUE) {
            WaitForSingleObjectEx(g_Device, INFINITE, FALSE);
            status = iosb.Status;
        }
        r.HaveNtStatus = true;
        r.NtStatus = status;
        r.Ok = status >= 0;
        // For METHOD_BUFFERED, nothing is copied back on an error status.
        r.Returned = (DWORD)status >= 0xC0000000 ? 0 : (DWORD)iosb.Information;
    } else {
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(g_Device, code, inputLength > 0 ? input : NULL, inputLength,
                                  outputLength > 0 ? output : NULL, outputLength, &returned, NULL);
        r.Ok = ok != FALSE;
        r.Win32Error = ok ? ERROR_SUCCESS : GetLastError();
        r.Returned = returned;
    }
    if (r.Returned > outputLength) {
        r.Returned = outputLength;
    }
    g_Sent++;
    if (r.Ok) {
        g_Succeeded++;
    }
    return r;
}

static void PrintGpioConfig(const BYTE *data, DWORD length)
{
    for (DWORD offset = 0; offset + 44 <= length; offset += 44) {
        const BYTE *e = data + offset;
        DWORD error = ReadDword(e + 4);
        printf("    GPIO %-3lu error %lu (%s)\n", ReadDword(e), error, PmicErrorText(error));
        printf("      config :");
        for (DWORD i = 0; i < 5; i++) {
            printf(" 0x%08lX", ReadDword(e + 8 + i * 4));
        }
        printf("\n      extra  :");
        for (DWORD i = 0; i < 16; i++) {
            printf(" %02X", e[0x1C + i]);
        }
        printf("\n");
    }
}

static void PrintLabel(const char *name)
{
    printf("    %-28s = ", name);
}

// The slots past the number of PMICs are zero-filled, so model 0 there means nothing.
static void PrintPmicsInfo(const BYTE *data, DWORD length)
{
    if (length < 4) {
        return;
    }
    DWORD count = ReadDword(data);
    PrintLabel("number of PMICs");
    printf("%lu\n", count);
    for (DWORD slot = 0; slot < 2; slot++) {
        DWORD offset = 4 + slot * 12;
        char name[32];
        _snprintf(name, sizeof(name), "PMIC %lu", slot);
        name[sizeof(name) - 1] = '\0';
        if (offset + 12 > length) {
            PrintLabel(name);
            printf("(not returned)\n");
        } else if (slot >= count) {
            PrintLabel(name);
            printf("unused (only %lu PMIC%s detected)\n", count, count == 1 ? "" : "s");
        } else {
            DWORD model = ReadDword(data + offset + 4);
            PrintLabel(name);
            printf("index %lu, model %lu (%s), revision %lu\n", ReadDword(data + offset), model,
                   PmicModelText(model), ReadDword(data + offset + 8));
        }
    }
    PrintFields("1C e error", data, length);
}

// A resource the PMIC does not have comes back as an all-zero structure (resource id 0).
static void PrintVregSettings(const IoctlInfo *info, const BYTE *input, DWORD inputLength,
                              const BYTE *data, DWORD length)
{
    if (inputLength >= 4 && length >= 8) {
        DWORD requested = ReadDword(input);
        DWORD returned = ReadDword(data + 4);
        if (requested != returned) {
            char name[32];
            VregResourceName(requested, name, sizeof(name));
            printf("    %s not present: the driver returned resource id %lu instead of %lu\n", name,
                   returned, requested);
            return;
        }
    }
    PrintFields(info->OutputSpec, data, length);
}

// Selector 3 is the 32-bit coulomb counter, 5 the signed average Vsense, the others 16-bit codes.
static void PrintBmsOutput(const BYTE *input, DWORD inputLength, const BYTE *data, DWORD length)
{
    DWORD selector = inputLength >= 8 ? ReadDword(input + 4) : 0;
    if (length >= 4) {
        DWORD v = ReadDword(data);
        PrintLabel(selector < BMS_SELECTOR_COUNT ? BmsSelectorText(selector) : "value");
        if (selector == 3) {
            printf("%ld (0x%08lX, signed 32-bit)\n", (LONG)v, v);
        } else if (selector == 5) {
            printf("%d (0x%04lX, signed 16-bit)\n", (int)(short)(v & 0xFFFF), v & 0xFFFF);
        } else {
            printf("%lu (0x%04lX)\n", v & 0xFFFF, v & 0xFFFF);
        }
        if (selector != 3 && (v >> 16) != 0) {
            printf("    Note: upper 16 bits not zero: 0x%08lX\n", v);
        }
        if (selector >= BMS_SELECTOR_COUNT) {
            printf("    Warning: selector %lu does not change the hardware selection, this is the\n"
                   "             previously selected register read again\n", selector);
        }
    }
    PrintFields("04 e error", data, length);
}

// Sends one IOCTL and prints everything about it.
static CallResult Execute(DWORD code, BYTE *input, DWORD inputLength, DWORD outputLength)
{
    const IoctlInfo *info = FindIoctlByCode(code);

    printf("------------------------------------------------------------------------------\n");
    printf("IOCTL  : 0x%08lX %s\n", code, info != NULL ? info->Name : "(unknown)");
    printf("         device type 0x%04lX, function %lu, method %lu, access %lu",
           (code >> 16) & 0xFFFF, (code >> 2) & 0xFFF, code & 3, (code >> 14) & 3);
    if (info != NULL) {
        printf(", %s", ClassText(info->Class));
    }
    printf("\n");
    if (info != NULL && info->Note != NULL) {
        printf("Note   : %s\n", info->Note);
    }
    if (info != NULL && (inputLength != info->InputSize || outputLength != info->OutputSize)) {
        printf("Note   : the driver requires exactly in=%u / out=%u bytes, this call will likely fail\n",
               info->InputSize, info->OutputSize);
    }

    printf("Input  : %lu bytes\n", inputLength);
    if (inputLength > 0) {
        PrintHexDump(input, inputLength, "  ");
        if (info != NULL && info->InputSpec != NULL) {
            PrintFields(info->InputSpec, input, inputLength);
        }
    }
    printf("Output : %lu bytes buffer\n", outputLength);

    CallResult r = SendIoctl(code, input, inputLength, g_OutputBuffer, outputLength);

    if (r.HaveNtStatus) {
        printf("Status : 0x%08lX %s%s, %lu bytes returned\n", (DWORD)r.NtStatus, NtStatusText(r.NtStatus),
               r.Ok ? "" : " - FAILED", r.Returned);
    } else if (r.Ok) {
        printf("Status : SUCCESS, %lu bytes returned\n", r.Returned);
    } else {
        printf("Status : FAILED, error %lu %s, %lu bytes returned\n", r.Win32Error,
               Win32ErrorText(r.Win32Error), r.Returned);
    }

    if (r.Returned > 0) {
        PrintHexDump(g_OutputBuffer, r.Returned, "  ");
        if (info != NULL && info->Special == SPECIAL_GPIO_CONFIG) {
            PrintGpioConfig(g_OutputBuffer, r.Returned);
        } else if (info != NULL && info->Special == SPECIAL_PMICS_INFO) {
            PrintPmicsInfo(g_OutputBuffer, r.Returned);
        } else if (info != NULL && info->Special == SPECIAL_VREG_SETTINGS) {
            PrintVregSettings(info, input, inputLength, g_OutputBuffer, r.Returned);
        } else if (info != NULL && info->Special == SPECIAL_BMS_OUTPUT) {
            PrintBmsOutput(input, inputLength, g_OutputBuffer, r.Returned);
        } else if (info != NULL && info->OutputSpec != NULL) {
            PrintFields(info->OutputSpec, g_OutputBuffer, r.Returned);
        } else {
            for (DWORD offset = 0; offset + 4 <= r.Returned; offset += 4) {
                DWORD v = ReadDword(g_OutputBuffer + offset);
                printf("    [%02lX] 0x%08lX  %10lu  %11ld\n", offset, v, v, (LONG)v);
            }
        }
    }
    return r;
}

// Sends an IOCTL whose input is a list of dwords, zero padded to the required size.
static CallResult ExecuteDwords(DWORD code, const DWORD *values, DWORD count)
{
    const IoctlInfo *info = FindIoctlByCode(code);
    DWORD inputLength = info != NULL ? info->InputSize : count * 4;
    DWORD outputLength = info != NULL ? info->OutputSize : 4;
    memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
    memcpy(g_InputBuffer, values, count * 4 <= inputLength ? count * 4 : inputLength);
    return Execute(code, g_InputBuffer, inputLength, outputLength);
}

// ---------------------------------------------------------------------------
// Auto mode: every read-only IOCTL, one after the other
// ---------------------------------------------------------------------------

#define AUTO_MPP_COUNT        12  // PM8921 has 12 MPPs, PM8038 fewer: the others fail
#define AUTO_IRQ_BLOCKS       4
#define AUTO_PRESCALAR_COUNT  16
#define AUTO_BMS_REGISTERS    8

// IOCTLs whose input is only the PMIC index
static const DWORD g_AutoPerPmic[] = {
    0x80020FA0, // IOCTL_PM_ADC_READ_DATA
    0x80020FB0, // IOCTL_PM_ADC_GET_CONVERSION_STATUS
    0x80020FD8, // IOCTL_PM_ADC_BTM_READ_DATA
    0x80020FE4, // IOCTL_PM_ADC_BTM_GET_CONVERSION_STATUS
    0x80020FF8, // IOCTL_PM_READ_SEQUENCER_TIMEOUT_FLAG
    0x80020FFC, // IOCTL_PM_READ_SEQUENCER_FIFO_FLAG
    0x80021000, // IOCTL_PM_CCADC_READ_DATA
    0x80021024, // IOCTL_PM_CCADC_GET_CONVERSION_STATUS
    0x80040FB0, // IOCTL_PM_KEYPAD_GET_POWER_KEY
    0x80080FAC, // IOCTL_PM_CHG_GET_STATE
    0x80080FD0, // IOCTL_PM_CHG_GET_BATTSAFE
    0x80080FD4, // IOCTL_PM_CHG_GET_REGULATION_LOOP
    0x80080FE4, // IOCTL_PM_CHG_READ_CHARGING_PARAMTERS
    0x800A0FA8, // IOCTL_PM_RTC_GET_TIME
    0x800A0FAC, // IOCTL_PM_RTC_GET_TIME_ADJUST
    0x800A0FC0, // IOCTL_PM_RTC_GET_ALARM_STATUS
    0x801A0FA8, // IOCTL_PM_BATALRM_READ_ALARM_STATUS
};

static void PrintSection(const char *title, int pmic)
{
    printf("\n==============================================================================\n");
    if (pmic >= 0) {
        printf("== %s - PMIC %d\n", title, pmic);
    } else {
        printf("== %s\n", title);
    }
    printf("==============================================================================\n");
}

// Last line of defence: auto mode must only send read-only IOCTLs.
static bool IsAutoSafe(DWORD code)
{
    const IoctlInfo *info = FindIoctlByCode(code);
    if (info == NULL || info->Class != CLS_READ) {
        printf("Internal error: 0x%08lX is not a read-only IOCTL, skipped\n", code);
        return false;
    }
    return true;
}

static void RunAuto(int onlyPmic)
{
    DWORD pmicCount = 1;

    PrintSection("Hardware", -1);
    ExecuteDwords(0x80150FA8, NULL, 0); // IOCTL_PM_HARDWARE_GET_NUMBER_OF_PMICS
    CallResult r = ExecuteDwords(0x80150FAC, NULL, 0); // IOCTL_PM_HARDWARE_GET_PMICS_INFO
    if (r.Ok && r.Returned >= 4) {
        pmicCount = ReadDword(g_OutputBuffer);
        if (pmicCount == 0 || pmicCount > 2) {
            printf("Unexpected number of PMICs (%lu), using 1\n", pmicCount);
            pmicCount = 1;
        }
    }
    ExecuteDwords(0x800F0FA4, NULL, 0); // IOCTL_GET_LED_INTERFACE

    // -p queries that PMIC even when it was not detected
    DWORD firstPmic = onlyPmic >= 0 ? (DWORD)onlyPmic : 0;
    DWORD endPmic = onlyPmic >= 0 ? (DWORD)onlyPmic + 1 : pmicCount;
    for (DWORD pmic = firstPmic; pmic < endPmic; pmic++) {
        PrintSection("Simple reads", (int)pmic);
        for (size_t i = 0; i < sizeof(g_AutoPerPmic) / sizeof(g_AutoPerPmic[0]); i++) {
            if (IsAutoSafe(g_AutoPerPmic[i])) {
                ExecuteDwords(g_AutoPerPmic[i], &pmic, 1);
            }
        }
        DWORD alarm[2] = { pmic, 0 };
        if (IsAutoSafe(0x800A0FBC)) {
            ExecuteDwords(0x800A0FBC, alarm, 2); // IOCTL_PM_RTC_GET_ALARM_TIME, alarm 0
        }

        PrintSection("ADC prescalers", (int)pmic);
        for (DWORD channel = 0; channel < AUTO_PRESCALAR_COUNT; channel++) {
            DWORD in[2] = { pmic, channel };
            if (IsAutoSafe(0x80020FC0)) {
                ExecuteDwords(0x80020FC0, in, 2); // IOCTL_PM_ADC_GET_PRESCALAR
            }
        }

        PrintSection("BMS output registers", (int)pmic);
        for (DWORD reg = 0; reg < AUTO_BMS_REGISTERS; reg++) {
            DWORD in[2] = { pmic, reg };
            if (IsAutoSafe(0x80190FA4)) {
                ExecuteDwords(0x80190FA4, in, 2); // IOCTL_PM_GAUGE_READ_BMS_OUTPUT_REG_BMS
            }
        }

        PrintSection("GPIO", (int)pmic);
        DWORD gpioCount = 0;
        if (IsAutoSafe(0x80030FC0)) {
            r = ExecuteDwords(0x80030FC0, &pmic, 1); // IOCTL_PM_GPIO_GET_NUMBER_OF_GPIOS
            if (r.Ok && r.Returned >= 12 && ReadDword(g_OutputBuffer + 8) == 0) {
                gpioCount = ReadDword(g_OutputBuffer + 4);
                if (gpioCount > MAX_GPIO_LIST) {
                    gpioCount = MAX_GPIO_LIST;
                }
            }
        }
        for (DWORD gpio = 0; gpio < gpioCount; gpio++) {
            for (DWORD source = 1; source != (DWORD)-1; source--) {
                DWORD in[3] = { pmic, gpio, source };
                if (IsAutoSafe(0x80030FB4)) {
                    ExecuteDwords(0x80030FB4, in, 3); // IOCTL_PM_GPIO_GET_GPIO_STATE
                }
            }
        }

        PrintSection("MPP", (int)pmic);
        for (DWORD mpp = 0; mpp < AUTO_MPP_COUNT; mpp++) {
            for (DWORD source = 1; source != (DWORD)-1; source--) {
                DWORD in[3] = { pmic, mpp, source };
                if (IsAutoSafe(0x80050FBC)) {
                    ExecuteDwords(0x80050FBC, in, 3); // IOCTL_PM_MPP_GET_INPUT_STATE
                }
            }
        }

        PrintSection("Interrupts", (int)pmic);
        for (DWORD block = 0; block < AUTO_IRQ_BLOCKS; block++) {
            for (DWORD type = 0; type <= 1; type++) {
                // pmic, block, 64-bit mask, type
                BYTE in[24];
                unsigned __int64 mask = 0xFFFFFFFFFFFFFFFFULL;
                memset(in, 0, sizeof(in));
                memcpy(in, &pmic, 4);
                memcpy(in + 4, &block, 4);
                memcpy(in + 8, &mask, 8);
                memcpy(in + 16, &type, 4);
                if (IsAutoSafe(0x80070FA8)) {
                    memcpy(g_InputBuffer, in, sizeof(in));
                    Execute(0x80070FA8, g_InputBuffer, sizeof(in), 16); // IOCTL_PM_IRQ_GET_INTERRUPTS_STATUS
                }
            }
        }
    }

    PrintSection("Voltage regulators (aggregated votes)", -1);
    for (DWORD id = 0; id < VREG_RESOURCE_COUNT; id++) {
        if (IsAutoSafe(0x80060FE4)) {
            ExecuteDwords(0x80060FE4, &id, 1); // IOCTL_PM_VREG_GET_POWER_SETTINGS
        }
    }

    printf("\n==============================================================================\n");
    printf("%d IOCTLs sent: %d succeeded, %d failed\n", g_Sent, g_Succeeded, g_Sent - g_Succeeded);
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

static void PrintUsage()
{
    printf("Usage: wp81PmicTool [options] <ioctl> [value ...]   send one IOCTL\n");
    printf("       wp81PmicTool [-d path] [-p pmic] auto         send every read-only IOCTL\n");
    printf("       wp81PmicTool list [filter]                    print the known IOCTLs\n\n");
    printf("  <ioctl>  code (0x800A0FA8) or name: IOCTL_PM_RTC_GET_TIME, RTC_GET_TIME or any\n");
    printf("           unique part of it (e.g. PMICS_INFO)\n");
    printf("  value    one value per input field, in the order shown by 'list' (the first one\n");
    printf("           is usually the PMIC index); decimal, 0x-hex or negative. Missing values\n");
    printf("           are 0. VREG resources can be named (LDO5), raw-byte fields take hex.\n");
    printf("Options:\n");
    printf("  -f       force: needed to send an IOCTL that changes the state of the phone\n");
    printf("  -r       raw values: each value is a dword appended to the buffer; use the\n");
    printf("           prefixes b: w: q: x: for a byte, 16-bit, 64-bit value or hex bytes\n");
    printf("  -x hex   raw input buffer, e.g. -x 00000000010000000000\n");
    printf("  -i n     input buffer size in bytes (default: the size the driver requires)\n");
    printf("  -o n     output buffer size in bytes (default: the size the driver requires)\n");
    printf("  -p n     auto mode: only query PMIC n\n");
    printf("  -d path  device path (default \\\\.\\QCOMPMIC)\n\n");
    printf("Examples:\n");
    printf("  wp81PmicTool auto\n");
    printf("  wp81PmicTool PMICS_INFO\n");
    printf("  wp81PmicTool RTC_GET_TIME 0\n");
    printf("  wp81PmicTool GET_POWER_SETTINGS LDO5\n");
    printf("  wp81PmicTool IRQ_GET_INTERRUPTS_STATUS 0 3 0xFFFFFFFFFFFFFFFF 0\n");
    printf("  wp81PmicTool -f WLED_ENABLE 0 1\n");
}

static void PrintIoctlList(const char *filter)
{
    printf("Classes: R read-only (auto)   r read-only (manual only)   W changes state (-f)\n");
    printf("         D may crash (-f)   U not handled   S stub   X device type not handled\n\n");
    printf("Code       C  In/Out   Name\n");
    for (size_t i = 0; i < IOCTL_COUNT; i++) {
        const IoctlInfo *info = &g_Ioctls[i];
        char code[16];
        _snprintf(code, sizeof(code), "0x%08lX", info->Code);
        code[sizeof(code) - 1] = '\0';
        if (filter != NULL && !ContainsNoCase(info->Name, filter) && !ContainsNoCase(code, filter)) {
            continue;
        }
        printf("%s %c  %3u/%-4u %s\n", code, info->Class, info->InputSize, info->OutputSize, info->Name);
        if (info->Class != CLS_NO_DEVICE && info->Class != CLS_UNHANDLED) {
            printf("                    args: ");
            PrintArgsUsage(info);
            printf("\n");
            if (filter != NULL && info->OutputSpec != NULL) {
                Field fields[MAX_FIELDS];
                int count = ParseSpec(info->OutputSpec, fields, MAX_FIELDS);
                printf("                    out :");
                for (int f = 0; f < count; f++) {
                    printf("%s %02lX %s", f ? "," : "", fields[f].Offset, fields[f].Name);
                }
                printf("\n");
            }
        }
        if (info->Note != NULL) {
            printf("                    note: %s\n", info->Note);
        }
    }
}

// Raw value: "12", "b:1", "w:0x100", "q:0xFFFFFFFFFFFFFFFF", "x:0102".
static bool AppendRawValue(const char *text, BYTE *buffer, DWORD *length)
{
    __int64 v;
    DWORD size = 4;
    if (text[0] != '\0' && text[1] == ':') {
        switch (text[0]) {
        case 'b': case 'B': size = 1; break;
        case 'w': case 'W': size = 2; break;
        case 'd': case 'D': size = 4; break;
        case 'q': case 'Q': size = 8; break;
        case 'x': case 'X': {
            DWORD n;
            if (!ParseHexBytes(text + 2, buffer + *length, MAX_BUFFER_SIZE - *length, &n)) {
                printf("Invalid hex bytes: %s\n", text);
                return false;
            }
            *length += n;
            return true;
        }
        default:
            printf("Unknown prefix in: %s\n", text);
            return false;
        }
        text += 2;
    }
    if (!ParseInt64(text, &v) || *length + size > MAX_BUFFER_SIZE) {
        printf("Invalid value: %s\n", text);
        return false;
    }
    memcpy(buffer + *length, &v, size);
    *length += size;
    return true;
}

int main(int argc, char **argv)
{
    const wchar_t *devicePath = DEFAULT_DEVICE_PATH;
    wchar_t devicePathBuffer[MAX_PATH];
    long outputSize = -1;
    long inputSize = -1;
    int onlyPmic = -1;
    bool force = false;
    bool raw = false;
    const char *rawHex = NULL;
    int argIndex = 1;

    // Options (a leading '-' followed by a digit is a negative value, not an option)
    while (argIndex < argc && argv[argIndex][0] == '-' && argv[argIndex][1] != '\0' &&
           !(argv[argIndex][1] >= '0' && argv[argIndex][1] <= '9')) {
        const char *opt = argv[argIndex];
        if (_stricmp(opt, "-h") == 0 || _stricmp(opt, "--help") == 0 || _stricmp(opt, "-?") == 0) {
            PrintUsage();
            return 0;
        }
        if (_stricmp(opt, "-f") == 0) {
            force = true;
            argIndex++;
            continue;
        }
        if (_stricmp(opt, "-r") == 0) {
            raw = true;
            argIndex++;
            continue;
        }
        if (argIndex + 1 >= argc) {
            printf("Missing value for option %s\n", opt);
            return 1;
        }
        const char *value = argv[argIndex + 1];
        if (_stricmp(opt, "-o") == 0 || _stricmp(opt, "-i") == 0) {
            DWORD size;
            if (!ParseDword(value, &size) || size > MAX_BUFFER_SIZE) {
                printf("Invalid size for %s: %s (max %d)\n", opt, value, MAX_BUFFER_SIZE);
                return 1;
            }
            if (opt[1] == 'o' || opt[1] == 'O') {
                outputSize = (long)size;
            } else {
                inputSize = (long)size;
            }
        } else if (_stricmp(opt, "-p") == 0) {
            DWORD pmic;
            if (!ParseDword(value, &pmic) || pmic > 1) {
                printf("Invalid PMIC index: %s (0 or 1)\n", value);
                return 1;
            }
            onlyPmic = (int)pmic;
        } else if (_stricmp(opt, "-x") == 0) {
            rawHex = value;
        } else if (_stricmp(opt, "-d") == 0) {
            if (MultiByteToWideChar(CP_ACP, 0, value, -1, devicePathBuffer, MAX_PATH) == 0) {
                printf("Invalid device path: %s\n", value);
                return 1;
            }
            devicePath = devicePathBuffer;
        } else {
            printf("Unknown option: %s\n\n", opt);
            PrintUsage();
            return 1;
        }
        argIndex += 2;
    }

    if (argIndex >= argc) {
        PrintUsage();
        return 1;
    }

    if (_stricmp(argv[argIndex], "list") == 0) {
        PrintIoctlList(argIndex + 1 < argc ? argv[argIndex + 1] : NULL);
        return 0;
    }

    if (_stricmp(argv[argIndex], "auto") == 0) {
        if (!OpenDevice(devicePath)) {
            return 2;
        }
        RunAuto(onlyPmic);
        CloseHandle(g_Device);
        return 0;
    }

    // IOCTL code
    DWORD code;
    bool ambiguous;
    const IoctlInfo *info = FindIoctlByName(argv[argIndex], &ambiguous);
    if (info != NULL) {
        code = info->Code;
    } else if (!ambiguous && ParseDword(argv[argIndex], &code)) {
        info = FindIoctlByCode(code);
    } else {
        if (!ambiguous) {
            printf("Unknown IOCTL: %s (use 'list' to see the known ones)\n", argv[argIndex]);
        }
        return 1;
    }
    argIndex++;

    if (info != NULL && (info->Class == CLS_WRITE || info->Class == CLS_DANGEROUS) && !force) {
        printf("0x%08lX %s %s.\n", info->Code, info->Name,
               info->Class == CLS_WRITE ? "changes the state of the phone" : "may crash the phone");
        if (info->Note != NULL) {
            printf("Note: %s\n", info->Note);
        }
        printf("Add -f to send it anyway.\n");
        return 1;
    }

    // Input buffer
    DWORD dataLength = 0;
    int valueCount = argc - argIndex;
    static DWORD gpioList[MAX_GPIO_LIST];
    memset(g_InputBuffer, 0, sizeof(g_InputBuffer));

    if (rawHex != NULL) {
        if (valueCount > 0) {
            printf("-x and input values cannot be combined\n");
            return 1;
        }
        if (!ParseHexBytes(rawHex, g_InputBuffer, MAX_BUFFER_SIZE, &dataLength)) {
            printf("Invalid hex input: %s\n", rawHex);
            return 1;
        }
    } else if (!raw && info != NULL && info->Special == SPECIAL_GPIO_CONFIG) {
        // pmic, then the GPIO numbers: the driver reads them through a pointer to this array
        DWORD pmic = 0;
        DWORD count = 0;
        if (valueCount > 0 && !ParseDword(argv[argIndex], &pmic)) {
            printf("Invalid PMIC index: %s\n", argv[argIndex]);
            return 1;
        }
        for (int i = 1; i < valueCount; i++) {
            if (count >= MAX_GPIO_LIST || !ParseDword(argv[argIndex + i], &gpioList[count])) {
                printf("Invalid GPIO number: %s (max %d GPIOs)\n", argv[argIndex + i], MAX_GPIO_LIST);
                return 1;
            }
            count++;
        }
        if (count == 0) {
            gpioList[count++] = 0;
        }
        DWORD pointer = (DWORD)(ULONG_PTR)gpioList;
        memcpy(g_InputBuffer, &pmic, 4);
        memcpy(g_InputBuffer + 4, &pointer, 4);
        memcpy(g_InputBuffer + 8, &count, 4);
        dataLength = 12;
    } else if (!raw && info != NULL && info->InputSpec != NULL) {
        Field fields[MAX_FIELDS];
        int count = ParseSpec(info->InputSpec, fields, MAX_FIELDS);
        if (valueCount > count) {
            printf("Too many values: %s takes ", info->Name);
            PrintArgsUsage(info);
            printf("\nUse -r to give raw dwords or -x to give the whole buffer.\n");
            return 1;
        }
        for (int i = 0; i < valueCount; i++) {
            if (!WriteFieldValue(&fields[i], argv[argIndex + i], g_InputBuffer)) {
                return 1;
            }
            if (fields[i].Offset + fields[i].Size > dataLength) {
                dataLength = fields[i].Offset + fields[i].Size;
            }
        }
        if (valueCount < count && valueCount > 0) {
            printf("Note: missing values are sent as 0 (args: ");
            PrintArgsUsage(info);
            printf(")\n");
        }
    } else {
        for (int i = 0; i < valueCount; i++) {
            if (!AppendRawValue(argv[argIndex + i], g_InputBuffer, &dataLength)) {
                return 1;
            }
        }
    }

    // The driver checks both lengths for an exact match, so default to the required sizes.
    DWORD inputLength;
    if (inputSize >= 0) {
        inputLength = (DWORD)inputSize;
    } else if (info != NULL && dataLength <= info->InputSize) {
        inputLength = info->InputSize;
    } else {
        inputLength = dataLength;
    }
    DWORD outputLength = outputSize >= 0 ? (DWORD)outputSize : (info != NULL ? info->OutputSize : 4);

    if (!OpenDevice(devicePath)) {
        return 2;
    }
    CallResult r = Execute(code, g_InputBuffer, inputLength, outputLength);
    CloseHandle(g_Device);
    return r.Ok ? 0 : 3;
}

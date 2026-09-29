# wp81PmicTool

A command-line tool for Windows Phone 8.1 (ARM32) that sends IOCTLs to the Qualcomm PMIC driver `qcpmic8930.sys` (`\\.\QCOMPMIC`) and prints the result.

The driver exposes almost every block of the PMIC (a PM8038 on the Lumia 520): ADC, GPIO, MPP, voltage regulators, interrupts, charger, RTC, BMS fuel gauge, LEDs and more, through about 140 IOCTLs. The tool knows all of them, with their exact buffer sizes and, when known, the layout of their input and output buffers.

It has two modes:

- **Manual**: send one IOCTL, given by code or name, with your own input values.
- **Auto**: send every read-only IOCTL, one after the other.

For every IOCTL sent, the tool prints its name and code, the input buffer (hex dump and decoded fields), the exact NTSTATUS, and the output buffer (hex dump and decoded fields).

## Usage

```
wp81PmicTool [options] <ioctl> [value ...]    send one IOCTL
wp81PmicTool [-d path] [-p pmic] auto         send every read-only IOCTL
wp81PmicTool list [filter]                    print the known IOCTLs
```

| Argument | Description |
|---|---|
| `<ioctl>` | A code (`0x800A0FA8`) or a name: `IOCTL_PM_RTC_GET_TIME`, `RTC_GET_TIME`, or any unique part of it, such as `PMICS_INFO`. Names are case-insensitive. If a part matches several IOCTLs, the tool lists the candidates. |
| `value` | One value per input field, in the order shown by `list`. The first one is usually the PMIC index (0 = primary PMIC). Values are decimal, `0x` hex or negative. Missing values are sent as 0. VREG resources can be given by name (`LDO5`, `SMPS1`, `CXO_CLOCK`); raw-byte fields take hex bytes. |
| `-f` | Force. Required to send an IOCTL that changes the state of the phone (class `W`) or may crash it (class `D`). |
| `-r` | Raw values: each value is appended to the buffer as a dword. Use the prefixes `b:`, `w:`, `q:` and `x:` for a byte, a 16-bit value, a 64-bit value or hex bytes. |
| `-x hex` | The whole input buffer as hex bytes, for example `-x 0000000001000000`. |
| `-i n` / `-o n` | Input / output buffer size in bytes. The default is the size the driver requires. |
| `-p n` | Auto mode: only query PMIC `n`. |
| `-d path` | Device path. The default is `\\.\QCOMPMIC`. |
| `list [filter]` | Print the known IOCTLs with their class, buffer sizes and arguments. With a filter (part of a name or code), it also prints the output fields. |

The driver checks both buffer lengths for an **exact** match against its descriptor table. The tool therefore always sends the required sizes, pads missing input values with zeros, and prints a note when `-i` or `-o` sets another size.

### Examples

```
wp81PmicTool auto
wp81PmicTool auto > \data\users\public\pmic.txt
wp81PmicTool PMICS_INFO
wp81PmicTool RTC_GET_TIME 0
wp81PmicTool GET_POWER_SETTINGS LDO5
wp81PmicTool GPIO_GET_GPIO_STATE 0 3 1
wp81PmicTool IRQ_GET_INTERRUPTS_STATUS 0 3 0xFFFFFFFFFFFFFFFF 0
wp81PmicTool -f WLED_ENABLE 0 1
wp81PmicTool -f VIB_CONTROL_REGISTER 0 30      start the vibrator at 3.0 V
wp81PmicTool -f VIB_CONTROL_REGISTER 0 0       stop it (there is no timer)
wp81PmicTool -f -r WLED_CONFIG 0 1 b:2 b:3 ...
wp81PmicTool list GPIO
```

### Output format

The output values below are placeholders (`..`); the layout is what the tool prints.

```
------------------------------------------------------------------------------
IOCTL  : 0x80060FE4 IOCTL_PM_VREG_GET_POWER_SETTINGS
         device type 0x8006, function 1017, method 0, access 0, read-only
Note   : the resource can be given by name (SMPS1, LDO5, CXO_CLOCK, ...)
Input  : 4 bytes
  0000: 0C 00 00 00                                      ....
    resource                     = 12 (LDO5)
Output : 60 bytes buffer
Status : 0x00000000 STATUS_SUCCESS, 60 bytes returned
  0000: .. .. .. .. 0C 00 00 00 .. .. .. .. .. .. .. ..  ................
  ...
    client ?                     = ..
    resource                     = 12 (LDO5)
    kind                         = .. (..)
    setting[0] ?                 = ..
    ...
```

## IOCTL classes

`list` shows the class of each IOCTL:

| Class | Meaning | Manual mode | Auto mode |
|---|---|---|---|
| `R` | Read-only | sent | sent |
| `r` | Read-only, but not safe enough for auto (unknown input, or may consume keypad events) | sent | not sent |
| `W` | Changes the state of the PMIC or the driver | needs `-f` | not sent |
| `D` | May crash the phone | needs `-f` | not sent |
| `U` | Accepted by the validation, but has no handler: `STATUS_NOT_SUPPORTED` | sent | not sent |
| `S` | Stub that always fails | sent | not sent |
| `X` | Descriptor of a device type the driver does not handle: `STATUS_INVALID_DEVICE_REQUEST` | sent | not sent |

Two IOCTLs are class `D`:

- `IOCTL_PM_VREG_VOTE_FOR_POWER_SETTINGS`: a wrong vote can switch off a regulator the phone needs.
- `IOCTL_PM_GPIO_GET_GPIO_CONFIG`: the driver reads the GPIO list through a raw user-mode pointer taken from the input buffer. If the request is handled in another process's context (for example, while `qcbms8930.sys` is using the driver), this can bugcheck. The tool builds the list itself: `-f GPIO_GET_GPIO_CONFIG <pmic> <gpio> [gpio ...]`.

## Auto mode

Auto mode sends 133 IOCTLs for one PMIC, in this order:

| Section | IOCTLs |
|---|---|
| Hardware | `HARDWARE_GET_NUMBER_OF_PMICS`, `HARDWARE_GET_PMICS_INFO` (gives the number of PMICs to query), `GET_LED_INTERFACE` |
| Simple reads | ADC, BTM and CCADC data and status, sequencer flags, power key, charger state, battsafe, regulation loop and parameters, RTC time, time adjust, alarm status and alarm 0 time, battery alarm status |
| ADC prescalers | `ADC_GET_PRESCALAR` for AMUX channels 0–15, printed with their names (VCOIN, VBAT, DCIN, ...); channel 15 fails |
| BMS output registers | `GAUGE_READ_BMS_OUTPUT_REG_BMS` for selectors 0–7, printed with the register name and the right width and sign (the coulomb counter is 32-bit signed, VSENSE_AVG 16-bit signed). Selector 7 selects nothing and only rereads the previous register; the tool warns about it |
| GPIO | `GPIO_GET_NUMBER_OF_GPIOS`, then `GPIO_GET_GPIO_STATE` for each GPIO, with source 1 (GPIO function) and source 0 (interrupt real-time status) |
| MPP | `MPP_GET_INPUT_STATE` for MPPs 0–11 with sources 1 and 0. On a PM8038, source 1 fails for every MPP and source 0 succeeds even for MPPs the chip does not have (the interrupt path does not check the MPP number) |
| Interrupts | `IRQ_GET_INTERRUPTS_STATUS` for blocks 0–3, type 0 (latched) and type 1 (real-time), with a full 64-bit mask |
| Voltage regulators | `VREG_GET_POWER_SETTINGS` for the 55 resources (SMPS1–8, LDO1–29, LVS1–7, MVS1–2, NCP, CXO, VDDCX corner, DV1–5). The kind is decoded (0 LDO, 1 SMPS, 3 LVS, 5 CXO buffers, 6 CXO clock) and the voltage is shown in volts. A resource the PMIC does not have (LVS3–7 on a PM8038) returns an all-zero structure; the tool reports it as not present |

The run ends with the number of IOCTLs sent, succeeded and failed. Failures are expected for blocks the PMIC model does not have.

The MPP and interrupt-block ranges come from the PM8921 hardware; values the PMIC does not have return an error.

`GAUGE_READ_BMS_OUTPUT_REG_BMS` is read-only, but reading a BMS output register may briefly select it in the BMS hardware, which `qcbms8930.sys` also does.

## Reading the results

- **Status**: the tool calls `NtDeviceIoControlFile`, so it shows the exact NTSTATUS. In particular, it tells apart the driver's validation errors: `STATUS_INVALID_PARAMETER_1` (no descriptor) through `STATUS_INVALID_PARAMETER_5` (output length mismatch).
- **PMIC error**: most IOCTLs return a PMIC library error code in their output (`0` = success, `9` = PMIC index out of range, `19` = function not available on this PMIC model, and so on). The tool decodes it.
- **Failed calls return no data**: when the driver fails a request (for example `STATUS_UNSUCCESSFUL` because the PMIC library returned an error), Windows does not copy the output buffer back, so the PMIC error code cannot be shown.
- **Fields past the returned length** are shown as `(not returned)`. For example, `CCADC_READ_DATA` reports 4 bytes returned, so its error field is never returned.
- **RTC times** are shown in seconds and as days hh:mm:ss only. The PMIC RTC isn't counted from a calendar epoch (it read about 7 days on a Lumia 520), so no date is derived from it. An alarm time of `0xFFFFFFFF` means no alarm is set.
- **`HARDWARE_GET_PMICS_INFO`** marks the PMIC slots past the detected number as unused instead of decoding their zeros.
- Names and fields followed by `?` are uncertain, as in the driver documentation.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The IOCTL succeeded, or `auto`, `list` or help completed. |
| 1 | Invalid command line, or `-f` is missing. |
| 2 | The device could not be opened. |
| 3 | The IOCTL failed. |

## Building

The tool is cross-compiled for ARM32 with clang-cl against the Windows Phone 8.1 SDK. It links only against `mincore.lib`.

### Requirements

- [CMake](https://cmake.org/) 3.20 or later
- [Ninja](https://ninja-build.org/)
- [LLVM](https://llvm.org/) with `clang-cl` and `lld-link`, installed in `C:\Program Files\LLVM`
- Windows Phone 8.1 SDK (`C:\Program Files (x86)\Windows Phone Kits\8.1`)
- Visual Studio 2012 Windows Phone SDK (`C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\WPSDK`)

If your tools are installed elsewhere, update the paths in [CMakeLists.txt](wp81PmicTool/CMakeLists.txt) and [CMakePresets.json](wp81PmicTool/CMakePresets.json).

### Build

```
cd wp81PmicTool
cmake --preset arm32-windows
cmake --build build
```

The executable is written to `wp81PmicTool/build/wp81PmicTool.exe`.

## Running on the device

Copy `wp81PmicTool.exe` to the phone and run it from a command-line shell on the device. Opening `\\.\QCOMPMIC` needs enough privileges; if it fails with `ERROR_ACCESS_DENIED`, run the tool from a shell with higher privileges.

Auto mode prints about 1,300 lines, so redirect it to a file.

## Project layout

```
wp81PmicTool/
├── CMakeLists.txt       Build definition (SDK include and library paths)
├── CMakePresets.json    arm32-windows preset (clang-cl, lld-link, Ninja)
└── src/
    └── main.cpp         The whole tool, including the IOCTL table
```

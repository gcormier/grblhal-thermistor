# grblhal-thermistor

A grblHAL plugin that reads an NTC thermistor connected through a unity-gain op-amp buffer into a standard auxiliary analog input. Temperature is reported in realtime status responses and can be queried or calibrated via system commands.

---

## Circuit

```
V_SUPPLY (5 V)
    │
   [R_REF 10 kΩ]
    │
    ├──── NTC thermistor ──── GND
    │
 [op-amp unity buffer]
    │
   [R_SERIES] ← pi-filter/protection resistor at MCU pin
    │
    ├──── [R_PULLDOWN 5.1 kΩ] ──── GND
    │
 MCU ADC pin (3.3 V ref, 12-bit)
```

The op-amp buffer isolates the voltage divider from the ADC input impedance. `R_SERIES` is the resistor in the pi-filter or protection circuit between the op-amp output and the MCU pin — set `$457` to this value so the plugin can correct for it.

Default circuit constants (overridable in `my_machine.h` before including `thermistor.h`):

| Macro | Default | Description |
|---|---|---|
| `THERMISTOR_R_REF` | `10000.0` Ω | Top-side voltage divider resistor |
| `THERMISTOR_V_SUPPLY` | `5.0` V | Supply voltage for the divider |
| `THERMISTOR_R_PULLDOWN` | `5100.0` Ω | Pulldown at the MCU ADC pin |
| `THERMISTOR_V_ADC_REF` | `3.3` V | MCU ADC reference voltage |

---

## Supported thermistors

| `$456` value | Type | Beta | R₀ | T₀ |
|---|---|---|---|---|
| `0` (default) | NTC3950 | 3950 | 10 kΩ | 25 °C |

Temperature is calculated using the simplified Steinhart-Hart (Beta) equation:

```
1/T = 1/T₀ + (1/B) × ln(R / R₀)
```

---

## Adding a new thermistor type

All thermistor constants live at the top of `thermistor.c`. The `ntc_type` field in
settings (`$456`) is reserved for selecting between types at runtime.

**Step 1** — add constants in `thermistor.c`:

```c
#define NTC_XXXX_BETA   4050.0f
#define NTC_XXXX_R0     10000.0f
#define NTC_XXXX_T0_K   298.15f   // 25 °C
```

**Step 2** — update `read_temperature()` to branch on `therm_settings.ntc_type`:

```c
float beta, r0, t0_k;
switch (therm_settings.ntc_type) {
    case 1:  beta = NTC_XXXX_BETA; r0 = NTC_XXXX_R0; t0_k = NTC_XXXX_T0_K; break;
    default: beta = NTC3950_BETA;  r0 = NTC3950_R0;  t0_k = NTC3950_T0_K;  break;
}
float t_k = 1.0f / (1.0f / t0_k + logf(r_ntc / r0) / beta);
```

**Step 3** — update the `$456` setting's `max_value` string in `plugin_settings[]` to
reflect the new highest valid type number.

---

## Settings

Settings are registered starting at `Setting_UserDefined_5` (ID 455). The exact `$`
numbers depend on what other plugins are loaded — check `$$` output to confirm.

| Setting | Default | Description |
|---|---|---|
| `$455` | unassigned | Aux analog port number. **Requires reboot after change.** |
| `$456` | `0` | NTC type (0 = NTC3950) |
| `$457` | `0.0` Ω | Series resistance at MCU pin — corrects the op-amp output divider |
| `$458` | `0.0` °C | Calibration offset, set automatically by `$THRMCAL` |
| `$459` | `0.0` °C | Over-temperature alarm threshold (0 = disabled) |

---

## Commands

| Command | Description |
|---|---|
| `$THRMTEMP` | Print the current temperature reading (works in any machine state) |
| `$THRMCAL=<deg>` | Calibrate: measures raw temperature and sets `$458` so the reading equals `<deg>`. Machine must be idle. |

Example output:

```
$THRMTEMP
[TH0: 23.4 degC (cal offset: 1.20)]
ok

$THRMCAL=25.0
[THRMCAL: offset set to 1.60 degC]
ok
```

The realtime status response (`?`) includes the temperature as a field:

```
<Idle|MPos:0.000,0.000,0.000|FS:0,0|TH0:23.4>
```

The `TH0:` field is suppressed if the reading hasn't changed by more than 0.1 °C since the last report (to reduce serial traffic). It always appears on a forced full report.

If the sensor reads open-circuit or short-circuit (ADC at rail), the field is omitted and `$THRMTEMP` reports `[TH0: sensor error]`.

If `$459` is set and the temperature exceeds it, the machine will abort the current cycle.

---

## Integration

### 1. Add the plugin files

Place `thermistor.c` and `thermistor.h` in your driver source tree, or add this repository as a submodule:

```sh
git submodule add https://github.com/youruser/grblhal-thermistor plugins/thermistor
```

### 2. Enable in `my_machine.h`

```c
#define THERMISTOR_ENABLE 1
```

Override circuit constants here if your hardware differs from the defaults:

```c
#define THERMISTOR_R_PULLDOWN  4700.0f   // example: 4.7 kΩ pulldown
```

### 3a. Build with CMake

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    plugins/thermistor/thermistor.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    plugins/thermistor
)
```

### 3b. Build with PlatformIO

In `platformio.ini`, under your `[common]` or `[env:...]` section:

```ini
build_flags =
    -I grblhal-thermistor
    -D THERMISTOR_ENABLE=1

lib_deps =
    file://grblhal-thermistor
```

**Cache note:** PlatformIO caches `file://` libraries in `.pio/libdeps/`. After editing
plugin source files you must clear the cache or your changes will be ignored:

```sh
rm -rf .pio/libdeps/<env-name>/grblhal-thermistor
```

Then rebuild:

```sh
pio run -e <env-name>
```

---

## Requirements

- grblHAL core with ioports support (`IOPORTS_ENABLE`)
- At least one unclaimed auxiliary analog input port
- Floating-point `printf` support — link with `-Wl,-u,_printf_float` (already needed for most grblHAL builds)
- Tested on STM32F4xx (LongBoard32); should work on any grblHAL driver with analog ioports

---

## License

LGPL-3.0 — same as grblHAL core.

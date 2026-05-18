/*
  thermistor.c - NTC thermistor temperature sensing plugin

  Part of grblHAL

  Supports NTC thermistors wired through a unity-gain op-amp buffer into a
  standard STM32 aux analog input. Temperature is reported in the realtime
  status ('?') as |TH0:xx.x and can be queried with $THRMTEMP at any time.
  Calibration offset is stored in NVS via $THRMCAL=<known_celsius>.

  Settings (IDs 455-459, pneumaseal occupies 450-454):
    $455 - Analog aux port number  (reboot required)
    $456 - NTC type  (0 = NTC3950)
    $457 - Series resistance at MCU pin (ohms)
    $458 - Calibration offset (degrees C)
    $459 - Max temperature alarm (degrees C, 0 = disabled)

  Commands:
    $THRMTEMP        - Print current temperature reading
    $THRMCAL=<temp>  - Calibrate: set offset so current reading = <temp>
*/

#include "driver.h"

#if THERMISTOR_ENABLE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grbl/hal.h"
#include "grbl/protocol.h"
#include "grbl/nvs_buffer.h"
#include "grbl/system.h"

#include "thermistor.h"

// NTC3950 Beta-equation constants
#define NTC3950_BETA   3950.0f
#define NTC3950_R0     10000.0f
#define NTC3950_T0_K   298.15f    // 25 °C in Kelvin

typedef struct {
    uint8_t port;
    uint8_t ntc_type;     // 0 = NTC3950
    float   r_series;     // Series resistance at MCU input (ohms)
    float   cal_offset;   // Calibration offset (degrees C)
    float   max_temp;     // Over-temperature alarm (degrees C, 0 = disabled)
} thermistor_settings_t;

static uint8_t therm_port;
static bool can_monitor = false;
static nvs_address_t nvs_address;
static thermistor_settings_t therm_settings;
static io_port_cfg_t a_in;

static on_report_options_ptr on_report_options;
static on_realtime_report_ptr on_realtime_report;

static void therm_settings_save(void);   // forward declaration

// ---------------------------------------------------------------------------
// Temperature conversion
// ---------------------------------------------------------------------------

static float read_temperature (int32_t adc_raw)
{
    // Guard against open/short circuit or invalid ADC readings
    if (adc_raw <= 0 || adc_raw >= 4095)
        return NAN;

    float v_mcu  = (float)adc_raw * (THERMISTOR_V_ADC_REF / 4095.0f);

    // Reconstruct junction voltage across the op-amp output divider:
    //   V_junc = V_mcu * (R_pulldown + R_series) / R_pulldown
    float v_junc = v_mcu * ((THERMISTOR_R_PULLDOWN + therm_settings.r_series) / THERMISTOR_R_PULLDOWN);

    if (v_junc <= 0.0f || v_junc >= THERMISTOR_V_SUPPLY)
        return NAN;

    // NTC resistance from voltage divider: V_junc = V_supply * R_ntc / (R_ref + R_ntc)
    float r_ntc = THERMISTOR_R_REF * v_junc / (THERMISTOR_V_SUPPLY - v_junc);

    if (r_ntc <= 0.0f)
        return NAN;

    // Simplified Steinhart-Hart (Beta equation):  1/T = 1/T0 + (1/B) * ln(R/R0)
    float t_k = 1.0f / (1.0f / NTC3950_T0_K + logf(r_ntc / NTC3950_R0) / NTC3950_BETA);

    return t_k - 273.15f + therm_settings.cal_offset;
}

// ---------------------------------------------------------------------------
// Realtime report hook  — adds |TH0:xx.x to every '?' response
// ---------------------------------------------------------------------------

static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report)
{
    static float prev_temp = NAN;

    char buf[20] = "";

    if (can_monitor) {
        int32_t raw  = ioport_wait_on_input(Port_Analog, therm_port, WaitMode_Immediate, 0.0f);
        float   temp = read_temperature(raw);

        if (!isnan(temp)) {
            if (isnan(prev_temp) || fabsf(temp - prev_temp) >= 0.1f || report.all) {
                strcpy(buf, "|TH0:");
                strcat(buf, ftoa(temp, 1));
                prev_temp = temp;
            }
            if (therm_settings.max_temp > 0.0f && temp > therm_settings.max_temp)
                system_set_exec_alarm(Alarm_AbortCycle);
        }
    }

    if (*buf != '\0')
        stream_write(buf);

    if (on_realtime_report)
        on_realtime_report(stream_write, report);
}

// ---------------------------------------------------------------------------
// System commands
// ---------------------------------------------------------------------------

// $THRMTEMP — query current temperature (allowed in any machine state)
// $THRMTEMP=1 — same, plus raw ADC, voltages, and computed R_ntc for diagnostics
static status_code_t cmd_temp (sys_state_t state, char *args)
{
    if (!can_monitor)
        return Status_InvalidStatement;

    int32_t raw  = ioport_wait_on_input(Port_Analog, therm_port, WaitMode_Immediate, 0.0f);
    float   temp = read_temperature(raw);

    char buf[96];
    if (isnan(temp)) {
        strcpy(buf, "[TH0: sensor error]\r\n");
    } else {
        char s_temp[12], s_offset[12];
        strncpy(s_temp,   ftoa(temp, 1), sizeof(s_temp) - 1);
        strncpy(s_offset, ftoa(therm_settings.cal_offset, 2), sizeof(s_offset) - 1);
        sprintf(buf, "[TH0: %s degC (cal offset: %s)]\r\n", s_temp, s_offset);
    }

    hal.stream.write(buf);

    if (args && *args) {
        float v_mcu  = (float)raw * (THERMISTOR_V_ADC_REF / 4095.0f);
        float v_junc = v_mcu * ((THERMISTOR_R_PULLDOWN + therm_settings.r_series) / THERMISTOR_R_PULLDOWN);
        float r_ntc  = (v_junc > 0.0f && v_junc < THERMISTOR_V_SUPPLY)
                       ? THERMISTOR_R_REF * v_junc / (THERMISTOR_V_SUPPLY - v_junc)
                       : NAN;
        char s_vmcu[12], s_vjunc[12], s_rntc[12];
        strncpy(s_vmcu,  ftoa(v_mcu, 4),  sizeof(s_vmcu)  - 1);
        strncpy(s_vjunc, ftoa(v_junc, 4), sizeof(s_vjunc) - 1);
        strncpy(s_rntc,  isnan(r_ntc) ? "OOB" : ftoa(r_ntc, 1), sizeof(s_rntc) - 1);
        sprintf(buf, "[TH0 dbg: raw=%ld v_mcu=%sV v_junc=%sV r_ntc=%s]\r\n",
                (long)raw, s_vmcu, s_vjunc, s_rntc);
        hal.stream.write(buf);
    }

    return Status_OK;
}

// $THRMCAL=<known_celsius> — calibrate at a known temperature (idle only)
static status_code_t cmd_cal (sys_state_t state, char *args)
{
    if (!can_monitor)
        return Status_InvalidStatement;

    if (!(state == STATE_IDLE || (state & STATE_ALARM)))
        return Status_IdleError;

    if (args == NULL || *args == '\0')
        return Status_BadNumberFormat;

    char *endptr;
    float known_temp = strtof(args, &endptr);
    if (endptr == args)
        return Status_BadNumberFormat;

    // Measure without the current calibration offset to get the raw deviation
    float saved = therm_settings.cal_offset;
    therm_settings.cal_offset = 0.0f;
    int32_t raw      = ioport_wait_on_input(Port_Analog, therm_port, WaitMode_Immediate, 0.0f);
    float   measured = read_temperature(raw);
    therm_settings.cal_offset = saved;

    if (isnan(measured))
        return Status_GcodeValueWordMissing;

    therm_settings.cal_offset = known_temp - measured;
    therm_settings_save();

    char buf[56];
    sprintf(buf, "[THRMCAL: offset set to %s degC]\r\n",
            ftoa(therm_settings.cal_offset, 2));
    hal.stream.write(buf);
    return Status_OK;
}

// ---------------------------------------------------------------------------
// Settings — port selection (NonCoreFn, routes through ioport API)
// ---------------------------------------------------------------------------

static status_code_t set_port (setting_id_t setting, float value)
{
    return a_in.set_value(&a_in, &therm_settings.port, (pin_cap_t){}, value);
}

static float get_port (setting_id_t setting)
{
    return a_in.get_value(&a_in, therm_settings.port);
}

static bool is_port_available (const setting_detail_t *setting, uint_fast16_t offset)
{
    return a_in.n_ports > 0;
}

static const setting_detail_t plugin_settings[] = {
    { Setting_UserDefined_5, Group_AuxPorts, "Thermistor analog port", NULL,
      Format_Integer, "-#0", "-1", a_in.port_maxs,
      Setting_NonCoreFn, set_port, get_port, is_port_available, { .reboot_required = On } },
    { Setting_UserDefined_6, Group_General, "Thermistor NTC type", NULL,
      Format_Integer, "#0", "0", "0",
      Setting_NonCore, &therm_settings.ntc_type, NULL, NULL },
    { Setting_UserDefined_7, Group_General, "Thermistor series resistance", "ohm",
      Format_Decimal, "####0.0", "0.0", "10000.0",
      Setting_NonCore, &therm_settings.r_series, NULL, NULL },
    { Setting_UserDefined_8, Group_General, "Thermistor calibration offset", "deg",
      Format_Decimal, "-##0.00", "-50.0", "50.0",
      Setting_NonCore, &therm_settings.cal_offset, NULL, NULL },
    { Setting_UserDefined_9, Group_General, "Thermistor max temp alarm", "deg",
      Format_Decimal, "##0.0", "0.0", "300.0",
      Setting_NonCore, &therm_settings.max_temp, NULL, NULL },
};

static const setting_descr_t plugin_settings_descr[] = {
    { Setting_UserDefined_5, "Aux analog port number for the NTC thermistor input. Requires reboot after change." },
    { Setting_UserDefined_6, "NTC type (0 = NTC3950). Reserved for future thermistor types." },
    { Setting_UserDefined_7, "Series resistance in ohms between op-amp output and MCU ADC pin (from pi-filter / protection circuit). Use 0 if unknown; $THRMCAL corrects the remaining offset error." },
    { Setting_UserDefined_8, "Temperature calibration offset in degrees Celsius. Set automatically by $THRMCAL=<known_temp>." },
    { Setting_UserDefined_9, "Over-temperature alarm threshold in degrees Celsius. Machine will abort if exceeded. Set to 0 to disable." },
};

// ---------------------------------------------------------------------------
// NVS save/load/restore
// ---------------------------------------------------------------------------

static void therm_settings_save (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&therm_settings, sizeof(thermistor_settings_t), true);
}

static void therm_settings_restore (void)
{
    therm_settings.port       = IOPORT_UNASSIGNED;
    therm_settings.ntc_type   = 0;
    therm_settings.r_series   = 0.0f;
    therm_settings.cal_offset = 0.0f;
    therm_settings.max_temp   = 0.0f;
    therm_settings_save();
}

static void therm_settings_load (void)
{
    if (hal.nvs.memcpy_from_nvs((uint8_t *)&therm_settings, nvs_address,
                                 sizeof(thermistor_settings_t), true) != NVS_TransferResult_OK)
        therm_settings_restore();

    therm_port = therm_settings.port;

    // Claim the port (or proceed unclaimed if not yet assigned — user configures post-boot)
    if (therm_port == IOPORT_UNASSIGNED || a_in.claim(&a_in, &therm_port, "Thermistor", (pin_cap_t){})) {
        on_realtime_report = grbl.on_realtime_report;
        grbl.on_realtime_report = onRealtimeReport;
        can_monitor = (therm_port != IOPORT_UNASSIGNED);
    }
}

// ---------------------------------------------------------------------------
// Report options hook — announces plugin name/version in $I output
// ---------------------------------------------------------------------------

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);
    if (!newopt)
        report_plugin("Thermistor", "0.03");
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void thermistor_init (void)
{
    static const sys_command_t cmd_list[] = {
        { "THRMTEMP", cmd_temp, {},                   { .str = "print current thermistor temperature, add =1 for debug" } },
        { "THRMCAL",  cmd_cal,  {},                   { .str = "calibrate: $THRMCAL=<known_temp_celsius>" } },
    };

    static sys_commands_t commands = {
        .n_commands = sizeof(cmd_list) / sizeof(sys_command_t),
        .commands   = cmd_list,
    };

    static setting_details_t setting_details = {
        .settings       = plugin_settings,
        .n_settings     = sizeof(plugin_settings) / sizeof(setting_detail_t),
        .descriptions   = plugin_settings_descr,
        .n_descriptions = sizeof(plugin_settings_descr) / sizeof(setting_descr_t),
        .save           = therm_settings_save,
        .load           = therm_settings_load,
        .restore        = therm_settings_restore,
    };

    if (ioports_cfg(&a_in, Port_Analog, Port_Input)->n_ports &&
        (nvs_address = nvs_alloc(sizeof(thermistor_settings_t)))) {

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = onReportOptions;

        settings_register(&setting_details);
        system_register_commands(&commands);

    } else
        task_run_on_startup(report_warning, "Thermistor plugin failed to initialize!");
}

#endif // THERMISTOR_ENABLE

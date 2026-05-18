#pragma once

/*
  thermistor.h - NTC thermistor temperature sensing plugin

  Part of grblHAL

  Circuit constants. Override any of these in my_machine.h before this header is included.

  Expected circuit topology:
    V_SUPPLY ── R_REF ──┬── NTC ── GND
                        │
                   [op-amp unity buffer]
                        │
                   R_SERIES (pi-filter series R at MCU pin)
                        │
                   ┬── R_PULLDOWN ── GND
                   │
              MCU ADC (V_ADC_REF, 12-bit)
*/

#ifndef THERMISTOR_R_REF
#define THERMISTOR_R_REF       10000.0f   // 10k pullup resistor value (ohms)
#endif

#ifndef THERMISTOR_V_SUPPLY
#define THERMISTOR_V_SUPPLY    5.0f       // Pullup supply voltage (volts)
#endif

#ifndef THERMISTOR_R_PULLDOWN
#define THERMISTOR_R_PULLDOWN  5100.0f    // Pulldown at MCU input (ohms)
#endif

#ifndef THERMISTOR_V_ADC_REF
#define THERMISTOR_V_ADC_REF   3.3f       // MCU ADC reference voltage (volts)
#endif

void thermistor_init(void);

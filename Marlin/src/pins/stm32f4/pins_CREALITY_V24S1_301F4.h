/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2022 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#pragma once

/**
 * Creality V24S1_301F4 (STM32F401RC) board pin assignments as found on Ender-3 S1.
 */

#ifndef BOARD_INFO_NAME
  #define BOARD_INFO_NAME "Creality V24S1-301F4"
#endif
#ifndef DEFAULT_MACHINE_NAME
  #define DEFAULT_MACHINE_NAME "Ender-3 S1 F4"
#endif

#define DISABLE_DEBUG false // DISABLE_(DEBUG|JTAG) is not supported for STM32F4.
#define ALLOW_STM32F4

#define EEPROM_EXCL_ZONE 916,926  // Ender-3S1 STM32F401 Bootloader EEPROM exclusion zone

  /**
   *                 ------
   *           PC6  | 1  2 | PB2
   *  UART2_TX PA2  | 3  4 | PA3 UART2_RX
   *           PB14   5  6 | PB13
   *           PB12 | 7  8 | PB15
   *            GND | 9 10 | 5V
   *                 ------
   *                  EXP3
   */

#define EXP3_03_PIN                         PA2
#define EXP3_04_PIN                         PA3

  /**
   *          ----
   *     VCC | 1  |
   *     VCC | 2  |
   *     GND | 3  |
   *    PA14 | 4  |
   *    PA13 | 5  |
   *     PB1 | 6  |
   *     PB2 | 7  |
   *     PA2 | 8  | UART2_TX
   *     PA3 | 9  | UART2_RX
   *     GND | 10 |
   *          ----
   *   Touch screen Interface
   */

#include "../stm32f1/pins_CREALITY_V24S1_301.h"

/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2025 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
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

#define TS_TYPICAL_V      1.405
#define TS_TYPICAL_TEMP   25
#define TS_TYPICAL_SLOPE  4.5

// TODO: Implement voltage scaling (calibrated Vrefint) and ADC resolution scaling (when applicable)
#define TEMP_SOC_SENSOR(RAW) ((TS_TYPICAL_V - (RAW) / float(OVERSAMPLENR) / float(HAL_ADC_RANGE) * (float(ADC_VREF_MV) / 1000.0f)) / ((TS_TYPICAL_SLOPE) / 1000.0f) + TS_TYPICAL_TEMP)

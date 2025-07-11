/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
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

/**
 * Platform-independent Arduino functions for I2C EEPROM.
 * Enable USE_SHARED_EEPROM if not supplied by the framework.
 */

#ifdef __STM32F1__

#include "../../../inc/MarlinConfig.h"

#if ENABLED(IIC_BL24CXX_EEPROM)

#include "../../../libs/BL24CXX.h"
#include "../../shared/eeprom_if.h"

void eeprom_init() { BL24CXX::init(); }

// ------------------------
// Public functions
// ------------------------

void eeprom_write_byte(uint8_t *pos, uint8_t value) {
  const unsigned eeprom_address = (unsigned)pos;
  BL24CXX::writeOneByte(eeprom_address, value);
}

uint8_t eeprom_read_byte(uint8_t *pos) {
  const unsigned eeprom_address = (unsigned)pos;
  return BL24CXX::readOneByte(eeprom_address);
}

#endif // IIC_BL24CXX_EEPROM
#endif // __STM32F1__

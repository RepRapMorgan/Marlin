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

#include "../../inc/MarlinConfig.h"

#if ENABLED(HOST_KEEPALIVE_FEATURE)

#include "../gcode.h"

/**
 * M113: Get or set Host Keepalive interval (0 to disable)
 *
 * Parameters:
 *   None        Report current keepalive interval
 *   S<seconds>  Set the keepalive interval (0-60)
 */
void GcodeSuite::M113() {

  if (parser.seenval('S')) {
    host_keepalive_interval = parser.value_byte();
    NOMORE(host_keepalive_interval, 60);
  }
  else
    SERIAL_ECHO_MSG("M113 S", host_keepalive_interval);

}

#endif // HOST_KEEPALIVE_FEATURE

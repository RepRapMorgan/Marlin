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

#if HAS_MEDIA

#include "../gcode.h"
#include "../../sd/cardreader.h"

#if HAS_MULTI_SERIAL
  #include "../queue.h"
#endif

/**
 * M28: Start SD Write
 *
 * Parameters:
 *   <filename>  File name to write
 *
 *   With BINARY_FILE_TRANSFER:
 *     B1  Set an optimized binary file transfer mode
 */
void GcodeSuite::M28() {

  #if ENABLED(BINARY_FILE_TRANSFER)

    bool binary_mode = false;
    char *p = parser.string_arg;
    if (p[0] == 'B' && NUMERIC(p[1])) {
      binary_mode = p[1] > '0';
      p += 2;
      while (*p == ' ') ++p;
    }

    // Binary transfer mode
    if ((card.flag.binary_mode = binary_mode)) {
      SERIAL_ECHO_MSG("Switching to Binary Protocol");
      TERN_(HAS_MULTI_SERIAL, card.transfer_port_index = queue.ring_buffer.command_port().index);
    }
    else
      card.openFileWrite(p);

  #else

    card.openFileWrite(parser.string_arg);

  #endif
}

/**
 * M29: Stop SD Write
 * (Processed in write-to-file routine)
 */
void GcodeSuite::M29() {
  card.flag.saving = false;
}

#endif // HAS_MEDIA

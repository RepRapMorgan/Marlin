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

#if ENABLED(PIDTEMP)

#include "../gcode.h"
#include "../../module/temperature.h"

/**
 * M301: Set Hotend PID
 *
 * Set PID parameters P I D (and optionally C, L)
 *
 * Parameters:
 *   E<extruder>  Default: 0
 *   P<float>     Kp term
 *   I<float>     Ki term (unscaled)
 *   D<float>     Kd term (unscaled)
 *
 *   With PID_EXTRUSION_SCALING:
 *     C<float> Kc term
 *     L<int> LPQ length
 *
 *   With PID_FAN_SCALING:
 *     F<float> Kf term
 */
void GcodeSuite::M301() {
  // multi-extruder PID patch: M301 updates or prints a single extruder's PID values
  // default behavior (omitting E parameter) is to update for extruder 0 only
  int8_t e = E_TERN0(parser.byteval('E', -1)); // extruder being updated

  if (!parser.seen("PID" TERN_(PID_EXTRUSION_SCALING, "CL") TERN_(PID_FAN_SCALING, "F")))
    return M301_report(true E_OPTARG(e));

  if (e == -1) e = 0;

  if (e < HOTENDS) { // catch bad input value

    if (parser.seenval('P')) SET_HOTEND_PID(Kp, e, parser.value_float());
    if (parser.seenval('I')) SET_HOTEND_PID(Ki, e, parser.value_float());
    if (parser.seenval('D')) SET_HOTEND_PID(Kd, e, parser.value_float());

    #if ENABLED(PID_EXTRUSION_SCALING)
      if (parser.seenval('C')) SET_HOTEND_PID(Kc, e, parser.value_float());
      if (parser.seenval('L')) thermalManager.lpq_len = parser.value_int();
      LIMIT(thermalManager.lpq_len, 0, LPQ_MAX_LEN);
    #endif

    #if ENABLED(PID_FAN_SCALING)
      if (parser.seenval('F')) SET_HOTEND_PID(Kf, e, parser.value_float());
    #endif

    thermalManager.updatePID();
  }
  else
    SERIAL_ERROR_MSG(STR_INVALID_EXTRUDER);
}

void GcodeSuite::M301_report(const bool forReplay/*=true*/ E_OPTARG(const int8_t eindex/*=-1*/)) {
  TERN_(MARLIN_SMALL_BUILD, return);

  report_heading(forReplay, F(STR_HOTEND_PID));
  IF_DISABLED(HAS_MULTI_EXTRUDER, constexpr int8_t eindex = -1);
  HOTEND_LOOP() {
    if (e == eindex || eindex == -1) {
      const hotend_pid_t &pid = thermalManager.temp_hotend[e].pid;
      report_echo_start(forReplay);
      SERIAL_ECHOPGM_P(
        #if ENABLED(PID_PARAMS_PER_HOTEND)
          PSTR("  M301 E"), e, SP_P_STR
        #else
          PSTR("  M301 P")
        #endif
        , pid.p(), PSTR(" I"), pid.i(), PSTR(" D"), pid.d()
      );
      #if ENABLED(PID_EXTRUSION_SCALING)
        SERIAL_ECHOPGM_P(SP_C_STR, pid.c());
        if (e == 0) SERIAL_ECHOPGM(" L", thermalManager.lpq_len);
      #endif
      #if ENABLED(PID_FAN_SCALING)
        SERIAL_ECHOPGM(" F", pid.f());
      #endif
      SERIAL_EOL();
    }
  }
}

#endif // PIDTEMP

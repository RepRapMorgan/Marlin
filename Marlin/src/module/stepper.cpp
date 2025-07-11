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
 * stepper.cpp - A singleton object to execute motion plans using stepper motors
 * Marlin Firmware
 *
 * Derived from Grbl
 * Copyright (c) 2009-2011 Simen Svale Skogsrud
 *
 * Grbl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Grbl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Grbl.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Timer calculations informed by the 'RepRap cartesian firmware' by Zack Smith
 * and Philipp Tiefenbacher.
 */

/**
 *         __________________________
 *        /|                        |\     _________________         ^
 *       / |                        | \   /|               |\        |
 *      /  |                        |  \ / |               | \       s
 *     /   |                        |   |  |               |  \      p
 *    /    |                        |   |  |               |   \     e
 *   +-----+------------------------+---+--+---------------+----+    e
 *   |               BLOCK 1            |      BLOCK 2          |    d
 *
 *                           time ----->
 *
 *  The speed over time graph forms a TRAPEZOID. The slope of acceleration is calculated by
 *    v = u + t
 *  where 't' is the accumulated timer values of the steps so far.
 *
 *  The Stepper ISR dynamically executes acceleration, deceleration, and cruising according to the block parameters.
 *    - Start at block->initial_rate.
 *    - Accelerate while step_events_completed < block->accelerate_before.
 *    - Cruise while step_events_completed < block->decelerate_start.
 *    - Decelerate after that, until all steps are completed.
 *    - Reset the trapezoid generator.
 */

/**
 * Marlin uses the Bresenham algorithm. For a detailed explanation of theory and
 * method see https://www.cs.helsinki.fi/group/goa/mallinnus/lines/bresenh.html
 */

/**
 * Jerk controlled movements planner added Apr 2018 by Eduardo José Tagle.
 * Equations based on Synthethos TinyG2 sources, but the fixed-point
 * implementation is new, as we are running the ISR with a variable period.
 * Also implemented the Bézier velocity curve evaluation in ARM assembler,
 * to avoid impacting ISR speed.
 */

#include "stepper.h"

Stepper stepper; // Singleton

#define BABYSTEPPING_EXTRA_DIR_WAIT

#include "stepper/cycles.h"
#ifdef __AVR__
  #include "stepper/speed_lookuptable.h"
#endif

#include "endstops.h"
#include "planner.h"
#include "motion.h"

#if ENABLED(FT_MOTION)
  #include "ft_motion.h"
#endif

#include "../lcd/marlinui.h"
#include "../gcode/queue.h"
#include "../sd/cardreader.h"
#include "../MarlinCore.h"
#include "../HAL/shared/Delay.h"

#if ENABLED(BD_SENSOR)
  #include "../feature/bedlevel/bdl/bdl.h"
#endif

#if ENABLED(BABYSTEPPING)
  #include "../feature/babystep.h"
#endif

#if MB(ALLIGATOR)
  #include "../feature/dac/dac_dac084s085.h"
#endif

#if ENABLED(MIXING_EXTRUDER)
  #include "../feature/mixing.h"
#endif

#if HAS_FILAMENT_RUNOUT_DISTANCE
  #include "../feature/runout.h"
#endif

#if ENABLED(AUTO_POWER_CONTROL)
  #include "../feature/power.h"
#endif

#if ENABLED(POWER_LOSS_RECOVERY)
  #include "../feature/powerloss.h"
#endif

#if HAS_CUTTER
  #include "../feature/spindle_laser.h"
#endif

#if ENABLED(EXTENSIBLE_UI)
  #include "../lcd/extui/ui_api.h"
#endif

#if ENABLED(I2S_STEPPER_STREAM)
  #include "../HAL/ESP32/i2s.h"
#endif

// public:

#if ANY(HAS_EXTRA_ENDSTOPS, Z_STEPPER_AUTO_ALIGN)
  bool Stepper::separate_multi_axis = false;
#endif

stepper_flags_t Stepper::axis_enabled; // {0}

// private:

block_t* Stepper::current_block; // (= nullptr) A pointer to the block currently being traced

AxisBits Stepper::last_direction_bits, // = 0
         Stepper::axis_did_move; // = 0

bool Stepper::abort_current_block;

#if DISABLED(MIXING_EXTRUDER) && HAS_MULTI_EXTRUDER
  uint8_t Stepper::last_moved_extruder = 0xFF;
#endif

#if ENABLED(X_DUAL_ENDSTOPS)
  bool Stepper::locked_X_motor = false, Stepper::locked_X2_motor = false;
#endif
#if ENABLED(Y_DUAL_ENDSTOPS)
  bool Stepper::locked_Y_motor = false, Stepper::locked_Y2_motor = false;
#endif

#if ANY(Z_MULTI_ENDSTOPS, Z_STEPPER_AUTO_ALIGN)
  bool Stepper::locked_Z_motor = false, Stepper::locked_Z2_motor = false
    #if NUM_Z_STEPPERS >= 3
      , Stepper::locked_Z3_motor = false
      #if NUM_Z_STEPPERS >= 4
        , Stepper::locked_Z4_motor = false
      #endif
    #endif
  ;
#endif

// In timer_ticks
uint32_t Stepper::acceleration_time, Stepper::deceleration_time;

#if MULTISTEPPING_LIMIT > 1
  uint8_t Stepper::steps_per_isr = 1; // Count of steps to perform per Stepper ISR call
#endif

#if DISABLED(OLD_ADAPTIVE_MULTISTEPPING)
  hal_timer_t Stepper::time_spent_in_isr = 0, Stepper::time_spent_out_isr = 0;
#endif

#if ENABLED(ADAPTIVE_STEP_SMOOTHING)
  #if ENABLED(ADAPTIVE_STEP_SMOOTHING_TOGGLE)
    bool Stepper::adaptive_step_smoothing_enabled; // Initialized by settings.load
  #else
    constexpr bool Stepper::adaptive_step_smoothing_enabled; // = true
  #endif
  // Oversampling factor (log2(multiplier)) to increase temporal resolution of axis
  uint8_t Stepper::oversampling_factor;
#else
  constexpr uint8_t Stepper::oversampling_factor; // = 0
#endif

#if ENABLED(FREEZE_FEATURE)
  bool Stepper::frozen; // = false
#endif

xyze_long_t Stepper::delta_error{0};

xyze_long_t Stepper::advance_dividend{0};
uint32_t Stepper::advance_divisor = 0,
         Stepper::step_events_completed = 0, // The number of step events executed in the current block
         Stepper::accelerate_before,         // The count at which to start cruising
         Stepper::decelerate_start,          // The count at which to start decelerating
         Stepper::step_event_count;          // The total event count for the current block

#if ANY(HAS_MULTI_EXTRUDER, MIXING_EXTRUDER)
  uint8_t Stepper::stepper_extruder;
#else
  constexpr uint8_t Stepper::stepper_extruder;
#endif

#if ENABLED(S_CURVE_ACCELERATION)
  int32_t __attribute__((used)) Stepper::bezier_A __asm__("bezier_A");    // A coefficient in Bézier speed curve with alias for assembler
  int32_t __attribute__((used)) Stepper::bezier_B __asm__("bezier_B");    // B coefficient in Bézier speed curve with alias for assembler
  int32_t __attribute__((used)) Stepper::bezier_C __asm__("bezier_C");    // C coefficient in Bézier speed curve with alias for assembler
  uint32_t __attribute__((used)) Stepper::bezier_F __asm__("bezier_F");   // F coefficient in Bézier speed curve with alias for assembler
  uint32_t __attribute__((used)) Stepper::bezier_AV __asm__("bezier_AV"); // AV coefficient in Bézier speed curve with alias for assembler
  #ifdef __AVR__
    bool __attribute__((used)) Stepper::A_negative __asm__("A_negative"); // If A coefficient was negative
  #endif
  bool Stepper::bezier_2nd_half;    // =false If Bézier curve has been initialized or not
#endif

#if ENABLED(LIN_ADVANCE)
  hal_timer_t Stepper::nextAdvanceISR = LA_ADV_NEVER,
              Stepper::la_interval = LA_ADV_NEVER;
  #if HAS_ROUGH_LIN_ADVANCE
    int32_t   Stepper::la_delta_error = 0,
              Stepper::la_dividend = 0,
              Stepper::la_advance_steps = 0;
    bool      Stepper::la_active = false;
  #else
    uint32_t  Stepper::curr_step_rate,
              Stepper::curr_timer_tick = 0;
  #endif
#endif

#if ENABLED(NONLINEAR_EXTRUSION)
  nonlinear_t Stepper::ne;              // Initialized by settings.load
#endif

#if HAS_ZV_SHAPING
  shaping_time_t      ShapingQueue::now = 0;
  #if ANY(MCU_LPC1768, MCU_LPC1769) && DISABLED(NO_LPC_ETHERNET_BUFFER)
    // Use the 16K LPC Ethernet buffer: https://github.com/MarlinFirmware/Marlin/issues/25432#issuecomment-1450420638
    #define _ATTR_BUFFER __attribute__((section("AHBSRAM1"),aligned))
  #else
    #define _ATTR_BUFFER
  #endif
  shaping_time_t      ShapingQueue::times[shaping_echoes] _ATTR_BUFFER;
  shaping_echo_axis_t ShapingQueue::echo_axes[shaping_echoes];
  uint16_t            ShapingQueue::tail = 0;

  #define SHAPING_VAR_DEFS(AXIS)                                           \
    shaping_time_t  ShapingQueue::delay_##AXIS;                            \
    shaping_time_t  ShapingQueue::_peek_##AXIS = shaping_time_t(-1);       \
    uint16_t        ShapingQueue::head_##AXIS = 0;                         \
    uint16_t        ShapingQueue::_free_count_##AXIS = shaping_echoes - 1; \
    ShapeParams     Stepper::shaping_##AXIS;

  TERN_(INPUT_SHAPING_X, SHAPING_VAR_DEFS(x))
  TERN_(INPUT_SHAPING_Y, SHAPING_VAR_DEFS(y))
  TERN_(INPUT_SHAPING_Z, SHAPING_VAR_DEFS(z))
#endif

#if ENABLED(BABYSTEPPING)
  hal_timer_t Stepper::nextBabystepISR = BABYSTEP_NEVER;
#endif

#if ENABLED(DIRECT_STEPPING)
  page_step_state_t Stepper::page_step_state;
#endif

hal_timer_t Stepper::ticks_nominal = 0;
#if DISABLED(S_CURVE_ACCELERATION)
  uint32_t Stepper::acc_step_rate; // needed for deceleration start point
#endif

xyz_long_t Stepper::endstops_trigsteps;
xyze_long_t Stepper::count_position{0};
xyze_int8_t Stepper::count_direction{0};

#define MINDIR(A) (count_direction[_AXIS(A)] < 0)
#define MAXDIR(A) (count_direction[_AXIS(A)] > 0)

#define STEPTEST(A,M,I) TERN0(USE_##A##I##_##M, !(TEST(endstops.state(), A##I##_##M) && M## DIR(A)) && !locked_ ##A##I##_motor)
#define _STEP_WRITE(A,I,V) A##I##_STEP_WRITE(V)

#define DUAL_ENDSTOP_APPLY_STEP(A,V)             \
  if (separate_multi_axis) {                     \
    if (ENABLED(A##_HOME_TO_MIN)) {              \
      if (STEPTEST(A,MIN, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MIN,2)) _STEP_WRITE(A,2,V); \
    }                                            \
    else if (ENABLED(A##_HOME_TO_MAX)) {         \
      if (STEPTEST(A,MAX, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MAX,2)) _STEP_WRITE(A,2,V); \
    }                                            \
  }                                              \
  else {                                         \
    _STEP_WRITE(A, ,V);                          \
    _STEP_WRITE(A,2,V);                          \
  }

#define DUAL_SEPARATE_APPLY_STEP(A,V)             \
  if (separate_multi_axis) {                      \
    if (!locked_##A## _motor) _STEP_WRITE(A, ,V); \
    if (!locked_##A##2_motor) _STEP_WRITE(A,2,V); \
  }                                               \
  else {                                          \
    _STEP_WRITE(A, ,V);                           \
    _STEP_WRITE(A,2,V);                           \
  }

#define TRIPLE_ENDSTOP_APPLY_STEP(A,V)           \
  if (separate_multi_axis) {                     \
    if (ENABLED(A##_HOME_TO_MIN)) {              \
      if (STEPTEST(A,MIN, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MIN,2)) _STEP_WRITE(A,2,V); \
      if (STEPTEST(A,MIN,3)) _STEP_WRITE(A,3,V); \
    }                                            \
    else if (ENABLED(A##_HOME_TO_MAX)) {         \
      if (STEPTEST(A,MAX, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MAX,2)) _STEP_WRITE(A,2,V); \
      if (STEPTEST(A,MAX,3)) _STEP_WRITE(A,3,V); \
    }                                            \
  }                                              \
  else {                                         \
    _STEP_WRITE(A, ,V);                          \
    _STEP_WRITE(A,2,V);                          \
    _STEP_WRITE(A,3,V);                          \
  }

#define TRIPLE_SEPARATE_APPLY_STEP(A,V)           \
  if (separate_multi_axis) {                      \
    if (!locked_##A## _motor) _STEP_WRITE(A, ,V); \
    if (!locked_##A##2_motor) _STEP_WRITE(A,2,V); \
    if (!locked_##A##3_motor) _STEP_WRITE(A,3,V); \
  }                                               \
  else {                                          \
    _STEP_WRITE(A, ,V);                           \
    _STEP_WRITE(A,2,V);                           \
    _STEP_WRITE(A,3,V);                           \
  }

#define QUAD_ENDSTOP_APPLY_STEP(A,V)             \
  if (separate_multi_axis) {                     \
    if (ENABLED(A##_HOME_TO_MIN)) {              \
      if (STEPTEST(A,MIN, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MIN,2)) _STEP_WRITE(A,2,V); \
      if (STEPTEST(A,MIN,3)) _STEP_WRITE(A,3,V); \
      if (STEPTEST(A,MIN,4)) _STEP_WRITE(A,4,V); \
    }                                            \
    else if (ENABLED(A##_HOME_TO_MAX)) {         \
      if (STEPTEST(A,MAX, )) _STEP_WRITE(A, ,V); \
      if (STEPTEST(A,MAX,2)) _STEP_WRITE(A,2,V); \
      if (STEPTEST(A,MAX,3)) _STEP_WRITE(A,3,V); \
      if (STEPTEST(A,MAX,4)) _STEP_WRITE(A,4,V); \
    }                                            \
  }                                              \
  else {                                         \
    _STEP_WRITE(A, ,V);                          \
    _STEP_WRITE(A,2,V);                          \
    _STEP_WRITE(A,3,V);                          \
    _STEP_WRITE(A,4,V);                          \
  }

#define QUAD_SEPARATE_APPLY_STEP(A,V)             \
  if (separate_multi_axis) {                      \
    if (!locked_##A## _motor) _STEP_WRITE(A, ,V); \
    if (!locked_##A##2_motor) _STEP_WRITE(A,2,V); \
    if (!locked_##A##3_motor) _STEP_WRITE(A,3,V); \
    if (!locked_##A##4_motor) _STEP_WRITE(A,4,V); \
  }                                               \
  else {                                          \
    _STEP_WRITE(A, ,V);                           \
    _STEP_WRITE(A,2,V);                           \
    _STEP_WRITE(A,3,V);                           \
    _STEP_WRITE(A,4,V);                           \
  }

#if HAS_SYNCED_X_STEPPERS
  #define X_APPLY_DIR(FWD,Q) do{ X_DIR_WRITE(FWD); X2_DIR_WRITE(INVERT_DIR(X2_VS_X, FWD)); }while(0)
  #if ENABLED(X_DUAL_ENDSTOPS)
    #define X_APPLY_STEP(STATE,Q) DUAL_ENDSTOP_APPLY_STEP(X,STATE)
  #else
    #define X_APPLY_STEP(STATE,Q) do{ X_STEP_WRITE(STATE); X2_STEP_WRITE(STATE); }while(0)
  #endif
#elif ENABLED(DUAL_X_CARRIAGE)
  #define X_APPLY_DIR(FWD,ALWAYS) do{ \
    if (extruder_duplication_enabled || ALWAYS) { X_DIR_WRITE(FWD); X2_DIR_WRITE((FWD) ^ idex_mirrored_mode); } \
    else if (last_moved_extruder) X2_DIR_WRITE(FWD); else X_DIR_WRITE(FWD); \
  }while(0)
  #define X_APPLY_STEP(STATE,ALWAYS) do{ \
    if (extruder_duplication_enabled || ALWAYS) { X_STEP_WRITE(STATE); X2_STEP_WRITE(STATE); } \
    else if (last_moved_extruder) X2_STEP_WRITE(STATE); else X_STEP_WRITE(STATE); \
  }while(0)
#elif HAS_X_AXIS
  #define X_APPLY_DIR(FWD,Q) X_DIR_WRITE(FWD)
  #define X_APPLY_STEP(STATE,Q) X_STEP_WRITE(STATE)
#endif

#if HAS_SYNCED_Y_STEPPERS
  #define Y_APPLY_DIR(FWD,Q) do{ Y_DIR_WRITE(FWD); Y2_DIR_WRITE(INVERT_DIR(Y2_VS_Y, FWD)); }while(0)
  #if ENABLED(Y_DUAL_ENDSTOPS)
    #define Y_APPLY_STEP(STATE,Q) DUAL_ENDSTOP_APPLY_STEP(Y,STATE)
  #else
    #define Y_APPLY_STEP(STATE,Q) do{ Y_STEP_WRITE(STATE); Y2_STEP_WRITE(STATE); }while(0)
  #endif
#elif HAS_Y_AXIS
  #define Y_APPLY_DIR(FWD,Q) Y_DIR_WRITE(FWD)
  #define Y_APPLY_STEP(STATE,Q) Y_STEP_WRITE(STATE)
#endif

#if NUM_Z_STEPPERS == 4
  #define Z_APPLY_DIR(FWD,Q) do{ \
    Z_DIR_WRITE(FWD); Z2_DIR_WRITE(INVERT_DIR(Z2_VS_Z, FWD)); \
    Z3_DIR_WRITE(INVERT_DIR(Z3_VS_Z, FWD)); Z4_DIR_WRITE(INVERT_DIR(Z4_VS_Z, FWD)); \
  }while(0)
  #if ENABLED(Z_MULTI_ENDSTOPS)
    #define Z_APPLY_STEP(STATE,Q) QUAD_ENDSTOP_APPLY_STEP(Z,STATE)
  #elif ENABLED(Z_STEPPER_AUTO_ALIGN)
    #define Z_APPLY_STEP(STATE,Q) QUAD_SEPARATE_APPLY_STEP(Z,STATE)
  #else
    #define Z_APPLY_STEP(STATE,Q) do{ Z_STEP_WRITE(STATE); Z2_STEP_WRITE(STATE); Z3_STEP_WRITE(STATE); Z4_STEP_WRITE(STATE); }while(0)
  #endif
#elif NUM_Z_STEPPERS == 3
  #define Z_APPLY_DIR(FWD,Q) do{ \
    Z_DIR_WRITE(FWD); Z2_DIR_WRITE(INVERT_DIR(Z2_VS_Z, FWD)); Z3_DIR_WRITE(INVERT_DIR(Z3_VS_Z, FWD)); \
  }while(0)
  #if ENABLED(Z_MULTI_ENDSTOPS)
    #define Z_APPLY_STEP(STATE,Q) TRIPLE_ENDSTOP_APPLY_STEP(Z,STATE)
  #elif ENABLED(Z_STEPPER_AUTO_ALIGN)
    #define Z_APPLY_STEP(STATE,Q) TRIPLE_SEPARATE_APPLY_STEP(Z,STATE)
  #else
    #define Z_APPLY_STEP(STATE,Q) do{ Z_STEP_WRITE(STATE); Z2_STEP_WRITE(STATE); Z3_STEP_WRITE(STATE); }while(0)
  #endif
#elif NUM_Z_STEPPERS == 2
  #define Z_APPLY_DIR(FWD,Q) do{ Z_DIR_WRITE(FWD); Z2_DIR_WRITE(INVERT_DIR(Z2_VS_Z, FWD)); }while(0)
  #if ENABLED(Z_MULTI_ENDSTOPS)
    #define Z_APPLY_STEP(STATE,Q) DUAL_ENDSTOP_APPLY_STEP(Z,STATE)
  #elif ENABLED(Z_STEPPER_AUTO_ALIGN)
    #define Z_APPLY_STEP(STATE,Q) DUAL_SEPARATE_APPLY_STEP(Z,STATE)
  #else
    #define Z_APPLY_STEP(STATE,Q) do{ Z_STEP_WRITE(STATE); Z2_STEP_WRITE(STATE); }while(0)
  #endif
#elif HAS_Z_AXIS
  #define Z_APPLY_DIR(FWD,Q) Z_DIR_WRITE(FWD)
  #define Z_APPLY_STEP(STATE,Q) Z_STEP_WRITE(STATE)
#endif

#if HAS_I_AXIS
  #define I_APPLY_DIR(FWD,Q) I_DIR_WRITE(FWD)
  #define I_APPLY_STEP(STATE,Q) I_STEP_WRITE(STATE)
#endif
#if HAS_J_AXIS
  #define J_APPLY_DIR(FWD,Q) J_DIR_WRITE(FWD)
  #define J_APPLY_STEP(STATE,Q) J_STEP_WRITE(STATE)
#endif
#if HAS_K_AXIS
  #define K_APPLY_DIR(FWD,Q) K_DIR_WRITE(FWD)
  #define K_APPLY_STEP(STATE,Q) K_STEP_WRITE(STATE)
#endif
#if HAS_U_AXIS
  #define U_APPLY_DIR(FWD,Q) U_DIR_WRITE(FWD)
  #define U_APPLY_STEP(STATE,Q) U_STEP_WRITE(STATE)
#endif
#if HAS_V_AXIS
  #define V_APPLY_DIR(FWD,Q) V_DIR_WRITE(FWD)
  #define V_APPLY_STEP(STATE,Q) V_STEP_WRITE(STATE)
#endif
#if HAS_W_AXIS
  #define W_APPLY_DIR(FWD,Q) W_DIR_WRITE(FWD)
  #define W_APPLY_STEP(STATE,Q) W_STEP_WRITE(STATE)
#endif

//#define E0_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(0) : REV_E_DIR(0); }while(0)
//#define E1_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(1) : REV_E_DIR(1); }while(0)
//#define E2_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(2) : REV_E_DIR(2); }while(0)
//#define E3_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(3) : REV_E_DIR(3); }while(0)
//#define E4_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(4) : REV_E_DIR(4); }while(0)
//#define E5_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(5) : REV_E_DIR(5); }while(0)
//#define E6_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(6) : REV_E_DIR(6); }while(0)
//#define E7_APPLY_DIR(FWD) do{ (FWD) ? FWD_E_DIR(7) : REV_E_DIR(7); }while(0)

#if ENABLED(MIXING_EXTRUDER)
  #define E_APPLY_DIR(FWD,Q) do{ if (FWD) { MIXER_STEPPER_LOOP(j) FWD_E_DIR(j); } else { MIXER_STEPPER_LOOP(j) REV_E_DIR(j); } }while(0)
#else
  #define E_APPLY_DIR(FWD,Q) do{ if (FWD) { FWD_E_DIR(stepper_extruder); } else { REV_E_DIR(stepper_extruder); } }while(0)
  #define E_APPLY_STEP(STATE,Q) E_STEP_WRITE(stepper_extruder, STATE)
#endif

constexpr uint32_t cycles_to_ns(const uint32_t CYC) { return 1000UL * (CYC) / ((F_CPU) / 1000000); }
constexpr uint32_t ns_per_pulse_timer_tick = 1000000000UL / (STEPPER_TIMER_RATE);

// Round up when converting from ns to timer ticks
constexpr hal_timer_t ns_to_pulse_timer_ticks(const uint32_t ns) { return (ns + ns_per_pulse_timer_tick / 2) / ns_per_pulse_timer_tick; }

constexpr uint32_t timer_setup_ns = cycles_to_ns(timer_read_add_and_store_cycles);
constexpr hal_timer_t PULSE_HIGH_TICK_COUNT = ns_to_pulse_timer_ticks(_min_pulse_high_ns - _MIN(_min_pulse_high_ns, timer_setup_ns));
constexpr hal_timer_t PULSE_LOW_TICK_COUNT = ns_to_pulse_timer_ticks(_min_pulse_low_ns - _MIN(_min_pulse_low_ns, timer_setup_ns));

#define USING_TIMED_PULSE() hal_timer_t start_pulse_count = 0
#define START_TIMED_PULSE() (start_pulse_count = HAL_timer_get_count(MF_TIMER_PULSE))
#define AWAIT_TIMED_PULSE(DIR) while (PULSE_##DIR##_TICK_COUNT > HAL_timer_get_count(MF_TIMER_PULSE) - start_pulse_count) { /* nada */ }
#define AWAIT_HIGH_PULSE() AWAIT_TIMED_PULSE(HIGH)
#define AWAIT_LOW_PULSE()  AWAIT_TIMED_PULSE(LOW)

#if MINIMUM_STEPPER_PRE_DIR_DELAY > 0
  #define DIR_WAIT_BEFORE() DELAY_NS(MINIMUM_STEPPER_PRE_DIR_DELAY)
#else
  #define DIR_WAIT_BEFORE()
#endif

#if MINIMUM_STEPPER_POST_DIR_DELAY > 0
  #define DIR_WAIT_AFTER() DELAY_NS(MINIMUM_STEPPER_POST_DIR_DELAY)
#else
  #define DIR_WAIT_AFTER()
#endif

void Stepper::enable_axis(const AxisEnum axis) {
  #define _CASE_ENABLE(N) case N##_AXIS: ENABLE_AXIS_##N(); break;
  switch (axis) {
    MAIN_AXIS_MAP(_CASE_ENABLE)
    default: break;
  }
  mark_axis_enabled(axis);

  TERN_(EXTENSIBLE_UI, ExtUI::onAxisEnabled(ExtUI::axis_to_axis_t(axis)));
}

/**
 * Mark an axis as disabled and power off its stepper(s).
 * If one of the axis steppers is still in use by a non-disabled axis the axis will remain powered.
 * DISCUSSION: It's basically just stepper ENA pins that are shared across axes, not whole steppers.
 *             Used on MCUs with a shortage of pins. We already track the overlap of ENA pins, so now
 *             we just need stronger logic to track which ENA pins are being set more than once.
 *
 *             It would be better to use a bit mask (i.e., Flags<NUM_DISTINCT_AXIS_ENUMS>).
 *             While the method try_to_disable in gcode/control/M17_M18_M84.cpp does use the
 *             bit mask, it is still only at the axis level.
 * TODO: Power off steppers that don't share another axis. Currently axis-based steppers turn off as a unit.
 *       So we'd need to power off the off axis, then power on the on axis (for a microsecond).
 *       A global solution would keep a usage count when enabling or disabling a stepper, but this partially
 *       defeats the purpose of an on/off mask.
 */
bool Stepper::disable_axis(const AxisEnum axis) {
  mark_axis_disabled(axis);

  // This scheme prevents shared steppers being disabled. It should consider several axes at once
  // and keep a count of how many times each ENA pin has been set.

  // If all the axes that share the enabled bit are disabled
  // toggle the ENA state that they all share.
  const bool can_disable = can_axis_disable(axis);
  if (can_disable) {
    #define _CASE_DISABLE(N) case N##_AXIS: DISABLE_AXIS_##N(); break;
    switch (axis) {
      MAIN_AXIS_MAP(_CASE_DISABLE)
      default: break;
    }
    TERN_(EXTENSIBLE_UI, ExtUI::onAxisDisabled(ExtUI::axis_to_axis_t(axis)));
  }

  return can_disable;
}

#if HAS_EXTRUDERS

  void Stepper::enable_extruder(E_TERN_(const uint8_t eindex)) {
    IF_DISABLED(HAS_MULTI_EXTRUDER, constexpr uint8_t eindex = 0);
    #define _CASE_ENA_E(N) case N: ENABLE_AXIS_E##N(); mark_axis_enabled(E_AXIS E_OPTARG(eindex)); break;
    switch (eindex) {
      REPEAT(E_STEPPERS, _CASE_ENA_E)
    }
  }

  bool Stepper::disable_extruder(E_TERN_(const uint8_t eindex/*=0*/)) {
    IF_DISABLED(HAS_MULTI_EXTRUDER, constexpr uint8_t eindex = 0);
    mark_axis_disabled(E_AXIS E_OPTARG(eindex));
    const bool can_disable = can_axis_disable(E_AXIS E_OPTARG(eindex));
    if (can_disable) {
      #define _CASE_DIS_E(N) case N: DISABLE_AXIS_E##N(); break;
      switch (eindex) { REPEAT(E_STEPPERS, _CASE_DIS_E) }
    }
    return can_disable;
  }

  void Stepper::enable_e_steppers() {
    #define _ENA_E(N) ENABLE_EXTRUDER(N);
    REPEAT(EXTRUDERS, _ENA_E)
  }

  void Stepper::disable_e_steppers() {
    #define _DIS_E(N) DISABLE_EXTRUDER(N);
    REPEAT(EXTRUDERS, _DIS_E)
  }

#endif

void Stepper::enable_all_steppers() {
  TERN_(AUTO_POWER_CONTROL, powerManager.power_on());
  NUM_AXIS_CODE(
    enable_axis(X_AXIS), enable_axis(Y_AXIS), enable_axis(Z_AXIS),
    enable_axis(I_AXIS), enable_axis(J_AXIS), enable_axis(K_AXIS),
    enable_axis(U_AXIS), enable_axis(V_AXIS), enable_axis(W_AXIS)
  );
  enable_e_steppers();

  TERN_(EXTENSIBLE_UI, ExtUI::onSteppersEnabled());
}

void Stepper::disable_all_steppers() {
  NUM_AXIS_CODE(
    disable_axis(X_AXIS), disable_axis(Y_AXIS), disable_axis(Z_AXIS),
    disable_axis(I_AXIS), disable_axis(J_AXIS), disable_axis(K_AXIS),
    disable_axis(U_AXIS), disable_axis(V_AXIS), disable_axis(W_AXIS)
  );
  disable_e_steppers();

  TERN_(EXTENSIBLE_UI, ExtUI::onSteppersDisabled());
}

#if ENABLED(FT_MOTION)
  // We'll compare the updated DIR bits to the last set state
  static AxisBits last_set_direction;
#endif

// Set a single axis direction based on the last set flags.
// A direction bit of "1" indicates forward or positive motion.
#define SET_STEP_DIR(A) do{                     \
    const bool fwd = motor_direction(_AXIS(A)); \
    A##_APPLY_DIR(fwd, false);                  \
    count_direction[_AXIS(A)] = fwd ? 1 : -1;   \
  }while(0)

/**
 * Set the stepper direction of each axis
 *
 *   COREXY: X_AXIS=A_AXIS and Y_AXIS=B_AXIS
 *   COREXZ: X_AXIS=A_AXIS and Z_AXIS=C_AXIS
 *   COREYZ: Y_AXIS=B_AXIS and Z_AXIS=C_AXIS
 */
void Stepper::apply_directions() {
  DIR_WAIT_BEFORE();

  LOGICAL_AXIS_CODE(
    SET_STEP_DIR(E),
    SET_STEP_DIR(X), SET_STEP_DIR(Y), SET_STEP_DIR(Z), // ABC
    SET_STEP_DIR(I), SET_STEP_DIR(J), SET_STEP_DIR(K),
    SET_STEP_DIR(U), SET_STEP_DIR(V), SET_STEP_DIR(W)
  );

  TERN_(FT_MOTION, last_set_direction = last_direction_bits);

  DIR_WAIT_AFTER();
}

#if ENABLED(S_CURVE_ACCELERATION)
  /**
   *  This uses a quintic (fifth-degree) Bézier polynomial for the velocity curve, giving
   *  a "linear pop" velocity curve; with pop being the sixth derivative of position:
   *  velocity - 1st, acceleration - 2nd, jerk - 3rd, snap - 4th, crackle - 5th, pop - 6th
   *
   *  The Bézier curve takes the form:
   *
   *  V(t) = P_0 * B_0(t) + P_1 * B_1(t) + P_2 * B_2(t) + P_3 * B_3(t) + P_4 * B_4(t) + P_5 * B_5(t)
   *
   *  Where 0 <= t <= 1, and V(t) is the velocity. P_0 through P_5 are the control points, and B_0(t)
   *  through B_5(t) are the Bernstein basis as follows:
   *
   *        B_0(t) =   (1-t)^5        =   -t^5 +  5t^4 - 10t^3 + 10t^2 -  5t   +   1
   *        B_1(t) =  5(1-t)^4 * t    =   5t^5 - 20t^4 + 30t^3 - 20t^2 +  5t
   *        B_2(t) = 10(1-t)^3 * t^2  = -10t^5 + 30t^4 - 30t^3 + 10t^2
   *        B_3(t) = 10(1-t)^2 * t^3  =  10t^5 - 20t^4 + 10t^3
   *        B_4(t) =  5(1-t)   * t^4  =  -5t^5 +  5t^4
   *        B_5(t) =             t^5  =    t^5
   *                                      ^       ^       ^       ^       ^       ^
   *                                      |       |       |       |       |       |
   *                                      A       B       C       D       E       F
   *
   *  Unfortunately, we cannot use forward-differencing to calculate each position through
   *  the curve, as Marlin uses variable timer periods. So, we require a formula of the form:
   *
   *        V_f(t) = A*t^5 + B*t^4 + C*t^3 + D*t^2 + E*t + F
   *
   *  Looking at the above B_0(t) through B_5(t) expanded forms, if we take the coefficients of t^5
   *  through t of the Bézier form of V(t), we can determine that:
   *
   *        A =    -P_0 +  5*P_1 - 10*P_2 + 10*P_3 -  5*P_4 +  P_5
   *        B =   5*P_0 - 20*P_1 + 30*P_2 - 20*P_3 +  5*P_4
   *        C = -10*P_0 + 30*P_1 - 30*P_2 + 10*P_3
   *        D =  10*P_0 - 20*P_1 + 10*P_2
   *        E = - 5*P_0 +  5*P_1
   *        F =     P_0
   *
   *  Now, since we will (currently) *always* want the initial acceleration and jerk values to be 0,
   *  We set P_i = P_0 = P_1 = P_2 (initial velocity), and P_t = P_3 = P_4 = P_5 (target velocity),
   *  which, after simplification, resolves to:
   *
   *        A = - 6*P_i +  6*P_t =  6*(P_t - P_i)
   *        B =  15*P_i - 15*P_t = 15*(P_i - P_t)
   *        C = -10*P_i + 10*P_t = 10*(P_t - P_i)
   *        D = 0
   *        E = 0
   *        F = P_i
   *
   *  As the t is evaluated in non uniform steps here, there is no other way rather than evaluating
   *  the Bézier curve at each point:
   *
   *        V_f(t) = A*t^5 + B*t^4 + C*t^3 + F          [0 <= t <= 1]
   *
   * Floating point arithmetic execution time cost is prohibitive, so we will transform the math to
   * use fixed point values to be able to evaluate it in realtime. Assuming a maximum of 250000 steps
   * per second (driver pulses should at least be 2µS hi/2µS lo), and allocating 2 bits to avoid
   * overflows on the evaluation of the Bézier curve, means we can use
   *
   *   t: unsigned Q0.32 (0 <= t < 1) |range 0 to 0xFFFFFFFF unsigned
   *   A:   signed Q24.7 ,            |range = +/- 250000 * 6 * 128 = +/- 192000000 = 0x0B71B000 | 28 bits + sign
   *   B:   signed Q24.7 ,            |range = +/- 250000 *15 * 128 = +/- 480000000 = 0x1C9C3800 | 29 bits + sign
   *   C:   signed Q24.7 ,            |range = +/- 250000 *10 * 128 = +/- 320000000 = 0x1312D000 | 29 bits + sign
   *   F:   signed Q24.7 ,            |range = +/- 250000     * 128 =      32000000 = 0x01E84800 | 25 bits + sign
   *
   * The trapezoid generator state contains the following information, that we will use to create and evaluate
   * the Bézier curve:
   *
   *  blk->step_event_count [TS] = The total count of steps for this movement. (=distance)
   *  blk->initial_rate     [VI] = The initial steps per second (=velocity)
   *  blk->final_rate       [VF] = The ending steps per second  (=velocity)
   *  and the count of events completed (step_events_completed) [CS] (=distance until now)
   *
   *  Note the abbreviations we use in the following formulae are between []s
   *
   *  For Any 32bit CPU:
   *
   *    At the start of each trapezoid, calculate the coefficients A,B,C,F and Advance [AV], as follows:
   *
   *      A =  6*128*(VF - VI) =  768*(VF - VI)
   *      B = 15*128*(VI - VF) = 1920*(VI - VF)
   *      C = 10*128*(VF - VI) = 1280*(VF - VI)
   *      F =    128*VI        =  128*VI
   *     AV = (1<<32)/TS      ~= 0xFFFFFFFF / TS (To use ARM UDIV, that is 32 bits) (this is computed at the planner, to offload expensive calculations from the ISR)
   *
   *    And for each point, evaluate the curve with the following sequence:
   *
   *      void lsrs(uint32_t& d, uint32_t s, int cnt) {
   *        d = s >> cnt;
   *      }
   *      void lsls(uint32_t& d, uint32_t s, int cnt) {
   *        d = s << cnt;
   *      }
   *      void lsrs(int32_t& d, uint32_t s, int cnt) {
   *        d = uint32_t(s) >> cnt;
   *      }
   *      void lsls(int32_t& d, uint32_t s, int cnt) {
   *        d = uint32_t(s) << cnt;
   *      }
   *      void umull(uint32_t& rlo, uint32_t& rhi, uint32_t op1, uint32_t op2) {
   *        uint64_t res = uint64_t(op1) * op2;
   *        rlo = uint32_t(res & 0xFFFFFFFF);
   *        rhi = uint32_t((res >> 32) & 0xFFFFFFFF);
   *      }
   *      void smlal(int32_t& rlo, int32_t& rhi, int32_t op1, int32_t op2) {
   *        int64_t mul = int64_t(op1) * op2;
   *        int64_t s = int64_t(uint32_t(rlo) | ((uint64_t(uint32_t(rhi)) << 32U)));
   *        mul += s;
   *        rlo = int32_t(mul & 0xFFFFFFFF);
   *        rhi = int32_t((mul >> 32) & 0xFFFFFFFF);
   *      }
   *      int32_t _eval_bezier_curve_arm(uint32_t curr_step) {
   *        uint32_t flo = 0;
   *        uint32_t fhi = bezier_AV * curr_step;
   *        uint32_t t = fhi;
   *        int32_t alo = bezier_F;
   *        int32_t ahi = 0;
   *        int32_t A = bezier_A;
   *        int32_t B = bezier_B;
   *        int32_t C = bezier_C;
   *
   *        lsrs(ahi, alo, 1);          // a  = F << 31
   *        lsls(alo, alo, 31);         //
   *        umull(flo, fhi, fhi, t);    // f *= t
   *        umull(flo, fhi, fhi, t);    // f>>=32; f*=t
   *        lsrs(flo, fhi, 1);          //
   *        smlal(alo, ahi, flo, C);    // a+=(f>>33)*C
   *        umull(flo, fhi, fhi, t);    // f>>=32; f*=t
   *        lsrs(flo, fhi, 1);          //
   *        smlal(alo, ahi, flo, B);    // a+=(f>>33)*B
   *        umull(flo, fhi, fhi, t);    // f>>=32; f*=t
   *        lsrs(flo, fhi, 1);          // f>>=33;
   *        smlal(alo, ahi, flo, A);    // a+=(f>>33)*A;
   *        lsrs(alo, ahi, 6);          // a>>=38
   *
   *        return alo;
   *      }
   *
   *  This is rewritten in ARM assembly for optimal performance (43 cycles to execute).
   *
   *  For AVR, the precision of coefficients is scaled so the Bézier curve can be evaluated in real-time:
   *  Let's reduce precision as much as possible. After some experimentation we found that:
   *
   *    Assume t and AV with 24 bits is enough
   *       A =  6*(VF - VI)
   *       B = 15*(VI - VF)
   *       C = 10*(VF - VI)
   *       F =     VI
   *      AV = (1<<24)/TS   (this is computed at the planner, to offload expensive calculations from the ISR)
   *
   *    Instead of storing sign for each coefficient, we will store its absolute value,
   *    and flag the sign of the A coefficient, so we can save to store the sign bit.
   *    It always holds that sign(A) = - sign(B) = sign(C)
   *
   *     So, the resulting range of the coefficients are:
   *
   *       t: unsigned (0 <= t < 1) |range 0 to 0xFFFFFF unsigned
   *       A:   signed Q24 , range = 250000 * 6 = 1500000 = 0x16E360 | 21 bits
   *       B:   signed Q24 , range = 250000 *15 = 3750000 = 0x393870 | 22 bits
   *       C:   signed Q24 , range = 250000 *10 = 2500000 = 0x1312D0 | 21 bits
   *       F:   signed Q24 , range = 250000     =  250000 = 0x0ED090 | 20 bits
   *
   *    And for each curve, estimate its coefficients with:
   *
   *      void _calc_bezier_curve_coeffs(int32_t v0, int32_t v1, uint32_t av) {
   *       // Calculate the Bézier coefficients
   *       if (v1 < v0) {
   *         A_negative = true;
   *         bezier_A = 6 * (v0 - v1);
   *         bezier_B = 15 * (v0 - v1);
   *         bezier_C = 10 * (v0 - v1);
   *       }
   *       else {
   *         A_negative = false;
   *         bezier_A = 6 * (v1 - v0);
   *         bezier_B = 15 * (v1 - v0);
   *         bezier_C = 10 * (v1 - v0);
   *       }
   *       bezier_F = v0;
   *      }
   *
   *    And for each point, evaluate the curve with the following sequence:
   *
   *      // unsigned multiplication of 24 bits x 24bits, return upper 16 bits
   *      void umul24x24to16hi(uint16_t& r, uint24_t op1, uint24_t op2) {
   *        r = (uint64_t(op1) * op2) >> 8;
   *      }
   *      // unsigned multiplication of 16 bits x 16bits, return upper 16 bits
   *      void umul16x16to16hi(uint16_t& r, uint16_t op1, uint16_t op2) {
   *        r = (uint32_t(op1) * op2) >> 16;
   *      }
   *      // unsigned multiplication of 16 bits x 24bits, return upper 24 bits
   *      void umul16x24to24hi(uint24_t& r, uint16_t op1, uint24_t op2) {
   *        r = uint24_t((uint64_t(op1) * op2) >> 16);
   *      }
   *
   *      int32_t _eval_bezier_curve(uint32_t curr_step) {
   *        // To save computing, the first step is always the initial speed
   *        if (!curr_step)
   *          return bezier_F;
   *
   *        uint16_t t;
   *        umul24x24to16hi(t, bezier_AV, curr_step);   // t: Range 0 - 1^16 = 16 bits
   *        uint16_t f = t;
   *        umul16x16to16hi(f, f, t);                   // Range 16 bits (unsigned)
   *        umul16x16to16hi(f, f, t);                   // Range 16 bits : f = t^3  (unsigned)
   *        uint24_t acc = bezier_F;                    // Range 20 bits (unsigned)
   *        if (A_negative) {
   *          uint24_t v;
   *          umul16x24to24hi(v, f, bezier_C);          // Range 21bits
   *          acc -= v;
   *          umul16x16to16hi(f, f, t);                 // Range 16 bits : f = t^4  (unsigned)
   *          umul16x24to24hi(v, f, bezier_B);          // Range 22bits
   *          acc += v;
   *          umul16x16to16hi(f, f, t);                 // Range 16 bits : f = t^5  (unsigned)
   *          umul16x24to24hi(v, f, bezier_A);          // Range 21bits + 15 = 36bits (plus sign)
   *          acc -= v;
   *        }
   *        else {
   *          uint24_t v;
   *          umul16x24to24hi(v, f, bezier_C);          // Range 21bits
   *          acc += v;
   *          umul16x16to16hi(f, f, t);                 // Range 16 bits : f = t^4  (unsigned)
   *          umul16x24to24hi(v, f, bezier_B);          // Range 22bits
   *          acc -= v;
   *          umul16x16to16hi(f, f, t);                 // Range 16 bits : f = t^5  (unsigned)
   *          umul16x24to24hi(v, f, bezier_A);          // Range 21bits + 15 = 36bits (plus sign)
   *          acc += v;
   *        }
   *        return acc;
   *      }
   *    These functions are translated to assembler for optimal performance.
   *    Coefficient calculation takes 70 cycles. Bezier point evaluation takes 150 cycles.
   */

  #ifdef __AVR__

    // For AVR we use assembly to maximize speed
    void Stepper::_calc_bezier_curve_coeffs(const int32_t v0, const int32_t v1, const uint32_t av) {

      // Store advance
      bezier_AV = av;

      // Calculate the rest of the coefficients
      uint8_t r2 = v0 & 0xFF;
      uint8_t r3 = (v0 >> 8) & 0xFF;
      uint8_t r12 = (v0 >> 16) & 0xFF;
      uint8_t r5 = v1 & 0xFF;
      uint8_t r6 = (v1 >> 8) & 0xFF;
      uint8_t r7 = (v1 >> 16) & 0xFF;
      uint8_t r4,r8,r9,r10,r11;

      __asm__ __volatile__(
        /* Calculate the Bézier coefficients */
        /*  %10:%1:%0 = v0*/
        /*  %5:%4:%3 = v1*/
        /*  %7:%6:%10 = temporary*/
        /*  %9 = val (must be high register!)*/
        /*  %10 (must be high register!)*/

        /* Store initial velocity*/
        A("sts bezier_F, %0")
        A("sts bezier_F+1, %1")
        A("sts bezier_F+2, %10")    /* bezier_F = %10:%1:%0 = v0 */

        /* Get delta speed */
        A("ldi %2,-1")              /* %2 = 0xFF, means A_negative = true */
        A("clr %8")                 /* %8 = 0 */
        A("sub %0,%3")
        A("sbc %1,%4")
        A("sbc %10,%5")             /*  v0 -= v1, C=1 if result is negative */
        A("brcc 1f")                /* branch if result is positive (C=0), that means v0 >= v1 */

        /*  Result was negative, get the absolute value*/
        A("com %10")
        A("com %1")
        A("neg %0")
        A("sbc %1,%2")
        A("sbc %10,%2")             /* %10:%1:%0 +1  -> %10:%1:%0 = -(v0 - v1) = (v1 - v0) */
        A("clr %2")                 /* %2 = 0, means A_negative = false */

        /*  Store negative flag*/
        L("1")
        A("sts A_negative, %2")     /* Store negative flag */

        /*  Compute coefficients A,B and C   [20 cycles worst case]*/
        A("ldi %9,6")               /* %9 = 6 */
        A("mul %0,%9")              /* r1:r0 = 6*LO(v0-v1) */
        A("sts bezier_A, r0")
        A("mov %6,r1")
        A("clr %7")                 /* %7:%6:r0 = 6*LO(v0-v1) */
        A("mul %1,%9")              /* r1:r0 = 6*MI(v0-v1) */
        A("add %6,r0")
        A("adc %7,r1")              /* %7:%6:?? += 6*MI(v0-v1) << 8 */
        A("mul %10,%9")             /* r1:r0 = 6*HI(v0-v1) */
        A("add %7,r0")              /* %7:%6:?? += 6*HI(v0-v1) << 16 */
        A("sts bezier_A+1, %6")
        A("sts bezier_A+2, %7")     /* bezier_A = %7:%6:?? = 6*(v0-v1) [35 cycles worst] */

        A("ldi %9,15")              /* %9 = 15 */
        A("mul %0,%9")              /* r1:r0 = 5*LO(v0-v1) */
        A("sts bezier_B, r0")
        A("mov %6,r1")
        A("clr %7")                 /* %7:%6:?? = 5*LO(v0-v1) */
        A("mul %1,%9")              /* r1:r0 = 5*MI(v0-v1) */
        A("add %6,r0")
        A("adc %7,r1")              /* %7:%6:?? += 5*MI(v0-v1) << 8 */
        A("mul %10,%9")             /* r1:r0 = 5*HI(v0-v1) */
        A("add %7,r0")              /* %7:%6:?? += 5*HI(v0-v1) << 16 */
        A("sts bezier_B+1, %6")
        A("sts bezier_B+2, %7")     /* bezier_B = %7:%6:?? = 5*(v0-v1) [50 cycles worst] */

        A("ldi %9,10")              /* %9 = 10 */
        A("mul %0,%9")              /* r1:r0 = 10*LO(v0-v1) */
        A("sts bezier_C, r0")
        A("mov %6,r1")
        A("clr %7")                 /* %7:%6:?? = 10*LO(v0-v1) */
        A("mul %1,%9")              /* r1:r0 = 10*MI(v0-v1) */
        A("add %6,r0")
        A("adc %7,r1")              /* %7:%6:?? += 10*MI(v0-v1) << 8 */
        A("mul %10,%9")             /* r1:r0 = 10*HI(v0-v1) */
        A("add %7,r0")              /* %7:%6:?? += 10*HI(v0-v1) << 16 */
        A("sts bezier_C+1, %6")
        " sts bezier_C+2, %7"       /* bezier_C = %7:%6:?? = 10*(v0-v1) [65 cycles worst] */
        : "+r" (r2),
          "+d" (r3),
          "=r" (r4),
          "+r" (r5),
          "+r" (r6),
          "+r" (r7),
          "=r" (r8),
          "=r" (r9),
          "=r" (r10),
          "=d" (r11),
          "+r" (r12)
        :
        : "r0", "r1", "cc", "memory"
      );
    }

    FORCE_INLINE int32_t Stepper::_eval_bezier_curve(const uint32_t curr_step) {

      // If dealing with the first step, save expensive computing and return the initial speed
      if (!curr_step)
        return bezier_F;

      uint8_t r0 = 0; /* Zero register */
      uint8_t r2 = (curr_step) & 0xFF;
      uint8_t r3 = (curr_step >> 8) & 0xFF;
      uint8_t r4 = (curr_step >> 16) & 0xFF;
      uint8_t r1,r5,r6,r7,r8,r9,r10,r11; /* Temporary registers */

      __asm__ __volatile(
        /* umul24x24to16hi(t, bezier_AV, curr_step);  t: Range 0 - 1^16 = 16 bits*/
        A("lds %9,bezier_AV")       /* %9 = LO(AV)*/
        A("mul %9,%2")              /* r1:r0 = LO(bezier_AV)*LO(curr_step)*/
        A("mov %7,r1")              /* %7 = LO(bezier_AV)*LO(curr_step) >> 8*/
        A("clr %8")                 /* %8:%7  = LO(bezier_AV)*LO(curr_step) >> 8*/
        A("lds %10,bezier_AV+1")    /* %10 = MI(AV)*/
        A("mul %10,%2")             /* r1:r0  = MI(bezier_AV)*LO(curr_step)*/
        A("add %7,r0")
        A("adc %8,r1")              /* %8:%7 += MI(bezier_AV)*LO(curr_step)*/
        A("lds r1,bezier_AV+2")     /* r11 = HI(AV)*/
        A("mul r1,%2")              /* r1:r0  = HI(bezier_AV)*LO(curr_step)*/
        A("add %8,r0")              /* %8:%7 += HI(bezier_AV)*LO(curr_step) << 8*/
        A("mul %9,%3")              /* r1:r0 =  LO(bezier_AV)*MI(curr_step)*/
        A("add %7,r0")
        A("adc %8,r1")              /* %8:%7 += LO(bezier_AV)*MI(curr_step)*/
        A("mul %10,%3")             /* r1:r0 =  MI(bezier_AV)*MI(curr_step)*/
        A("add %8,r0")              /* %8:%7 += LO(bezier_AV)*MI(curr_step) << 8*/
        A("mul %9,%4")              /* r1:r0 =  LO(bezier_AV)*HI(curr_step)*/
        A("add %8,r0")              /* %8:%7 += LO(bezier_AV)*HI(curr_step) << 8*/
        /* %8:%7 = t*/

        /* uint16_t f = t;*/
        A("mov %5,%7")              /* %6:%5 = f*/
        A("mov %6,%8")
        /* %6:%5 = f*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits (unsigned) [17] */
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %9,r1")              /* store MIL(LO(f) * LO(t)) in %9, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %9,r0")              /* %9 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %9,r0")              /* %9 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t)) */
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 = */
        A("mov %6,%11")             /* f = %10:%11*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits : f = t^3  (unsigned) [17]*/
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %1,r1")              /* store MIL(LO(f) * LO(t)) in %1, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %1,r0")              /* %1 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %1,r0")              /* %1 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 =*/
        A("mov %6,%11")             /* f = %10:%11*/
        /* [15 +17*2] = [49]*/

        /* %4:%3:%2 will be acc from now on*/

        /* uint24_t acc = bezier_F; / Range 20 bits (unsigned)*/
        A("clr %9")                 /* "decimal place we get for free"*/
        A("lds %2,bezier_F")
        A("lds %3,bezier_F+1")
        A("lds %4,bezier_F+2")      /* %4:%3:%2 = acc*/

        /* if (A_negative) {*/
        A("lds r0,A_negative")
        A("or r0,%0")               /* Is flag signalling negative? */
        A("brne 3f")                /* If yes, Skip next instruction if A was negative*/
        A("rjmp 1f")                /* Otherwise, jump */

        /* uint24_t v; */
        /* umul16x24to24hi(v, f, bezier_C); / Range 21bits [29] */
        /* acc -= v; */
        L("3")
        A("lds %10, bezier_C")      /* %10 = LO(bezier_C)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_C) * LO(f)*/
        A("sub %9,r1")
        A("sbc %2,%0")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(LO(bezier_C) * LO(f))*/
        A("lds %11, bezier_C+1")    /* %11 = MI(bezier_C)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_C) * LO(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_C) * LO(f)*/
        A("lds %1, bezier_C+2")     /* %1 = HI(bezier_C)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_C) * LO(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(bezier_C) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_C) * MI(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= LO(bezier_C) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_C) * MI(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_C) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_C) * LO(f)*/
        A("sub %3,r0")
        A("sbc %4,r1")              /* %4:%3:%2:%9 -= HI(bezier_C) * LO(f) << 16*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits : f = t^3  (unsigned) [17]*/
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %1,r1")              /* store MIL(LO(f) * LO(t)) in %1, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %1,r0")              /* %1 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %1,r0")              /* %1 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 =*/
        A("mov %6,%11")             /* f = %10:%11*/

        /* umul16x24to24hi(v, f, bezier_B); / Range 22bits [29]*/
        /* acc += v; */
        A("lds %10, bezier_B")      /* %10 = LO(bezier_B)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_B) * LO(f)*/
        A("add %9,r1")
        A("adc %2,%0")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(LO(bezier_B) * LO(f))*/
        A("lds %11, bezier_B+1")    /* %11 = MI(bezier_B)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_B) * LO(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_B) * LO(f)*/
        A("lds %1, bezier_B+2")     /* %1 = HI(bezier_B)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_B) * LO(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(bezier_B) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_B) * MI(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += LO(bezier_B) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_B) * MI(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_B) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_B) * LO(f)*/
        A("add %3,r0")
        A("adc %4,r1")              /* %4:%3:%2:%9 += HI(bezier_B) * LO(f) << 16*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits : f = t^5  (unsigned) [17]*/
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %1,r1")              /* store MIL(LO(f) * LO(t)) in %1, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %1,r0")              /* %1 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %1,r0")              /* %1 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 =*/
        A("mov %6,%11")             /* f = %10:%11*/

        /* umul16x24to24hi(v, f, bezier_A); / Range 21bits [29]*/
        /* acc -= v; */
        A("lds %10, bezier_A")      /* %10 = LO(bezier_A)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_A) * LO(f)*/
        A("sub %9,r1")
        A("sbc %2,%0")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(LO(bezier_A) * LO(f))*/
        A("lds %11, bezier_A+1")    /* %11 = MI(bezier_A)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_A) * LO(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_A) * LO(f)*/
        A("lds %1, bezier_A+2")     /* %1 = HI(bezier_A)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_A) * LO(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(bezier_A) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_A) * MI(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= LO(bezier_A) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_A) * MI(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_A) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_A) * LO(f)*/
        A("sub %3,r0")
        A("sbc %4,r1")              /* %4:%3:%2:%9 -= HI(bezier_A) * LO(f) << 16*/
        A("jmp 2f")                 /* Done!*/

        L("1")

        /* uint24_t v; */
        /* umul16x24to24hi(v, f, bezier_C); / Range 21bits [29]*/
        /* acc += v; */
        A("lds %10, bezier_C")      /* %10 = LO(bezier_C)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_C) * LO(f)*/
        A("add %9,r1")
        A("adc %2,%0")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(LO(bezier_C) * LO(f))*/
        A("lds %11, bezier_C+1")    /* %11 = MI(bezier_C)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_C) * LO(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_C) * LO(f)*/
        A("lds %1, bezier_C+2")     /* %1 = HI(bezier_C)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_C) * LO(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(bezier_C) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_C) * MI(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += LO(bezier_C) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_C) * MI(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_C) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_C) * LO(f)*/
        A("add %3,r0")
        A("adc %4,r1")              /* %4:%3:%2:%9 += HI(bezier_C) * LO(f) << 16*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits : f = t^3  (unsigned) [17]*/
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %1,r1")              /* store MIL(LO(f) * LO(t)) in %1, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %1,r0")              /* %1 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %1,r0")              /* %1 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 =*/
        A("mov %6,%11")             /* f = %10:%11*/

        /* umul16x24to24hi(v, f, bezier_B); / Range 22bits [29]*/
        /* acc -= v;*/
        A("lds %10, bezier_B")      /* %10 = LO(bezier_B)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_B) * LO(f)*/
        A("sub %9,r1")
        A("sbc %2,%0")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(LO(bezier_B) * LO(f))*/
        A("lds %11, bezier_B+1")    /* %11 = MI(bezier_B)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_B) * LO(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_B) * LO(f)*/
        A("lds %1, bezier_B+2")     /* %1 = HI(bezier_B)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_B) * LO(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= HI(bezier_B) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_B) * MI(f)*/
        A("sub %9,r0")
        A("sbc %2,r1")
        A("sbc %3,%0")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= LO(bezier_B) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_B) * MI(f)*/
        A("sub %2,r0")
        A("sbc %3,r1")
        A("sbc %4,%0")              /* %4:%3:%2:%9 -= MI(bezier_B) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_B) * LO(f)*/
        A("sub %3,r0")
        A("sbc %4,r1")              /* %4:%3:%2:%9 -= HI(bezier_B) * LO(f) << 16*/

        /* umul16x16to16hi(f, f, t); / Range 16 bits : f = t^5  (unsigned) [17]*/
        A("mul %5,%7")              /* r1:r0 = LO(f) * LO(t)*/
        A("mov %1,r1")              /* store MIL(LO(f) * LO(t)) in %1, we need it for rounding*/
        A("clr %10")                /* %10 = 0*/
        A("clr %11")                /* %11 = 0*/
        A("mul %5,%8")              /* r1:r0 = LO(f) * HI(t)*/
        A("add %1,r0")              /* %1 += LO(LO(f) * HI(t))*/
        A("adc %10,r1")             /* %10 = HI(LO(f) * HI(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%7")              /* r1:r0 = HI(f) * LO(t)*/
        A("add %1,r0")              /* %1 += LO(HI(f) * LO(t))*/
        A("adc %10,r1")             /* %10 += HI(HI(f) * LO(t))*/
        A("adc %11,%0")             /* %11 += carry*/
        A("mul %6,%8")              /* r1:r0 = HI(f) * HI(t)*/
        A("add %10,r0")             /* %10 += LO(HI(f) * HI(t))*/
        A("adc %11,r1")             /* %11 += HI(HI(f) * HI(t))*/
        A("mov %5,%10")             /* %6:%5 =*/
        A("mov %6,%11")             /* f = %10:%11*/

        /* umul16x24to24hi(v, f, bezier_A); / Range 21bits [29]*/
        /* acc += v; */
        A("lds %10, bezier_A")      /* %10 = LO(bezier_A)*/
        A("mul %10,%5")             /* r1:r0 = LO(bezier_A) * LO(f)*/
        A("add %9,r1")
        A("adc %2,%0")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(LO(bezier_A) * LO(f))*/
        A("lds %11, bezier_A+1")    /* %11 = MI(bezier_A)*/
        A("mul %11,%5")             /* r1:r0 = MI(bezier_A) * LO(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_A) * LO(f)*/
        A("lds %1, bezier_A+2")     /* %1 = HI(bezier_A)*/
        A("mul %1,%5")              /* r1:r0 = MI(bezier_A) * LO(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += HI(bezier_A) * LO(f) << 8*/
        A("mul %10,%6")             /* r1:r0 = LO(bezier_A) * MI(f)*/
        A("add %9,r0")
        A("adc %2,r1")
        A("adc %3,%0")
        A("adc %4,%0")              /* %4:%3:%2:%9 += LO(bezier_A) * MI(f)*/
        A("mul %11,%6")             /* r1:r0 = MI(bezier_A) * MI(f)*/
        A("add %2,r0")
        A("adc %3,r1")
        A("adc %4,%0")              /* %4:%3:%2:%9 += MI(bezier_A) * MI(f) << 8*/
        A("mul %1,%6")              /* r1:r0 = HI(bezier_A) * LO(f)*/
        A("add %3,r0")
        A("adc %4,r1")              /* %4:%3:%2:%9 += HI(bezier_A) * LO(f) << 16*/
        L("2")
        " clr __zero_reg__"         /* C runtime expects r1 = __zero_reg__ = 0 */
        : "+r"(r0),
          "+r"(r1),
          "+r"(r2),
          "+r"(r3),
          "+r"(r4),
          "+r"(r5),
          "+r"(r6),
          "+r"(r7),
          "+r"(r8),
          "+r"(r9),
          "+r"(r10),
          "+r"(r11)
        :
        :"cc","r0","r1"
      );
      return (r2 | (uint16_t(r3) << 8)) | (uint32_t(r4) << 16);
    }

  #else // !__AVR__

    // For all the other 32bit CPUs
    FORCE_INLINE void Stepper::_calc_bezier_curve_coeffs(const int32_t v0, const int32_t v1, const uint32_t av) {
      // Calculate the Bézier coefficients
      bezier_A =  768 * (v1 - v0);
      bezier_B = 1920 * (v0 - v1);
      bezier_C = 1280 * (v1 - v0);
      bezier_F =  128 * v0;
      bezier_AV = av;
    }

    FORCE_INLINE int32_t Stepper::_eval_bezier_curve(const uint32_t curr_step) {
      #if (defined(__arm__) || defined(__thumb__)) && __ARM_ARCH >= 6 && !defined(STM32G0B1xx) // TODO: Test define STM32G0xx versus STM32G0B1xx

        // For ARM Cortex M3/M4 CPUs, we have the optimized assembler version, that takes 43 cycles to execute
        uint32_t flo = 0;
        uint32_t fhi = bezier_AV * curr_step;
        uint32_t t = fhi;
        int32_t alo = bezier_F;
        int32_t ahi = 0;
        int32_t A = bezier_A;
        int32_t B = bezier_B;
        int32_t C = bezier_C;

         __asm__ __volatile__(
          ".syntax unified" "\n\t"              // is to prevent CM0,CM1 non-unified syntax
          A("lsrs  %[ahi],%[alo],#1")           // a  = F << 31      1 cycles
          A("lsls  %[alo],%[alo],#31")          //                   1 cycles
          A("umull %[flo],%[fhi],%[fhi],%[t]")  // f *= t            5 cycles [fhi:flo=64bits]
          A("umull %[flo],%[fhi],%[fhi],%[t]")  // f>>=32; f*=t      5 cycles [fhi:flo=64bits]
          A("lsrs  %[flo],%[fhi],#1")           //                   1 cycles [31bits]
          A("smlal %[alo],%[ahi],%[flo],%[C]")  // a+=(f>>33)*C;     5 cycles
          A("umull %[flo],%[fhi],%[fhi],%[t]")  // f>>=32; f*=t      5 cycles [fhi:flo=64bits]
          A("lsrs  %[flo],%[fhi],#1")           //                   1 cycles [31bits]
          A("smlal %[alo],%[ahi],%[flo],%[B]")  // a+=(f>>33)*B;     5 cycles
          A("umull %[flo],%[fhi],%[fhi],%[t]")  // f>>=32; f*=t      5 cycles [fhi:flo=64bits]
          A("lsrs  %[flo],%[fhi],#1")           // f>>=33;           1 cycles [31bits]
          A("smlal %[alo],%[ahi],%[flo],%[A]")  // a+=(f>>33)*A;     5 cycles
          A("lsrs  %[alo],%[ahi],#6")           // a>>=38            1 cycles
          : [alo]"+r"( alo ) ,
            [flo]"+r"( flo ) ,
            [fhi]"+r"( fhi ) ,
            [ahi]"+r"( ahi ) ,
            [A]"+r"( A ) ,  // <== Note: Even if A, B, C, and t registers are INPUT ONLY
            [B]"+r"( B ) ,  //  GCC does bad optimizations on the code if we list them as
            [C]"+r"( C ) ,  //  such, breaking this function. So, to avoid that problem,
            [t]"+r"( t )    //  we list all registers as input-outputs.
          :
          : "cc"
        );
        return alo;

      #else

        // For non ARM targets, we provide a fallback implementation. Really doubt it
        // will be useful, unless the processor is fast and 32bit

        uint32_t t = bezier_AV * curr_step;               // t: Range 0 - 1^32 = 32 bits
        uint64_t f = t;
        f *= t;                                           // Range 32*2 = 64 bits (unsigned)
        f >>= 32;                                         // Range 32 bits  (unsigned)
        f *= t;                                           // Range 32*2 = 64 bits  (unsigned)
        f >>= 32;                                         // Range 32 bits : f = t^3  (unsigned)
        int64_t acc = (int64_t) bezier_F << 31;           // Range 63 bits (signed)
        acc += ((uint32_t) f >> 1) * (int64_t) bezier_C;  // Range 29bits + 31 = 60bits (plus sign)
        f *= t;                                           // Range 32*2 = 64 bits
        f >>= 32;                                         // Range 32 bits : f = t^3  (unsigned)
        acc += ((uint32_t) f >> 1) * (int64_t) bezier_B;  // Range 29bits + 31 = 60bits (plus sign)
        f *= t;                                           // Range 32*2 = 64 bits
        f >>= 32;                                         // Range 32 bits : f = t^3  (unsigned)
        acc += ((uint32_t) f >> 1) * (int64_t) bezier_A;  // Range 28bits + 31 = 59bits (plus sign)
        acc >>= (31 + 7);                                 // Range 24bits (plus sign)
        return (int32_t) acc;

      #endif
    }

  #endif // !__AVR__

#endif // S_CURVE_ACCELERATION

/**
 * Stepper Driver Interrupt
 *
 * Directly pulses the stepper motors at high frequency.
 */

HAL_STEP_TIMER_ISR() {
  #ifndef __AVR__
    // Disable interrupts, to avoid ISR preemption while we reprogram the period
    // (AVR enters the ISR with global interrupts disabled, so no need to do it here)
    hal.isr_off();
  #endif

  HAL_timer_isr_prologue(MF_TIMER_STEP);

  Stepper::isr();

  HAL_timer_isr_epilogue(MF_TIMER_STEP);
}

#ifdef CPU_32_BIT
  #define STEP_MULTIPLY(A,B) MultiU32X24toH32(A, B)
#else
  #define STEP_MULTIPLY(A,B) MultiU24X32toH16(A, B)
#endif

#if ENABLED(SMOOTH_LIN_ADVANCE)
  FORCE_INLINE static constexpr int32_t MULT_Q(uint8_t q, int32_t x, int32_t y) { return (int64_t(x) * y) >> q; }
#endif

void Stepper::isr() {

  static hal_timer_t nextMainISR = 0;  // Interval until the next main Stepper Pulse phase (0 = Now)

  #if ENABLED(SMOOTH_LIN_ADVANCE)
    static hal_timer_t smoothLinAdvISR = 0;
  #endif

  // Program timer compare for the maximum period, so it does NOT
  // flag an interrupt while this ISR is running - So changes from small
  // periods to big periods are respected and the timer does not reset to 0
  HAL_timer_set_compare(MF_TIMER_STEP, hal_timer_t(HAL_TIMER_TYPE_MAX));

  // Count of ticks for the next ISR
  hal_timer_t next_isr_ticks = 0;

  // Limit the amount of iterations
  uint8_t max_loops = 10;

  #if ENABLED(FT_MOTION)
    static uint32_t ftMotion_nextAuxISR = 0U;  // Storage for the next ISR of the auxiliary tasks.
    const bool using_ftMotion = ftMotion.cfg.active;
  #else
    constexpr bool using_ftMotion = false;
  #endif

  // We need this variable here to be able to use it in the following loop
  hal_timer_t min_ticks;
  do {

    hal_timer_t interval = 0;

    #if ENABLED(FT_MOTION)

      if (using_ftMotion) {
        ftMotion_stepper();             // Run FTM Stepping

        // Define 2.5 msec task for auxiliary functions.
        if (!ftMotion_nextAuxISR) {
          TERN_(BABYSTEPPING, if (babystep.has_steps()) babystepping_isr());
          ftMotion_nextAuxISR = (STEPPER_TIMER_RATE) / 400;
        }

        // Enable ISRs to reduce latency for higher priority ISRs
        hal.isr_on();

        interval = FTM_MIN_TICKS;
        ftMotion_nextAuxISR -= interval;
      }

    #endif

    if (!using_ftMotion) {

      TERN_(HAS_ZV_SHAPING, shaping_isr());               // Do Shaper stepping, if needed

      if (!nextMainISR) pulse_phase_isr();                // 0 = Do coordinated axes Stepper pulses

      #if ENABLED(LIN_ADVANCE)
        if (!nextAdvanceISR) {                            // 0 = Do Linear Advance E Stepper pulses
          advance_isr();
          nextAdvanceISR = la_interval;
        }
        else if (nextAdvanceISR > la_interval)            // Start/accelerate LA steps if necessary
          nextAdvanceISR = la_interval;
      #endif

      #if ENABLED(BABYSTEPPING)
        const bool is_babystep = (nextBabystepISR == 0);  // 0 = Do Babystepping (XY)Z pulses
        if (is_babystep) nextBabystepISR = babystepping_isr();
      #endif

      // Enable ISRs to reduce latency for higher priority ISRs, or all ISRs if no prioritization.
      hal.isr_on();

      // ^== Time critical. NOTHING besides pulse generation should be above here!!!

      if (!nextMainISR) nextMainISR = block_phase_isr();  // Manage acc/deceleration, get next block
      #if ENABLED(SMOOTH_LIN_ADVANCE)
        if (!smoothLinAdvISR) smoothLinAdvISR = smooth_lin_adv_isr();  // Manage la
      #endif

      #if ENABLED(BABYSTEPPING)
        if (is_babystep)                                  // Avoid ANY stepping too soon after baby-stepping
          NOLESS(nextMainISR, (BABYSTEP_TICKS) / 8);      // FULL STOP for 125µs after a baby-step

        if (nextBabystepISR != BABYSTEP_NEVER)            // Avoid baby-stepping too close to axis Stepping
          NOLESS(nextBabystepISR, nextMainISR / 2);       // TODO: Only look at axes enabled for baby-stepping
      #endif

      // Get the interval to the next ISR call
      interval = _MIN(nextMainISR, uint32_t(HAL_TIMER_TYPE_MAX));         // Time until the next Pulse / Block phase
      TERN_(INPUT_SHAPING_X, NOMORE(interval, ShapingQueue::peek_x()));   // Time until next input shaping echo for X
      TERN_(INPUT_SHAPING_Y, NOMORE(interval, ShapingQueue::peek_y()));   // Time until next input shaping echo for Y
      TERN_(INPUT_SHAPING_Z, NOMORE(interval, ShapingQueue::peek_z()));   // Time until next input shaping echo for Z
      TERN_(LIN_ADVANCE, NOMORE(interval, nextAdvanceISR));               // Come back early for Linear Advance?
      TERN_(SMOOTH_LIN_ADVANCE, NOMORE(interval, smoothLinAdvISR));       // Come back early for Linear Advance rate update?
      TERN_(BABYSTEPPING, NOMORE(interval, nextBabystepISR));             // Come back early for Babystepping?

      //
      // Compute remaining time for each ISR phase
      //     NEVER : The phase is idle
      //      Zero : The phase will occur on the next ISR call
      //  Non-zero : The phase will occur on a future ISR call
      //

      nextMainISR -= interval;
      TERN_(HAS_ZV_SHAPING, ShapingQueue::decrement_delays(interval));
      TERN_(LIN_ADVANCE, if (nextAdvanceISR != LA_ADV_NEVER) nextAdvanceISR -= interval);
      TERN_(SMOOTH_LIN_ADVANCE, if (smoothLinAdvISR != LA_ADV_NEVER) smoothLinAdvISR -= interval);
      TERN_(BABYSTEPPING, if (nextBabystepISR != BABYSTEP_NEVER) nextBabystepISR -= interval);

    } // standard motion control

    /**
     * This needs to avoid a race-condition caused by interleaving
     * of interrupts required by both the LA and Stepper algorithms.
     *
     * Assume the following tick times for stepper pulses:
     *   Stepper ISR (S):  1 1000 2000 3000 4000
     *   Linear Adv. (E): 10 1010 2010 3010 4010
     *
     * The current algorithm tries to interleave them, giving:
     *  1:S 10:E 1000:S 1010:E 2000:S 2010:E 3000:S 3010:E 4000:S 4010:E
     *
     * Ideal timing would yield these delta periods:
     *  1:S  9:E  990:S   10:E  990:S   10:E  990:S   10:E  990:S   10:E
     *
     * But, since each event must fire an ISR with a minimum duration, the
     * minimum delta might be 900, so deltas under 900 get rounded up:
     *  900:S d900:E d990:S d900:E d990:S d900:E d990:S d900:E d990:S d900:E
     *
     * It works, but divides the speed of all motors by half, leading to a sudden
     * reduction to 1/2 speed! Such jumps in speed lead to lost steps (not even
     * accounting for double/quad stepping, which makes it even worse).
     */

    // Compute the tick count for the next ISR
    next_isr_ticks += interval;

    /**
     * The following section must be done with global interrupts disabled.
     * We want nothing to interrupt it, as that could mess the calculations
     * we do for the next value to program in the period register of the
     * stepper timer and lead to skipped ISRs (if the value we happen to program
     * is less than the current count due to something preempting between the
     * read and the write of the new period value).
     */
    hal.isr_off();

    /**
     * Get the current tick value + margin
     * Assuming at least 6µs between calls to this ISR...
     * On AVR the ISR epilogue+prologue is estimated at 100 instructions - Give 8µs as margin
     * On ARM the ISR epilogue+prologue is estimated at 20 instructions - Give 1µs as margin
     */
    min_ticks = HAL_timer_get_count(MF_TIMER_STEP) + hal_timer_t(TERN(__AVR__, 8, 1) * (STEPPER_TIMER_TICKS_PER_US));

    #if ENABLED(OLD_ADAPTIVE_MULTISTEPPING)
      /**
       * NB: If for some reason the stepper monopolizes the MPU, eventually the
       * timer will wrap around (and so will 'next_isr_ticks'). So, limit the
       * loop to 10 iterations. Beyond that, there's no way to ensure correct pulse
       * timing, since the MCU isn't fast enough.
       */
      if (!--max_loops) next_isr_ticks = min_ticks;
    #endif

    // Advance pulses if not enough time to wait for the next ISR
  } while (TERN(OLD_ADAPTIVE_MULTISTEPPING, true, --max_loops) && next_isr_ticks < min_ticks);

  #if DISABLED(OLD_ADAPTIVE_MULTISTEPPING)

    // Track the time spent in the ISR
    const hal_timer_t time_spent = HAL_timer_get_count(MF_TIMER_STEP);
    time_spent_in_isr += time_spent;

    if (next_isr_ticks < min_ticks) {
      next_isr_ticks = min_ticks;

      // When forced out of the ISR, increase multi-stepping
      #if MULTISTEPPING_LIMIT > 1
        if (steps_per_isr < MULTISTEPPING_LIMIT) {
          steps_per_isr <<= 1;
          // ticks_nominal will need to be recalculated if we are in cruise phase
          ticks_nominal = 0;
        }
      #endif
    }
    else {
      // Track the time spent voluntarily outside the ISR
      time_spent_out_isr += next_isr_ticks;
      time_spent_out_isr -= time_spent;
    }

  #endif // !OLD_ADAPTIVE_MULTISTEPPING

  // Now 'next_isr_ticks' contains the period to the next Stepper ISR - And we are
  // sure that the time has not arrived yet - Warrantied by the scheduler

  // Set the next ISR to fire at the proper time
  HAL_timer_set_compare(MF_TIMER_STEP, next_isr_ticks);

  // Don't forget to finally reenable interrupts on non-AVR.
  // AVR automatically calls sei() for us on Return-from-Interrupt.
  #ifndef __AVR__
    hal.isr_on();
  #endif
}

#if MINIMUM_STEPPER_PULSE_NS || MAXIMUM_STEPPER_RATE
  #define ISR_PULSE_CONTROL 1
#endif
#if ISR_PULSE_CONTROL && MULTISTEPPING_LIMIT > 1 && DISABLED(I2S_STEPPER_STREAM)
  #define ISR_MULTI_STEPS 1
#endif

/**
 * This phase of the ISR should ONLY create the pulses for the steppers.
 * This prevents jitter caused by the interval between the start of the
 * interrupt and the start of the pulses. DON'T add any logic ahead of the
 * call to this method that might cause variation in the timing. The aim
 * is to keep pulse timing as regular as possible.
 */
void Stepper::pulse_phase_isr() {

  // If we must abort the current block, do so!
  if (abort_current_block) {
    abort_current_block = false;
    if (current_block) {
      discard_current_block();
      #if HAS_ZV_SHAPING
        ShapingQueue::purge();
        #if ENABLED(INPUT_SHAPING_X)
          shaping_x.delta_error = 0;
          shaping_x.last_block_end_pos = count_position.x;
        #endif
        #if ENABLED(INPUT_SHAPING_Y)
          shaping_y.delta_error = 0;
          shaping_y.last_block_end_pos = count_position.y;
        #endif
        #if ENABLED(INPUT_SHAPING_Z)
          shaping_z.delta_error = 0;
          shaping_z.last_block_end_pos = count_position.z;
        #endif
      #endif
    }
  }

  // If there is no current block, do nothing
  if (!current_block || step_events_completed >= step_event_count) return;

  // Skipping step processing causes motion to freeze
  if (TERN0(FREEZE_FEATURE, frozen)) return;

  // Count of pending loops and events for this iteration
  const uint32_t pending_events = step_event_count - step_events_completed;
  uint8_t events_to_do = _MIN(pending_events, steps_per_isr);

  // Just update the value we will get at the end of the loop
  step_events_completed += events_to_do;

  TERN_(ISR_PULSE_CONTROL, USING_TIMED_PULSE());

  // Take multiple steps per interrupt. For high speed moves.
  #if ENABLED(ISR_MULTI_STEPS)
    bool firstStep = true;
  #endif

  // Direct Stepping page?
  const bool is_page = current_block->is_page();

  do {
    AxisFlags step_needed{0};

    #define _APPLY_STEP(AXIS, STATE, ALWAYS) AXIS ##_APPLY_STEP(STATE, ALWAYS)
    #define _STEP_STATE(AXIS) STEP_STATE_## AXIS

    // Determine if a pulse is needed using Bresenham
    #define PULSE_PREP(AXIS) do{ \
      int32_t de = delta_error[_AXIS(AXIS)] + advance_dividend[_AXIS(AXIS)]; \
      if (de >= 0) { \
        step_needed.set(_AXIS(AXIS)); \
        de -= advance_divisor_cached; \
      } \
      delta_error[_AXIS(AXIS)] = de; \
    }while(0)

    // With input shaping, direction changes can happen with almost only
    // AWAIT_LOW_PULSE() and  DIR_WAIT_BEFORE() between steps. To work around
    // the TMC2208 / TMC2225 shutdown bug (#16076), add a half step hysteresis
    // in each direction. This results in the position being off by half an
    // average half step during travel but correct at the end of each segment.
    #if AXIS_DRIVER_TYPE_X(TMC2208) || AXIS_DRIVER_TYPE_X(TMC2208_STANDALONE) || \
        AXIS_DRIVER_TYPE_X(TMC5160) || AXIS_DRIVER_TYPE_X(TMC5160_STANDALONE)
      #define HYSTERESIS_X 64
    #else
      #define HYSTERESIS_X 0
    #endif
    #if AXIS_DRIVER_TYPE_Y(TMC2208) || AXIS_DRIVER_TYPE_Y(TMC2208_STANDALONE) || \
        AXIS_DRIVER_TYPE_Y(TMC5160) || AXIS_DRIVER_TYPE_Y(TMC5160_STANDALONE)
      #define HYSTERESIS_Y 64
    #else
      #define HYSTERESIS_Y 0
    #endif
    #if AXIS_DRIVER_TYPE_Z(TMC2208) || AXIS_DRIVER_TYPE_Z(TMC2208_STANDALONE) || \
        AXIS_DRIVER_TYPE_Z(TMC5160) || AXIS_DRIVER_TYPE_Z(TMC5160_STANDALONE)
      #define HYSTERESIS_Z 64
    #else
      #define HYSTERESIS_Z 0
    #endif
    #define _HYSTERESIS(AXIS) HYSTERESIS_##AXIS
    #define HYSTERESIS(AXIS) _HYSTERESIS(AXIS)

    #define PULSE_PREP_SHAPING(AXIS, DELTA_ERROR, DIVIDEND) do{ \
      int16_t de = DELTA_ERROR + (DIVIDEND); \
      const bool step_fwd = de >=  (64 + HYSTERESIS(AXIS)), \
                 step_bak = de <= -(64 + HYSTERESIS(AXIS)); \
      if (step_fwd || step_bak) { \
        de += step_fwd ? -128 : 128; \
        if ((MAXDIR(AXIS) && step_bak) || (MINDIR(AXIS) && step_fwd)) { \
          { USING_TIMED_PULSE(); START_TIMED_PULSE(); AWAIT_LOW_PULSE(); } \
          last_direction_bits.toggle(_AXIS(AXIS)); \
          DIR_WAIT_BEFORE(); \
          SET_STEP_DIR(AXIS); \
          TERN_(FT_MOTION, last_set_direction = last_direction_bits); \
          DIR_WAIT_AFTER(); \
        } \
      } \
      else \
        step_needed.clear(_AXIS(AXIS)); \
      DELTA_ERROR = de; \
    }while(0)

    // Start an active pulse if needed
    #define PULSE_START(AXIS) do{ \
      if (step_needed.test(_AXIS(AXIS))) { \
        count_position[_AXIS(AXIS)] += count_direction[_AXIS(AXIS)]; \
        _APPLY_STEP(AXIS, _STEP_STATE(AXIS), 0); \
      } \
    }while(0)

    // Stop an active pulse if needed
    #define PULSE_STOP(AXIS) do { \
      if (step_needed.test(_AXIS(AXIS))) { \
        _APPLY_STEP(AXIS, !_STEP_STATE(AXIS), 0); \
      } \
    }while(0)

    #if ENABLED(DIRECT_STEPPING)
      // Direct stepping is currently not ready for HAS_I_AXIS
      if (is_page) {

        #if STEPPER_PAGE_FORMAT == SP_4x4D_128

          #define PAGE_SEGMENT_UPDATE(AXIS, VALUE) do{      \
                 if ((VALUE) <  7) dm[_AXIS(AXIS)] = false; \
            else if ((VALUE) >  7) dm[_AXIS(AXIS)] = true;  \
            page_step_state.sd[_AXIS(AXIS)] = VALUE;        \
            page_step_state.bd[_AXIS(AXIS)] += VALUE;       \
          }while(0)

          #define PAGE_PULSE_PREP(AXIS) do{ \
            step_needed.set(_AXIS(AXIS), \
              pgm_read_byte(&segment_table[page_step_state.sd[_AXIS(AXIS)]][page_step_state.segment_steps & 0x7])); \
          }while(0)

          switch (page_step_state.segment_steps) {
            case DirectStepping::Config::SEGMENT_STEPS:
              page_step_state.segment_idx += 2;
              page_step_state.segment_steps = 0;
              // fallthru
            case 0: {
              const uint8_t low = page_step_state.page[page_step_state.segment_idx],
                           high = page_step_state.page[page_step_state.segment_idx + 1];
              const AxisBits dm = last_direction_bits;

              PAGE_SEGMENT_UPDATE(X, low >> 4);
              PAGE_SEGMENT_UPDATE(Y, low & 0xF);
              PAGE_SEGMENT_UPDATE(Z, high >> 4);
              PAGE_SEGMENT_UPDATE(E, high & 0xF);

              if (dm != last_direction_bits) set_directions(dm);

            } break;

            default: break;
          }

          PAGE_PULSE_PREP(X);
          PAGE_PULSE_PREP(Y);
          PAGE_PULSE_PREP(Z);
          TERN_(HAS_EXTRUDERS, PAGE_PULSE_PREP(E));

          page_step_state.segment_steps++;

        #elif STEPPER_PAGE_FORMAT == SP_4x2_256

          #define PAGE_SEGMENT_UPDATE(AXIS, VALUE) \
            page_step_state.sd[_AXIS(AXIS)] = VALUE; \
            page_step_state.bd[_AXIS(AXIS)] += VALUE;

          #define PAGE_PULSE_PREP(AXIS) do{ \
            step_needed.set(_AXIS(AXIS), \
              pgm_read_byte(&segment_table[page_step_state.sd[_AXIS(AXIS)]][page_step_state.segment_steps & 0x3])); \
          }while(0)

          switch (page_step_state.segment_steps) {
            case DirectStepping::Config::SEGMENT_STEPS:
              page_step_state.segment_idx++;
              page_step_state.segment_steps = 0;
              // fallthru
            case 0: {
              const uint8_t b = page_step_state.page[page_step_state.segment_idx];
              PAGE_SEGMENT_UPDATE(X, (b >> 6) & 0x3);
              PAGE_SEGMENT_UPDATE(Y, (b >> 4) & 0x3);
              PAGE_SEGMENT_UPDATE(Z, (b >> 2) & 0x3);
              PAGE_SEGMENT_UPDATE(E, (b >> 0) & 0x3);
            } break;
            default: break;
          }

          PAGE_PULSE_PREP(X);
          PAGE_PULSE_PREP(Y);
          PAGE_PULSE_PREP(Z);
          TERN_(HAS_EXTRUDERS, PAGE_PULSE_PREP(E));

          page_step_state.segment_steps++;

        #elif STEPPER_PAGE_FORMAT == SP_4x1_512

          #define PAGE_PULSE_PREP(AXIS, NBIT) do{            \
            step_needed.set(_AXIS(AXIS), TEST(steps, NBIT)); \
            if (step_needed.test(_AXIS(AXIS)))               \
              page_step_state.bd[_AXIS(AXIS)]++;             \
          }while(0)

          uint8_t steps = page_step_state.page[page_step_state.segment_idx >> 1];
          if (page_step_state.segment_idx & 0x1) steps >>= 4;

          PAGE_PULSE_PREP(X, 3);
          PAGE_PULSE_PREP(Y, 2);
          PAGE_PULSE_PREP(Z, 1);
          PAGE_PULSE_PREP(E, 0);

          page_step_state.segment_idx++;

        #else
          #error "Unknown direct stepping page format!"
        #endif
      }

    #endif // DIRECT_STEPPING

    if (!is_page) {
      // Give the compiler a clue to store advance_divisor in registers for what follows
      const uint32_t advance_divisor_cached = advance_divisor;

      // Determine if pulses are needed
      #if HAS_X_STEP
        PULSE_PREP(X);
      #endif
      #if HAS_Y_STEP
        PULSE_PREP(Y);
      #endif
      #if HAS_Z_STEP
        PULSE_PREP(Z);
      #endif
      #if HAS_I_STEP
        PULSE_PREP(I);
      #endif
      #if HAS_J_STEP
        PULSE_PREP(J);
      #endif
      #if HAS_K_STEP
        PULSE_PREP(K);
      #endif
      #if HAS_U_STEP
        PULSE_PREP(U);
      #endif
      #if HAS_V_STEP
        PULSE_PREP(V);
      #endif
      #if HAS_W_STEP
        PULSE_PREP(W);
      #endif

      #if ANY(HAS_E0_STEP, MIXING_EXTRUDER)
        PULSE_PREP(E);
      #endif

      #if HAS_ROUGH_LIN_ADVANCE
        if (la_active && step_needed.e) {
          // don't actually step here, but do subtract movements steps
          // from the linear advance step count
          step_needed.e = false;
          la_advance_steps--;
        }
      #elif ENABLED(SMOOTH_LIN_ADVANCE)
        // Extruder steps are exclusively managed by the LA isr
        step_needed.e = false;
      #endif

      #if HAS_ZV_SHAPING
        // record an echo if a step is needed in the primary bresenham
        const bool x_step = TERN0(INPUT_SHAPING_X, step_needed.x && shaping_x.enabled),
                   y_step = TERN0(INPUT_SHAPING_Y, step_needed.y && shaping_y.enabled),
                   z_step = TERN0(INPUT_SHAPING_Z, step_needed.z && shaping_z.enabled);
        if (x_step || y_step || z_step)
          ShapingQueue::enqueue(x_step, TERN0(INPUT_SHAPING_X, shaping_x.forward), y_step, TERN0(INPUT_SHAPING_Y, shaping_y.forward), z_step, TERN0(INPUT_SHAPING_Z, shaping_z.forward));

        // do the first part of the secondary bresenham
        #if ENABLED(INPUT_SHAPING_X)
          if (x_step)
            PULSE_PREP_SHAPING(X, shaping_x.delta_error, shaping_x.forward ? shaping_x.factor1 : -shaping_x.factor1);
        #endif
        #if ENABLED(INPUT_SHAPING_Y)
          if (y_step)
            PULSE_PREP_SHAPING(Y, shaping_y.delta_error, shaping_y.forward ? shaping_y.factor1 : -shaping_y.factor1);
        #endif
        #if ENABLED(INPUT_SHAPING_Z)
          if (z_step)
            PULSE_PREP_SHAPING(Z, shaping_z.delta_error, shaping_z.forward ? shaping_z.factor1 : -shaping_z.factor1);
        #endif
      #endif
    }

    #if ISR_MULTI_STEPS
      if (firstStep)
        firstStep = false;
      else
        AWAIT_LOW_PULSE();
    #endif

    // Pulse start
    #if HAS_X_STEP
      PULSE_START(X);
    #endif
    #if HAS_Y_STEP
      PULSE_START(Y);
    #endif
    #if HAS_Z_STEP
      PULSE_START(Z);
    #endif
    #if HAS_I_STEP
      PULSE_START(I);
    #endif
    #if HAS_J_STEP
      PULSE_START(J);
    #endif
    #if HAS_K_STEP
      PULSE_START(K);
    #endif
    #if HAS_U_STEP
      PULSE_START(U);
    #endif
    #if HAS_V_STEP
      PULSE_START(V);
    #endif
    #if HAS_W_STEP
      PULSE_START(W);
    #endif

    #if ENABLED(MIXING_EXTRUDER)
      if (step_needed.e) {
        count_position.e += count_direction.e;
        E_STEP_WRITE(mixer.get_next_stepper(), STEP_STATE_E);
      }
    #elif HAS_E0_STEP
      PULSE_START(E);
    #endif

    TERN_(I2S_STEPPER_STREAM, i2s_push_sample());

    // TODO: need to deal with MINIMUM_STEPPER_PULSE_NS over i2s
    #if ISR_PULSE_CONTROL
      START_TIMED_PULSE();
      AWAIT_HIGH_PULSE();
    #endif

    // Pulse stop
    #if HAS_X_STEP
      PULSE_STOP(X);
    #endif
    #if HAS_Y_STEP
      PULSE_STOP(Y);
    #endif
    #if HAS_Z_STEP
      PULSE_STOP(Z);
    #endif
    #if HAS_I_STEP
      PULSE_STOP(I);
    #endif
    #if HAS_J_STEP
      PULSE_STOP(J);
    #endif
    #if HAS_K_STEP
      PULSE_STOP(K);
    #endif
    #if HAS_U_STEP
      PULSE_STOP(U);
    #endif
    #if HAS_V_STEP
      PULSE_STOP(V);
    #endif
    #if HAS_W_STEP
      PULSE_STOP(W);
    #endif

    #if ENABLED(MIXING_EXTRUDER)
      if (step_needed.e) E_STEP_WRITE(mixer.get_stepper(), !STEP_STATE_E);
    #elif HAS_E0_STEP
      PULSE_STOP(E);
    #endif

    #if ISR_MULTI_STEPS
      if (events_to_do) START_TIMED_PULSE();
    #endif

  } while (--events_to_do);
}

#if HAS_ZV_SHAPING

  void Stepper::shaping_isr() {
    AxisFlags step_needed{0};

    // Clear the echoes that are ready to process. If the buffers are too full and risk overflow, also apply echoes early.
    TERN_(INPUT_SHAPING_X, step_needed.x = !ShapingQueue::peek_x() || ShapingQueue::free_count_x() < steps_per_isr);
    TERN_(INPUT_SHAPING_Y, step_needed.y = !ShapingQueue::peek_y() || ShapingQueue::free_count_y() < steps_per_isr);
    TERN_(INPUT_SHAPING_Z, step_needed.z = !ShapingQueue::peek_z() || ShapingQueue::free_count_z() < steps_per_isr);

    if (bool(step_needed)) while (true) {
      #if ENABLED(INPUT_SHAPING_X)
        if (step_needed.x) {
          const bool forward = ShapingQueue::dequeue_x();
          PULSE_PREP_SHAPING(X, shaping_x.delta_error, (forward ? shaping_x.factor2 : -shaping_x.factor2));
          PULSE_START(X);
        }
      #endif

      #if ENABLED(INPUT_SHAPING_Y)
        if (step_needed.y) {
          const bool forward = ShapingQueue::dequeue_y();
          PULSE_PREP_SHAPING(Y, shaping_y.delta_error, (forward ? shaping_y.factor2 : -shaping_y.factor2));
          PULSE_START(Y);
        }
      #endif

      #if ENABLED(INPUT_SHAPING_Z)
        if (step_needed.z) {
          const bool forward = ShapingQueue::dequeue_z();
          PULSE_PREP_SHAPING(Z, shaping_z.delta_error, (forward ? shaping_z.factor2 : -shaping_z.factor2));
          PULSE_START(Z);
        }
      #endif

      TERN_(I2S_STEPPER_STREAM, i2s_push_sample());

      USING_TIMED_PULSE();
      if (bool(step_needed)) {
        #if ISR_MULTI_STEPS
          START_TIMED_PULSE();
          AWAIT_HIGH_PULSE();
        #endif
        #if ENABLED(INPUT_SHAPING_X)
          PULSE_STOP(X);
        #endif
        #if ENABLED(INPUT_SHAPING_Y)
          PULSE_STOP(Y);
        #endif
        #if ENABLED(INPUT_SHAPING_Z)
          PULSE_STOP(Z);
        #endif
      }

      TERN_(INPUT_SHAPING_X, step_needed.x = !ShapingQueue::peek_x() || ShapingQueue::free_count_x() < steps_per_isr);
      TERN_(INPUT_SHAPING_Y, step_needed.y = !ShapingQueue::peek_y() || ShapingQueue::free_count_y() < steps_per_isr);
      TERN_(INPUT_SHAPING_Z, step_needed.z = !ShapingQueue::peek_z() || ShapingQueue::free_count_z() < steps_per_isr);

      if (!bool(step_needed)) break;

      START_TIMED_PULSE();
      AWAIT_LOW_PULSE();
    }
  }

#endif // HAS_ZV_SHAPING

// Calculate timer interval, with all limits applied.
hal_timer_t Stepper::calc_timer_interval(uint32_t step_rate) {

  #ifdef CPU_32_BIT

    // A fast processor can just do integer division
    return step_rate > minimal_step_rate ? uint32_t(STEPPER_TIMER_RATE) / step_rate : HAL_TIMER_TYPE_MAX;

  #else

    if (step_rate >= 0x0800) {  // higher step rate
      // AVR is able to keep up at around 65kHz Stepping ISR rate at most.
      // So values for step_rate > 65535 might as well be truncated.
      // Handle it as quickly as possible. i.e., assume highest byte is zero
      // because non-zero would represent a step rate far beyond AVR capabilities.
      if (uint8_t(step_rate >> 16))
        return uint32_t(STEPPER_TIMER_RATE) / 0x10000;

      const uintptr_t table_address = uintptr_t(&speed_lookuptable_fast[uint8_t(step_rate >> 8)]);
      const uint16_t base = uint16_t(pgm_read_word(table_address));
      const uint8_t gain = uint8_t(pgm_read_byte(table_address + 2));
      return base - MultiU8X8toH8(uint8_t(step_rate & 0x00FF), gain);
    }
    else if (step_rate > minimal_step_rate) { // lower step rates
      step_rate -= minimal_step_rate; // Correct for minimal speed
      const uintptr_t table_address = uintptr_t(&speed_lookuptable_slow[uint8_t(step_rate >> 3)]);
      return uint16_t(pgm_read_word(table_address))
             - ((uint16_t(pgm_read_word(table_address + 2)) * uint8_t(step_rate & 0x0007)) >> 3);
    }

    return uint16_t(pgm_read_word(uintptr_t(speed_lookuptable_slow)));

  #endif // !CPU_32_BIT
}

#if NONLINEAR_EXTRUSION_Q24
  void Stepper::calc_nonlinear_e(const uint32_t step_rate) {
    const uint32_t velocity_q24 = ne.scale_q24 * step_rate; // Scale step_rate first so all intermediate values stay in range of 8.24 fixed point math
    int32_t vd_q24 = ((((int64_t(ne.q24.A) * velocity_q24) >> 24) * velocity_q24) >> 24) + ((int64_t(ne.q24.B) * velocity_q24) >> 24);
    NOLESS(vd_q24, 0);

    advance_dividend.e = (uint64_t(ne.q24.C + vd_q24) * ne.edividend) >> 24;
  }
#endif

// Get the timer interval and the number of loops to perform per tick
hal_timer_t Stepper::calc_multistep_timer_interval(uint32_t step_rate) {

  #if ENABLED(OLD_ADAPTIVE_MULTISTEPPING)

    #if MULTISTEPPING_LIMIT == 1

      // Just make sure the step rate is doable
      NOMORE(step_rate, uint32_t(MAX_STEP_ISR_FREQUENCY_1X));

    #else

      // The stepping frequency limits for each multistepping rate
      static const uint32_t limit[] PROGMEM = {
            max_step_isr_frequency_sh(0)
          , max_step_isr_frequency_sh(1)
        #if MULTISTEPPING_LIMIT >= 4
          , max_step_isr_frequency_sh(2)
        #endif
        #if MULTISTEPPING_LIMIT >= 8
          , max_step_isr_frequency_sh(3)
        #endif
        #if MULTISTEPPING_LIMIT >= 16
          , max_step_isr_frequency_sh(4)
        #endif
        #if MULTISTEPPING_LIMIT >= 32
          , max_step_isr_frequency_sh(5)
        #endif
        #if MULTISTEPPING_LIMIT >= 64
          , max_step_isr_frequency_sh(6)
        #endif
        #if MULTISTEPPING_LIMIT >= 128
          , max_step_isr_frequency_sh(7)
        #endif
      };

      // Find a doable step rate using multistepping
      uint8_t multistep = 1;
      for (uint8_t i = 0; i < COUNT(limit) && step_rate > uint32_t(pgm_read_dword(&limit[i])); ++i) {
        step_rate >>= 1;
        multistep <<= 1;
      }
      steps_per_isr = multistep;

    #endif

  #elif MULTISTEPPING_LIMIT > 1

    uint8_t loops = steps_per_isr;
    if (MULTISTEPPING_LIMIT >= 16 && loops >= 16) { step_rate >>= 4; loops >>= 4; }
    if (MULTISTEPPING_LIMIT >=  4 && loops >=  4) { step_rate >>= 2; loops >>= 2; }
    if (MULTISTEPPING_LIMIT >=  2 && loops >=  2) { step_rate >>= 1; }

  #endif

  return calc_timer_interval(step_rate);
}

// Method to get all moving axes (for proper endstop handling)
void Stepper::set_axis_moved_for_current_block() {

  #if IS_CORE
    // Define conditions for checking endstops
    #define S_(N) current_block->steps[CORE_AXIS_##N]
    #define D_(N) current_block->direction_bits[CORE_AXIS_##N]
  #endif

  #if CORE_IS_XY || CORE_IS_XZ
    /**
     * Head direction in -X axis for CoreXY and CoreXZ bots.
     *
     * If steps differ, both axes are moving.
     * If DeltaA == -DeltaB, the movement is only in the 2nd axis (Y or Z, handled below)
     * If DeltaA ==  DeltaB, the movement is only in the 1st axis (X)
     */
    #if ANY(COREXY, COREXZ)
      #define X_CMP(A,B) ((A)==(B))
    #else
      #define X_CMP(A,B) ((A)!=(B))
    #endif
    #define X_MOVE_TEST ( S_(1) != S_(2) || (S_(1) > 0 && X_CMP(D_(1),D_(2))) )
  #elif ENABLED(MARKFORGED_XY)
    #define X_MOVE_TEST (current_block->steps.a != current_block->steps.b)
  #else
    #define X_MOVE_TEST !!current_block->steps.a
  #endif

  #if CORE_IS_XY || CORE_IS_YZ
    /**
     * Head direction in -Y axis for CoreXY / CoreYZ bots.
     *
     * If steps differ, both axes are moving
     * If DeltaA ==  DeltaB, the movement is only in the 1st axis (X or Y)
     * If DeltaA == -DeltaB, the movement is only in the 2nd axis (Y or Z)
     */
    #if ANY(COREYX, COREYZ)
      #define Y_CMP(A,B) ((A)==(B))
    #else
      #define Y_CMP(A,B) ((A)!=(B))
    #endif
    #define Y_MOVE_TEST ( S_(1) != S_(2) || (S_(1) > 0 && Y_CMP(D_(1),D_(2))) )
  #elif ENABLED(MARKFORGED_YX)
    #define Y_MOVE_TEST (current_block->steps.a != current_block->steps.b)
  #else
    #define Y_MOVE_TEST !!current_block->steps.b
  #endif

  #if CORE_IS_XZ || CORE_IS_YZ
    /**
     * Head direction in -Z axis for CoreXZ or CoreYZ bots.
     *
     * If steps differ, both axes are moving
     * If DeltaA ==  DeltaB, the movement is only in the 1st axis (X or Y, already handled above)
     * If DeltaA == -DeltaB, the movement is only in the 2nd axis (Z)
     */
    #if ANY(COREZX, COREZY)
      #define Z_CMP(A,B) ((A)==(B))
    #else
      #define Z_CMP(A,B) ((A)!=(B))
    #endif
    #define Z_MOVE_TEST ( S_(1) != S_(2) || (S_(1) > 0 && Z_CMP(D_(1),D_(2))) )
  #else
    #define Z_MOVE_TEST !!current_block->steps.c
  #endif

  // Set flags for all axes that move in this block
  // These are set per-axis, not per-stepper
  AxisBits didmove;
  NUM_AXIS_CODE(
    if (X_MOVE_TEST)              didmove.a = true, // Cartesian X or Kinematic A
    if (Y_MOVE_TEST)              didmove.b = true, // Cartesian Y or Kinematic B
    if (Z_MOVE_TEST)              didmove.c = true, // Cartesian Z or Kinematic C
    if (!!current_block->steps.i) didmove.i = true,
    if (!!current_block->steps.j) didmove.j = true,
    if (!!current_block->steps.k) didmove.k = true,
    if (!!current_block->steps.u) didmove.u = true,
    if (!!current_block->steps.v) didmove.v = true,
    if (!!current_block->steps.w) didmove.w = true
  );
  axis_did_move = didmove;
}

/**
 * This last phase of the stepper interrupt processes and properly
 * schedules planner blocks. This is executed after the step pulses
 * have been done, so it is less time critical.
 */
hal_timer_t Stepper::block_phase_isr() {
  #if DISABLED(OLD_ADAPTIVE_MULTISTEPPING)
    // If the ISR uses < 50% of MPU time, halve multi-stepping
    const hal_timer_t time_spent = HAL_timer_get_count(MF_TIMER_STEP);
    #if MULTISTEPPING_LIMIT > 1
      if (steps_per_isr > 1 && time_spent_out_isr >= time_spent_in_isr + time_spent) {
        steps_per_isr >>= 1;
        // ticks_nominal will need to be recalculated if we are in cruise phase
        ticks_nominal = 0;
      }
    #endif
    time_spent_in_isr = -time_spent;    // unsigned but guaranteed to be +ve when needed
    time_spent_out_isr = 0;
  #endif

  // If no queued movements, just wait 1ms for the next block
  hal_timer_t interval = (STEPPER_TIMER_RATE) / 1000UL;

  // If there is a current block
  if (current_block) {
    // If current block is finished, reset pointer and finalize state
    if (step_events_completed >= step_event_count) {
      #if ENABLED(DIRECT_STEPPING)
        // Direct stepping is currently not ready for HAS_I_AXIS
        #if STEPPER_PAGE_FORMAT == SP_4x4D_128
          #define PAGE_SEGMENT_UPDATE_POS(AXIS) \
            count_position[_AXIS(AXIS)] += page_step_state.bd[_AXIS(AXIS)] - 128 * 7;
        #elif STEPPER_PAGE_FORMAT == SP_4x1_512 || STEPPER_PAGE_FORMAT == SP_4x2_256
          #define PAGE_SEGMENT_UPDATE_POS(AXIS) \
            count_position[_AXIS(AXIS)] += page_step_state.bd[_AXIS(AXIS)] * count_direction[_AXIS(AXIS)];
        #endif

        if (current_block->is_page()) {
          PAGE_SEGMENT_UPDATE_POS(X);
          PAGE_SEGMENT_UPDATE_POS(Y);
          PAGE_SEGMENT_UPDATE_POS(Z);
          PAGE_SEGMENT_UPDATE_POS(E);
        }
      #endif
      TERN_(HAS_FILAMENT_RUNOUT_DISTANCE, runout.block_completed(current_block));
      discard_current_block();
    }
    else {
      // Step events not completed yet...

      // Are we in acceleration phase ?
      if (step_events_completed < accelerate_before) { // Calculate new timer value

        #if ENABLED(S_CURVE_ACCELERATION)
          // Get the next speed to use (Jerk limited!)
          uint32_t acc_step_rate = acceleration_time < current_block->acceleration_time
                                   ? _eval_bezier_curve(acceleration_time)
                                   : current_block->cruise_rate;
        #else
          acc_step_rate = STEP_MULTIPLY(acceleration_time, current_block->acceleration_rate) + current_block->initial_rate;
          NOMORE(acc_step_rate, current_block->nominal_rate);
        #endif

        // acc_step_rate is in steps/second

        // step_rate to timer interval and steps per stepper isr
        interval = calc_multistep_timer_interval(acc_step_rate << oversampling_factor);
        acceleration_time += interval;
        deceleration_time = 0; // Reset since we're doing acceleration first.

        calc_nonlinear_e(acc_step_rate << oversampling_factor);

        #if HAS_ROUGH_LIN_ADVANCE
          if (la_active) {
            const uint32_t la_step_rate = la_advance_steps < current_block->max_adv_steps ? current_block->la_advance_rate : 0;
            la_interval = calc_timer_interval((acc_step_rate + la_step_rate) >> current_block->la_scaling);
          }
        #endif

        /**
         * Adjust Laser Power - Accelerating
         *
         *  isPowered - True when a move is powered.
         *  isEnabled - laser power is active.
         *
         * Laser power variables are calulated and stored in this block by the planner code.
         *  trap_ramp_active_pwr - the active power in this block across accel or decel trap steps.
         *  trap_ramp_entry_incr - holds the precalculated value to increase the current power per accel step.
         */
        #if ENABLED(LASER_POWER_TRAP)
          if (cutter.cutter_mode == CUTTER_MODE_CONTINUOUS) {
            if (planner.laser_inline.status.isPowered && planner.laser_inline.status.isEnabled) {
              if (current_block->laser.trap_ramp_entry_incr > 0) {
                cutter.apply_power(current_block->laser.trap_ramp_active_pwr);
                current_block->laser.trap_ramp_active_pwr += current_block->laser.trap_ramp_entry_incr * steps_per_isr;
              }
            }
            // Not a powered move.
            else cutter.apply_power(0);
          }
        #endif
        TERN_(SMOOTH_LIN_ADVANCE, curr_step_rate = acc_step_rate;)
      }
      // Are we in Deceleration phase ?
      else if (step_events_completed >= decelerate_start) {
        uint32_t step_rate;

        #if ENABLED(S_CURVE_ACCELERATION)
          // If this is the 1st time we process the 2nd half of the trapezoid...
          if (!bezier_2nd_half) {
            // Initialize the Bézier speed curve
            _calc_bezier_curve_coeffs(current_block->cruise_rate, current_block->final_rate, current_block->deceleration_time_inverse);
            bezier_2nd_half = true;
          }
          // Calculate the next speed to use
          step_rate = deceleration_time < current_block->deceleration_time
            ? _eval_bezier_curve(deceleration_time)
            : current_block->final_rate;
        #else
          // Using the old trapezoidal control
          step_rate = STEP_MULTIPLY(deceleration_time, current_block->acceleration_rate);
          if (step_rate < acc_step_rate) {
            step_rate = acc_step_rate - step_rate;
            NOLESS(step_rate, current_block->final_rate);
          }
          else
            step_rate = current_block->final_rate;

        #endif

        // step_rate to timer interval and steps per stepper isr
        interval = calc_multistep_timer_interval(step_rate << oversampling_factor);
        deceleration_time += interval;

        calc_nonlinear_e(step_rate << oversampling_factor);

        #if HAS_ROUGH_LIN_ADVANCE
          if (la_active) {
            const uint32_t la_step_rate = la_advance_steps > current_block->final_adv_steps ? current_block->la_advance_rate : 0;
            if (la_step_rate != step_rate) {
              const bool forward_e = la_step_rate < step_rate;
              la_interval = calc_timer_interval((forward_e ? step_rate - la_step_rate : la_step_rate - step_rate) >> current_block->la_scaling);

              if (forward_e != motor_direction(E_AXIS)) {
                last_direction_bits.toggle(E_AXIS);
                count_direction.e = -count_direction.e;

                DIR_WAIT_BEFORE();

                E_APPLY_DIR(forward_e, false);

                TERN_(FT_MOTION, last_set_direction = last_direction_bits);

                DIR_WAIT_AFTER();
              }
            }
            else
              la_interval = LA_ADV_NEVER;
          }
        #endif // LIN_ADVANCE

        // Adjust Laser Power - Decelerating
        #if ENABLED(LASER_POWER_TRAP)
          if (cutter.cutter_mode == CUTTER_MODE_CONTINUOUS) {
            if (planner.laser_inline.status.isPowered && planner.laser_inline.status.isEnabled) {
              if (current_block->laser.trap_ramp_exit_decr > 0) {
                current_block->laser.trap_ramp_active_pwr -= current_block->laser.trap_ramp_exit_decr * steps_per_isr;
                cutter.apply_power(current_block->laser.trap_ramp_active_pwr);
              }
              // Not a powered move.
              else cutter.apply_power(0);
            }
          }
        #endif
        TERN_(SMOOTH_LIN_ADVANCE, curr_step_rate = step_rate;)
      }
      else {  // Must be in cruise phase otherwise

        // Calculate the ticks_nominal for this nominal speed, if not done yet
        if (ticks_nominal == 0) {
          // step_rate to timer interval and loops for the nominal speed
          ticks_nominal = calc_multistep_timer_interval(current_block->nominal_rate << oversampling_factor);
          // Prepare for deceleration
          IF_DISABLED(S_CURVE_ACCELERATION, acc_step_rate = current_block->nominal_rate);
          TERN_(SMOOTH_LIN_ADVANCE, curr_step_rate = current_block->nominal_rate;)
          deceleration_time = ticks_nominal / 2;

          calc_nonlinear_e(current_block->nominal_rate << oversampling_factor);

          #if HAS_ROUGH_LIN_ADVANCE
            if (la_active)
              la_interval = calc_timer_interval(current_block->nominal_rate >> current_block->la_scaling);
          #endif

          // Adjust Laser Power - Cruise
          #if ENABLED(LASER_POWER_TRAP)
            if (cutter.cutter_mode == CUTTER_MODE_CONTINUOUS) {
              if (planner.laser_inline.status.isPowered && planner.laser_inline.status.isEnabled) {
                if (current_block->laser.trap_ramp_entry_incr > 0) {
                  current_block->laser.trap_ramp_active_pwr = current_block->laser.power;
                  cutter.apply_power(current_block->laser.power);
                }
              }
              // Not a powered move.
              else cutter.apply_power(0);
            }
          #endif
        }

        // The timer interval is just the nominal value for the nominal speed
        interval = ticks_nominal;
      }
    }

    #if ENABLED(LASER_FEATURE)
      /**
       * CUTTER_MODE_DYNAMIC is experimental and developing.
       * Super-fast method to dynamically adjust the laser power OCR value based on the input feedrate in mm-per-minute.
       * TODO: Set up Min/Max OCR offsets to allow tuning and scaling of various lasers.
       * TODO: Integrate accel/decel +-rate into the dynamic laser power calc.
       */
      if (cutter.cutter_mode == CUTTER_MODE_DYNAMIC
        && planner.laser_inline.status.isPowered                  // isPowered flag set on any parsed G1, G2, G3, or G5 move; cleared on any others.
        && current_block                                          // Block may not be available if steps completed (see discard_current_block() above)
        && cutter.last_block_power != current_block->laser.power  // Prevent constant update without change
      ) {
        cutter.apply_power(current_block->laser.power);
        cutter.last_block_power = current_block->laser.power;
      }
    #endif
  }
  else { // !current_block
    #if ENABLED(LASER_FEATURE)
      if (cutter.cutter_mode == CUTTER_MODE_DYNAMIC)
        cutter.apply_power(0);  // No movement in dynamic mode so turn Laser off
    #endif
  }

  // If there is no current block at this point, attempt to pop one from the buffer
  // and prepare its movement
  if (!current_block) {

    // Anything in the buffer?
    if ((current_block = planner.get_current_block())) {

      // Run through all sync blocks
      while (current_block->is_sync()) {

        // Set laser power
        #if ENABLED(LASER_POWER_SYNC)
          if (cutter.cutter_mode == CUTTER_MODE_CONTINUOUS) {
            if (current_block->is_sync_pwr()) {
              planner.laser_inline.status.isSyncPower = true;
              cutter.apply_power(current_block->laser.power);
            }
          }
        #endif

        // Set "fan speeds" for a laser module
        #if ENABLED(LASER_SYNCHRONOUS_M106_M107)
          if (current_block->is_sync_fan()) planner.sync_fan_speeds(current_block->fan_speed);
        #endif

        // Set position
        if (current_block->is_sync_pos()) _set_position(current_block->position);

        // Done with this block
        discard_current_block();

        // Try to get a new block. Exit if there are no more.
        if (!(current_block = planner.get_current_block()))
          return interval; // No more queued movements!
      }

      // For non-inline cutter, grossly apply power
      #if HAS_CUTTER
        if (cutter.cutter_mode == CUTTER_MODE_STANDARD) {
          cutter.apply_power(current_block->cutter_power);
        }
      #endif

      #if ENABLED(POWER_LOSS_RECOVERY)
        recovery.info.sdpos = current_block->sdpos;
        recovery.info.current_position = current_block->start_position;
      #endif

      #if ENABLED(DIRECT_STEPPING)
        if (current_block->is_page()) {
          page_step_state.segment_steps = 0;
          page_step_state.segment_idx = 0;
          page_step_state.page = page_manager.get_page(current_block->page_idx);
          page_step_state.bd.reset();

          if (DirectStepping::Config::DIRECTIONAL)
            current_block->direction_bits = last_direction_bits;

          if (!page_step_state.page) {
            discard_current_block();
            return interval;
          }
        }
      #endif

      // Set flags for all moving axes, accounting for kinematics
      set_axis_moved_for_current_block();

      #if ENABLED(ADAPTIVE_STEP_SMOOTHING)
        oversampling_factor = 0;

        // Decide if axis smoothing is possible
        if (adaptive_step_smoothing_enabled) {
          uint32_t max_rate = current_block->nominal_rate;  // Get the step event rate
          while (max_rate < min_step_isr_frequency) {       // As long as more ISRs are possible...
            max_rate <<= 1;                                 // Try to double the rate
            if (max_rate < min_step_isr_frequency)          // Don't exceed the estimated ISR limit
              ++oversampling_factor;                        // Increase the oversampling (used for left-shift)
          }
        }
      #endif

      // Based on the oversampling factor, do the calculations
      step_event_count = current_block->step_event_count << oversampling_factor;

      // Initialize Bresenham delta errors to 1/2
      delta_error = -int32_t(step_event_count);
      TERN_(HAS_ROUGH_LIN_ADVANCE, la_delta_error = delta_error);

      // Calculate Bresenham dividends and divisors
      advance_dividend = (current_block->steps << 1).asLong();
      advance_divisor = step_event_count << 1;

      #if ENABLED(INPUT_SHAPING_X)
        if (shaping_x.enabled) {
          const int64_t steps = current_block->direction_bits.x ? int64_t(current_block->steps.x) : -int64_t(current_block->steps.x);
          shaping_x.last_block_end_pos += steps;

          // If there are any remaining echos unprocessed, then direction change must
          // be delayed and processed in PULSE_PREP_SHAPING. This will cause half a step
          // to be missed, which will need recovering and this can be done through shaping_x.remainder.
          shaping_x.forward = current_block->direction_bits.x;
          if (!ShapingQueue::empty_x()) current_block->direction_bits.x = last_direction_bits.x;
        }
      #endif

      // Y and Z follow the same logic as X (but the comments aren't repeated)
      #if ENABLED(INPUT_SHAPING_Y)
        if (shaping_y.enabled) {
          const int64_t steps = current_block->direction_bits.y ? int64_t(current_block->steps.y) : -int64_t(current_block->steps.y);
          shaping_y.last_block_end_pos += steps;
          shaping_y.forward = current_block->direction_bits.y;
          if (!ShapingQueue::empty_y()) current_block->direction_bits.y = last_direction_bits.y;
        }
      #endif

      #if ENABLED(INPUT_SHAPING_Z)
        if (shaping_z.enabled) {
          const int64_t steps = current_block->direction_bits.z ? int64_t(current_block->steps.z) : -int64_t(current_block->steps.z);
          shaping_z.last_block_end_pos += steps;
          shaping_z.forward = current_block->direction_bits.z;
          if (!ShapingQueue::empty_z()) current_block->direction_bits.z = last_direction_bits.z;
        }
      #endif

      // No step events completed so far
      step_events_completed = 0;

      // Compute the acceleration and deceleration points
      accelerate_before = current_block->accelerate_before << oversampling_factor;
      decelerate_start = current_block->decelerate_start << oversampling_factor;

      TERN_(MIXING_EXTRUDER, mixer.stepper_setup(current_block->b_color));

      E_TERN_(stepper_extruder = current_block->extruder);

      // Initialize the trapezoid generator from the current block.
      #if HAS_ROUGH_LIN_ADVANCE
        #if DISABLED(MIXING_EXTRUDER) && E_STEPPERS > 1
          // If the now active extruder wasn't in use during the last move, its pressure is most likely gone.
          if (stepper_extruder != last_moved_extruder) la_advance_steps = 0;
        #endif
        la_active = (current_block->la_advance_rate != 0);
        if (la_active) {
          // Apply LA scaling and discount the effect of frequency scaling
          la_dividend = (advance_dividend.e << current_block->la_scaling) << oversampling_factor;
        }
      #endif

      if ( ENABLED(DUAL_X_CARRIAGE) // TODO: Find out why this fixes "jittery" small circles
        || current_block->direction_bits != last_direction_bits
        || TERN(MIXING_EXTRUDER, false, stepper_extruder != last_moved_extruder)
      ) {
        E_TERN_(last_moved_extruder = stepper_extruder);
        set_directions(current_block->direction_bits);
      }

      #if ENABLED(LASER_FEATURE)
        if (cutter.cutter_mode == CUTTER_MODE_CONTINUOUS) {           // Planner controls the laser
          if (planner.laser_inline.status.isSyncPower)
            // If the previous block was a M3 sync power then skip the trap power init otherwise it will 0 the sync power.
            planner.laser_inline.status.isSyncPower = false;          // Clear the flag to process subsequent trap calc's.
          else if (current_block->laser.status.isEnabled) {
            #if ENABLED(LASER_POWER_TRAP)
              TERN_(DEBUG_LASER_TRAP, SERIAL_ECHO_MSG("InitTrapPwr:",current_block->laser.trap_ramp_active_pwr));
              cutter.apply_power(current_block->laser.status.isPowered ? current_block->laser.trap_ramp_active_pwr : 0);
            #else
              TERN_(DEBUG_CUTTER_POWER, SERIAL_ECHO_MSG("InlinePwr:",current_block->laser.power));
              cutter.apply_power(current_block->laser.status.isPowered ? current_block->laser.power : 0);
            #endif
          }
        }
      #endif // LASER_FEATURE

      // If the endstop is already pressed, endstop interrupts won't invoke
      // endstop_triggered and the move will grind. So check here for a
      // triggered endstop, which marks the block for discard on the next ISR.
      endstops.update();

      #if ENABLED(Z_LATE_ENABLE)
        // If delayed Z enable, enable it now. This option will severely interfere with
        // timing between pulses when chaining motion between blocks, and it could lead
        // to lost steps in both X and Y axis, so avoid using it unless strictly necessary!!
        if (current_block->steps.z) enable_axis(Z_AXIS);
      #endif

      // Mark ticks_nominal as not-yet-calculated
      ticks_nominal = 0;

      #if ENABLED(S_CURVE_ACCELERATION)
        // Initialize the Bézier speed curve
        _calc_bezier_curve_coeffs(current_block->initial_rate, current_block->cruise_rate, current_block->acceleration_time_inverse);
        // We haven't started the 2nd half of the trapezoid
        bezier_2nd_half = false;
      #else
        // Set as deceleration point the initial rate of the block
        acc_step_rate = current_block->initial_rate;
      #endif

      // Calculate Nonlinear Extrusion fixed-point quotients
      #if NONLINEAR_EXTRUSION_Q24
        ne.edividend = advance_dividend.e;
        const float scale = (float(ne.edividend) / advance_divisor) * planner.mm_per_step[E_AXIS_N(current_block->extruder)];
        ne.scale_q24 = _BV32(24) * scale;
        if (ne.settings.enabled && current_block->direction_bits.e && ANY_AXIS_MOVES(current_block)) {
          ne.q24.A = _BV32(24) * ne.settings.coeff.A;
          ne.q24.B = _BV32(24) * ne.settings.coeff.B;
          ne.q24.C = _BV32(24) * ne.settings.coeff.C;
        }
        else {
          ne.q24.A = ne.q24.B = 0;
          ne.q24.C = _BV32(24);
        }
      #endif

      // Calculate the initial timer interval
      interval = calc_multistep_timer_interval(current_block->initial_rate << oversampling_factor);
      // Initialize ac/deceleration time as if half the time passed.
      acceleration_time = deceleration_time = interval / 2;

      calc_nonlinear_e(current_block->initial_rate << oversampling_factor);

      #if ENABLED(LIN_ADVANCE)
        #if ENABLED(SMOOTH_LIN_ADVANCE)
          curr_timer_tick = 0;
        #else
          if (la_active) {
            const uint32_t la_step_rate = la_advance_steps < current_block->max_adv_steps ? current_block->la_advance_rate : 0;
            la_interval = calc_timer_interval((current_block->initial_rate + la_step_rate) >> current_block->la_scaling);
          }
        #endif
      #endif
    }
  } // !current_block

  // Return the interval to wait
  return interval;
}

#if ENABLED(LIN_ADVANCE)

  #if ENABLED(SMOOTH_LIN_ADVANCE)

    float Stepper::extruder_advance_tau[DISTINCT_E];
    uint32_t Stepper::extruder_advance_tau_ticks[DISTINCT_E],
             Stepper::extruder_advance_alpha_q30[DISTINCT_E];

    void Stepper::set_la_interval(int32_t step_rate) {
      if (step_rate == 0) {
        la_interval = LA_ADV_NEVER;
      }
      else {
        const bool forward_e = step_rate > 0;

        #if ENABLED(NONLINEAR_EXTRUSION)
          if (ne.settings.enabled && forward_e && ANY_AXIS_MOVES(current_block)) {
            // Maximum polynomial value is just above 1, like 1.05..1.2, less than 2 anyway, so we can use 30 bits for fractional part
            int32_t vd_q30 = ne.q30.A * sq(step_rate) + ne.q30.B * step_rate;
            NOLESS(vd_q30, 0);
            step_rate = (int64_t(step_rate) * (ne.q30.C + vd_q30)) >> 30;
          }
        #endif

        la_interval = calc_timer_interval(uint32_t(ABS(step_rate)));
        if (forward_e != motor_direction(E_AXIS)) {
          last_direction_bits.toggle(E_AXIS);
          count_direction.e = -count_direction.e;
          DIR_WAIT_BEFORE();
          E_APPLY_DIR(forward_e, false);
          TERN_(FT_MOTION, last_set_direction = last_direction_bits);
          DIR_WAIT_AFTER();
        }
      }
    }

    #if ENABLED(INPUT_SHAPING_E_SYNC)

      constexpr uint16_t IS_COMPENSATION_BUFFER_SIZE = uint16_t(float(SMOOTH_LIN_ADV_HZ) / (2.0f * (SHAPING_MIN_FREQ)) + 0.5f);

      typedef struct {
        xy_long_t buffer[IS_COMPENSATION_BUFFER_SIZE];
        uint16_t index;
        FORCE_INLINE void add(const xy_long_t &input) {
          buffer[index] = input;
          if (++index == IS_COMPENSATION_BUFFER_SIZE) index = 0;
        }
        FORCE_INLINE xy_long_t past_item(const uint16_t n) {
          int16_t i = int16_t(index) - n;
          if (i < 0) i += IS_COMPENSATION_BUFFER_SIZE;
          // The following only happens when IS Freq is set below the minimum
          // configured at build time ...in which case IS will also misbehave!
          // Using setters whenever possible prevents values being set too low.
          if (TERN0(MARLIN_DEV_MODE, i < 0)) return {0, 0};
          return buffer[i];
        }
      } DelayBuffer;

      DelayBuffer delayBuffer;

      xy_long_t Stepper::smooth_lin_adv_lookback(const shaping_time_t stepper_ticks) {
        constexpr uint32_t adv_ticks_per_stepper_ticks_Q30 = (uint64_t(SMOOTH_LIN_ADV_HZ) * _BV32(30)) / (STEPPER_TIMER_RATE);
        const uint16_t delay_steps = MULT_Q(30, stepper_ticks, adv_ticks_per_stepper_ticks_Q30);
        return delayBuffer.past_item(delay_steps);
      }

    #endif // INPUT_SHAPING_E_SYNC

    #if ENABLED(S_CURVE_ACCELERATION)
      int32_t Stepper::calc_bezier_curve(const int32_t v0, const int32_t v1, const uint32_t av, const uint32_t curr_step) {
        int32_t A = bezier_A, B = bezier_B, C = bezier_C;
        uint32_t F = bezier_F, AV = bezier_AV;

        _calc_bezier_curve_coeffs(v0, v1, av);
        uint32_t rate = _eval_bezier_curve(curr_step);

        bezier_A = A; bezier_B = B; bezier_C = C; bezier_F = F; bezier_AV = AV;
        return rate;
      }
    #endif

    int32_t Stepper::smooth_lin_adv_lookahead(uint32_t stepper_ticks) {
      for (uint8_t i = 0; block_t *block = planner.get_future_block(i); i++) {
        if (block->is_sync()) continue;
        if (stepper_ticks <= block->acceleration_time) {
          if (!block->use_advance_lead) return 0;
          uint32_t rate;
          #if ENABLED(S_CURVE_ACCELERATION)
            rate = calc_bezier_curve(block->initial_rate, block->cruise_rate, block->acceleration_time_inverse, stepper_ticks);
          #else
            rate = STEP_MULTIPLY(stepper_ticks, block->acceleration_rate) + block->initial_rate;
            NOMORE(rate, block->nominal_rate);
          #endif
          return MULT_Q(30, rate, block->e_step_ratio_q30);
        }
        stepper_ticks -= block->acceleration_time;

        if (stepper_ticks <= block->cruise_time) {
          if (!block->use_advance_lead) return 0;
          return MULT_Q(30, block->cruise_rate, block->e_step_ratio_q30);
        }
        stepper_ticks -= block->cruise_time;

        if (stepper_ticks <= block->deceleration_time) {
          if (!block->use_advance_lead) return 0;
          uint32_t rate;
          #if ENABLED(S_CURVE_ACCELERATION)
            rate = calc_bezier_curve(block->cruise_rate, block->final_rate, block->deceleration_time_inverse, stepper_ticks);
          #else
            rate = STEP_MULTIPLY(stepper_ticks, block->acceleration_rate);
            if (rate < block->cruise_rate) {
              rate = block->cruise_rate - rate;
              NOLESS(rate, block->final_rate);
            }
            else
              rate = block->final_rate;
          #endif
          return MULT_Q(30, rate, block->e_step_ratio_q30);
        }
        stepper_ticks -= block->deceleration_time;
      }
      return 0;
    }

    hal_timer_t Stepper::smooth_lin_adv_isr() {
      int32_t target_adv_steps = 0;
      if (current_block) {
        const uint32_t stepper_ticks = extruder_advance_tau_ticks[E_INDEX_N(active_extruder)] + curr_timer_tick;
        target_adv_steps = MULT_Q(27, smooth_lin_adv_lookahead(stepper_ticks), planner.get_advance_k_q27());
      }
      else {
        curr_step_rate = 0;
      }
      static int32_t last_target_adv_steps = 0;
      constexpr uint16_t dt_inv = SMOOTH_LIN_ADV_HZ;
      int32_t la_step_rate = (target_adv_steps - last_target_adv_steps) * dt_inv;
      last_target_adv_steps = target_adv_steps;

      static int32_t smoothed_vals[SMOOTH_LIN_ADV_EXP_ORDER] = {0};

      for (uint8_t i = 0; i < SMOOTH_LIN_ADV_EXP_ORDER; i++) {
        // Approximate gaussian smoothing via higher order exponential smoothing
        smoothed_vals[i] += MULT_Q(30, la_step_rate - smoothed_vals[i], extruder_advance_alpha_q30[E_INDEX_N(active_extruder)]);
        la_step_rate = smoothed_vals[i];
      }

      const int32_t planned_step_rate = current_block
        ? MULT_Q(30, curr_step_rate, current_block->e_step_ratio_q30)
        : 0;

      #if ENABLED(INPUT_SHAPING_E_SYNC)

        int32_t unshaped_rate_e = la_step_rate + planned_step_rate;

        xy_long_t pre_shaping_rate{0}, first_pulse_rate{0};
        if (current_block && current_block->xy_length_inv_q30 > 0) {
          pre_shaping_rate = xy_long_t({
            MULT_Q(30, unshaped_rate_e * current_block->steps.x, current_block->xy_length_inv_q30),
            MULT_Q(30, unshaped_rate_e * current_block->steps.y, current_block->xy_length_inv_q30)
          });
          unshaped_rate_e = 0;

          first_pulse_rate = xy_long_t({
            TERN_(INPUT_SHAPING_X, shaping_x.enabled ? (pre_shaping_rate.x * shaping_x.factor1) >> 7 :) pre_shaping_rate.x,
            TERN_(INPUT_SHAPING_Y, shaping_y.enabled ? (pre_shaping_rate.y * shaping_y.factor1) >> 7 :) pre_shaping_rate.y
          });
        }

        const xy_long_t second_pulse_rate = xy_long_t({
          TERN0(INPUT_SHAPING_X, shaping_x.enabled ? (smooth_lin_adv_lookback(ShapingQueue::get_delay_x()).x * shaping_x.factor2) >> 7 : 0),
          TERN0(INPUT_SHAPING_Y, shaping_y.enabled ? (smooth_lin_adv_lookback(ShapingQueue::get_delay_y()).y * shaping_y.factor2) >> 7 : 0)
        });

        delayBuffer.add(pre_shaping_rate);

        set_la_interval(unshaped_rate_e + first_pulse_rate.x + second_pulse_rate.x + first_pulse_rate.y + second_pulse_rate.y);

      #else // !INPUT_SHAPING_E_SYNC

        set_la_interval(la_step_rate + planned_step_rate);

      #endif

      curr_timer_tick += SMOOTH_LIN_ADV_INTERVAL;
      return SMOOTH_LIN_ADV_INTERVAL;
    }

  #endif // SMOOTH_LIN_ADVANCE

  // Timer interrupt for E. LA_steps is set in the main routine
  void Stepper::advance_isr() {
    // Apply Bresenham algorithm so that linear advance can piggy back on
    // the acceleration and speed values calculated in block_phase_isr().
    // This helps keep LA in sync with, for example, S_CURVE_ACCELERATION.
    #if HAS_ROUGH_LIN_ADVANCE
      la_delta_error += la_dividend;
      const bool e_step_needed = la_delta_error >= 0;
    #else
      constexpr bool e_step_needed = true;
    #endif

    if (e_step_needed) {
      count_position.e += count_direction.e;
      #if HAS_ROUGH_LIN_ADVANCE
        la_advance_steps += count_direction.e;
        la_delta_error -= advance_divisor;
      #endif

      // Set the STEP pulse ON
      E_STEP_WRITE(TERN(MIXING_EXTRUDER, mixer.get_next_stepper(), stepper_extruder), STEP_STATE_E);
    }

    TERN_(I2S_STEPPER_STREAM, i2s_push_sample());

    if (e_step_needed) {
      // Enforce a minimum duration for STEP pulse ON
      #if ISR_PULSE_CONTROL
        USING_TIMED_PULSE();
        START_TIMED_PULSE();
        AWAIT_HIGH_PULSE();
      #endif

      // Set the STEP pulse OFF
      E_STEP_WRITE(TERN(MIXING_EXTRUDER, mixer.get_stepper(), stepper_extruder), !STEP_STATE_E);
    }
  }

#endif // LIN_ADVANCE

#if ENABLED(BABYSTEPPING)

  // Timer interrupt for baby-stepping
  hal_timer_t Stepper::babystepping_isr() {
    babystep.task();
    return babystep.has_steps() ? BABYSTEP_TICKS : BABYSTEP_NEVER;
  }

#endif

// Check if the given block is busy or not - Must not be called from ISR contexts
// The current_block could change in the middle of the read by an Stepper ISR, so
// we must explicitly prevent that!
bool Stepper::is_block_busy(const block_t * const block) {
  #ifdef __AVR__
    // A SW memory barrier, to ensure GCC does not overoptimize loops
    #define sw_barrier() asm volatile("": : :"memory");

    // Keep reading until 2 consecutive reads return the same value,
    // meaning there was no update in-between caused by an interrupt.
    // This works because stepper ISRs happen at a slower rate than
    // successive reads of a variable, so 2 consecutive reads with
    // the same value means no interrupt updated it.
    block_t *vold, *vnew = current_block;
    sw_barrier();
    do {
      vold = vnew;
      vnew = current_block;
      sw_barrier();
    } while (vold != vnew);
  #else
    block_t *vnew = current_block;
  #endif

  // Return if the block is busy or not
  return block == vnew;
}

void Stepper::init() {

  #if MB(ALLIGATOR)
    const float motor_current[] = MOTOR_CURRENT;
    unsigned int digipot_motor = 0;
    for (uint8_t i = 0; i < 3 + EXTRUDERS; ++i) {
      digipot_motor = 255 * (motor_current[i] / 2.5);
      dac084s085::setValue(i, digipot_motor);
    }
  #endif

  // Init Microstepping Pins
  TERN_(HAS_MICROSTEPS, microstep_init());

  // Init Dir Pins
  TERN_(HAS_X_DIR,  X_DIR_INIT());
  TERN_(HAS_X2_DIR, X2_DIR_INIT());
  TERN_(HAS_Y_DIR,  Y_DIR_INIT());
  TERN_(HAS_Y2_DIR, Y2_DIR_INIT());
  TERN_(HAS_Z_DIR,  Z_DIR_INIT());
  TERN_(HAS_Z2_DIR, Z2_DIR_INIT());
  TERN_(HAS_Z3_DIR, Z3_DIR_INIT());
  TERN_(HAS_Z4_DIR, Z4_DIR_INIT());
  TERN_(HAS_I_DIR,  I_DIR_INIT());
  TERN_(HAS_J_DIR,  J_DIR_INIT());
  TERN_(HAS_K_DIR,  K_DIR_INIT());
  TERN_(HAS_U_DIR,  U_DIR_INIT());
  TERN_(HAS_V_DIR,  V_DIR_INIT());
  TERN_(HAS_W_DIR,  W_DIR_INIT());
  TERN_(HAS_E0_DIR, E0_DIR_INIT());
  TERN_(HAS_E1_DIR, E1_DIR_INIT());
  TERN_(HAS_E2_DIR, E2_DIR_INIT());
  TERN_(HAS_E3_DIR, E3_DIR_INIT());
  TERN_(HAS_E4_DIR, E4_DIR_INIT());
  TERN_(HAS_E5_DIR, E5_DIR_INIT());
  TERN_(HAS_E6_DIR, E6_DIR_INIT());
  TERN_(HAS_E7_DIR, E7_DIR_INIT());

  // Init Enable Pins - Steppers default to disabled.
  #define _INIT_CONFIG_ENABLE(A) do{ A##_ENABLE_INIT(); if (A##_ENABLE_INIT_STATE) A##_ENABLE_WRITE(HIGH); }while(0)
  TERN_(HAS_X_ENABLE,  _INIT_CONFIG_ENABLE(X));
  TERN_(HAS_X2_ENABLE, _INIT_CONFIG_ENABLE(X2));
  TERN_(HAS_Y_ENABLE,  _INIT_CONFIG_ENABLE(Y));
  TERN_(HAS_Y2_ENABLE, _INIT_CONFIG_ENABLE(Y2));
  TERN_(HAS_Z_ENABLE,  _INIT_CONFIG_ENABLE(Z));
  TERN_(HAS_Z2_ENABLE, _INIT_CONFIG_ENABLE(Z2));
  TERN_(HAS_Z3_ENABLE, _INIT_CONFIG_ENABLE(Z3));
  TERN_(HAS_Z4_ENABLE, _INIT_CONFIG_ENABLE(Z4));
  TERN_(HAS_I_ENABLE,  _INIT_CONFIG_ENABLE(I));
  TERN_(HAS_J_ENABLE,  _INIT_CONFIG_ENABLE(J));
  TERN_(HAS_K_ENABLE,  _INIT_CONFIG_ENABLE(K));
  TERN_(HAS_U_ENABLE,  _INIT_CONFIG_ENABLE(U));
  TERN_(HAS_V_ENABLE,  _INIT_CONFIG_ENABLE(V));
  TERN_(HAS_W_ENABLE,  _INIT_CONFIG_ENABLE(W));
  TERN_(HAS_E0_ENABLE, _INIT_CONFIG_ENABLE(E0));
  TERN_(HAS_E1_ENABLE, _INIT_CONFIG_ENABLE(E1));
  TERN_(HAS_E2_ENABLE, _INIT_CONFIG_ENABLE(E2));
  TERN_(HAS_E3_ENABLE, _INIT_CONFIG_ENABLE(E3));
  TERN_(HAS_E4_ENABLE, _INIT_CONFIG_ENABLE(E4));
  TERN_(HAS_E5_ENABLE, _INIT_CONFIG_ENABLE(E5));
  TERN_(HAS_E6_ENABLE, _INIT_CONFIG_ENABLE(E6));
  TERN_(HAS_E7_ENABLE, _INIT_CONFIG_ENABLE(E7));

  #define _STEP_INIT(AXIS) AXIS ##_STEP_INIT()
  #define _WRITE_STEP(AXIS, HIGHLOW) AXIS ##_STEP_WRITE(HIGHLOW)
  #define _DISABLE_AXIS(AXIS) DISABLE_AXIS_## AXIS()

  #define AXIS_INIT(AXIS, PIN) \
    _STEP_INIT(AXIS); \
    _WRITE_STEP(AXIS, !_STEP_STATE(PIN)); \
    _DISABLE_AXIS(AXIS)

  #define E_AXIS_INIT(NUM) DEFER(AXIS_INIT)(E##NUM, E)

  // Init Step Pins
  #if HAS_X_STEP
    #if HAS_X2_STEPPER
      X2_STEP_INIT();
      X2_STEP_WRITE(!STEP_STATE_X);
    #endif
    AXIS_INIT(X, X);
  #endif

  #if HAS_Y_STEP
    #if HAS_Y2_STEPPER
      Y2_STEP_INIT();
      Y2_STEP_WRITE(!STEP_STATE_Y);
    #endif
    AXIS_INIT(Y, Y);
  #endif

  #if HAS_Z_STEP
    #if NUM_Z_STEPPERS >= 2
      Z2_STEP_INIT();
      Z2_STEP_WRITE(!STEP_STATE_Z);
    #endif
    #if NUM_Z_STEPPERS >= 3
      Z3_STEP_INIT();
      Z3_STEP_WRITE(!STEP_STATE_Z);
    #endif
    #if NUM_Z_STEPPERS >= 4
      Z4_STEP_INIT();
      Z4_STEP_WRITE(!STEP_STATE_Z);
    #endif
    AXIS_INIT(Z, Z);
  #endif
  TERN_(HAS_I_STEP, AXIS_INIT(I, I));
  TERN_(HAS_J_STEP, AXIS_INIT(J, J));
  TERN_(HAS_K_STEP, AXIS_INIT(K, K));
  TERN_(HAS_U_STEP, AXIS_INIT(U, U));
  TERN_(HAS_V_STEP, AXIS_INIT(V, V));
  TERN_(HAS_W_STEP, AXIS_INIT(W, W));

  TERN_(HAS_E0_STEP, E_AXIS_INIT(0));
  TERN_(HAS_E1_STEP, E_AXIS_INIT(1));
  TERN_(HAS_E2_STEP, E_AXIS_INIT(2));
  TERN_(HAS_E3_STEP, E_AXIS_INIT(3));
  TERN_(HAS_E4_STEP, E_AXIS_INIT(4));
  TERN_(HAS_E5_STEP, E_AXIS_INIT(5));
  TERN_(HAS_E6_STEP, E_AXIS_INIT(6));
  TERN_(HAS_E7_STEP, E_AXIS_INIT(7));

  #if DISABLED(I2S_STEPPER_STREAM)
    HAL_timer_start(MF_TIMER_STEP, 122); // Init Stepper ISR to 122 Hz for quick starting
    wake_up();
    sei();
  #endif

  // Init direction states
  apply_directions();

  #if HAS_MOTOR_CURRENT_SPI || HAS_MOTOR_CURRENT_PWM
    initialized = true;
    digipot_init();
  #endif
}

#if HAS_ZV_SHAPING

  /**
   * Calculate a fixed point factor to apply to the signal and its echo
   * when shaping an axis.
   */
  void Stepper::set_shaping_damping_ratio(const AxisEnum axis, const_float_t zeta) {
    // From the damping ratio, get a factor that can be applied to advance_dividend for fixed-point maths.
    // For ZV, we use amplitudes 1/(1+K) and K/(1+K) where K = exp(-zeta * π / sqrt(1.0f - zeta * zeta))
    // which can be converted to 1:7 fixed point with an excellent fit with a 3rd-order polynomial.
    float factor2;
    if (zeta <= 0.0f) factor2 = 64.0f;
    else if (zeta >= 1.0f) factor2 = 0.0f;
    else {
      factor2 = 64.44056192 + -99.02008832 * zeta;
      const float zeta2 = sq(zeta);
      factor2 += -7.58095488 * zeta2;
      const float zeta3 = zeta2 * zeta;
      factor2 += 43.073216 * zeta3;
      factor2 = FLOOR(factor2);
    }

    const bool was_on = hal.isr_state();
    hal.isr_off();
    TERN_(INPUT_SHAPING_X, if (axis == X_AXIS) { shaping_x.factor2 = factor2; shaping_x.factor1 = 128 - factor2; shaping_x.zeta = zeta; })
    TERN_(INPUT_SHAPING_Y, if (axis == Y_AXIS) { shaping_y.factor2 = factor2; shaping_y.factor1 = 128 - factor2; shaping_y.zeta = zeta; })
    TERN_(INPUT_SHAPING_Z, if (axis == Z_AXIS) { shaping_z.factor2 = factor2; shaping_z.factor1 = 128 - factor2; shaping_z.zeta = zeta; })
    if (was_on) hal.isr_on();
  }

  float Stepper::get_shaping_damping_ratio(const AxisEnum axis) {
    TERN_(INPUT_SHAPING_X, if (axis == X_AXIS) return shaping_x.zeta);
    TERN_(INPUT_SHAPING_Y, if (axis == Y_AXIS) return shaping_y.zeta);
    TERN_(INPUT_SHAPING_Z, if (axis == Z_AXIS) return shaping_z.zeta);
    return -1;
  }

  void Stepper::set_shaping_frequency(const AxisEnum axis, const_float_t freq) {
    // enabling or disabling shaping whilst moving can result in lost steps
    planner.synchronize();

    const bool was_on = hal.isr_state();
    hal.isr_off();

    const shaping_time_t delay = freq ? float(uint32_t(STEPPER_TIMER_RATE) / 2) / freq : shaping_time_t(-1);
    #define SHAPING_SET_FREQ_FOR_AXIS(AXISN, AXISL)                                 \
      if (axis == AXISN) {                                                          \
        ShapingQueue::set_delay(AXISN, delay);                                      \
        shaping_##AXISL.frequency = freq;                                           \
        shaping_##AXISL.enabled = !!freq;                                           \
        shaping_##AXISL.delta_error = 0;                                            \
        shaping_##AXISL.last_block_end_pos = count_position.AXISL;                  \
      }

    TERN_(INPUT_SHAPING_X, SHAPING_SET_FREQ_FOR_AXIS(X_AXIS, x))
    TERN_(INPUT_SHAPING_Y, SHAPING_SET_FREQ_FOR_AXIS(Y_AXIS, y))
    TERN_(INPUT_SHAPING_Z, SHAPING_SET_FREQ_FOR_AXIS(Z_AXIS, z))

    if (was_on) hal.isr_on();
  }

  float Stepper::get_shaping_frequency(const AxisEnum axis) {
    TERN_(INPUT_SHAPING_X, if (axis == X_AXIS) return shaping_x.frequency);
    TERN_(INPUT_SHAPING_Y, if (axis == Y_AXIS) return shaping_y.frequency);
    TERN_(INPUT_SHAPING_Z, if (axis == Z_AXIS) return shaping_z.frequency);
    return -1;
  }

#endif // HAS_ZV_SHAPING

/**
 * Set the stepper positions directly in steps
 *
 * The input is based on the typical per-axis XYZE steps.
 * For CORE machines XYZ needs to be translated to ABC.
 *
 * This allows get_axis_position_mm to correctly
 * derive the current XYZE position later on.
 */
void Stepper::_set_position(const abce_long_t &spos) {
  #if ENABLED(INPUT_SHAPING_X)
    const int32_t x_shaping_delta = count_position.x - shaping_x.last_block_end_pos;
  #endif
  #if ENABLED(INPUT_SHAPING_Y)
    const int32_t y_shaping_delta = count_position.y - shaping_y.last_block_end_pos;
  #endif
  #if ENABLED(INPUT_SHAPING_Z)
    const int32_t z_shaping_delta = count_position.z - shaping_z.last_block_end_pos;
  #endif

  #if ANY(IS_CORE, MARKFORGED_XY, MARKFORGED_YX)
    // Core equations follow the form of the dA and dB equations at https://www.corexy.com/theory.html
    #if CORE_IS_XY
      count_position.set(spos.a + spos.b, CORESIGN(spos.a - spos.b) OPTARG(HAS_Z_AXIS, spos.c));
    #elif CORE_IS_XZ
      count_position.set(spos.a + spos.c, spos.b, CORESIGN(spos.a - spos.c));
    #elif CORE_IS_YZ
      count_position.set(spos.a, spos.b + spos.c, CORESIGN(spos.b - spos.c));
    #elif ENABLED(MARKFORGED_XY)
      count_position.set(spos.a TERN(MARKFORGED_INVERSE, +, -) spos.b, spos.b, spos.c);
    #elif ENABLED(MARKFORGED_YX)
      count_position.set(spos.a, spos.b TERN(MARKFORGED_INVERSE, +, -) spos.a, spos.c);
    #endif
    SECONDARY_AXIS_CODE(
      count_position.i = spos.i,
      count_position.j = spos.j,
      count_position.k = spos.k,
      count_position.u = spos.u,
      count_position.v = spos.v,
      count_position.w = spos.w
    );
    TERN_(HAS_EXTRUDERS, count_position.e = spos.e);
  #else
    // default non-h-bot planning
    count_position = spos;
  #endif

  #if ENABLED(INPUT_SHAPING_X)
    if (shaping_x.enabled) {
      count_position.x += x_shaping_delta;
      shaping_x.last_block_end_pos = spos.x;
    }
  #endif
  #if ENABLED(INPUT_SHAPING_Y)
    if (shaping_y.enabled) {
      count_position.y += y_shaping_delta;
      shaping_y.last_block_end_pos = spos.y;
    }
  #endif
  #if ENABLED(INPUT_SHAPING_Z)
    if (shaping_z.enabled) {
      count_position.z += z_shaping_delta;
      shaping_z.last_block_end_pos = spos.z;
    }
  #endif
}

// AVR requires guards to ensure any atomic memory operation greater than 8 bits
#define ATOMIC_SECTION_START() const bool was_enabled = suspend()
#define ATOMIC_SECTION_END() if (was_enabled) wake_up()
#define AVR_ATOMIC_SECTION_START() TERN_(__AVR__, ATOMIC_SECTION_START())
#define AVR_ATOMIC_SECTION_END() TERN_(__AVR__, ATOMIC_SECTION_END())

/**
 * Get a stepper's position in steps.
 */
int32_t Stepper::position(const AxisEnum axis) {
  AVR_ATOMIC_SECTION_START();
  const int32_t v = count_position[axis];
  AVR_ATOMIC_SECTION_END();
  return v;
}

/**
 * Set all axis stepper positions in steps
 */
void Stepper::set_position(const xyze_long_t &spos) {
  planner.synchronize();
  ATOMIC_SECTION_START();
  _set_position(spos);
  ATOMIC_SECTION_END();
}

/**
 * Set a single axis stepper position in steps
 */
void Stepper::set_axis_position(const AxisEnum a, const int32_t &v) {
  planner.synchronize();

  #if ANY(__AVR__, INPUT_SHAPING_X, INPUT_SHAPING_Y, INPUT_SHAPING_Z)
    ATOMIC_SECTION_START();
  #endif

  count_position[a] = v;
  TERN_(INPUT_SHAPING_X, if (a == X_AXIS) shaping_x.last_block_end_pos = v);
  TERN_(INPUT_SHAPING_Y, if (a == Y_AXIS) shaping_y.last_block_end_pos = v);
  TERN_(INPUT_SHAPING_Z, if (a == Z_AXIS) shaping_z.last_block_end_pos = v);

  #if ANY(__AVR__, INPUT_SHAPING_X, INPUT_SHAPING_Y, INPUT_SHAPING_Z)
    ATOMIC_SECTION_END();
  #endif
}

#if HAS_EXTRUDERS

  void Stepper::set_e_position(const int32_t &v) {
    planner.synchronize();

    AVR_ATOMIC_SECTION_START();
    count_position.e = v;
    AVR_ATOMIC_SECTION_END();
  }

#endif // HAS_EXTRUDERS

#if ENABLED(FT_MOTION)

  void Stepper::ftMotion_syncPosition() {
    planner.synchronize();

    // Update stepper positions from the planner
    AVR_ATOMIC_SECTION_START();
    count_position = planner.position;
    AVR_ATOMIC_SECTION_END();
  }

#endif // FT_MOTION

/**
 * Record stepper positions and discard the rest of the current block
 *
 * WARNING! This function may be called from ISR context!
 * If the Stepper ISR is preempted (e.g., by the endstop ISR) we
 * must ensure the move is properly canceled before the ISR resumes.
 */
void Stepper::endstop_triggered(const AxisEnum axis) {

  ATOMIC_SECTION_START();   // Suspend the Stepper ISR on all platforms

  endstops_trigsteps[axis] = (
    #if IS_CORE
      (axis == CORE_AXIS_2
        ? CORESIGN(count_position[CORE_AXIS_1] - count_position[CORE_AXIS_2])
        : count_position[CORE_AXIS_1] + count_position[CORE_AXIS_2]
      ) * double(0.5)
    #elif ENABLED(MARKFORGED_XY)
      axis == CORE_AXIS_1
        ? count_position[CORE_AXIS_1] TERN(MARKFORGED_INVERSE, +, -) count_position[CORE_AXIS_2]
        : count_position[CORE_AXIS_2]
    #elif ENABLED(MARKFORGED_YX)
      axis == CORE_AXIS_1
        ? count_position[CORE_AXIS_1]
        : count_position[CORE_AXIS_2] TERN(MARKFORGED_INVERSE, +, -) count_position[CORE_AXIS_1]
    #else // !IS_CORE
      count_position[axis]
    #endif
  );

  // Discard the rest of the move if there is a current block
  quick_stop();

  ATOMIC_SECTION_END();     // Suspend the Stepper ISR on all platforms
}

// Return the "triggered" position for an axis (that hit an endstop)
int32_t Stepper::triggered_position(const AxisEnum axis) {
  AVR_ATOMIC_SECTION_START();
  const int32_t v = endstops_trigsteps[axis];
  AVR_ATOMIC_SECTION_END();
  return v;
}

#if ANY(CORE_IS_XY, CORE_IS_XZ, MARKFORGED_XY, MARKFORGED_YX, IS_SCARA, DELTA)
  #define SAYS_A 1
#endif
#if ANY(CORE_IS_XY, CORE_IS_YZ, MARKFORGED_XY, MARKFORGED_YX, IS_SCARA, DELTA, POLAR)
  #define SAYS_B 1
#endif
#if ANY(CORE_IS_XZ, CORE_IS_YZ, DELTA)
  #define SAYS_C 1
#endif

void Stepper::report_a_position(const xyz_long_t &pos) {
  #if NUM_AXES
    SERIAL_ECHOLNPGM_P(
      LIST_N(DOUBLE(NUM_AXES),
        TERN(SAYS_A, PSTR(STR_COUNT_A), PSTR(STR_COUNT_X)), pos.x,
        TERN(SAYS_B, PSTR("B:"), SP_Y_LBL), pos.y,
        TERN(SAYS_C, PSTR("C:"), SP_Z_LBL), pos.z,
        SP_I_LBL, pos.i, SP_J_LBL, pos.j, SP_K_LBL, pos.k,
        SP_U_LBL, pos.u, SP_V_LBL, pos.v, SP_W_LBL, pos.w
      )
    );
  #endif
}

void Stepper::report_positions() {
  AVR_ATOMIC_SECTION_START();
  const xyz_long_t pos = count_position;
  AVR_ATOMIC_SECTION_END();
  report_a_position(pos);
}

#if ENABLED(FT_MOTION)

  /**
   * Run stepping from the Stepper ISR at regular short intervals.
   *
   * - Set ftMotion.sts_stepperBusy state to reflect whether there are any commands in the circular buffer.
   * - If there are no commands in the buffer, return.
   * - Get the next command from the circular buffer ftMotion.stepperCmdBuff[].
   * - If the block is being aborted, return without processing the command.
   * - Apply STEP/DIR along with any delays required. A command may be empty, with no STEP/DIR.
   */
  void Stepper::ftMotion_stepper() {

    // Check if the buffer is empty.
    ftMotion.sts_stepperBusy = (ftMotion.stepperCmdBuff_produceIdx != ftMotion.stepperCmdBuff_consumeIdx);
    if (!ftMotion.sts_stepperBusy) return;

    // "Pop" one command from current motion buffer
    const ft_command_t command = ftMotion.stepperCmdBuff[ftMotion.stepperCmdBuff_consumeIdx];
    if (++ftMotion.stepperCmdBuff_consumeIdx == (FTM_STEPPERCMD_BUFF_SIZE))
      ftMotion.stepperCmdBuff_consumeIdx = 0;

    if (abort_current_block) return;

    USING_TIMED_PULSE();

    // Get FT Motion command flags for axis STEP / DIR
    #define _FTM_STEP(AXIS) TEST(command, FT_BIT_STEP_##AXIS)
    #define _FTM_DIR(AXIS) TEST(command, FT_BIT_DIR_##AXIS)

    /**
     * Update direction bits for steppers that were stepped by this command.
     * HX, HY, HZ direction bits were set for Core kinematics
     * when the block was fetched and are not overwritten here.
     */

    #define _FTM_SET_DIR(AXIS) if (_FTM_STEP(AXIS)) last_direction_bits.bset(_AXIS(AXIS), _FTM_DIR(AXIS));
    LOGICAL_AXIS_MAP(_FTM_SET_DIR);

    if (last_set_direction != last_direction_bits) {
      // Apply directions (generally applying to the entire linear move)
      #define _FTM_APPLY_DIR(A) if (last_direction_bits.A != last_set_direction.A) \
                                  SET_STEP_DIR(A);
      LOGICAL_AXIS_MAP(_FTM_APPLY_DIR);

      last_set_direction = last_direction_bits;

      // Any DIR change requires a wait period
      DIR_WAIT_AFTER();
    }

    // Start step pulses. Edge stepping will toggle the STEP pin.
    #define _FTM_STEP_START(A) A##_APPLY_STEP(_FTM_STEP(A), false);
    LOGICAL_AXIS_MAP(_FTM_STEP_START);

    // Apply steps via I2S
    TERN_(I2S_STEPPER_STREAM, i2s_push_sample());

    // Begin waiting for the minimum pulse duration
    START_TIMED_PULSE();

    // Update step counts
    #define _FTM_STEP_COUNT(A) if (_FTM_STEP(A)) count_position.A += last_direction_bits.A ? 1 : -1;
    LOGICAL_AXIS_MAP(_FTM_STEP_COUNT);

    // Provide EDGE flags for E stepper(s)
    #if HAS_EXTRUDERS
      #if ENABLED(E_DUAL_STEPPER_DRIVERS)
        constexpr bool e_axis_has_dedge = AXIS_HAS_DEDGE(E0) && AXIS_HAS_DEDGE(E1);
      #else
        #define _EDGE_BIT(N) | (AXIS_HAS_DEDGE(E##N) << TOOL_ESTEPPER(N))
        constexpr Flags<E_STEPPERS> e_stepper_dedge { 0 REPEAT(EXTRUDERS, _EDGE_BIT) };
        const bool e_axis_has_dedge = e_stepper_dedge[stepper_extruder];
      #endif
    #endif

    // Only wait for axes without edge stepping
    const bool any_wait = false LOGICAL_AXIS_GANG(
      || (!e_axis_has_dedge  && _FTM_STEP(E)),
      || (!AXIS_HAS_DEDGE(X) && _FTM_STEP(X)), || (!AXIS_HAS_DEDGE(Y) && _FTM_STEP(Y)), || (!AXIS_HAS_DEDGE(Z) && _FTM_STEP(Z)),
      || (!AXIS_HAS_DEDGE(I) && _FTM_STEP(I)), || (!AXIS_HAS_DEDGE(J) && _FTM_STEP(J)), || (!AXIS_HAS_DEDGE(K) && _FTM_STEP(K)),
      || (!AXIS_HAS_DEDGE(U) && _FTM_STEP(U)), || (!AXIS_HAS_DEDGE(V) && _FTM_STEP(V)), || (!AXIS_HAS_DEDGE(W) && _FTM_STEP(W))
    );

    // Allow pulses to be registered by stepper drivers
    if (any_wait) AWAIT_HIGH_PULSE();

    // Stop pulses. Axes with DEDGE will do nothing, assuming STEP_STATE_* is HIGH
    #define _FTM_STEP_STOP(AXIS) AXIS##_APPLY_STEP(!STEP_STATE_##AXIS, false);
    LOGICAL_AXIS_MAP(_FTM_STEP_STOP);

  } // Stepper::ftMotion_stepper

#endif // FT_MOTION

#if ENABLED(BABYSTEPPING)

  #define _ENABLE_AXIS(A) enable_axis(_AXIS(A))
  #define _READ_DIR(AXIS) AXIS ##_DIR_READ()
  #define _APPLY_DIR(AXIS, FWD) AXIS ##_APPLY_DIR(FWD, true)

  #if MINIMUM_STEPPER_PULSE_NS
    #define STEP_PULSE_CYCLES ((MINIMUM_STEPPER_PULSE_NS) * CYCLES_PER_MICROSECOND / 1000)
  #else
    #define STEP_PULSE_CYCLES 0
  #endif

  #if ENABLED(DELTA)
    #define CYCLES_EATEN_BABYSTEP (2 * 15)
  #else
    #define CYCLES_EATEN_BABYSTEP 0
  #endif

  #if CYCLES_EATEN_BABYSTEP < STEP_PULSE_CYCLES
    #define EXTRA_CYCLES_BABYSTEP (STEP_PULSE_CYCLES - (CYCLES_EATEN_BABYSTEP))
  #else
    #define EXTRA_CYCLES_BABYSTEP 0
  #endif

  #if EXTRA_CYCLES_BABYSTEP > 20
    #define _SAVE_START() const hal_timer_t pulse_start = HAL_timer_get_count(MF_TIMER_PULSE)
    #define _PULSE_WAIT() while (EXTRA_CYCLES_BABYSTEP > uint32_t(HAL_timer_get_count(MF_TIMER_PULSE) - pulse_start) * (PULSE_TIMER_PRESCALE)) { /* nada */ }
  #else
    #define _SAVE_START() NOOP
    #if EXTRA_CYCLES_BABYSTEP > 0
      #define _PULSE_WAIT() DELAY_CYCLES(EXTRA_CYCLES_BABYSTEP)
    #elif ENABLED(DELTA)
      #define _PULSE_WAIT() DELAY_US(2);
    #elif STEP_PULSE_CYCLES > 0
      #define _PULSE_WAIT() NOOP
    #else
      #define _PULSE_WAIT() DELAY_US(4);
    #endif
  #endif

  #if ENABLED(BABYSTEPPING_EXTRA_DIR_WAIT)
    #define EXTRA_DIR_WAIT_BEFORE DIR_WAIT_BEFORE
    #define EXTRA_DIR_WAIT_AFTER  DIR_WAIT_AFTER
  #else
    #define EXTRA_DIR_WAIT_BEFORE()
    #define EXTRA_DIR_WAIT_AFTER()
  #endif

  #if DISABLED(DELTA)

    #define BABYSTEP_AXIS(AXIS, FWD, INV) do{      \
      const bool old_fwd = _READ_DIR(AXIS);        \
      _ENABLE_AXIS(AXIS);                          \
      DIR_WAIT_BEFORE();                           \
      _APPLY_DIR(AXIS, (FWD)^(INV));               \
      DIR_WAIT_AFTER();                            \
      _SAVE_START();                               \
      _APPLY_STEP(AXIS, _STEP_STATE(AXIS), true);  \
      _PULSE_WAIT();                               \
      _APPLY_STEP(AXIS, !_STEP_STATE(AXIS), true); \
      EXTRA_DIR_WAIT_BEFORE();                     \
      _APPLY_DIR(AXIS, old_fwd);                   \
      EXTRA_DIR_WAIT_AFTER();                      \
    }while(0)

  #endif

  #if IS_CORE

    #define BABYSTEP_CORE(A, B, FWD, INV, ALT) do{              \
      const xy_byte_t old_fwd = { _READ_DIR(A), _READ_DIR(B) }; \
      _ENABLE_AXIS(A); _ENABLE_AXIS(B);                         \
      DIR_WAIT_BEFORE();                                        \
      _APPLY_DIR(A, (FWD)^(INV));                               \
      _APPLY_DIR(B, (FWD)^(INV)^(ALT));                         \
      DIR_WAIT_AFTER();                                         \
      _SAVE_START();                                            \
      _APPLY_STEP(A, _STEP_STATE(A), true);                     \
      _APPLY_STEP(B, _STEP_STATE(B), true);                     \
      _PULSE_WAIT();                                            \
      _APPLY_STEP(A, !_STEP_STATE(A), true);                    \
      _APPLY_STEP(B, !_STEP_STATE(B), true);                    \
      EXTRA_DIR_WAIT_BEFORE();                                  \
      _APPLY_DIR(A, old_fwd.a); _APPLY_DIR(B, old_fwd.b);       \
      EXTRA_DIR_WAIT_AFTER();                                   \
    }while(0)

  #endif

  // MUST ONLY BE CALLED BY AN ISR,
  // No other ISR should ever interrupt this!
  void Stepper::do_babystep(const AxisEnum axis, const bool direction) {

    IF_DISABLED(BABYSTEPPING, cli());

    switch (axis) {

      #if ENABLED(BABYSTEP_XY)

        case X_AXIS:
          #if CORE_IS_XY
            BABYSTEP_CORE(X, Y, direction, 0, 0);
          #elif CORE_IS_XZ
            BABYSTEP_CORE(X, Z, direction, 0, 0);
          #else
            BABYSTEP_AXIS(X, direction, 0);
          #endif
          break;

        case Y_AXIS:
          #if CORE_IS_XY
            BABYSTEP_CORE(X, Y, direction, 0, (CORESIGN(1)>0));
          #elif CORE_IS_YZ
            BABYSTEP_CORE(Y, Z, direction, 0, (CORESIGN(1)<0));
          #else
            BABYSTEP_AXIS(Y, direction, 0);
          #endif
          break;

      #endif

      case Z_AXIS: {

        #if CORE_IS_XZ
          BABYSTEP_CORE(X, Z, direction, ENABLED(BABYSTEP_INVERT_Z), (CORESIGN(1)>0));
        #elif CORE_IS_YZ
          BABYSTEP_CORE(Y, Z, direction, ENABLED(BABYSTEP_INVERT_Z), (CORESIGN(1)<0));
        #elif DISABLED(DELTA)
          BABYSTEP_AXIS(Z, direction, ENABLED(BABYSTEP_INVERT_Z));

        #else // DELTA

          const bool z_direction = TERN_(BABYSTEP_INVERT_Z, !) direction;

          enable_axis(A_AXIS); enable_axis(B_AXIS); enable_axis(C_AXIS);

          DIR_WAIT_BEFORE();

          const bool old_fwd[3] = { X_DIR_READ(), Y_DIR_READ(), Z_DIR_READ() };

          X_DIR_WRITE(z_direction);
          Y_DIR_WRITE(z_direction);
          Z_DIR_WRITE(z_direction);

          DIR_WAIT_AFTER();

          _SAVE_START();

          X_STEP_WRITE(STEP_STATE_X);
          Y_STEP_WRITE(STEP_STATE_Y);
          Z_STEP_WRITE(STEP_STATE_Z);

          _PULSE_WAIT();

          X_STEP_WRITE(!STEP_STATE_X);
          Y_STEP_WRITE(!STEP_STATE_Y);
          Z_STEP_WRITE(!STEP_STATE_Z);

          // Restore direction bits
          EXTRA_DIR_WAIT_BEFORE();

          X_DIR_WRITE(old_fwd[A_AXIS]);
          Y_DIR_WRITE(old_fwd[B_AXIS]);
          Z_DIR_WRITE(old_fwd[C_AXIS]);

          EXTRA_DIR_WAIT_AFTER();

        #endif

      } break;

      default: break;
    }

    IF_DISABLED(BABYSTEPPING, sei());
  }

#endif // BABYSTEPPING

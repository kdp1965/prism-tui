// =======================================================
// PRISM Dutymeter chroma
//
// This is a Chroma (personality) for the TinyQV PRISM
// peripheral.  It integrates the HIGH time of an external
// PWM/PDM bitstream into count1, turning PRISM into the
// decimator half of a 1-bit ADC: the host delta-polls the
// counter at audio rate and each delta IS a PCM sample.
//
//   duty = (count1_prev - count1_now) / (rdtime delta in clocks)
//
// The intended source is the PWL synth's audio PWM looped
// back with a jumper, so the CPU can decode / display what
// the other synth is singing without owning uo_out[7].
//
// Operation:
//
//   * STATE_IDLE parks the FSM until the host raises
//     host_in[0].  The entry arm to the measure state loads
//     count1 from the preload register (write 0xFFFFFF) so
//     the integral starts from a known top.
//
//   * STATE_MEASURE self-loops forever, decrementing count1
//     on every clock the input is HIGH.  One condition per
//     state is all the STEW format offers, and it is spent
//     on the input pin - so the state tests nothing else and
//     the decrement runs at the full clock rate.
//
//   * count1 SATURATES at zero (the RTL guards the decrement
//     with count1 != 0), so the HOST owns the reload: when a
//     poll sees count1 below a safety margin (~2^20), pulse
//     the FSM enable bit off/on.  The FSM restarts in IDLE,
//     sees host_in[0] still set, reloads count1 and re-enters
//     MEASURE within two clocks.  Reload right after a read
//     and re-read the new baseline - the delta math never
//     sees a discontinuity.
//
//   Host register use:
//     preload = 0xFFFFFF (written once at setup)
//     count1  = live integral (read reg 0x24 [23:0])
//     host_in[0] = arm bit (set once before enabling)
//     ctrl enable bit = reload trigger (off/on pulse)
//
// FSM pin assignments.
//   PRISM_SIGNAL    TT Pin        Function
//   ============    ===========   ======================
//   prism_in[0]     ui_in[0]      PWM/PDM input (jumper
//                                 from pwl-synth uo_out[7])
//
// =======================================================

`default_nettype none

module chroma_dutymeter
(
   input wire           clk,
   input wire           rst_n,         // Global reset active low
   input wire           fsm_enable,    // Global FSM enable active high

   // Input data
   input wire  [15:0]   in_data,       // Input data

   // Output data
   output wire [10:0]   out_data,      // Static State outputs
   output reg  [0:0]    cond_out,      // Conditional outputs
   output reg  [31:0]   ctrl_reg
);

   // Local FSM states
   localparam [2:0]  STATE_IDLE     = 3'h0;
   localparam [2:0]  STATE_MEASURE  = 3'h1;

   // Control Register State (everything at its quiet default)
   localparam [1:0]  SHIFT_IN_SEL       = 2'h0;  // Shift input not used
   localparam [1:0]  SHIFT_OUT_SEL      = 2'h0;  // Shift output not used
   localparam [1:0]  COND_OUT_SEL       = 2'h0;  // cond_out not used
   localparam [0:0]  LOAD4              = 1'b0;  // No count2 preload FIFO
   localparam [0:0]  LATCH_IN_OUT       = 1'b0;  // No input latching
   localparam [0:0]  SHIFT_EN           = 1'b0;  // No shift operation
   localparam [0:0]  SHIFT_DIR          = 1'b0;  // MSB first (unused)
   localparam [0:0]  SHIFT_24_EN        = 1'b0;  // 8-bit shift (unused)
   localparam [0:0]  FIFO_24            = 1'b0;  // 24-bit reg is a counter
   localparam [0:0]  COUNT2_DEC         = 1'b0;  // count2 not used
   localparam [0:0]  LATCH2             = 1'b0;  // No latch enable pin

   reg   [2:0]    curr_state, next_state;

   // =======================================================
   // Wires to map inputs
   // =======================================================
   wire [6:0]     pin_in;
   wire           shift_in_data;
   wire [1:0]     host_in;
   wire [1:0]     pin_compare;
   wire           count1_zero;
   wire           count2_equal;
   wire           shift_zero;
   wire           count2_eq_comm;

   // =======================================================
   // Wires to map outputs based on PRISM RTL
   // =======================================================
   reg  [5:0]     pin_out;
   reg            count1_dec;
   reg            count1_load;
   reg            count2_inc;
   reg            count2_clear;
   reg            shift_en;

   // =======================================================
   // Assign in_data bits to individual signals.
   // =======================================================
   assign pin_in               = in_data[6:0];
   assign shift_in_data        = in_data[7];
   assign host_in              = in_data[9:8];
   assign count1_zero          = in_data[10];
   assign count2_equal         = in_data[11];
   assign pin_compare          = in_data[13:12];
   assign shift_zero           = in_data[14];
   assign count2_eq_comm       = in_data[15];

   // Assign out_data to array
   assign out_data[5:0]        = pin_out;
   assign out_data[6]          = shift_en;
   assign out_data[7]          = count1_dec;
   assign out_data[8]          = count1_load;
   assign out_data[9]          = count2_inc;
   assign out_data[10]         = count2_clear;

   /*
   ==========================================================
   Clocked block to update current state
   ==========================================================
   */
   always @(posedge clk or negedge rst_n)
   begin
      if (~rst_n)
         curr_state <= 3'h0;
      else
      begin
         curr_state <= fsm_enable ? next_state : 'h0;
      end
   end

   /*
   ==========================================================
   Combinatorial block to set next state and drive out_data.
   ==========================================================
   */
   always @*
   begin
      // Default to staying in current state
      next_state = curr_state;

      // Default outputs
      pin_out        = 6'h0;
      count1_dec     = 1'b0;
      count1_load    = 1'b0;
      count2_inc     = 1'b0;
      count2_clear   = 1'b0;
      shift_en       = 1'b0;
      cond_out[0]    = 1'b0;
      ctrl_reg       = {18'h0, LATCH2, COUNT2_DEC, FIFO_24, SHIFT_24_EN, SHIFT_DIR, SHIFT_EN,
                        LATCH_IN_OUT, LOAD4, COND_OUT_SEL, SHIFT_OUT_SEL, SHIFT_IN_SEL};

      // =========================================================
      // State machine logic
      // =========================================================
      case (curr_state)

      STATE_IDLE:
         begin
            // Wait for the host arm bit; load the integral top on the
            // way into the measure loop
            if (host_in[0])
            begin
               count1_load = 1'b1;
               next_state = STATE_MEASURE;
            end
         end

      STATE_MEASURE:
         begin
            // Integrate HIGH time at full clock rate; the host reloads
            // the saturating counter via an enable off/on pulse
            if (pin_in[0])
               count1_dec = 1'b1;
         end

      endcase
   end

endmodule

// =======================================================
// PRISM Synth Chroma
//
// This is a Chroma (personality) for the TinyQV PRISM
// peripheral.  It implements a square wave tone generator
// with a PWM amplitude "DAC" for 80's style synthesizer
// sounds on the audio PMOD (200KHz low pass filter on
// uo_out[7]).
//
// Operation:
//
//   * count2 free-runs as the PWM carrier timebase.  The
//     8-bit comm register holds the DUTY value D (matched
//     with count2 == comm_data, a single cycle pulse that
//     is only tested in dedicated wait states) and the
//     count2_compare register holds the PERIOD value M
//     (matched with count2 >= compare, a level signal that
//     is safe to test anywhere).  With M = 62 the carrier
//     period is exactly M+2 = 64 cycles (1MHz at 64MHz).
//
//   * count1 is a 24-bit note oscillator: it decrements
//     every cycle, and when it reaches zero the FSM swaps
//     to a mirrored copy of the PWM loop with inverted
//     output polarity and reloads count1 from the preload
//     register.  Phase A drives duty (D+1)/(M+2), phase B
//     drives the exact complement, so after the PMOD's low
//     pass filter the result is a square wave at the note
//     frequency with amplitude proportional to (2(D+1)-(M+2)).
//     D = M/2 = 31 gives exact silence; D in [1..61] sweeps
//     the amplitude linearly through zero.
//
//   * Each phase swap raises the host interrupt (count2_clear
//     and count2_inc asserted together, with count2 already
//     at zero so the preserved count is harmless).  The host
//     can ignore it (steady square wave), or use it to
//     alternate two preload values (variable pulse width /
//     PWM sweep timbres) or load random periods (noise).
//
//   Host register use:
//     comm_data      = duty D (volume), byte write any time
//     count2_compare = period M = 62 (also rewritten by the
//                      host_toggle interrupt clear write)
//     preload        = note half period in clocks, ~32e6/freq
//
// FSM pin assignments.
//   PRISM_SIGNAL    TT Pin        Function
//   ============    ===========   ======================
//   prism_out[6]    uo_out[7]     Audio PWM output
//
// This assumes:
//   1. shift_en      = 0 (No shifting; out[6] is a plain pin)
//   2. shift_out_sel = 0 (uo_out[7] driven by out[6] directly)
//   3. fifo_24       = 0 (count1 is the 24-bit down counter)
//   4. count2_dec    = 0 (count2 only counts up / clears)
//   5. latch_in_out  = 0, latch3 = 0, load4 = 0, cond unused
//
// Diagram of our "circuit" for the synth:
//
//  +-------------------------------+
//  |                               |
//  |            PRISM              |
//  |                               |
//  |  +-------------+              |
//  |  |   24-bit    |  note        |
//  |  |  preload    |  half-period |
//  |  +------+------+              |
//  |         v load on phase swap  |
//  |  +-------------+              |      +------------+
//  |  |   24-bit    |  zero =      |      |   Audio    |
//  |  |  count1 dec |  swap phase  |      |   PMOD     |
//  |  +-------------+              |      | 200KHz LPF |
//  |                    uo_out[7]  |      |            |
//  |  +---------------+ ----------------->| Data       |
//  |  | 8-bit count2  |  PWM:      |      |            |
//  |  |  == comm  (D) |  duty by   |      +------------+
//  |  |  >= cmp   (M) |  phase     |
//  |  +---------------+            |
//  |                               |
//  |              host_interrupt +---------> (phase swap)
//  +-------------------------------+
//
// =======================================================

`default_nettype none

module chroma_synth
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

   // Local FSM states.  A phase: PWM duty (D+1)/(M+2) high.
   // B phase: inverted, (D+1)/(M+2) low.  The DECIDE states
   // test the note oscillator once per carrier period; GLUE
   // states restart the carrier and fire the interrupt on a
   // phase swap.
   localparam [2:0]  STATE_A_HIGH   = 3'h0;
   localparam [2:0]  STATE_A_LOW    = 3'h1;
   localparam [2:0]  STATE_A_DECIDE = 3'h2;
   localparam [2:0]  STATE_A_GLUE   = 3'h3;
   localparam [2:0]  STATE_B_LOW    = 3'h4;
   localparam [2:0]  STATE_B_HIGH   = 3'h5;
   localparam [2:0]  STATE_B_DECIDE = 3'h6;
   localparam [2:0]  STATE_B_GLUE   = 3'h7;

   // Control Register State
   localparam [1:0]  SHIFT_IN_SEL       = 2'h0;  // Shift input not used
   localparam [1:0]  SHIFT_OUT_SEL      = 2'h0;  // uo_out[7] = out[6] directly
   localparam [1:0]  COND_OUT_SEL       = 2'h0;  // cond_out not used
   localparam [0:0]  LOAD4              = 1'b0;  // No comm load from preload
   localparam [0:0]  LATCH_IN_OUT       = 1'b0;  // Latched inputs not used
   localparam [0:0]  SHIFT_EN           = 1'b0;  // No shifting: out[6] is a pin
   localparam [0:0]  SHIFT_DIR          = 1'b0;  // Don't care
   localparam [0:0]  SHIFT_24_EN        = 1'b0;  // count1 is a counter
   localparam [0:0]  FIFO_24            = 1'b0;  // Not using 24-bit reg as FIFO
   localparam [0:0]  COUNT2_DEC         = 1'b0;  // No count2 decrement
   localparam [0:0]  LATCH2             = 1'b0;  // No input latching

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

   // =======================================================
   // Wires to map outputs based on PRISM RTL
   // =======================================================
   reg  [5:0]     pin_out;
   reg            audio;
   reg            count1_dec;
   reg            count1_load;
   reg            count2_inc;
   reg            count2_clear;
   wire           count2_eq_comm;

   // =======================================================
   // Assign in_data bits to individual signals.
   //
   // This assignment is specific to the application in which
   // the prism_fsm is being used.
   // =======================================================
   assign pin_in               = in_data[6:0];
   assign shift_in_data        = in_data[7];
   assign host_in              = in_data[9:8];
   assign count1_zero          = in_data[10];
   assign count2_equal         = in_data[11];    // count2 >= compare (M)
   assign pin_compare          = in_data[13:12];
   assign shift_zero           = in_data[14];
   assign count2_eq_comm       = in_data[15];    // count2 == comm (D)

   // Assign out_data to array.  out[6] drives uo_out[7], the
   // audio PMOD pin (shifting disabled, shift_out_sel = 0).
   assign out_data[5:0]        = pin_out;
   assign out_data[6]          = audio;
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

      // Defaults outputs
      pin_out[5:0]   = 6'h0;
      count1_dec     = 1'b0;
      count1_load    = 1'b0;
      count2_inc     = 1'b0;
      count2_clear   = 1'b0;
      audio          = 1'b0;
      cond_out[0]    = 1'b0;
      ctrl_reg       = {18'h0, LATCH2, COUNT2_DEC, FIFO_24, SHIFT_24_EN, SHIFT_DIR, SHIFT_EN,
                        LATCH_IN_OUT, LOAD4, COND_OUT_SEL, SHIFT_OUT_SEL, SHIFT_IN_SEL};

      // =========================================================
      // State machine logic
      //
      // In tinyqv_periph, we have only 8 states...use them wisely
      // =========================================================
      case (curr_state)

      STATE_A_HIGH:
         begin
            // High portion of the phase A carrier period.  count2
            // runs 1..D here (the A_GLUE transition set it to 1).
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            // Duty count reached: drop the output for the rest
            // of the carrier period
            if (count2_eq_comm)
            begin
               audio      = 1'b0;
               next_state = STATE_A_LOW;
            end
         end

      STATE_A_LOW:
         begin
            // Low portion of the phase A carrier period, count2
            // runs D+1..M
            audio      = 1'b0;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            // Period count reached: restart the carrier with a
            // clean count2 = 0 and go test the note oscillator
            if (count2_equal)
            begin
               count2_inc   = 1'b0;
               count2_clear = 1'b1;
               next_state   = STATE_A_DECIDE;
            end
         end

      STATE_A_DECIDE:
         begin
            // First high cycle of the next carrier period.  count2
            // holds at 0 for this cycle.
            audio      = 1'b1;
            count1_dec = 1'b1;

            // Note oscillator expired: swap to phase B (inverted
            // polarity), reload the half period counter and raise
            // the host interrupt (count2_clear + count2_inc with
            // count2 already 0, so the preserved count is clean)
            if (count1_zero)
            begin
               audio        = 1'b0;
               count1_dec   = 1'b0;
               count1_load  = 1'b1;
               count2_clear = 1'b1;
               count2_inc   = 1'b1;
               next_state   = STATE_B_LOW;
            end
            else
               next_state = STATE_A_GLUE;
         end

      STATE_A_GLUE:
         begin
            // Second high cycle of the carrier period; get count2
            // counting again (0 -> 1) and rejoin the A carrier loop
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;
            next_state = STATE_A_HIGH;
         end

      STATE_B_LOW:
         begin
            // Low portion of the phase B carrier period (mirror
            // of A_HIGH), count2 runs 1..D
            audio      = 1'b0;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            // Duty count reached: raise the output for the rest
            // of the carrier period
            if (count2_eq_comm)
            begin
               audio      = 1'b1;
               next_state = STATE_B_HIGH;
            end
         end

      STATE_B_HIGH:
         begin
            // High portion of the phase B carrier period, count2
            // runs D+1..M
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            // Period count reached: restart the carrier.  This
            // transition cycle stays high, mirroring the low
            // A_LOW exit cycle, so the two phases are exact
            // complements of each other.
            if (count2_equal)
            begin
               count2_inc   = 1'b0;
               count2_clear = 1'b1;
               next_state   = STATE_B_DECIDE;
            end
         end

      STATE_B_DECIDE:
         begin
            // First low cycle of the next carrier period (mirror
            // of A_DECIDE)
            audio      = 1'b0;
            count1_dec = 1'b1;

            // Note oscillator expired: swap back to phase A
            if (count1_zero)
            begin
               audio        = 1'b1;
               count1_dec   = 1'b0;
               count1_load  = 1'b1;
               count2_clear = 1'b1;
               count2_inc   = 1'b1;
               next_state   = STATE_A_HIGH;
            end
            else
               next_state = STATE_B_GLUE;
         end

      STATE_B_GLUE:
         begin
            // Second low cycle of the carrier period; rejoin the
            // B carrier loop
            audio      = 1'b0;
            count2_inc = 1'b1;
            count1_dec = 1'b1;
            next_state = STATE_B_LOW;
         end

      default:
         begin
            // All others, go to phase A
            next_state = STATE_A_HIGH;
         end
      endcase
   end

endmodule

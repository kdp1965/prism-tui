// =======================================================
// PRISM PCM Chroma
//
// This is a Chroma (personality) for the TinyQV PRISM
// peripheral.  It implements a fixed 250KHz 8-bit PWM DAC
// with a programmable sample-rate interrupt, for PCM audio
// playback (e.g. ADPCM streamed from flash) on the audio
// PMOD (uo_out[7]).
//
// Operation:
//
//   * count2 free-runs as the PWM carrier.  The comm register
//     holds the PERIOD value M (matched with count2 == comm,
//     written once at setup; M = 254 gives a carrier period of
//     exactly M+2 = 256 clocks, 250KHz at 64MHz).  The
//     count2_compare register holds the 8-bit DUTY D (matched
//     with count2 >= compare - a LEVEL comparison, so a host
//     write landing mid-period can never miss the compare and
//     glitch a whole carrier period).  Pulse width is D+1
//     clocks: duty (D+1)/256.
//
//   * Because HOST_TOGGLE byte writes land in count2_compare
//     AND clear the interrupt, the playback interrupt handler
//     delivers the next PCM sample with a single byte store.
//
//   * count1 decrements every clock and provides the sample
//     clock: when it reaches zero the FSM reloads it from the
//     preload register and raises the interrupt (count2 is at
//     zero when it fires, so the carrier is undisturbed).  The
//     sample period is the preload rounded up to the carrier
//     grid: preload 1984 -> 2048 clocks = 31250Hz at 64MHz.
//
//   * Both FSM phases drive the SAME output polarity - the
//     A/B structure exists only to generate the per-sample
//     interrupt, the PWM itself is seamless across swaps.
//
//   Host register use:
//     comm_data      = carrier period M = 254 (written once)
//     count2_compare = duty D (every sample, via HOST_TOGGLE)
//     preload        = sample period in clocks (rounds up to
//                      a multiple of M+2)
//
// FSM pin assignments.
//   PRISM_SIGNAL    TT Pin        Function
//   ============    ===========   ======================
//   prism_out[6]    uo_out[7]     PWM audio output
//
// =======================================================

`default_nettype none

module chroma_pcm
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

   // Local FSM states.  A and B are identical PWM loops with the same
   // polarity; the swap between them raises the sample interrupt.
   localparam [2:0]  STATE_A_HIGH   = 3'h0;
   localparam [2:0]  STATE_A_LOW    = 3'h1;
   localparam [2:0]  STATE_A_DECIDE = 3'h2;
   localparam [2:0]  STATE_A_GLUE   = 3'h3;
   localparam [2:0]  STATE_B_HIGH   = 3'h4;
   localparam [2:0]  STATE_B_LOW    = 3'h5;
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
   // =======================================================
   assign pin_in               = in_data[6:0];
   assign shift_in_data        = in_data[7];
   assign host_in              = in_data[9:8];
   assign count1_zero          = in_data[10];
   assign count2_equal         = in_data[11];    // count2 >= compare (DUTY)
   assign pin_compare          = in_data[13:12];
   assign shift_zero           = in_data[14];
   assign count2_eq_comm       = in_data[15];    // count2 == comm (PERIOD)

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

      case (curr_state)

      STATE_A_HIGH:
         begin
            // High portion of the carrier period, count2 runs 1..D.
            // The duty compare is >= so a mid-period host write can
            // only shorten or lengthen this pulse, never lose it.
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            if (count2_equal)
            begin
               audio      = 1'b0;
               next_state = STATE_A_LOW;
            end
         end

      STATE_A_LOW:
         begin
            // Low portion of the carrier period, count2 runs D+1..M
            audio      = 1'b0;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            if (count2_eq_comm)
            begin
               count2_inc   = 1'b0;
               count2_clear = 1'b1;
               next_state   = STATE_A_DECIDE;
            end
         end

      STATE_A_DECIDE:
         begin
            // First high cycle of the next carrier period; count2 holds 0
            audio      = 1'b1;
            count1_dec = 1'b1;

            // Sample clock expired: swap to the (identical) B loop,
            // reload the sample counter and raise the interrupt
            if (count1_zero)
            begin
               audio        = 1'b1;
               count1_dec   = 1'b0;
               count1_load  = 1'b1;
               count2_clear = 1'b1;
               count2_inc   = 1'b1;
               next_state   = STATE_B_HIGH;
            end
            else
               next_state = STATE_A_GLUE;
         end

      STATE_A_GLUE:
         begin
            // Second high cycle; restart count2 and rejoin the loop
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;
            next_state = STATE_A_HIGH;
         end

      STATE_B_HIGH:
         begin
            // Identical to A_HIGH - same polarity, seamless PWM
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            if (count2_equal)
            begin
               audio      = 1'b0;
               next_state = STATE_B_LOW;
            end
         end

      STATE_B_LOW:
         begin
            audio      = 1'b0;
            count2_inc = 1'b1;
            count1_dec = 1'b1;

            if (count2_eq_comm)
            begin
               count2_inc   = 1'b0;
               count2_clear = 1'b1;
               next_state   = STATE_B_DECIDE;
            end
         end

      STATE_B_DECIDE:
         begin
            audio      = 1'b1;
            count1_dec = 1'b1;

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
            audio      = 1'b1;
            count2_inc = 1'b1;
            count1_dec = 1'b1;
            next_state = STATE_B_HIGH;
         end

      default:
         begin
            next_state = STATE_A_HIGH;
         end
      endcase
   end

endmodule

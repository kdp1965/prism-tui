// =======================================================
// PRISM GPIO-24 Chroma
//
// This is a Chroma (personality) for the TinyQV PRISM
// peripheral. It implements a 24-input / 24-output
// I/O expander assuming you have connected 3 74165
// input shift registers and 3 74595 output shift register
// chips externally.
//
// FSM Deined pin assignments.
//   PRISM_SIGNAL    TT Pin        Function
//   ============    ===========   ======================
//   prism_in[0]     ui_in[0]      CSB
//   prism_in[1]     ui_in[1]      SCLK In
//   prism_in[2]     ui_in[2]      MOSI In
//   cond_out[0]     uo_out[2]     MISO Out
//
// This assumes:
//   1. shift_in_sel  = 2 (Shift input data on ui_in[2])
//   2. shift_24_le   = 0 (Enable 8-bit shift)
//   3. shift_en      = 1 (Enable shift operation)
//   4. shift_dir     = 0 (MSB first)
//   5. shift_out_sel = 1 (Route shift_data to uo_out[2])
//   6. fifo_24       = 1 (Not using 24-bit reg as FIFO)
//   7. latch_in_out  = 0 (Readback latched out data [6:1])
//   8. cond_sel      = 0 (cond_out not used)
// 
// We will use:
//
// Diagram of our "circuit" for SPI Slave
//                                                                             
//  +---------------------------+                          
//  |                           |      +------------+       
//  |          PRISM            |      |            |      
//  |                           |      |     SPI    |      
//  |                           |      |    Master  |      
//  |   +----------+            |      |            |      
//  |   |          |      in[0] |<-----+ CSB        |      
//  |   |   FIFO   |            |      |            |      
//  |   |          |      in[1] |<-----+ SCLK       |      
//  |   +-------+--+            |      |            |      
//  |      ^    |         in[2] |<-----+ MOSI       |      
//  |      |    v               |      |            |      
//  |  +---+----------+  out[2] +----->|            |      
//  |  |  comm_data   |         |      |            |         
//  |  +--------------+         |      +------------+         
//  |                           |
//  |                           |
//  |            host_interrupt +---------> 
//  +---------------------------+
//
//       ___                                                   ___
// CSB      |_________________________________________________|
//             __    __    __    __    __    __    __    __   
// SCLK  _____|  |__|  |__|  |__|  |__|  |__|  |__|  |__|  |__
//             ___________             _____             _____
// MOSI  _____|           |___________|     |___________|     |___
//                   ___________                   _____
// MISO  ___________|           |_________________|     |_________
//
// =======================================================

`default_nettype none

module chroma_spislave
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
   localparam [2:0]  STATE_IDLE                 = 3'h0;
   localparam [2:0]  STATE_CHECK_SCLK           = 3'h1;
   localparam [2:0]  STATE_CHECK_SCLK2          = 3'h2;
   localparam [2:0]  STATE_CHECK_CSB_DEASSERT1  = 3'h3;
   localparam [2:0]  STATE_AWAIT_SCLK_FALLING   = 3'h4;
   localparam [2:0]  STATE_CHECK_CSB_DEASSERT2  = 3'h5;
   localparam [2:0]  STATE_CHECK_COUNT          = 3'h6;
   localparam [2:0]  STATE_GEN_INTERRUPT        = 3'h7;

   // Control Register State
   localparam [1:0]  SHIFT_IN_SEL       = 2'h2;  // Shift input data on ui_in[2]
   localparam [1:0]  SHIFT_OUT_SEL      = 2'h0;  // Not using shift out
   localparam [1:0]  COND_OUT_SEL       = 2'h1;  // Route cond_out to uo_out[2]
   localparam [0:0]  LOAD4              = 1'b1;  // Not using out[4] to load from count2_preload FIFO 
   localparam [1:0]  COND_OUT_SEL       = 2'h1;  // Route cond_out to uo_out[2]
   localparam [0:0]  LATCH_IN_OUT       = 1'b0;  // Readback latched in data {shift_data,ui_in[0]}
   localparam [0:0]  SHIFT_EN           = 1'b1;  // Enable shift operation
   localparam [0:0]  SHIFT_DIR          = 1'b0;  // MSB first
   localparam [0:0]  SHIFT_24_EN        = 1'b0;  // Enable 24-bit shift
   localparam [0:0]  FIFO_24            = 1'b1;  // Using 24-bit reg as FIFO
   localparam [0:0]  COUNT2_DEC         = 1'b0;  // No count2 decrement
   localparam [0:0]  LATCH2             = 1'b1;  // Use prism_out[2] as input latch enable

   localparam integer   PIN_CSB     = 0;
   localparam integer   PIN_SCLK    = 1;
   localparam integer   PIN_LATCH2  = 2;
   localparam integer   PIN_LOAD    = 4;

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
   reg  [1:0]     latched_out;
   reg            count1_dec;
   reg            count1_load;
   reg            count2_inc;
   reg            count2_clear;
   reg            shift_en;
   wire           count2_eq_comm;

   wire           shift_out_reg;

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

   // Chroma specific pin assignments
   assign shift_out_reg         = in_data[13];
   
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
      pin_out[5:0]   = 5'h0;
      count1_dec     = 1'b0;
      count1_load    = 1'b0;
      count2_inc     = 1'b0;
      count2_clear   = 1'b0;
      shift_en       = 1'b0;
      cond_out[0]    = 1'b0;
      ctrl_reg       = {18'h0, LATCH2, COUNT2_DEC, FIFO_24, SHIFT_24_EN, SHIFT_DIR, SHIFT_EN,
                        LATCH_IN_OUT, LOAD4, COND_OUT_SEL, SHIFT_OUT_SEL, SHIFT_IN_SEL};

      // Send registered shift_out data to cond_out so it says steady during
      // shift operation at falling SCLK edge.
      if (shift_out_reg)
          cond_out[0]    = 1'b1;

      // =========================================================
      // State machine logic
      //
      // In tinyqv_periph, we have only 8 states...use them wisely
      // =========================================================
      case (curr_state)

      STATE_IDLE:
         begin
            // Detect falling CSB
            if (!pin_in[PIN_CSB])
            begin
               // Go to wait for SCLK
               next_state = STATE_CHECK_SCLK;
            end
         end

      STATE_CHECK_SCLK:
         begin
            // Test for rising SCLK on first bit of byte
            if (pin_in[PIN_SCLK] & shift_zero)
            begin
               // Load count1_preload "FIFO" byte to comm_data
               pin_out[PIN_LOAD]   = 1'b1;
            end

            next_state = STATE_CHECK_SCLK2;
         end

      STATE_CHECK_SCLK2:
         begin
            // Test for rising SCLK
            if (pin_in[PIN_SCLK])
            begin
               // Register the output shift_data to it remains stable during
               // the shift operation at the falling SCLK edge.
               pin_out[PIN_LATCH2] = 1'b1;

               // Go check the shift_coun
               next_state = STATE_AWAIT_SCLK_FALLING;
            end
            else
            begin
               // No rising SCLK ... go check for end of transaction
               next_state = STATE_CHECK_CSB_DEASSERT1;
            end
         end
      
      STATE_CHECK_CSB_DEASSERT1:
         begin
            // Test if CSB is high
            if (pin_in[PIN_CSB])
               next_state = STATE_IDLE;
         end

      STATE_AWAIT_SCLK_FALLING:
         begin
            // Wait for CSB to go low
            if (!pin_in[PIN_SCLK])
            begin
               // Shift data into comm_data
               shift_en = 1'b1;

               // Go wait for next SCLK
               next_state = STATE_CHECK_COUNT;

            end
            else
               // Go check if CSB was deasserted
               next_state = STATE_CHECK_CSB_DEASSERT2;
         end

      STATE_CHECK_CSB_DEASSERT2:
         begin
            // Test if CSB is high
            if (pin_in[PIN_CSB])
               next_state = STATE_IDLE;
         end

      STATE_CHECK_COUNT:
         begin
            // Check if the shift count is zero (byte received)
            if (!shift_zero)
               // Not 8 bits yet, go wati for next SCLK
               next_state = STATE_CHECK_SCLK2;
            else
               // Byte received, go generate interrupt to host
               next_state = STATE_GEN_INTERRUPT;
         end

      STATE_GEN_INTERRUPT:
         begin
            // Push byte to FIFO while generating interrupt
            count2_clear = 1'b1;    // Setting both generates interrupt
            count2_inc   = 1'b1;

            // Count1 load becomes FIFO write in FIFO mode
            count1_load  = 1'b1;

            // Load next TX value from FIFO
            pin_out[PIN_LOAD] = 1'b1;

            // Now go wait for CSB to go low
            next_state = STATE_CHECK_SCLK;
         end

      default:
         begin
            // All others, go to IDLE state
            next_state = STATE_IDLE;
         end
      endcase
   end

endmodule


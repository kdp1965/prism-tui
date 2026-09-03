// =======================================================
// PRISM Encoder chroma
//
// This is a Chroma (personality) for the TinyQV PRISM
// peripheral. It implements a rotary encoder that increments
// or decrements the count in count2 depending on the direction
// and speed of rotation.
//
// FSM Deined pin assignments.
//   PRISM_SIGNAL    TT Pin        Function
//   ============    ===========   ======================
//   prism_in[0]     ui_in[0]      Rotary input a
//   prism_in[1]     ui_in[1]      Rotary input b
//
// This assumes:
//   1. shift_in_sel  = 0 (Shift input data on ui_in[2])
//   2. shift_24_le   = 0 (Enable 8-bit shift)
//   3. shift_en      = 0 (Disalbe shift operation)
//   4. shift_dir     = 0 (MSB first)
//   5. shift_out_sel = 0 (Route shift_data to uo_out[2])
//   6. fifo_24       = 0 (Not using 24-bit reg as FIFO)
//   7. latch_in_out  = 0 (Readback latched in data {shift_data, cond_out})
//   8. cond_sel      = 0 (cond_out not used)
// 
// We will use:
//
// Diagram of our "circuit" for SPI Slave
//                                                                             
//  +-----------------------------+                          
//  |                             |      +------------+       
//  |            PRISM            |      |            |      
//  |                             |      |  Rotary    |      
//  |                             |      |  Encoder   |      
//  |  +-------------+    ui_in[0]|      |            |      
//  |  |   24-bit    |<-----------|------+ Output A   |      
//  |  |  debounce   |    ui_in[1]|      |            |      
//  |  |   counter   |<-----------|------+ Output B   |      
//  |  +------+------+            |      |            |      
//  |         |                   |      +------------+      
//  |         v                   |
//  |  +----------------------+   |                          
//  |  | 8-bit up/dn Counter  |   |                          
//  |  | used to track pulses |   |                             
//  |  | and rotation dir.    |   |                             
//  |  +----------------------+   |                    
//  |                             |                    
//  |              host_interrupt +---------> 
//  +-----------------------------+
//
// =======================================================

`default_nettype none

module chroma_encoder
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
   localparam [2:0]  STATE_DEBOUNCE_RISING      = 3'h1;
   localparam [2:0]  STATE_DEBOUNCE_RISING2     = 3'h2;
   localparam [2:0]  STATE_DECREMENT            = 3'h3;
   localparam [2:0]  STATE_INCREMENT            = 3'h4;

   // Control Register State
   localparam [1:0]  SHIFT_IN_SEL       = 2'h0;  // Shift input data on ui_in[2]
   localparam [1:0]  SHIFT_OUT_SEL      = 2'h0;  // Not using shift out
   localparam [1:0]  COND_OUT_SEL       = 2'h0;  // Route cond_out to uo_out[2]
   localparam [0:0]  LOAD4              = 1'b0;  // Not using out[4] to load from count2_preload FIFO 
   localparam [0:0]  LATCH_IN_OUT       = 1'b0;  // Readback latched in data {shift_data,cond_out[0]}
   localparam [0:0]  SHIFT_EN           = 1'b0;  // Enable shift operation
   localparam [0:0]  SHIFT_DIR          = 1'b0;  // MSB first
   localparam [0:0]  SHIFT_24_EN        = 1'b0;  // Enable 24-bit shift
   localparam [0:0]  FIFO_24            = 1'b0;  // Using 24-bit reg as FIFO
   localparam [0:0]  COUNT2_DEC         = 1'b1;  // Enable count2 decrement
   localparam [0:0]  LATCH2             = 1'b1;  // Use prism_out[2] as input latch enable

   localparam integer   PIN_DATA        = 0;
   localparam integer   PIN_LATCH2      = 2;
   localparam integer   PIN_COUNT2_DEC  = 5;

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

   wire           pin0_in_prev;

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
   assign pin0_in_prev          = in_data[12];
   
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

      // Use cond_out to reflect pin_in[0] so we can latch it and 
      // detect transitions
      if (pin_in[0])
          cond_out[0]= 1'b1;

      // =========================================================
      // State machine logic
      //
      // In tinyqv_periph, we have only 8 states...use them wisely
      // =========================================================
      case (curr_state)

      STATE_IDLE:
         begin
            // Detect rising edge of input A
            if (pin_in[0] != pin0_in_prev)
            begin
               // Load the debounce count to count1
               count1_load = 1'b1;

               // Latch pin_in[0] state (through cond_out)
               pin_out[PIN_LATCH2] = 1'b1;

               // Go wait for debounce count to expire
               next_state = STATE_DEBOUNCE_RISING;
            end
         end

      STATE_DEBOUNCE_RISING:
         begin
            // Decrement count1 debounce counter
            count1_dec = 1'b1;

            // Test if count1 count is zero
            if (count1_zero)
               next_state = STATE_DECREMENT;
            else
               next_state = STATE_DEBOUNCE_RISING2;
         end

      STATE_DEBOUNCE_RISING2:
         begin
            // Continue decrementing count1 debounce counter
            count1_dec = 1'b1;

            // Test if input A went low again
            if (pin_in[0] != pin0_in_prev)
            begin
               // Latch pin_in[0] state (through cond_out)
               pin_out[PIN_LATCH2] = 1'b1;

               // Go check the shift_coun
               next_state = STATE_IDLE;
            end
         end
      
      STATE_DECREMENT:
         begin
            // Test if we need to decrement count2
            if (pin_in[0] == pin_in[1])
            begin
               // Decrement count2 
               pin_out[PIN_COUNT2_DEC] = 1'b1;
               next_state = STATE_IDLE;
            end
            else
               next_state = STATE_INCREMENT;
         end

      STATE_INCREMENT:
         begin
            // Increment count2
            count2_inc = 1'b1;
            next_state = STATE_IDLE;
         end

      endcase
   end

endmodule


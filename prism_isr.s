# Raw PRISM interrupt handler for play_dsm_l4z (see play.c).
#
# Defining tqv_user_interrupt08_raw overrides the weak default in the SDK
# start.s.  Entry state (see start.s): only s1, a0, a1 are saved (to the
# scratch area, by the short_isr_entry custom op), there is NO stack,
# a0 = mcause and s1 = mcause << 2.  Return with short_isr_exit.
#
# Fast path (streaming active): feed the PRISM the next unpacked 24-bit
# DSM sample from the ping-pong buffers - one aligned word load, one
# PRELOAD store and one HOST_TOGGLE byte store (which latches the sample
# and clears the interrupt; the byte value lands in count2_compare, which
# the DSM chroma does not use).  Buffer swap and the consumed flag are
# handled here so the foreground only ever refills.
#
# When l4z_ch.active is 0 this falls through to the SDK's default
# behaviour (full context save, dispatch to the C tqv_user_interrupt08)
# so 'irq en', selftest and the rest keep working.
#
# struct l4z_channel field offsets (must match play.c):
#   0  rd          next sample the ISR will play
#   4  end         end of the current buffer
#   8  next        buffer to switch to when rd reaches end
#   12 next_end
#   16 consumed    set to 1 on buffer switch (foreground clears)
#   20 active      fast path enable

.include "macros.s"

.section .text

.globl tqv_user_interrupt08_raw
tqv_user_interrupt08_raw:
    lui  a0, %hi(l4z_ch)
    lw   a1, %lo(l4z_ch+20)(a0)     # streaming mode: 0 off, 1 DSM words,
    beqz a1, 9f                     #   2 = PCM duty bytes
    addi a1, a1, -1
    bnez a1, 20f

    # ---- mode 1: 24-bit DSM sample words (play_dsm_l4z) ----
    lw   a1, %lo(l4z_ch+0)(a0)      # a1 = rd
    lw   s1, 0(a1)                  # s1 = next 24-bit sample
    addi a1, a1, 4
    sw   a1, %lo(l4z_ch+0)(a0)
    lw   a0, %lo(l4z_ch+4)(a0)      # a0 = end
    beq  a1, a0, 2f
1:
    lui  a1, 0x08000                # PRISM at peripheral base + 0x200
    sw   s1, 0x220(a1)              # PRELOAD = sample
    sb   s1, 0x221(a1)              # HOST_TOGGLE: latch + clear interrupt
    short_isr_exit

2:  # buffer drained - switch to the one the foreground prepared
    lui  a0, %hi(l4z_ch)
    lw   a1, %lo(l4z_ch+8)(a0)      # rd   = next
    sw   a1, %lo(l4z_ch+0)(a0)
    lw   a1, %lo(l4z_ch+12)(a0)     # end  = next_end
    sw   a1, %lo(l4z_ch+4)(a0)
    li   a1, 1
    sw   a1, %lo(l4z_ch+16)(a0)     # consumed = 1 for the foreground
    j    1b

    # ---- mode 2: 8-bit PWM duty bytes (play_adpcm, PCM chroma) ----
    # The PCM chroma takes its duty from count2_compare, so ONE byte
    # write to HOST_TOGGLE delivers the sample AND clears the interrupt.
20: lw   a1, %lo(l4z_ch+0)(a0)      # a1 = rd
    lbu  s1, 0(a1)                  # s1 = next duty byte
    addi a1, a1, 1
    sw   a1, %lo(l4z_ch+0)(a0)
    lw   a0, %lo(l4z_ch+4)(a0)      # a0 = end
    beq  a1, a0, 22f
21:
    lui  a1, 0x08000
    sb   s1, 0x221(a1)              # duty -> compare, irq cleared
    short_isr_exit

22: # buffer drained - switch, same handshake as mode 1
    lui  a0, %hi(l4z_ch)
    lw   a1, %lo(l4z_ch+8)(a0)
    sw   a1, %lo(l4z_ch+0)(a0)
    lw   a1, %lo(l4z_ch+12)(a0)
    sw   a1, %lo(l4z_ch+4)(a0)
    li   a1, 1
    sw   a1, %lo(l4z_ch+16)(a0)
    j    21b

9:  # not streaming: SDK default - full context, C handler dispatch
    addi a1, s1, 0xb0
    full_isr_entry
    lw   a1, (a1)
    jalr ra, (a1)
    isr_exit

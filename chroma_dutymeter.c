/*
==============================================================
PRISM Downloadable Configuration

Input:    chroma_dutymeter.sv
Config:   tinyqv.cfg
==============================================================
*/

#include <stdint.h>

const uint32_t chroma_dutymeter[] =
{
   0x000003c0, 0x0000e000, 
   0x000003c0, 0x0000c000, 
   0x000003c0, 0x0000a000, 
   0x000003c0, 0x00008000, 
   0x000003c0, 0x00006000, 
   0x000003c0, 0x00004000, 
   0x00000384, 0x00002000, 
   0x00000288, 0x00002010, 

};
const uint32_t chroma_dutymeter_count   = 8;
const uint32_t chroma_dutymeter_width   = 44;
const uint32_t chroma_dutymeter_ctrlReg = 0x00000000;

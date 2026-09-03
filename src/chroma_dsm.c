/*
==============================================================
PRISM Downloadable Configuration

Input:    chroma_dsm.sv
Config:   tinyqv.cfg
==============================================================
*/

#include <stdint.h>

const uint32_t chroma_dsm[] =
{
   0x00000bc0, 0x00001000, 
   0x00000bc0, 0x00001000, 
   0x00000bc0, 0x00001000, 
   0x00000bc0, 0x00001000, 
   0x000009b8, 0x24003191, 
   0x00000960, 0x0400301d, 
   0x00000a92, 0x0200501e, 
   0x000009b8, 0x24003190, 

};
const uint32_t chroma_dsm_count   = 8;
const uint32_t chroma_dsm_width   = 44;
const uint32_t chroma_dsm_ctrlReg = 0x0000250C;

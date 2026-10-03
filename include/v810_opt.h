///////////////////////////////////////////////////////////////
// File:  v810_dis.h
//
// Author:  Bob VanderClay (vandercl@umich.edu)
//
// Description:  Defines used in v810_dis.cpp(not)
//
//
// History:
// --------
// 8 Apr 1998 - Created
// 9 Apr 1998 - Version 1.0 completed
// Many mods byb David Tucker....
//

#ifndef V810_OPT_H_
#define V810_OPT_H_

#include "vb_types.h"

///////////////////////////////////////////////////////////////////
// Opcodes for V810 Instruction set
#define V810_OP_MOV     0x00
#define V810_OP_ADD     0x01
#define V810_OP_SUB     0x02
#define V810_OP_CMP     0x03
#define V810_OP_SHL     0x04
#define V810_OP_SHR     0x05
#define V810_OP_JMP     0x06
#define V810_OP_SAR     0x07
#define V810_OP_MUL     0x08
#define V810_OP_DIV     0x09
#define V810_OP_MULU    0x0A
#define V810_OP_DIVU    0x0B
#define V810_OP_OR      0x0C
#define V810_OP_AND     0x0D
#define V810_OP_XOR     0x0E
#define V810_OP_NOT     0x0F
#define V810_OP_MOV_I   0x10
#define V810_OP_ADD_I   0x11
#define V810_OP_SETF    0x12
#define V810_OP_CMP_I   0x13
#define V810_OP_SHL_I   0x14
#define V810_OP_SHR_I   0x15
#define V810_OP_CLI     0x16
#define V810_OP_SAR_I   0x17
#define V810_OP_TRAP    0x18
#define V810_OP_RETI    0x19
#define V810_OP_HALT    0x1A
      //0x1B
#define V810_OP_LDSR    0x1C
#define V810_OP_STSR    0x1D
#define V810_OP_SEI     0x1E
#define V810_OP_BSTR    0x1F  //Special Bit String Inst
      //0x20 - 0x27  // Lost to Branch Instructions
#define V810_OP_MOVEA   0x28
#define V810_OP_ADDI    0x29
#define V810_OP_JR      0x2A
#define V810_OP_JAL     0x2B
#define V810_OP_ORI     0x2C
#define V810_OP_ANDI    0x2D
#define V810_OP_XORI    0x2E
#define V810_OP_MOVHI   0x2F
#define V810_OP_LD_B    0x30
#define V810_OP_LD_H    0x31
#define V810_OP_MULI    0x32  // Unknown
#define V810_OP_LD_W    0x33
#define V810_OP_ST_B    0x34
#define V810_OP_ST_H    0x35
#define V810_OP_MACI    0x36
#define V810_OP_ST_W    0x37
#define V810_OP_IN_B    0x38
#define V810_OP_IN_H    0x39
#define V810_OP_CAXI    0x3A
#define V810_OP_IN_W    0x3B
#define V810_OP_OUT_B   0x3C
#define V810_OP_OUT_H   0x3D
#define V810_OP_FPP     0x3E  //Special Float Inst
#define V810_OP_OUT_W   0x3F

//      Branch Instructions ( Extended opcode only for Branch command)
//  Common instrcutions commented out

#define V810_OP_BV      0x40
#define V810_OP_BL      0x41
#define V810_OP_BE      0x42
#define V810_OP_BNH     0x43
#define V810_OP_BN      0x44
#define V810_OP_BR      0x45
#define V810_OP_BLT     0x46
#define V810_OP_BLE     0x47
#define V810_OP_BNV     0x48
#define V810_OP_BNL     0x49
#define V810_OP_BNE     0x4A
#define V810_OP_BH      0x4B
#define V810_OP_BP      0x4C
#define V810_OP_NOP     0x4D
#define V810_OP_BGE     0x4E
#define V810_OP_BGT     0x4F

//#define V810_OP_BC      0x41
//#define V810_OP_BZ      0x42
//#define V810_OP_BNC     0x49
//#define V810_OP_BNZ     0x4A

//  Bit String Subopcodes
#define V810_OP_SCH0BSU 0x00
#define V810_OP_SCH0BSD 0x01
#define V810_OP_SCH1BSU 0x02
#define V810_OP_SCH1BSD 0x03

#define V810_OP_ORBSU   0x08
#define V810_OP_ANDBSU  0x09
#define V810_OP_XORBSU  0x0A
#define V810_OP_MOVBSU  0x0B
#define V810_OP_ORNBSU  0x0C
#define V810_OP_ANDNBSU 0x0D
#define V810_OP_XORNBSU 0x0E
#define V810_OP_NOTBSU  0x0F

//  Floating Point Subopcodes
#define V810_OP_CMPF_S  0x00

#define V810_OP_CVT_WS  0x02
#define V810_OP_CVT_SW  0x03
#define V810_OP_ADDF_S  0x04
#define V810_OP_SUBF_S  0x05
#define V810_OP_MULF_S  0x06
#define V810_OP_DIVF_S  0x07
#define V810_OP_XB      0x08
#define V810_OP_XH      0x09
#define V810_OP_REV     0x0A
#define V810_OP_TRNC_SW 0x0B
#define V810_OP_MPYHW   0x0C

#endif // V810_OPT_H_

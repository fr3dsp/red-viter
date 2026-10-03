#ifndef DRC_CORE_H
#define DRC_CORE_H

#include "vb_types.h"
#include "v810_mem.h"

#if (defined(__arm__) && __ARM_ARCH >= 7) || defined(DRC_HOST_SIM)
#define DRC_AVAILABLE true
#else
#define DRC_AVAILABLE false
#endif

#define CACHE_SIZE 0x400000
#define DRC_MAX_V810_INST 48
#define DRC_BLOCK_SLACK 0x2000

enum {
    DRC_ERR_BAD_PC      = 2,
    DRC_ERR_BAD_INST    = 6,
};

extern bool drc_ready;

int drc_handleInterrupts(WORD cpsr, WORD *PC);
void drc_relocTable(void);
void drc_clearCache(void);
void drc_reopen_vm(void);
void drc_resume(void);

void drc_set_enabled(bool enabled);
bool drc_get_enabled(void);
bool drc_available(void);

void drc_init(void);
void drc_reset(void);
int drc_run(void);

#endif

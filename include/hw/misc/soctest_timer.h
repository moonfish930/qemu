#ifndef HW_MISC_SOCTEST_TIMER_H
#define HW_MISC_SOCTEST_TIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#include "qemu/bitops.h"

#define TYPE_SOCTEST_TIMER "soctest-timer"
OBJECT_DECLARE_SIMPLE_TYPE(SocTestTimerState, SOCTEST_TIMER)

#define SOCTEST_TIMER_MMIO_SIZE 0x1000

enum SocTestTimerReg {
    SOCTEST_TIMER_R_MAGIC   = 0x00,
    SOCTEST_TIMER_R_VERSION = 0x04,
    SOCTEST_TIMER_R_SCRATCH = 0x08,
    SOCTEST_TIMER_R_PERIOD  = 0x0C,
    SOCTEST_TIMER_R_CTRL    = 0x10,
    SOCTEST_TIMER_R_COUNT   = 0x14,
    SOCTEST_TIMER_R_TICKS   = 0x18,
    SOCTEST_TIMER_R_STATUS  = 0x1C,
    SOCTEST_TIMER_R_MAX     = 0x20,
};

#define SOCTEST_TIMER_NR_REGS (SOCTEST_TIMER_R_MAX / 4)

#define SOCTEST_TIMER_MAGIC    0x534F4354U   /* "SOCT" */
#define SOCTEST_TIMER_VERSION  0x00010000U

#define SOCTEST_TIMER_CTRL_EN      BIT(0)
#define SOCTEST_TIMER_CTRL_IRQ_EN  BIT(1)
#define SOCTEST_TIMER_STATUS_MATCH BIT(0)

struct SocTestTimerState {
    SysBusDevice parent_obj;

    //public
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t regs[SOCTEST_TIMER_NR_REGS];
};
 
#endif
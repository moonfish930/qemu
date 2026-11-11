#ifndef HW_MISC_SOCTEST_TIMER_H
#define HW_MISC_SOCTEST_TIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_SOCTEST_TIMER "soctest-timer"
OBJECT_DECLARE_SIMPLE_TYPE(SocTestTimerState, SOCTEST_TIMER)

#define SOCTEST_TIMER_MMIO_SIZE 0x1000

enum SocTestTimerReg {
    R_MAGIC   = 0x00,
    R_VERSION = 0x04,
    R_SCRATCH = 0x08,
    R_PERIOD  = 0x0C,
    R_CTRL    = 0x10,
    R_COUNT   = 0x14,
    R_TICKS   = 0x18,
    R_STATUS  = 0x1C,
    R_MAX     = 0x20,
};

#define SOCTEST_TIMER_NR_REGS (R_MAX / 4)

#define SOCTEST_TIMER_MAGIC    0x534F4354U   /* "SOCT" */
#define SOCTEST_TIMER_VERSION  0x00010000U

#define CTRL_EN      BIT(0)
#define CTRL_IRQ_EN  BIT(1)
#define STATUS_MATCH BIT(0)

struct SocTestTimerState {
    SysBusDevice parent_obj;

    //public
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t regs[SOCTEST_TIMER_NR_REGS];
};
 
#endif
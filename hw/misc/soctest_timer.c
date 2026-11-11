#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/misc/soctest_timer.h"

static void soctest_timer_update_irq(SocTestTimerState *s){
    bool level = (s->regs[SOCTEST_TIMER_R_CTRL / 4] & SOCTEST_TIMER_CTRL_EN) &&
                 (s->regs[SOCTEST_TIMER_R_CTRL / 4] & SOCTEST_TIMER_CTRL_IRQ_EN) &&
                 (s->regs[SOCTEST_TIMER_R_STATUS /4] & SOCTEST_TIMER_STATUS_MATCH);
    qemu_set_irq(s->irq, level);              
}

static void soctest_timer_expire(void *opaque){
    SocTestTimerState *s = opaque;

    s->regs[SOCTEST_TIMER_R_TICKS / 4] ++;
    s->regs[SOCTEST_TIMER_R_STATUS / 4] |= SOCTEST_TIMER_STATUS_MATCH;      // match位置1
    trace_soctest_timer_tick(s->regs[SOCTEST_TIMER_R_TICKS / 4]);
    soctest_timer_update_irq(s);
}

static void soctest_timer_load(SocTestTimerState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_period(s->timer, 1);                         /* 1 tick = 1 ns */
    ptimer_set_limit(s->timer, s->regs[SOCTEST_TIMER_R_PERIOD / 4], 1);    /* 计数值 = ns,reload=1 */
    ptimer_run(s->timer, 0);                                /* 0 = 周期模式 */
    ptimer_transaction_commit(s->timer);
}

static uint64_t soctest_timer_read(void *opaque, hwaddr offset, unsigned size){
    SocTestTimerState *s = opaque;
    uint64_t value = 0;

    switch(offset){
        case SOCTEST_TIMER_R_MAGIC:
            value = SOCTEST_TIMER_MAGIC;
            break;
        case SOCTEST_TIMER_R_VERSION:
            value = SOCTEST_TIMER_VERSION;
            break;
        case SOCTEST_TIMER_R_COUNT:    
            value = ptimer_get_count(s->timer);   // 实时计算当前定时器的剩余计数值
            break;
        case SOCTEST_TIMER_R_SCRATCH:
        case SOCTEST_TIMER_R_PERIOD:
        case SOCTEST_TIMER_R_CTRL:
        case SOCTEST_TIMER_R_TICKS:
        case SOCTEST_TIMER_R_STATUS:
            value = s->regs[offset / 4];
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);    
            return 0;               
    }

    trace_soctest_timer_read(offset, size, value);
    return value;
}

static void soctest_timer_write(void *opaque, hwaddr offset,
                                uint64_t value64, unsigned size){
    SocTestTimerState *s = opaque;
    uint32_t value = value64;

    trace_soctest_timer_write(offset, size, value);

    switch(offset){
        case SOCTEST_TIMER_R_MAGIC:
        case SOCTEST_TIMER_R_VERSION:
        case SOCTEST_TIMER_R_COUNT:    
        case SOCTEST_TIMER_R_TICKS:
            qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to RO reg 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
            return;
        case SOCTEST_TIMER_R_SCRATCH:
            s->regs[offset / 4] = value;
            return;
        case SOCTEST_TIMER_R_STATUS:
            /* W1C:写 1 清 MATCH */
            s->regs[offset / 4] &= ~value;
            soctest_timer_update_irq(s);
            return;
        case SOCTEST_TIMER_R_PERIOD:
            // 修改 ptimer 的状态（如频率、运行/停止）必须包裹在 ptimer_transaction_begin() 和 ptimer_transaction_commit() 之间
            if(value == 0){
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: period 0 is invalid, using 1\n", __func__);
                value = 1;
            }
            s->regs[SOCTEST_TIMER_R_PERIOD / 4] = value;
            soctest_timer_load(s);
            return;
        case SOCTEST_TIMER_R_CTRL:
            ptimer_transaction_begin(s->timer);
            {
                uint32_t old_ctrl = s->regs[SOCTEST_TIMER_R_CTRL / 4];
                s->regs[offset / 4] = value;

                if((old_ctrl & SOCTEST_TIMER_CTRL_EN) && !(value & SOCTEST_TIMER_CTRL_EN)){
                    ptimer_stop(s->timer);
                }else if(!(old_ctrl & SOCTEST_TIMER_CTRL_EN) && (value & SOCTEST_TIMER_CTRL_EN)){
                    ptimer_run(s->timer, 0);
                }
            }
            ptimer_transaction_commit(s->timer);

            soctest_timer_update_irq(s);
            return;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);    
            return;               
    }
}

static const MemoryRegionOps soctest_timer_ops = {
    .read = soctest_timer_read,
    .write = soctest_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void soctest_timer_reset_hold(Object *obj, ResetType type){
    SocTestTimerState *s = SOCTEST_TIMER(obj);

    s->regs[SOCTEST_TIMER_R_SCRATCH / 4] = 0;
    s->regs[SOCTEST_TIMER_R_PERIOD / 4]  = 1000000;
    s->regs[SOCTEST_TIMER_R_CTRL / 4]    = 0;
    s->regs[SOCTEST_TIMER_R_TICKS / 4]   = 0;
    s->regs[SOCTEST_TIMER_R_STATUS / 4]  = 0;
    
    soctest_timer_load(s);
    qemu_set_irq(s->irq, 0);
}

static void soctest_timer_realize(DeviceState *dev, Error **errp){
    SocTestTimerState *s = SOCTEST_TIMER(dev);

    s->timer = ptimer_init(soctest_timer_expire, s,
                       PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                       PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                       PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);

    memory_region_init_io(&s->iomem,
                          OBJECT(s),
                          &soctest_timer_ops,
                          s,
                          TYPE_SOCTEST_TIMER,
                          SOCTEST_TIMER_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);                      
}

static const VMStateDescription vmstate_soctest_timer = {
    .name = TYPE_SOCTEST_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SocTestTimerState, SOCTEST_TIMER_NR_REGS),
        VMSTATE_PTIMER(timer, SocTestTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void soctest_timer_class_init(ObjectClass *oc, const void *data){
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass  *rc = RESETTABLE_CLASS(oc);

    dc->desc = "socTest programmable timer";
    dc->realize = soctest_timer_realize;
    dc->vmsd = &vmstate_soctest_timer;
    rc->phases.hold = soctest_timer_reset_hold;
}

static const TypeInfo soctest_timer_typeinfo[] = {
    {
        .name = TYPE_SOCTEST_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SocTestTimerState),
        .class_init = soctest_timer_class_init,
    }
};

DEFINE_TYPES(soctest_timer_typeinfo)
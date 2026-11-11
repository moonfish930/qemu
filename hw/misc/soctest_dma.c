/*
 * socTest DMA engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 一个最小的 bus-master 设备,演示三件事:
 *   1) 设备如何通过 AddressSpace 主动读写 guest 内存
 *   2) 线性搬运 与 描述符(S/G)搬运
 *   3) 完成后如何用电平中断通知(配合 PLIC)
 *
 * 关键设计:所有内存访问都走 s->dma_as,而 dma_as 由 "dma-memory"
 * link 决定。板级现在指向 get_system_memory(),将来接 IOMMU 时只
 * 需要把同一个 link 换成 IOMMUMemoryRegion,设备代码一行都不用改。
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

#include "hw/misc/soctest_dma.h"

/* 中断是电平型的:IRQ_EN 且 STATUS 有 DONE/ERROR 时拉高 */
static void soctest_dma_update_irq(SocTestDMAState *s){
    uint32_t status = s->regs[SOCTEST_DMA_R_STATUS /4];

    // irq enable 且 status 有效
    bool level = (s->regs[SOCTEST_DMA_R_CTRL / 4] & SOCTEST_DMA_CTRL_IRQ_EN) &&
                 (status & (SOCTEST_DMA_STATUS_DONE | SOCTEST_DMA_STATUS_ERROR));
    qemu_set_irq(s->irq, level);             
}

/*
 * 搬运一段数据。返回 SocTestDMAReqStatus。
 * 注意:必须检查 MemTxResult —— 目标未映射、越界、被 IOMMU 拒绝
 * 都会在这里返回非 MEMTX_OK。
 */
static uint32_t soctest_dma_copy_one(SocTestDMAState *s, uint64_t src, uint64_t dst, uint32_t len){
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || len > SOCTEST_DMA_MAX_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid length %u (max %u)\n",
                      __func__, len, SOCTEST_DMA_MAX_LEN);
        return SOCTEST_DMA_DESC_LEN;
    }

    buf = g_malloc(len);

    if(address_space_read(&s->dma_as, src, MEMTXATTRS_UNSPECIFIED,
                          buf, len) != MEMTX_OK){
        trace_soctest_dma_fault(false, src, len);
        return SOCTEST_DMA_DESC_SRC;
    }

    if(address_space_write(&s->dma_as, dst, MEMTXATTRS_UNSPECIFIED,
                           buf, len) != MEMTX_OK){
        trace_soctest_dma_fault(true, src, len);
        return SOCTEST_DMA_DESC_DST;
    }

    trace_soctest_dma_copy(src, dst, len);
    return SOCTEST_DMA_DESC_OK;
}

/* 线性模式:SRC -> DST,共 LEN 字节 */
static uint32_t soctest_dma_run_single(SocTestDMAState *s)
{
    uint64_t src = ((uint64_t)s->regs[SOCTEST_DMA_R_SRC_HI / 4] << 32) |
                   s->regs[SOCTEST_DMA_R_SRC_LO / 4];
    uint64_t dst = ((uint64_t)s->regs[SOCTEST_DMA_R_DST_HI / 4] << 32) |
                   s->regs[SOCTEST_DMA_R_DST_LO / 4];
    uint32_t len = s->regs[SOCTEST_DMA_R_LEN / 4];

    if (soctest_dma_copy_one(s, src, dst, len) != SOCTEST_DMA_DESC_OK) {
        return SOCTEST_DMA_STATUS_ERROR;
    }
    return SOCTEST_DMA_STATUS_DONE;
}

/*
 * 描述符模式:描述符数组本身也在 guest 内存里,同样要通过 dma_as 读写。
 * 每个描述符的 status 字段会被回写,guest 可以据此判断哪一条失败。
 */
static uint32_t soctest_dma_run_desc(SocTestDMAState *s){
    uint64_t base = ((uint64_t)s->regs[SOCTEST_DMA_R_DESC_HI / 4] << 32) |
                    s->regs[SOCTEST_DMA_R_DESC_LO / 4];
    uint32_t num = s->regs[SOCTEST_DMA_R_NUM / 4];
    uint32_t ret = SOCTEST_DMA_STATUS_DONE;
    uint32_t i;

    if (num == 0 || num > SOCTEST_DMA_MAX_DESC) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid descriptor count %u (max %u)\n",
                      __func__, num, SOCTEST_DMA_MAX_DESC);
        return SOCTEST_DMA_STATUS_ERROR;
    }

    for(i = 0; i < num; i++){
        uint8_t raw[sizeof(SocTestDMADesc)];
        uint64_t addr = base + (uint64_t)i * sizeof(SocTestDMADesc);
        uint32_t code;
        
        if (address_space_read(&s->dma_as, addr, MEMTXATTRS_UNSPECIFIED,
                            raw, sizeof(raw)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: cannot read descriptor %u at 0x%" PRIx64 "\n",
                            __func__, i, addr);
            ret = SOCTEST_DMA_STATUS_ERROR;
            break;
        }

        /* 显式按小端解析,不依赖 host 字节序 */
        code = soctest_dma_copy_one(s,
                                    ldq_le_p(raw + 0),   /* src  */
                                    ldq_le_p(raw + 8),   /* dst  */
                                    ldl_le_p(raw + 16)); /* len  */

        stl_le_p(raw + 24, code);                  /* status 回写 */     
        if (address_space_write(&s->dma_as, addr, MEMTXATTRS_UNSPECIFIED,
                                raw, sizeof(raw)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cannot write back descriptor %u\n",
                          __func__, i);
            ret = SOCTEST_DMA_STATUS_ERROR;
            break;
        }
        
        trace_soctest_dma_desc(i, ldq_le_p(raw + 0), ldq_le_p(raw + 8),
                               ldl_le_p(raw + 16), code);
        if (code != SOCTEST_DMA_DESC_OK) {
            ret = SOCTEST_DMA_STATUS_ERROR;
        }                       
    }

    return ret;
}

static void soctest_dma_run(SocTestDMAState *s)
{
    uint32_t status;

    if (s->regs[SOCTEST_DMA_R_CTRL / 4] & SOCTEST_DMA_CTRL_DESC_MODE) {
        status = soctest_dma_run_desc(s);
    } else {
        status = soctest_dma_run_single(s);
    }

    /* 累积:guest 不清 STATUS 的话,旧的 DONE 会一直保留 */
    s->regs[SOCTEST_DMA_R_STATUS / 4] |= status;

    trace_soctest_dma_done(s->regs[SOCTEST_DMA_R_STATUS / 4]);
    soctest_dma_update_irq(s);
}

static uint64_t soctest_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    SocTestDMAState *s = opaque;
    uint64_t value = 0;

    // 保证32位对齐，最后两位一定是0x00
    if (!QEMU_IS_ALIGNED(offset, 4) || offset >= SOCTEST_DMA_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (offset) {
        case SOCTEST_DMA_R_SRC_LO:
        case SOCTEST_DMA_R_SRC_HI:
        case SOCTEST_DMA_R_DST_LO:
        case SOCTEST_DMA_R_DST_HI:
        case SOCTEST_DMA_R_LEN:
        case SOCTEST_DMA_R_CTRL:
        case SOCTEST_DMA_R_STATUS:
        case SOCTEST_DMA_R_DESC_LO:
        case SOCTEST_DMA_R_DESC_HI:
        case SOCTEST_DMA_R_NUM:
            value = s->regs[offset / 4];
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: unimplemented read offset 0x%" HWADDR_PRIx "\n",
                        __func__, offset);
            break;
    }

    trace_soctest_dma_read(offset, size, value);
    return value;
}

static void soctest_dma_write(void *opaque, hwaddr offset,
                              uint64_t value64, unsigned size)
{
    SocTestDMAState *s = opaque;
    uint32_t value = value64;

    if (!QEMU_IS_ALIGNED(offset, 4) || offset >= SOCTEST_DMA_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    trace_soctest_dma_write(offset, size, value);

    switch(offset){
        case SOCTEST_DMA_R_CTRL:
            /* START 是触发位,不保存 */
            s->regs[SOCTEST_DMA_R_CTRL / 4] = value & (SOCTEST_DMA_CTRL_IRQ_EN | SOCTEST_DMA_CTRL_DESC_MODE);
            if (value & SOCTEST_DMA_CTRL_START) {
                soctest_dma_run(s);
            } else {
                soctest_dma_update_irq(s);
            }
            return;
        case SOCTEST_DMA_R_STATUS:
            /* W1C:写 1 清 DONE/ERROR,清完把中断线放下来 */
            s->regs[SOCTEST_DMA_R_STATUS / 4] &= ~value;
            soctest_dma_update_irq(s);
            return;
        case SOCTEST_DMA_R_SRC_LO:
        case SOCTEST_DMA_R_SRC_HI:
        case SOCTEST_DMA_R_DST_LO:
        case SOCTEST_DMA_R_DST_HI:
        case SOCTEST_DMA_R_LEN:
        case SOCTEST_DMA_R_DESC_LO:
        case SOCTEST_DMA_R_DESC_HI:
        case SOCTEST_DMA_R_NUM:   
            s->regs[offset / 4] = value;
            return;    
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: unimplemented write offset 0x%" HWADDR_PRIx "\n",
                        __func__, offset);
            return;    
    }
}


static const MemoryRegionOps soctest_dma_ops = {
    .read = soctest_dma_read,
    .write = soctest_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void soctest_dma_reset_hold(Object *obj, ResetType type)
{
    SocTestDMAState *s = SOCTEST_DMA(obj);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}


static void soctest_dma_realize(DeviceState *dev, Error **errp){
    SocTestDMAState *s = SOCTEST_DMA(dev);

    if (!s->dma_mr) {
        error_setg(errp, TYPE_SOCTEST_DMA ": 'dma-memory' link not set");
        return;
    }

    address_space_init(&s->dma_as, s->dma_mr, TYPE_SOCTEST_DMA "-dma");

    memory_region_init_io(&s->iomem, OBJECT(s), &soctest_dma_ops,
                          s, TYPE_SOCTEST_DMA, SOCTEST_DMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);                      
}

static const VMStateDescription vmstate_soctest_dma = {
    .name = TYPE_SOCTEST_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SocTestDMAState, SOCTEST_DMA_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

// 相当于
// │ soctest_dma_properties[]（属性表）                   │
// │                                                     │
// │  {                                                  │
// │    .name   = "dma-memory",                          │
// │    .type   = TYPE_MEMORY_REGION,                    │
// │    .offset = offsetof(SocTestDMAState, dma_mr),     │
// │    .link   = MemoryRegion *                         │
// │  }    
// 如果 DMA 设备内部自己硬编码去寻找 system_memory，它就和特定的 Machine 紧密耦合了，
// 无法作为一个通用的、可复用的 IP 模块。 
// 通过 DEFINE_PROP_LINK，DMA 设备只知道：“只要有人给我配置了 dma-memory，我就往这个内存区域发起读写”。
// 至于这个内存区域背后是整个系统的内存、还是某个特定的 DMA 专用总线（Bus），DMA 设备本身并不关心。
// 实际上machine代码通过object_property_set_link注入 dma_mr
static const Property soctest_dma_properties[] = {
    DEFINE_PROP_LINK("dma-memory", SocTestDMAState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void soctest_dma_class_init(ObjectClass *oc, const void *data){
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "socTest bus-master DMA engine";
    dc->realize = soctest_dma_realize;
    dc->vmsd = &vmstate_soctest_dma;
    device_class_set_props(dc, soctest_dma_properties);
    rc->phases.hold = soctest_dma_reset_hold;
}

static const TypeInfo soctest_dma_typeinfo[] = {
    {
        .name = TYPE_SOCTEST_DMA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SocTestDMAState),
        .class_init = soctest_dma_class_init,
    },
};

DEFINE_TYPES(soctest_dma_typeinfo)
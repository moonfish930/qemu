#ifndef HW_MISC_SOCTEST_DMA_H
#define HW_MISC_SOCTEST_DMA_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"
#include "qemu/bitops.h"

#define TYPE_SOCTEST_DMA "soctest-dma"
OBJECT_DECLARE_SIMPLE_TYPE(SocTestDMAState, SOCTEST_DMA)

#define SOCTEST_DMA_MMIO_SIZE 0x1000

/* 每次搬运的字节数上限(防止 guest 写个巨大 LEN 把 host 内存吃光) */
#define SOCTEST_DMA_MAX_LEN   0x1000
/* 单次描述符链的最大条目数 */
#define SOCTEST_DMA_MAX_DESC  64

enum SocTestDMAReg {
    SOCTEST_DMA_R_SRC_LO  = 0x00,   /* 源地址低 32 位 */
    SOCTEST_DMA_R_SRC_HI  = 0x04,   /* 源地址高 32 位 */
    SOCTEST_DMA_R_DST_LO  = 0x08,   /* 目的地址低 32 位 */
    SOCTEST_DMA_R_DST_HI  = 0x0C,   /* 目的地址高 32 位 */
    SOCTEST_DMA_R_LEN     = 0x10,   /* 线性模式:字节数 */
    SOCTEST_DMA_R_CTRL    = 0x14,   /* bit0 START(自清零触发), bit1 IRQ_EN, bit2 DESC_MODE */
    SOCTEST_DMA_R_STATUS  = 0x18,   /* bit0 DONE, bit1 ERROR(写 1 清除) */
    SOCTEST_DMA_R_DESC_LO = 0x1C,   /* 描述符数组地址低 32 位 */
    SOCTEST_DMA_R_DESC_HI = 0x20,   /* 描述符数组地址高 32 位 */
    SOCTEST_DMA_R_NUM     = 0x24,   /* 描述符数量 */
    SOCTEST_DMA_R_MAX     = 0x28,
};

#define SOCTEST_DMA_NR_REGS (SOCTEST_DMA_R_MAX / 4)

#define SOCTEST_DMA_CTRL_START      BIT(0)  /* 写 1 启动一次传输,不会被保存 */
#define SOCTEST_DMA_CTRL_IRQ_EN     BIT(1)
#define SOCTEST_DMA_CTRL_DESC_MODE  BIT(2)  /* 1 = 描述符模式,0 = 线性模式 */

#define SOCTEST_DMA_STATUS_DONE     BIT(0)
#define SOCTEST_DMA_STATUS_ERROR    BIT(1)


typedef struct SocTestDMADesc {
    uint64_t src;
    uint64_t dst;
    uint32_t len;
    uint32_t flags;   /* bit0: 预留(完成中断),当前忽略 */
    uint32_t status;  /* 设备回写:0=OK,1=源读失败,2=目的写失败,3=长度非法 */
    uint32_t rsvd;
} SocTestDMADesc;

enum SocTestDMAReqStatus {
    SOCTEST_DMA_DESC_OK  = 0,
    SOCTEST_DMA_DESC_SRC = 1,
    SOCTEST_DMA_DESC_DST = 2,
    SOCTEST_DMA_DESC_LEN = 3,
};

struct SocTestDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /*
     * 设备唯一的内存入口。板级通过 "dma-memory" link 注入:
     *   现在     -> get_system_memory()
     *   以后接IOMMU -> IOMMUMemoryRegion
     */
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    uint32_t regs[SOCTEST_DMA_NR_REGS];
};

#endif
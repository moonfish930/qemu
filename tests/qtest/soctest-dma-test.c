/*
 * QTest for the socTest DMA engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 覆盖:线性搬运 / 描述符链 / 两个错误路径(未映射目标、超长 LEN)
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "hw/misc/soctest_dma.h"

#define DDR_BASE      0x80000000
#define DMA_BASE      0x10004000
#define PLIC_BASE     0x0c000000

/* 寄存器偏移 / 位定义取自设备头文件 */
#define DMA_SRC_LO     SOCTEST_DMA_R_SRC_LO
#define DMA_SRC_HI     SOCTEST_DMA_R_SRC_HI
#define DMA_DST_LO     SOCTEST_DMA_R_DST_LO
#define DMA_DST_HI     SOCTEST_DMA_R_DST_HI
#define DMA_LEN        SOCTEST_DMA_R_LEN
#define DMA_CTRL       SOCTEST_DMA_R_CTRL
#define DMA_STATUS     SOCTEST_DMA_R_STATUS
#define DMA_DESC_LO    SOCTEST_DMA_R_DESC_LO
#define DMA_DESC_HI    SOCTEST_DMA_R_DESC_HI
#define DMA_NUM        SOCTEST_DMA_R_NUM

#define CTRL_START      SOCTEST_DMA_CTRL_START
#define CTRL_IRQ_EN     SOCTEST_DMA_CTRL_IRQ_EN
#define CTRL_DESC_MODE  SOCTEST_DMA_CTRL_DESC_MODE

#define STATUS_DONE     SOCTEST_DMA_STATUS_DONE
#define STATUS_ERROR    SOCTEST_DMA_STATUS_ERROR

#define PLIC_PENDING    (PLIC_BASE + 0x1000)
#define PLIC_CLAIM(ctx) (PLIC_BASE + 0x200004 + 0x1000 * (ctx))

#define IRQ_DMA        4

/* 描述符布局直接取自 SocTestDMADesc */
#define DESC_SRC      offsetof(SocTestDMADesc, src)
#define DESC_DST      offsetof(SocTestDMADesc, dst)
#define DESC_LEN      offsetof(SocTestDMADesc, len)
#define DESC_STATUS   offsetof(SocTestDMADesc, status)
#define DESC_SIZE     sizeof(SocTestDMADesc)

static void dma_start_linear(QTestState *qts, uint32_t src, uint32_t dst,
                             uint32_t len, uint32_t ctrl)
{
    qtest_writel(qts, DMA_BASE + DMA_SRC_LO, src);
    qtest_writel(qts, DMA_BASE + DMA_SRC_HI, 0);
    qtest_writel(qts, DMA_BASE + DMA_DST_LO, dst);
    qtest_writel(qts, DMA_BASE + DMA_DST_HI, 0);
    qtest_writel(qts, DMA_BASE + DMA_LEN, len);
    qtest_writel(qts, DMA_BASE + DMA_CTRL, ctrl);
}

static void test_linear_copy(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    qtest_writel(qts, DDR_BASE + 0x100, 0xdeadbeef);

    dma_start_linear(qts, DDR_BASE + 0x100, DDR_BASE + 0x200, 4,
                     CTRL_START | CTRL_IRQ_EN);

    /* 传输完成,数据搬过去了 */
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_STATUS), ==, STATUS_DONE);
    g_assert_cmphex(qtest_readl(qts, DDR_BASE + 0x200), ==, 0xdeadbeef);

    /* 完成中断送到了 PLIC 的源 4 */
    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & (1u << IRQ_DMA),
                    ==, 1u << IRQ_DMA);

    /* 清 STATUS(W1C)后中断线落下 */
    qtest_writel(qts, DMA_BASE + DMA_STATUS, STATUS_DONE);
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_desc_chain(void)
{
    QTestState *qts = qtest_init("-machine soctest");
    uint32_t desc = DDR_BASE + 0x1000;
    int i;

    /* 构造两条描述符:0x100 -> 0x300,0x104 -> 0x304 */
    for (i = 0; i < 2; i++) {
        uint32_t d = desc + i * DESC_SIZE;

        qtest_writel(qts, DDR_BASE + 0x100 + i * 4, 0x11110000 + i);
        qtest_writel(qts, d + DESC_SRC, DDR_BASE + 0x100 + i * 4);
        qtest_writel(qts, d + DESC_SRC + 4, 0);
        qtest_writel(qts, d + DESC_DST, DDR_BASE + 0x300 + i * 4);
        qtest_writel(qts, d + DESC_DST + 4, 0);
        qtest_writel(qts, d + DESC_LEN, 4);
        qtest_writel(qts, d + DESC_STATUS, 0xff);   /* 待设备回写为 0 */
    }

    qtest_writel(qts, DMA_BASE + DMA_DESC_LO, desc);
    qtest_writel(qts, DMA_BASE + DMA_DESC_HI, 0);
    qtest_writel(qts, DMA_BASE + DMA_NUM, 2);
    qtest_writel(qts, DMA_BASE + DMA_CTRL,
                 CTRL_START | CTRL_DESC_MODE);

    for (i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(qts, DDR_BASE + 0x300 + i * 4),
                        ==, 0x11110000 + i);
        /* 设备把每条描述符的 status 回写为 0(OK) */
        g_assert_cmphex(qtest_readl(qts, desc + i * DESC_SIZE + DESC_STATUS),
                        ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_STATUS), ==, STATUS_DONE);

    qtest_quit(qts);
}

static void test_error_unmapped(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    qtest_writel(qts, DDR_BASE + 0x100, 0xdeadbeef);

    /* 目的地址落在未映射区域 → address_space_write 失败 */
    dma_start_linear(qts, DDR_BASE + 0x100, 0x20000000, 4, CTRL_START);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_STATUS), ==, STATUS_ERROR);

    qtest_quit(qts);
}

static void test_error_len(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    /* LEN 超过 SOCTEST_DMA_MAX_LEN(0x1000)→ 直接报错,不碰内存 */
    dma_start_linear(qts, DDR_BASE + 0x100, DDR_BASE + 0x200, 0x2000,
                     CTRL_START);

    g_assert_cmphex(qtest_readl(qts, DMA_BASE + DMA_STATUS), ==, STATUS_ERROR);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/soctest-dma/linear", test_linear_copy);
    qtest_add_func("/soctest-dma/desc", test_desc_chain);
    qtest_add_func("/soctest-dma/err-unmapped", test_error_unmapped);
    qtest_add_func("/soctest-dma/err-len", test_error_len);
    return g_test_run();
}

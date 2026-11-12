/*
 * QTest for the socTest timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 寄存器偏移和位定义直接取自设备头文件,避免测试里散落魔法数字;
 * 头文件改了,这里自动跟着变。
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "hw/misc/soctest_timer.h"

#define TIMER_BASE      0x10001000

/* 寄存器地址 = 基址 + 头文件里的偏移 */
#define TIMER_MAGIC     (TIMER_BASE + SOCTEST_TIMER_R_MAGIC)
#define TIMER_VERSION   (TIMER_BASE + SOCTEST_TIMER_R_VERSION)
#define TIMER_SCRATCH   (TIMER_BASE + SOCTEST_TIMER_R_SCRATCH)
#define TIMER_PERIOD    (TIMER_BASE + SOCTEST_TIMER_R_PERIOD)
#define TIMER_CTRL      (TIMER_BASE + SOCTEST_TIMER_R_CTRL)
#define TIMER_TICKS     (TIMER_BASE + SOCTEST_TIMER_R_TICKS)
#define TIMER_STATUS    (TIMER_BASE + SOCTEST_TIMER_R_STATUS)

/* 测试参数 */
#define TEST_SCRATCH_VALUE  0xdeadbeef
#define TEST_PERIOD_NS      1000                    /* 1us 一个 tick */
#define TEST_STEP_NS        (10 * 1000 * 1000)      /* 推进 10ms 虚拟时间 */

static void test_regs(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    /* 只读 ID 寄存器 */
    g_assert_cmphex(qtest_readl(qts, TIMER_MAGIC), ==, SOCTEST_TIMER_MAGIC);
    g_assert_cmphex(qtest_readl(qts, TIMER_VERSION), ==, SOCTEST_TIMER_VERSION);

    /* SCRATCH 读写 */
    qtest_writel(qts, TIMER_SCRATCH, TEST_SCRATCH_VALUE);
    g_assert_cmphex(qtest_readl(qts, TIMER_SCRATCH), ==, TEST_SCRATCH_VALUE);

    /* PERIOD/CTRL 生效:TICKS 会递增(只需要 EN,IRQ_EN 由 PLIC 测试覆盖) */
    qtest_writel(qts, TIMER_PERIOD, TEST_PERIOD_NS);
    qtest_writel(qts, TIMER_CTRL, SOCTEST_TIMER_CTRL_EN);
    qtest_clock_step(qts, TEST_STEP_NS);
    g_assert_cmpuint(qtest_readl(qts, TIMER_TICKS), >, 0);

    /* W1C:写 1 清 STATUS */
    qtest_writel(qts, TIMER_STATUS, SOCTEST_TIMER_STATUS_MATCH);
    g_assert_cmphex(qtest_readl(qts, TIMER_STATUS), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/soctest-timer/regs", test_regs);
    return g_test_run();
}
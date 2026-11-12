/*
 * QTest for the socTest timer -> PLIC path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 覆盖:PLIC 交付条件(priority/enable/threshold)、claim 的副作用、
 *       以及"pending 与 enable 无关"这个容易误解的点。
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "hw/misc/soctest_timer.h"   /* timer 的寄存器偏移/位定义 */

#define TIMER_BASE      0x10001000
#define PLIC_BASE       0x0c000000

/*
 * PLIC 寄存器布局:由 sifive_plic_create() 的参数决定(见 hw/riscv/soc_test.c
 * 里的 SOCTEST_PLIC_*),树里没有公共头文件,所以这里按同一份布局重述。
 */
#define PLIC_PRIO(irq)  (PLIC_BASE + 4 * (irq))
#define PLIC_PENDING    (PLIC_BASE + 0x1000)
#define PLIC_ENABLE(ctx) (PLIC_BASE + 0x2000 + 0x80 * (ctx))
#define PLIC_THRESH(ctx) (PLIC_BASE + 0x200000 + 0x1000 * (ctx))
#define PLIC_CLAIM(ctx)  (PLIC_BASE + 0x200004 + 0x1000 * (ctx))

/* "MS" 里 M 在前:ctx0 = hart0 M 模式,ctx1 = hart0 S 模式 */
#define PLIC_CTX_M      0

/* timer 寄存器地址:基址 + 设备头文件里的偏移 */
#define TIMER_PERIOD    (TIMER_BASE + SOCTEST_TIMER_R_PERIOD)
#define TIMER_CTRL      (TIMER_BASE + SOCTEST_TIMER_R_CTRL)
#define TIMER_TICKS     (TIMER_BASE + SOCTEST_TIMER_R_TICKS)
#define TIMER_STATUS    (TIMER_BASE + SOCTEST_TIMER_R_STATUS)

/* 中断源号:与 soc_test.c 的 SOCTEST_IRQ_* 一致(PLIC 0 号源保留) */
#define IRQ_TIMER      2
#define TIMER_IRQ_MASK (1u << IRQ_TIMER)

/* 测试参数 */
#define TEST_PRIORITY   1
#define TEST_PERIOD_NS  1000        /* 1us 一个 tick */
#define TEST_STEP_NS    100000      /* 推进 100us → 约 100 次 tick */

static void timer_start(QTestState *qts)
{
    qtest_writel(qts, TIMER_PERIOD, TEST_PERIOD_NS);
    qtest_writel(qts, TIMER_CTRL,
                 SOCTEST_TIMER_CTRL_EN | SOCTEST_TIMER_CTRL_IRQ_EN);
    qtest_clock_step(qts, TEST_STEP_NS);
}

static void test_timer_irq(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    /* PLIC:源 2 优先级 TEST_PRIORITY,hart0 M-context 使能,阈值 0 */
    qtest_writel(qts, PLIC_PRIO(IRQ_TIMER), TEST_PRIORITY);
    qtest_writel(qts, PLIC_ENABLE(PLIC_CTX_M), TIMER_IRQ_MASK);
    qtest_writel(qts, PLIC_THRESH(PLIC_CTX_M), 0);

    timer_start(qts);
    g_assert_cmpuint(qtest_readl(qts, TIMER_TICKS), >, 0);

    /* timer 拉高线 → PLIC pending 置位 */
    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & TIMER_IRQ_MASK,
                    ==, TIMER_IRQ_MASK);

    /* claim:返回源号,并清掉 pending(读即消费) */
    g_assert_cmphex(qtest_readl(qts, PLIC_CLAIM(PLIC_CTX_M)), ==, IRQ_TIMER);
    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & TIMER_IRQ_MASK, ==, 0);

    /* complete + 清 timer 的 MATCH(电平中断必须"清源") */
    qtest_writel(qts, PLIC_CLAIM(PLIC_CTX_M), IRQ_TIMER);
    qtest_writel(qts, TIMER_STATUS, SOCTEST_TIMER_STATUS_MATCH);
    g_assert_cmphex(qtest_readl(qts, TIMER_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_pending_needs_no_enable(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    /* 只设优先级,不打开 enable */
    qtest_writel(qts, PLIC_PRIO(IRQ_TIMER), TEST_PRIORITY);

    timer_start(qts);

    /* pending 只由中断源决定,与 enable 无关 */
    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & TIMER_IRQ_MASK,
                    ==, TIMER_IRQ_MASK);

    /* 但 enable=0 时不会被交付,claim 拿不到东西 */
    g_assert_cmphex(qtest_readl(qts, PLIC_CLAIM(PLIC_CTX_M)), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/soctest-plic/timer-irq", test_timer_irq);
    qtest_add_func("/soctest-plic/no-enable", test_pending_needs_no_enable);
    return g_test_run();
}

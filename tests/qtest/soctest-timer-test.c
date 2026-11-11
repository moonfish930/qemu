#include "qemu/osdep.h"
#include "libqtest.h"

#define BASE 0x10001000

static void test_regs(void){
    QTestState *qts = qtest_init("-machine soctest");

    /* 只读魔数 */
    g_assert_cmphex(qtest_readl(qts, BASE + 0x00), ==,  0x534f4354);

    /* SCRATCH 读写 */
    qtest_writel(qts, BASE + 0x08, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x08), ==, 0xdeadbeef);

    /* PERIOD/CTRL 生效:TICKS 会递增 */
    qtest_writel(qts, BASE + 0x0C, 1000);      /* 1us */
    qtest_writel(qts, BASE + 0x10, 1);         /* EN */
    qtest_clock_step(qts, 10000000);           /* 10ms 虚拟时间 */
    g_assert_cmpuint(qtest_readl(qts, BASE + 0x18), >, 0);
    
    
    /* W1C:清 STATUS */
    qtest_writel(qts, BASE + 0x1C, 1);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x1C), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/soctest-timer/regs", test_regs);
    return g_test_run();
}
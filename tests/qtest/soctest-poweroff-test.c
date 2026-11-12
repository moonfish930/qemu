/*
 * QTest for the socTest SiFive Test finisher (poweroff / reset)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * guest 写 0x5555(PASS)/0x3333(FAIL)会让 QEMU 关机,
 * 写 0x7777(RESET)会让 QEMU 复位。这是 guest 主动结束模拟的正规途径,
 * 也是 OpenSBI 的 sbi_shutdown 的落点。
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define TEST_BASE  0x00100000

#define FINISHER_FAIL   0x3333
#define FINISHER_PASS   0x5555
#define FINISHER_RESET  0x7777

static void test_poweroff(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    qtest_writel(qts, TEST_BASE, FINISHER_PASS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");

    qtest_quit(qts);
}

static void test_guest_reset(void)
{
    QTestState *qts = qtest_init("-machine soctest");

    qtest_writel(qts, TEST_BASE, FINISHER_RESET);
    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/soctest-poweroff/pass", test_poweroff);
    qtest_add_func("/soctest-poweroff/reset", test_guest_reset);
    return g_test_run();
}

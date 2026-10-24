/*
 * QTest testcase for the RISC-V CBQRI bandwidth controller
 *
 * Copyright (c) 2026 ByteDance
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CBQRI_BASE             0x01000000
#define BC_CAPABILITIES        0x00
#define BC_ALLOC_CTL           0x18
#define BC_BW_ALLOC            0x20

#define BC_ALLOC_OP_CONFIG_LIMIT 1
#define BC_ALLOC_OP_READ_LIMIT   2

#define BC_ALLOC_STATUS_SUCCESS    1
#define BC_ALLOC_STATUS_INVAL_AT   4
#define BC_ALLOC_STATUS_INVAL_BLKS 5

static QTestState *cbqri_start(bool inactive_entry)
{
    return qtest_initf(
        "-machine virt -nodefaults -display none "
        "-device riscv.cbqri.bandwidth,mmio_base=0x%x,max_rcids=8,"
        "nbwblks=16,mrbwb=8,inactive-entry=%s",
        CBQRI_BASE, inactive_entry ? "on" : "off");
}

static QTestState *cbqri_start_code_only(void)
{
    return qtest_initf(
        "-machine virt -nodefaults -display none "
        "-device riscv.cbqri.bandwidth,mmio_base=0x%x,max_rcids=8,"
        "nbwblks=16,mrbwb=8,inactive-entry=on,at_data=off,"
        "at_code=on",
        CBQRI_BASE);
}

static uint64_t bw_alloc_value(uint16_t rbwb, uint8_t mweight)
{
    return rbwb | ((uint64_t)mweight << 20);
}

static uint64_t shared_alloc_value(uint8_t shared_at)
{
    return ((uint64_t)shared_at << 28) | (1ULL << 31);
}

static uint64_t unique_alloc_value(uint16_t rbwb, uint8_t mweight,
                                   uint8_t ignored_shared_at)
{
    return bw_alloc_value(rbwb, mweight) |
           ((uint64_t)ignored_shared_at << 28);
}

static uint64_t alloc_ctl_value(uint8_t op, uint16_t rcid, uint8_t at)
{
    return op | ((uint64_t)at << 5) | ((uint64_t)rcid << 8);
}

static uint8_t alloc_status(QTestState *qts)
{
    return (qtest_readq(qts, CBQRI_BASE + BC_ALLOC_CTL) >> 32) & 0x7f;
}

static uint8_t config_limit(QTestState *qts, uint16_t rcid, uint8_t at,
                            uint16_t rbwb, uint8_t mweight)
{
    qtest_writeq(qts, CBQRI_BASE + BC_BW_ALLOC,
                 bw_alloc_value(rbwb, mweight));
    qtest_writeq(qts, CBQRI_BASE + BC_ALLOC_CTL,
                 alloc_ctl_value(BC_ALLOC_OP_CONFIG_LIMIT, rcid, at));
    return alloc_status(qts);
}

static uint8_t config_limit_raw(QTestState *qts, uint16_t rcid, uint8_t at,
                                uint64_t value)
{
    qtest_writeq(qts, CBQRI_BASE + BC_BW_ALLOC, value);
    qtest_writeq(qts, CBQRI_BASE + BC_ALLOC_CTL,
                 alloc_ctl_value(BC_ALLOC_OP_CONFIG_LIMIT, rcid, at));
    return alloc_status(qts);
}

static uint64_t read_limit(QTestState *qts, uint16_t rcid, uint8_t at)
{
    qtest_writeq(qts, CBQRI_BASE + BC_ALLOC_CTL,
                 alloc_ctl_value(BC_ALLOC_OP_READ_LIMIT, rcid, at));
    g_assert_cmpuint(alloc_status(qts), ==, BC_ALLOC_STATUS_SUCCESS);
    return qtest_readq(qts, CBQRI_BASE + BC_BW_ALLOC);
}

static void test_legacy_rejects_zero_rbwb(void)
{
    QTestState *qts = cbqri_start(false);
    uint64_t value;

    value = qtest_readq(qts, CBQRI_BASE + BC_CAPABILITIES);
    g_assert_cmphex(value & 0xff, ==, 0x10);
    g_assert_cmpuint(config_limit(qts, 1, 0, 0, 0), ==,
                     BC_ALLOC_STATUS_INVAL_BLKS);
    g_assert_cmpuint(config_limit_raw(qts, 1, 1, shared_alloc_value(0)), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_inactive_entry(void)
{
    QTestState *qts = cbqri_start(true);
    uint64_t value;

    value = qtest_readq(qts, CBQRI_BASE + BC_CAPABILITIES);
    g_assert_cmphex(value & 0xff, ==, 0x11);
    g_assert_cmpuint(config_limit(qts, 4, 0, 0, 1), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 4, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(0, 1));

    g_assert_cmpuint(config_limit_raw(qts, 4, 0,
                     unique_alloc_value(0, 0, 1)), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 4, 0);
    g_assert_cmphex(value, ==, unique_alloc_value(0, 0, 1));

    g_assert_cmpuint(config_limit(qts, 5, 0, 4, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 5, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(4, 0));
    g_assert_cmpuint(config_limit(qts, 5, 0, 0, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 5, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(0, 0));

    g_assert_cmpuint(config_limit(qts, 1, 0, 6, 10), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 2, 0, 2, 10), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 3, 0, 1, 10), ==,
                     BC_ALLOC_STATUS_INVAL_BLKS);

    g_assert_cmpuint(config_limit(qts, 1, 0, 0, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 1, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(0, 0));

    g_assert_cmpuint(config_limit(qts, 3, 0, 6, 10), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 3, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(6, 10));

    g_assert_cmpuint(config_limit(qts, 3, 0, 0, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 1, 0, 6, 10), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 1, 0);
    g_assert_cmphex(value, ==, bw_alloc_value(6, 10));

    qtest_quit(qts);
}

static void test_access_types_are_independent(void)
{
    QTestState *qts = cbqri_start(true);

    g_assert_cmpuint(config_limit(qts, 1, 0, 3, 4), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 1, 1, 2, 4), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 1, 0, 0, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);

    g_assert_cmphex(read_limit(qts, 1, 0), ==, bw_alloc_value(0, 0));
    g_assert_cmphex(read_limit(qts, 1, 1), ==, bw_alloc_value(2, 4));

    qtest_quit(qts);
}

static void test_code_only_access_type(void)
{
    QTestState *qts = cbqri_start_code_only();

    g_assert_cmpuint(config_limit(qts, 1, 0, 2, 4), ==,
                     BC_ALLOC_STATUS_INVAL_AT);
    g_assert_cmpuint(config_limit(qts, 1, 1, 2, 4), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmphex(read_limit(qts, 1, 1), ==, bw_alloc_value(2, 4));
    g_assert_cmpuint(config_limit(qts, 1, 1, 0, 0), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmphex(read_limit(qts, 1, 1), ==, bw_alloc_value(0, 0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("virt")) {
        qtest_add_func("/riscv/cbqri/legacy-rejects-zero-rbwb",
                       test_legacy_rejects_zero_rbwb);
        qtest_add_func("/riscv/cbqri/inactive-entry", test_inactive_entry);
        qtest_add_func("/riscv/cbqri/access-types",
                       test_access_types_are_independent);
        qtest_add_func("/riscv/cbqri/code-only-access-type",
                       test_code_only_access_type);
    }

    return g_test_run();
}

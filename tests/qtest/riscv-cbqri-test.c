/*
 * QTest testcases for the RISC-V CBQRI controllers
 *
 * Copyright (c) 2026 ByteDance
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CBQRI_BASE             0x01000000
#define BC_CAPABILITIES        0x00
#define BC_MON_CTL             0x08
#define BC_MON_CTR_VAL         0x10
#define BC_ALLOC_CTL           0x18
#define BC_BW_ALLOC            0x20
#define BC_RCID_MAP_CTL        0x28

#define CC_CAPABILITIES        0x00
#define CC_MON_CTL             0x08
#define CC_MON_CTR_VAL         0x10
#define CC_RCID_MAP_CTL        0x30

#define BC_ALLOC_OP_CONFIG_LIMIT 1
#define BC_ALLOC_OP_READ_LIMIT   2

#define BC_ALLOC_STATUS_SUCCESS    1
#define BC_ALLOC_STATUS_INVAL_AT   4
#define BC_ALLOC_STATUS_INVAL_BLKS 5

#define BC_MON_OP_CONFIG_EVENT 1
#define BC_MON_OP_READ_COUNTER 2
#define BC_MON_STATUS_SUCCESS 1
#define BC_MON_STATUS_INVAL_OP 2
#define BC_MON_STATUS_NO_COUNTER 6

#define RCID_MAP_OP_CONFIG 1
#define RCID_MAP_OP_READ 2
#define RCID_MAP_OP_READ_ENTRY 3
#define RCID_MAP_STATUS_SUCCESS 1
#define RCID_MAP_STATUS_INVAL_REQ 3
#define RCID_MAP_STATUS_INVAL_INT 4
#define RCID_MAP_STATUS_BUSY 5

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

static QTestState *cbqri_start_idmap(void)
{
    return qtest_initf(
        "-machine virt -nodefaults -display none "
        "-device riscv.cbqri.bandwidth,mmio_base=0x%x,max_rcids=4,"
        "max_mcids=2,nbwblks=16,mrbwb=8,inactive-entry=on,"
        "rcid-map=on,mcid-map=on", CBQRI_BASE);
}

static QTestState *cbqri_capacity_start_idmap(void)
{
    return qtest_initf(
        "-machine virt -nodefaults -display none "
        "-device riscv.cbqri.capacity,mmio_base=0x%x,max_rcids=4,"
        "max_mcids=2,ncblks=16,rcid-map=on,mcid-map=on",
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

static uint64_t rcid_map_value(uint8_t op, uint16_t req_rcid,
                               uint16_t int_rcid)
{
    return op | ((uint64_t)req_rcid << 8) | ((uint64_t)int_rcid << 20);
}

static uint8_t rcid_map_status(QTestState *qts)
{
    return (qtest_readq(qts, CBQRI_BASE + BC_RCID_MAP_CTL) >> 32) & 0x7f;
}

static uint8_t config_map(QTestState *qts, uint16_t req_rcid,
                          uint16_t int_rcid)
{
    qtest_writeq(qts, CBQRI_BASE + BC_RCID_MAP_CTL,
                 rcid_map_value(RCID_MAP_OP_CONFIG, req_rcid, int_rcid));
    return rcid_map_status(qts);
}

static uint16_t read_map(QTestState *qts, uint16_t req_rcid)
{
    uint64_t value;

    qtest_writeq(qts, CBQRI_BASE + BC_RCID_MAP_CTL,
                 rcid_map_value(RCID_MAP_OP_READ, req_rcid, 0));
    g_assert_cmpuint(rcid_map_status(qts), ==, RCID_MAP_STATUS_SUCCESS);
    value = qtest_readq(qts, CBQRI_BASE + BC_RCID_MAP_CTL);
    return (value >> 20) & 0xfff;
}

static uint64_t read_map_entry(QTestState *qts, uint64_t offset,
                               uint16_t int_rcid)
{
    qtest_writeq(qts, CBQRI_BASE + offset,
                 rcid_map_value(RCID_MAP_OP_READ_ENTRY, 0, int_rcid));
    return qtest_readq(qts, CBQRI_BASE + offset);
}

static uint64_t mon_ctl_value(uint8_t op, uint16_t mcid, uint8_t evt_id)
{
    return op | ((uint64_t)mcid << 8) | ((uint64_t)evt_id << 20);
}

static uint8_t mon_status(QTestState *qts)
{
    return (qtest_readq(qts, CBQRI_BASE + BC_MON_CTL) >> 32) & 0x7f;
}

static uint8_t capacity_rcid_map_status(QTestState *qts)
{
    return (qtest_readq(qts, CBQRI_BASE + CC_RCID_MAP_CTL) >> 32) & 0x7f;
}

static uint8_t capacity_config_map(QTestState *qts, uint16_t req_rcid,
                                   uint16_t int_rcid)
{
    qtest_writeq(qts, CBQRI_BASE + CC_RCID_MAP_CTL,
                 rcid_map_value(RCID_MAP_OP_CONFIG, req_rcid, int_rcid));
    return capacity_rcid_map_status(qts);
}

static uint16_t capacity_read_map(QTestState *qts, uint16_t req_rcid)
{
    uint64_t value;

    qtest_writeq(qts, CBQRI_BASE + CC_RCID_MAP_CTL,
                 rcid_map_value(RCID_MAP_OP_READ, req_rcid, 0));
    g_assert_cmpuint(capacity_rcid_map_status(qts), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = qtest_readq(qts, CBQRI_BASE + CC_RCID_MAP_CTL);
    return (value >> 20) & 0xfff;
}

static uint8_t capacity_mon_status(QTestState *qts)
{
    return (qtest_readq(qts, CBQRI_BASE + CC_MON_CTL) >> 32) & 0x7f;
}

static uint8_t capacity_config_event(QTestState *qts, uint16_t mcid,
                                     uint8_t evt_id)
{
    qtest_writeq(qts, CBQRI_BASE + CC_MON_CTL,
                 mon_ctl_value(BC_MON_OP_CONFIG_EVENT, mcid, evt_id));
    return capacity_mon_status(qts);
}

static uint8_t capacity_read_counter(QTestState *qts, uint16_t mcid)
{
    qtest_writeq(qts, CBQRI_BASE + CC_MON_CTL,
                 mon_ctl_value(BC_MON_OP_READ_COUNTER, mcid, 0));
    return capacity_mon_status(qts);
}

static uint8_t config_event(QTestState *qts, uint16_t mcid, uint8_t evt_id)
{
    qtest_writeq(qts, CBQRI_BASE + BC_MON_CTL,
                 mon_ctl_value(BC_MON_OP_CONFIG_EVENT, mcid, evt_id));
    return mon_status(qts);
}

static uint8_t read_counter(QTestState *qts, uint16_t mcid)
{
    qtest_writeq(qts, CBQRI_BASE + BC_MON_CTL,
                 mon_ctl_value(BC_MON_OP_READ_COUNTER, mcid, 0));
    return mon_status(qts);
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
                     BC_ALLOC_STATUS_INVAL_BLKS);
    g_assert_cmpuint(config_limit_raw(qts, 4, 0,
                     unique_alloc_value(0, 0, 1)), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    value = read_limit(qts, 4, 0);
    g_assert_cmphex(value, ==, unique_alloc_value(0, 0, 1));

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

static void test_sparse_rcid_map(void)
{
    QTestState *qts = cbqri_start_idmap();
    uint64_t value;

    value = qtest_readq(qts, CBQRI_BASE + BC_CAPABILITIES);
    g_assert_cmphex(value & 0xff, ==, 0x12);
    g_assert_true(value & (1ULL << 62));
    g_assert_true(value & (1ULL << 63));

    value = read_map_entry(qts, BC_RCID_MAP_CTL, 1);
    g_assert_cmpuint((value >> 32) & 0x7f, ==, RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint((value >> 8) & 0xfff, ==, 1);
    g_assert_true(value & (1ULL << 40));

    g_assert_cmpuint(read_map(qts, 1), ==, 1);
    g_assert_cmpuint(read_map(qts, 20), ==, 0);
    g_assert_cmpuint(config_map(qts, 1, 0), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = read_map_entry(qts, BC_RCID_MAP_CTL, 1);
    g_assert_false(value & (1ULL << 40));
    g_assert_cmpuint(config_map(qts, 20, 1), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = read_map_entry(qts, BC_RCID_MAP_CTL, 1);
    g_assert_cmpuint((value >> 8) & 0xfff, ==, 20);
    g_assert_true(value & (1ULL << 40));
    g_assert_cmpuint(read_map(qts, 20), ==, 1);
    g_assert_cmpuint(config_map(qts, 21, 1), ==, RCID_MAP_STATUS_BUSY);
    g_assert_cmpuint(config_map(qts, 2, 0), ==,
                     RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint(config_map(qts, 20, 2), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = read_map_entry(qts, BC_RCID_MAP_CTL, 1);
    g_assert_false(value & (1ULL << 40));
    value = read_map_entry(qts, BC_RCID_MAP_CTL, 2);
    g_assert_cmpuint((value >> 8) & 0xfff, ==, 20);
    g_assert_true(value & (1ULL << 40));
    g_assert_cmpuint(read_map(qts, 20), ==, 2);
    g_assert_cmpuint(config_map(qts, 21, 1), ==,
                     RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint(config_map(qts, 22, 4), ==,
                     RCID_MAP_STATUS_INVAL_INT);
    value = read_map_entry(qts, BC_RCID_MAP_CTL, 4);
    g_assert_cmpuint((value >> 32) & 0x7f, ==,
                     RCID_MAP_STATUS_INVAL_INT);
    g_assert_cmpuint(config_map(qts, 0, 1), ==,
                     RCID_MAP_STATUS_INVAL_REQ);

    /* Reprogramming stale state in an unbound entry consumes no budget. */
    g_assert_cmpuint(config_map(qts, 20, 0), ==,
                     RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 2, 0, 7, 1), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_map(qts, 21, 0), ==,
                     RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint(config_limit(qts, 2, 0, 2, 1), ==,
                     BC_ALLOC_STATUS_SUCCESS);
    g_assert_cmpuint(config_map(qts, 22, 2), ==,
                     RCID_MAP_STATUS_SUCCESS);

    qtest_quit(qts);
}

static void test_sparse_mcid_map(void)
{
    QTestState *qts = cbqri_start_idmap();
    uint64_t value;

    /* Low numbered MCIDs are identity-bound at reset for old software. */
    g_assert_cmpuint(read_counter(qts, 0), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(read_counter(qts, 1), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 0, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 1, 0), ==,
                     BC_MON_STATUS_SUCCESS);

    g_assert_cmpuint(config_event(qts, 100, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 200, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 300, 1), ==,
                     BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(read_counter(qts, 100), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(read_counter(qts, 300), ==,
                     BC_MON_STATUS_NO_COUNTER);
    value = qtest_readq(qts, CBQRI_BASE + BC_MON_CTR_VAL);
    g_assert_true(value & (1ULL << 62));

    g_assert_cmpuint(config_event(qts, 100, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 300, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(read_counter(qts, 100), ==,
                     BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(read_counter(qts, 300), ==, BC_MON_STATUS_SUCCESS);

    qtest_quit(qts);
}

static void test_mcid_release_failure(void)
{
    QTestState *qts = qtest_initf(
        "-machine virt -nodefaults -display none "
        "-device riscv.cbqri.bandwidth,mmio_base=0x%x,max_rcids=4,"
        "max_mcids=2,nbwblks=16,mrbwb=8,inactive-entry=on,"
        "rcid-map=on,mcid-map=on,release-failures=1,release-fail-mcid=100",
        CBQRI_BASE);

    g_assert_cmpuint(config_event(qts, 0, 0), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 1, 0), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 100, 1), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 200, 1), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 100, 0), ==, BC_MON_STATUS_INVAL_OP);
    g_assert_cmpuint(read_counter(qts, 100), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(config_event(qts, 300, 1), ==, BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(config_event(qts, 100, 0), ==, BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(read_counter(qts, 100), ==, BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(config_event(qts, 300, 1), ==, BC_MON_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_capacity_sparse_rcid_map(void)
{
    QTestState *qts = cbqri_capacity_start_idmap();
    uint64_t value;

    value = qtest_readq(qts, CBQRI_BASE + CC_CAPABILITIES);
    g_assert_cmphex(value & 0xff, ==, 0x12);
    g_assert_true(value & (1ULL << 62));
    g_assert_true(value & (1ULL << 63));

    value = read_map_entry(qts, CC_RCID_MAP_CTL, 1);
    g_assert_cmpuint((value >> 32) & 0x7f, ==, RCID_MAP_STATUS_SUCCESS);
    g_assert_cmpuint((value >> 8) & 0xfff, ==, 1);
    g_assert_true(value & (1ULL << 40));

    g_assert_cmpuint(capacity_read_map(qts, 1), ==, 1);
    g_assert_cmpuint(capacity_read_map(qts, 20), ==, 0);
    g_assert_cmpuint(capacity_config_map(qts, 1, 0), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = read_map_entry(qts, CC_RCID_MAP_CTL, 1);
    g_assert_false(value & (1ULL << 40));
    g_assert_cmpuint(capacity_config_map(qts, 20, 1), ==,
                     RCID_MAP_STATUS_SUCCESS);
    value = read_map_entry(qts, CC_RCID_MAP_CTL, 1);
    g_assert_cmpuint((value >> 8) & 0xfff, ==, 20);
    g_assert_true(value & (1ULL << 40));
    g_assert_cmpuint(capacity_read_map(qts, 20), ==, 1);
    g_assert_cmpuint(capacity_config_map(qts, 21, 1), ==,
                     RCID_MAP_STATUS_BUSY);
    value = read_map_entry(qts, CC_RCID_MAP_CTL, 4);
    g_assert_cmpuint((value >> 32) & 0x7f, ==,
                     RCID_MAP_STATUS_INVAL_INT);

    qtest_quit(qts);
}

static void test_capacity_sparse_mcid_map(void)
{
    QTestState *qts = cbqri_capacity_start_idmap();
    uint64_t value;

    g_assert_cmpuint(capacity_read_counter(qts, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_read_counter(qts, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 0, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 1, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 100, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 200, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 300, 1), ==,
                     BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(capacity_read_counter(qts, 300), ==,
                     BC_MON_STATUS_NO_COUNTER);
    value = qtest_readq(qts, CBQRI_BASE + CC_MON_CTR_VAL);
    g_assert_true(value & (1ULL << 63));

    g_assert_cmpuint(capacity_config_event(qts, 100, 0), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_config_event(qts, 300, 1), ==,
                     BC_MON_STATUS_SUCCESS);
    g_assert_cmpuint(capacity_read_counter(qts, 100), ==,
                     BC_MON_STATUS_NO_COUNTER);
    g_assert_cmpuint(capacity_read_counter(qts, 300), ==,
                     BC_MON_STATUS_SUCCESS);

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
        qtest_add_func("/riscv/cbqri/sparse-rcid-map",
                       test_sparse_rcid_map);
        qtest_add_func("/riscv/cbqri/sparse-mcid-map",
                       test_sparse_mcid_map);
        qtest_add_func("/riscv/cbqri/mcid-release-failure",
                       test_mcid_release_failure);
        qtest_add_func("/riscv/cbqri/capacity-sparse-rcid-map",
                       test_capacity_sparse_rcid_map);
        qtest_add_func("/riscv/cbqri/capacity-sparse-mcid-map",
                       test_capacity_sparse_mcid_map);
    }

    return g_test_run();
}

/*
 * RISC-V Capacity and Bandwidth QoS Register Interface
 * URL: https://github.com/riscv-non-isa/riscv-cbqri/releases/tag/v1.0
 *
 * Copyright (c) 2023 BayLibre SAS
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file contains the Bandwidth-controller QoS Register Interface.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/cbqri.h"

#define CBQRI_INACTIVE_ENTRY_VERSION 0x11
#define CBQRI_ID_MAP_VERSION 0x12

/* Encodings of `AT` field */
enum {
    BC_AT_DATA = 0,
    BC_AT_CODE = 1,
};

/* Capabilities */
REG64(BC_CAPABILITIES, 0);
FIELD(BC_CAPABILITIES, VER, 0, 8);
FIELD(BC_CAPABILITIES, VER_MINOR, 0, 4);
FIELD(BC_CAPABILITIES, VER_MAJOR, 4, 4);
FIELD(BC_CAPABILITIES, NBWBLKS, 8, 16);
FIELD(BC_CAPABILITIES, RPFX, 24, 1);
FIELD(BC_CAPABILITIES, P, 25, 4);
FIELD(BC_CAPABILITIES, MRBWB, 32, 16);
FIELD(BC_CAPABILITIES, RCID_MAP, 62, 1);
FIELD(BC_CAPABILITIES, MCID_MAP, 63, 1);

/* Usage monitoring control */
REG64(BC_MON_CTL, 8);
FIELD(BC_MON_CTL, OP, 0, 5);
FIELD(BC_MON_CTL, AT, 5, 3);
FIELD(BC_MON_CTL, MCID, 8, 12);
FIELD(BC_MON_CTL, EVT_ID, 20, 8);
FIELD(BC_MON_CTL, ATV, 28, 1);
FIELD(BC_MON_CTL, STATUS, 32, 7);
FIELD(BC_MON_CTL, BUSY, 39, 1);

/* Usage monitoring operations */
enum {
    BC_MON_OP_CONFIG_EVENT = 1,
    BC_MON_OP_READ_COUNTER = 2,
};

/* Bandwidth monitoring event ID */
enum {
    BC_EVT_ID_None = 0,
    BC_EVT_ID_RDWR_count = 1,
    BC_EVT_ID_RDONLY_count = 2,
    BC_EVT_ID_WRONLY_count = 3,
};

/* BC_MON_CTL.STATUS field encodings */
enum {
    BC_MON_CTL_STATUS_SUCCESS = 1,
    BC_MON_CTL_STATUS_INVAL_OP = 2,
    BC_MON_CTL_STATUS_INVAL_MCID = 3,
    BC_MON_CTL_STATUS_INVAL_EVT_ID = 4,
    BC_MON_CTL_STATUS_INVAL_AT = 5,
    BC_MON_CTL_STATUS_NO_COUNTER = 6,
};

/* Monitoring counter value */
REG64(BC_MON_CTR_VAL, 16);
FIELD(BC_MON_CTR_VAL, CTR, 0, 62);
FIELD(BC_MON_CTR_VAL, INVALID, 62, 1);
FIELD(BC_MON_CTR_VAL, OVF, 63, 1);

/* Bandwidth Allocation control */
REG64(BC_ALLOC_CTL, 24);
FIELD(BC_ALLOC_CTL, OP, 0, 5);
FIELD(BC_ALLOC_CTL, AT, 5, 3);
FIELD(BC_ALLOC_CTL, RCID, 8, 12);
FIELD(BC_ALLOC_CTL, STATUS, 32, 7);
FIELD(BC_ALLOC_CTL, BUSY, 39, 1);

/* Bandwidth allocation operations */
enum {
    BC_ALLOC_OP_CONFIG_LIMIT = 1,
    BC_ALLOC_OP_READ_LIMIT = 2,
};

/* BC_ALLOC_CTL.STATUS field encodings */
enum {
    BC_ALLOC_STATUS_SUCCESS = 1,
    BC_ALLOC_STATUS_INVAL_OP = 2,
    BC_ALLOC_STATUS_INVAL_RCID = 3,
    BC_ALLOC_STATUS_INVAL_AT = 4,
    BC_ALLOC_STATUS_INVAL_BLKS = 5,
};

/* Bandwidth allocation */
REG64(BC_BW_ALLOC, 32);
FIELD(BC_BW_ALLOC, Rbwb, 0, 16);
FIELD(BC_BW_ALLOC, Mweight, 20, 8);
FIELD(BC_BW_ALLOC, sharedAT, 28, 3);
FIELD(BC_BW_ALLOC, useShared, 31, 1);

/* Sparse RCID mapping control */
REG64(BC_RCID_MAP_CTL, 40);
FIELD(BC_RCID_MAP_CTL, OP, 0, 5);
FIELD(BC_RCID_MAP_CTL, REQ_RCID, 8, 12);
FIELD(BC_RCID_MAP_CTL, INT_RCID, 20, 12);
FIELD(BC_RCID_MAP_CTL, STATUS, 32, 7);
FIELD(BC_RCID_MAP_CTL, BUSY, 39, 1);
FIELD(BC_RCID_MAP_CTL, VALID, 40, 1);

enum {
    BC_RCID_MAP_OP_CONFIG = 1,
    BC_RCID_MAP_OP_READ = 2,
    BC_RCID_MAP_OP_READ_ENTRY = 3,
};

enum {
    BC_RCID_MAP_STATUS_SUCCESS = 1,
    BC_RCID_MAP_STATUS_INVAL_OP = 2,
    BC_RCID_MAP_STATUS_INVAL_REQ_RCID = 3,
    BC_RCID_MAP_STATUS_INVAL_INT_RCID = 4,
    BC_RCID_MAP_STATUS_INT_RCID_BUSY = 5,
    BC_RCID_MAP_STATUS_CONSTRAINT = 6,
};


typedef struct MonitorCounter {
    uint64_t ctr_val;
    uint16_t mcid_tag;
    int at;
    int evt_id;
    bool active;
    bool tag_valid;
} MonitorCounter;

typedef struct RcidTag {
    uint16_t req_rcid;
    bool valid;
} RcidTag;

typedef struct BandwidthAllocation {
    uint32_t Rbwb:16;
    uint32_t Mweight:8;
    uint32_t sharedAT:3;
    bool useShared:1;
} BandwidthAllocation;

typedef struct RiscvCbqriBandwidthState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    /* cached value of some registers */
    uint64_t bc_mon_ctl;
    uint64_t bc_mon_ctr_val;
    uint64_t bc_alloc_ctl;
    uint64_t bc_bw_alloc;
    uint64_t bc_rcid_map_ctl;

    MonitorCounter *mon_counters;
    BandwidthAllocation *bw_allocations;
    RcidTag *rcid_tags;

    /* properties */

    uint64_t mmio_base;
    char *target;
    uint16_t nb_mcids;
    uint16_t nb_rcids;

    uint16_t nbwblks;
    uint16_t mrbwb;

    bool supports_at_data;
    bool supports_at_code;

    bool supports_alloc_op_config_limit;
    bool supports_alloc_op_read_limit;

    bool supports_inactive_entry;
    bool supports_rcid_map;
    bool supports_mcid_map;

    bool supports_mon_op_config_event;
    bool supports_mon_op_read_counter;

    bool supports_mon_evt_id_none;
    bool supports_mon_evt_id_rdwr_count;
    bool supports_mon_evt_id_rdonly_count;
    bool supports_mon_evt_id_wronly_count;
} RiscvCbqriBandwidthState;

#define RISCV_CBQRI_BC(obj) \
    OBJECT_CHECK(RiscvCbqriBandwidthState, (obj), TYPE_RISCV_CBQRI_BC)

static unsigned int get_num_ats(RiscvCbqriBandwidthState *bc)
{
    if (bc->supports_at_code) {
        return BC_AT_CODE + 1;
    }
    return 1;
}

static BandwidthAllocation *get_bw_alloc(RiscvCbqriBandwidthState *bc,
                                         uint32_t rcid, uint32_t at)
{
    /*
     * All bandwidth allocation records are contiguous to simplify
     * allocation. The first one is used to hold the BC_BW_ALLOC register
     * content, followed by respective records for each AT per RCID.
     */
    unsigned int nb_ats = get_num_ats(bc);

    assert(at < nb_ats);

    return &bc->bw_allocations[1 + rcid * nb_ats + at];
}

static int find_rcid_tag(RiscvCbqriBandwidthState *bc, uint16_t req_rcid)
{
    uint32_t int_rcid;

    if (req_rcid == 0) {
        return 0;
    }

    for (int_rcid = 1; int_rcid < bc->nb_rcids; int_rcid++) {
        if (bc->rcid_tags[int_rcid].valid &&
            bc->rcid_tags[int_rcid].req_rcid == req_rcid) {
            return int_rcid;
        }
    }

    return 0;
}

static int find_mcid_tag(RiscvCbqriBandwidthState *bc, uint16_t mcid)
{
    uint32_t counter;

    for (counter = 0; counter < bc->nb_mcids; counter++) {
        if (bc->mon_counters[counter].tag_valid &&
            bc->mon_counters[counter].mcid_tag == mcid) {
            return counter;
        }
    }

    return -1;
}

static int find_free_counter(RiscvCbqriBandwidthState *bc)
{
    uint32_t counter;

    for (counter = 0; counter < bc->nb_mcids; counter++) {
        if (!bc->mon_counters[counter].tag_valid) {
            return counter;
        }
    }

    return -1;
}

static void reset_identifier_maps(RiscvCbqriBandwidthState *bc)
{
    uint32_t counter;
    uint32_t int_rcid;

    memset(bc->mon_counters, 0, sizeof(*bc->mon_counters) * bc->nb_mcids);
    if (bc->supports_mcid_map) {
        for (counter = 0; counter < bc->nb_mcids; counter++) {
            bc->mon_counters[counter].mcid_tag = counter;
            bc->mon_counters[counter].tag_valid = true;
        }
    }
    memset(bc->rcid_tags, 0, sizeof(*bc->rcid_tags) * bc->nb_rcids);
    if (!bc->supports_rcid_map) {
        return;
    }

    for (int_rcid = 1; int_rcid < bc->nb_rcids; int_rcid++) {
        bc->rcid_tags[int_rcid].req_rcid = int_rcid;
        bc->rcid_tags[int_rcid].valid = true;
    }
}

static uint64_t bandwidth_reserved_sum(RiscvCbqriBandwidthState *bc)
{
    unsigned int nb_ats = get_num_ats(bc);
    uint64_t total = 0;
    uint32_t rcid;
    uint32_t at;

    for (rcid = 0; rcid < bc->nb_rcids; rcid++) {
        if (rcid != 0 && bc->supports_rcid_map &&
            !bc->rcid_tags[rcid].valid) {
            continue;
        }
        for (at = 0; at < nb_ats; at++) {
            BandwidthAllocation *alloc = get_bw_alloc(bc, rcid, at);

            if (!alloc->useShared) {
                total += alloc->Rbwb;
            }
        }
    }

    return total;
}

static uint32_t bandwidth_config(RiscvCbqriBandwidthState *bc,
                                 uint32_t rcid, uint32_t at,
                                 bool *busy)
{
    BandwidthAllocation *bw_alloc = get_bw_alloc(bc, rcid, at);
    BandwidthAllocation *new_alloc = &bc->bw_allocations[0];
    uint64_t reserved = bandwidth_reserved_sum(bc);
    bool entry_active;

    entry_active = !bc->supports_rcid_map || rcid == 0 ||
                   bc->rcid_tags[rcid].valid;

    /*
     * Bandwidth is allocated in multiples of bandwidth blocks, and the
     * value in Rbwb must be at least 1 unless this controller supports the
     * inactive-entry extension. Rbwb is ignored for shared allocations.
     */
    if (!new_alloc->useShared && new_alloc->Rbwb < 1) {
        if (!bc->supports_inactive_entry || new_alloc->Mweight != 0) {
            return BC_ALLOC_STATUS_INVAL_BLKS;
        }
    }

    if (entry_active && !bw_alloc->useShared) {
        reserved -= bw_alloc->Rbwb;
    }
    if (entry_active && !new_alloc->useShared) {
        if (new_alloc->Rbwb > bc->mrbwb) {
            return BC_ALLOC_STATUS_INVAL_BLKS;
        }

        reserved += new_alloc->Rbwb;
        if (reserved > bc->mrbwb) {
            return BC_ALLOC_STATUS_INVAL_BLKS;
        }
    }

    /* Save contents of BC_BW_ALLOC register for this rcid and at */
    *bw_alloc = *new_alloc;
    return BC_ALLOC_STATUS_SUCCESS;
}

static uint32_t bandwidth_read(RiscvCbqriBandwidthState *bc,
                               uint32_t rcid, uint32_t at,
                               bool *busy)
{
    BandwidthAllocation *bw_alloc = get_bw_alloc(bc, rcid, at);

    /* Populate BC_BW_ALLOC register with selected content */
    bc->bw_allocations[0] = *bw_alloc;
    return BC_ALLOC_STATUS_SUCCESS;
}

static bool is_valid_at(RiscvCbqriBandwidthState *bc, uint32_t at)
{
    switch (at) {
    case BC_AT_DATA:
        return bc->supports_at_data;
    case BC_AT_CODE:
        return bc->supports_at_code;
    default:
        return false;
    }
}

static void riscv_cbqri_bc_write_mon_ctl(RiscvCbqriBandwidthState *bc,
                                         uint64_t value)
{
    MonitorCounter *counter = NULL;
    int counter_id = -1;
    if (!bc->supports_mon_op_config_event &&
        !bc->supports_mon_op_read_counter) {
        /* monitoring not supported: leave mon_ctl set to 0 */
        return;
    }

    /* extract writable fields */
    uint32_t op = FIELD_EX64(value, BC_MON_CTL, OP);
    uint32_t at = FIELD_EX64(value, BC_MON_CTL, AT);
    uint32_t mcid = FIELD_EX64(value, BC_MON_CTL, MCID);
    uint32_t evt_id = FIELD_EX64(value, BC_MON_CTL, EVT_ID);
    bool atv = FIELD_EX64(value, BC_MON_CTL, ATV);

    /* extract read-only fields */
    uint32_t status = FIELD_EX64(bc->bc_mon_ctl, BC_MON_CTL, STATUS);
    bool busy = FIELD_EX64(bc->bc_mon_ctl, BC_MON_CTL, BUSY);

    if (busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: busy flag still set, ignored",
                      __func__);
        return;
    }

    if (!bc->supports_at_data &&
        !bc->supports_at_code) {
        /* AT not supported: hardwire to 0 */
        at = 0;
        atv = false;
    }

    if (bc->supports_mcid_map) {
        counter_id = find_mcid_tag(bc, mcid);
    } else if (mcid < bc->nb_mcids) {
        counter_id = mcid;
    }

    if (!bc->supports_mcid_map && mcid >= bc->nb_mcids) {
        status = BC_MON_CTL_STATUS_INVAL_MCID;
    } else if (op == BC_MON_OP_CONFIG_EVENT &&
               bc->supports_mon_op_config_event) {
        if (evt_id == BC_EVT_ID_None &&
            bc->supports_mon_evt_id_none) {
            if (counter_id >= 0) {
                counter = &bc->mon_counters[counter_id];
                counter->active = false;
                if (bc->supports_mcid_map) {
                    counter->tag_valid = false;
                }
            }
            status = BC_MON_CTL_STATUS_SUCCESS;
        } else if ((evt_id == BC_EVT_ID_RDWR_count &&
                    bc->supports_mon_evt_id_rdwr_count) ||
                   (evt_id == BC_EVT_ID_RDONLY_count &&
                    bc->supports_mon_evt_id_rdonly_count) ||
                   (evt_id == BC_EVT_ID_WRONLY_count &&
                    bc->supports_mon_evt_id_wronly_count)) {
            if (atv && !is_valid_at(bc, at)) {
                status = BC_MON_CTL_STATUS_INVAL_AT;
            } else {
                if (counter_id < 0 && bc->supports_mcid_map) {
                    counter_id = find_free_counter(bc);
                }
                if (counter_id < 0) {
                    status = BC_MON_CTL_STATUS_NO_COUNTER;
                    goto done;
                }
                counter = &bc->mon_counters[counter_id];
                counter->ctr_val =
                    FIELD_DP64(0, BC_MON_CTR_VAL, INVALID, 1);
                counter->evt_id = evt_id;
                counter->at = atv ? at : -1;
                counter->active = true;
                if (bc->supports_mcid_map) {
                    counter->mcid_tag = mcid;
                    counter->tag_valid = true;
                }
                status = BC_MON_CTL_STATUS_SUCCESS;
            }
        } else {
            status = BC_MON_CTL_STATUS_INVAL_EVT_ID;
        }
    } else if (op == BC_MON_OP_READ_COUNTER &&
               bc->supports_mon_op_read_counter) {
        if (counter_id < 0) {
            bc->bc_mon_ctr_val =
                FIELD_DP64(0, BC_MON_CTR_VAL, INVALID, 1);
            status = BC_MON_CTL_STATUS_NO_COUNTER;
        } else {
            bc->bc_mon_ctr_val = bc->mon_counters[counter_id].ctr_val;
            status = BC_MON_CTL_STATUS_SUCCESS;
        }
    } else {
        status = BC_MON_CTL_STATUS_INVAL_OP;
    }

done:
    /* reconstruct updated register value */
    value = 0;
    value = FIELD_DP64(value, BC_MON_CTL, OP, op);
    value = FIELD_DP64(value, BC_MON_CTL, AT, at);
    value = FIELD_DP64(value, BC_MON_CTL, MCID, mcid);
    value = FIELD_DP64(value, BC_MON_CTL, EVT_ID, evt_id);
    value = FIELD_DP64(value, BC_MON_CTL, ATV, atv);
    value = FIELD_DP64(value, BC_MON_CTL, STATUS, status);
    value = FIELD_DP64(value, BC_MON_CTL, BUSY, busy);
    bc->bc_mon_ctl = value;
}

static void riscv_cbqri_bc_write_rcid_map_ctl(
    RiscvCbqriBandwidthState *bc, uint64_t value)
{
    uint32_t op = FIELD_EX64(value, BC_RCID_MAP_CTL, OP);
    uint32_t req_rcid = FIELD_EX64(value, BC_RCID_MAP_CTL, REQ_RCID);
    uint32_t int_rcid = FIELD_EX64(value, BC_RCID_MAP_CTL, INT_RCID);
    uint32_t status = BC_RCID_MAP_STATUS_SUCCESS;
    bool valid = false;
    uint64_t reserved;
    uint32_t at;
    int old_int_rcid;

    if (!bc->supports_rcid_map) {
        return;
    }

    if (op == BC_RCID_MAP_OP_READ) {
        int_rcid = find_rcid_tag(bc, req_rcid);
    } else if (op == BC_RCID_MAP_OP_READ_ENTRY) {
        if (int_rcid >= bc->nb_rcids) {
            status = BC_RCID_MAP_STATUS_INVAL_INT_RCID;
        } else if (int_rcid != 0 && bc->rcid_tags[int_rcid].valid) {
            req_rcid = bc->rcid_tags[int_rcid].req_rcid;
            valid = true;
        } else {
            req_rcid = 0;
        }
    } else if (op == BC_RCID_MAP_OP_CONFIG) {
        if (req_rcid == 0 && int_rcid != 0) {
            status = BC_RCID_MAP_STATUS_INVAL_REQ_RCID;
        } else if (int_rcid >= bc->nb_rcids) {
            status = BC_RCID_MAP_STATUS_INVAL_INT_RCID;
        } else if (int_rcid != 0 && bc->rcid_tags[int_rcid].valid &&
                   bc->rcid_tags[int_rcid].req_rcid != req_rcid) {
            status = BC_RCID_MAP_STATUS_INT_RCID_BUSY;
        } else {
            old_int_rcid = find_rcid_tag(bc, req_rcid);
            reserved = bandwidth_reserved_sum(bc);
            if (old_int_rcid != 0 && old_int_rcid != int_rcid) {
                for (at = 0; at < get_num_ats(bc); at++) {
                    BandwidthAllocation *alloc;

                    alloc = get_bw_alloc(bc, old_int_rcid, at);
                    if (!alloc->useShared) {
                        reserved -= alloc->Rbwb;
                    }
                }
            }
            if (int_rcid != 0 && !bc->rcid_tags[int_rcid].valid) {
                for (at = 0; at < get_num_ats(bc); at++) {
                    BandwidthAllocation *alloc;

                    alloc = get_bw_alloc(bc, int_rcid, at);
                    if (!alloc->useShared) {
                        reserved += alloc->Rbwb;
                    }
                }
            }
            if (reserved > bc->mrbwb) {
                status = BC_RCID_MAP_STATUS_CONSTRAINT;
                goto done;
            }
            if (old_int_rcid != 0) {
                bc->rcid_tags[old_int_rcid].valid = false;
            }
            if (int_rcid != 0) {
                bc->rcid_tags[int_rcid].req_rcid = req_rcid;
                bc->rcid_tags[int_rcid].valid = true;
            }
        }
    } else {
        status = BC_RCID_MAP_STATUS_INVAL_OP;
    }

done:
    value = 0;
    value = FIELD_DP64(value, BC_RCID_MAP_CTL, OP, op);
    value = FIELD_DP64(value, BC_RCID_MAP_CTL, REQ_RCID, req_rcid);
    value = FIELD_DP64(value, BC_RCID_MAP_CTL, INT_RCID, int_rcid);
    value = FIELD_DP64(value, BC_RCID_MAP_CTL, STATUS, status);
    value = FIELD_DP64(value, BC_RCID_MAP_CTL, VALID, valid);
    bc->bc_rcid_map_ctl = value;
}

static void riscv_cbqri_bc_write_alloc_ctl(RiscvCbqriBandwidthState *bc,
                                           uint64_t value)
{
    if (bc->nbwblks == 0 ||
        (!bc->supports_alloc_op_config_limit &&
         !bc->supports_alloc_op_read_limit)) {
        /* capacity allocation not supported: leave bc_alloc_ctl set to 0 */
        return;
    }

    /* extract writable fields */
    uint32_t op = FIELD_EX64(value, BC_ALLOC_CTL, OP);
    uint32_t at = FIELD_EX64(value, BC_ALLOC_CTL, AT);
    uint32_t rcid = FIELD_EX64(value, BC_ALLOC_CTL, RCID);

    /* extract read-only fields */
    uint32_t status = FIELD_EX64(bc->bc_alloc_ctl, BC_ALLOC_CTL, STATUS);
    bool busy = FIELD_EX64(bc->bc_alloc_ctl, BC_ALLOC_CTL, BUSY);

    if (busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: busy flag still set, ignored",
                      __func__);
        return;
    }

    bool atv = true;
    if (!bc->supports_at_data &&
        !bc->supports_at_code) {
        /* AT not supported: hardwire to 0 */
        at = 0;
        atv = false;
    }

    if (rcid >= bc->nb_rcids) {
        status = BC_ALLOC_STATUS_INVAL_RCID;
    } else if (atv && !is_valid_at(bc, at)) {
        status = BC_ALLOC_STATUS_INVAL_AT;
    } else if (op == BC_ALLOC_OP_CONFIG_LIMIT &&
               bc->supports_alloc_op_config_limit) {
        status = bandwidth_config(bc, rcid, at, &busy);
    } else if (op == BC_ALLOC_OP_READ_LIMIT &&
               bc->supports_alloc_op_read_limit) {
        status = bandwidth_read(bc, rcid, at, &busy);
    } else {
        status = BC_ALLOC_STATUS_INVAL_OP;
    }

    /* reconstruct updated register value */
    value = 0;
    value = FIELD_DP64(value, BC_ALLOC_CTL, OP, op);
    value = FIELD_DP64(value, BC_ALLOC_CTL, AT, at);
    value = FIELD_DP64(value, BC_ALLOC_CTL, RCID, rcid);
    value = FIELD_DP64(value, BC_ALLOC_CTL, STATUS, status);
    value = FIELD_DP64(value, BC_ALLOC_CTL, BUSY, busy);
    bc->bc_alloc_ctl = value;
}

static void riscv_cbqri_bc_write_bw_alloc(RiscvCbqriBandwidthState *bc,
                                          uint64_t value)
{
    if (bc->nbwblks == 0) {
        /* capacity allocation not supported: leave bw_alloc set to 0 */
        return;
    }

    BandwidthAllocation *bc_bw_alloc = &bc->bw_allocations[0];

    /* extract writable fields */
    bc_bw_alloc->Rbwb = FIELD_EX64(value, BC_BW_ALLOC, Rbwb);
    bc_bw_alloc->Mweight = FIELD_EX64(value, BC_BW_ALLOC, Mweight);
    bc_bw_alloc->sharedAT = FIELD_EX64(value, BC_BW_ALLOC, sharedAT);
    bc_bw_alloc->useShared = FIELD_EX64(value, BC_BW_ALLOC, useShared);

    if (!bc->supports_at_data &&
        !bc->supports_at_code) {
        /* AT not supported: hardwire to 0 */
        bc_bw_alloc->sharedAT = 0;
        bc_bw_alloc->useShared = false;
    }
}

static void riscv_cbqri_bc_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    RiscvCbqriBandwidthState *bc = opaque;

    assert((addr % 8) == 0);

    switch (addr) {
    case A_BC_CAPABILITIES:
        /* read-only register */
        break;
    case A_BC_MON_CTL:
        riscv_cbqri_bc_write_mon_ctl(bc, value);
        break;
    case A_BC_MON_CTR_VAL:
        /* read-only register */
        break;
    case A_BC_ALLOC_CTL:
        riscv_cbqri_bc_write_alloc_ctl(bc, value);
        break;
    case A_BC_BW_ALLOC:
        riscv_cbqri_bc_write_bw_alloc(bc, value);
        break;
    case A_BC_RCID_MAP_CTL:
        riscv_cbqri_bc_write_rcid_map_ctl(bc, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: out of bounds (addr=0x%x)",
                      __func__, (uint32_t)addr);
    }
}

static uint64_t riscv_cbqri_bc_read(void *opaque, hwaddr addr, unsigned size)
{
    RiscvCbqriBandwidthState *bc = opaque;
    uint64_t value = 0;

    assert((addr % 8) == 0);

    switch (addr) {
    case A_BC_CAPABILITIES:
        if (bc->supports_rcid_map || bc->supports_mcid_map) {
            value = FIELD_DP64(value, BC_CAPABILITIES, VER,
                               CBQRI_ID_MAP_VERSION);
        } else if (bc->supports_inactive_entry) {
            value = FIELD_DP64(value, BC_CAPABILITIES, VER,
                               CBQRI_INACTIVE_ENTRY_VERSION);
        } else {
            value = FIELD_DP64(value, BC_CAPABILITIES, VER_MAJOR,
                               RISCV_CBQRI_VERSION_MAJOR);
            value = FIELD_DP64(value, BC_CAPABILITIES, VER_MINOR,
                               RISCV_CBQRI_VERSION_MINOR);
        }
        value = FIELD_DP64(value, BC_CAPABILITIES, RPFX, 0);
        value = FIELD_DP64(value, BC_CAPABILITIES, P, 0);
        value = FIELD_DP64(value, BC_CAPABILITIES, NBWBLKS, bc->nbwblks);
        value = FIELD_DP64(value, BC_CAPABILITIES, MRBWB, bc->mrbwb);
        value = FIELD_DP64(value, BC_CAPABILITIES, RCID_MAP,
                           bc->supports_rcid_map);
        value = FIELD_DP64(value, BC_CAPABILITIES, MCID_MAP,
                           bc->supports_mcid_map);
        break;
    case A_BC_MON_CTL:
        value = bc->bc_mon_ctl;
        break;
    case A_BC_MON_CTR_VAL:
        value = bc->bc_mon_ctr_val;
        break;
    case A_BC_ALLOC_CTL:
        value = bc->bc_alloc_ctl;
        break;
    case A_BC_BW_ALLOC:
        BandwidthAllocation *bc_bw_alloc = &bc->bw_allocations[0];
        value = FIELD_DP64(value, BC_BW_ALLOC, Rbwb, bc_bw_alloc->Rbwb);
        value = FIELD_DP64(value, BC_BW_ALLOC, Mweight, bc_bw_alloc->Mweight);
        value = FIELD_DP64(value, BC_BW_ALLOC, sharedAT, bc_bw_alloc->sharedAT);
        value = FIELD_DP64(value, BC_BW_ALLOC, useShared,
                           bc_bw_alloc->useShared);
        break;
    case A_BC_RCID_MAP_CTL:
        value = bc->bc_rcid_map_ctl;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: out of bounds (addr=0x%x)",
                      __func__, (uint32_t)addr);
    }

    return value;
}

static uint64_t riscv_cbqri_bc_read_wrapper(void *opaque, hwaddr addr,
                                            unsigned size)
{
    uint64_t value = riscv_cbqri_bc_read(opaque, addr & ~0x7UL, 8);
    if (size == 4) {
        if (addr & 0x7) {
            return value >> 32;
        } else {
            return value & 0xffffffff;
        }
    }
    return value;
}

static void riscv_cbqri_bc_write_wrapper(void *opaque, hwaddr addr,
                                             uint64_t value, unsigned size)
{
    if (size == 4) {
        uint64_t reg = riscv_cbqri_bc_read(opaque, addr & ~0x7UL, 8);
        if (addr & 0x7) {
            value = value << 32 | (reg & 0xffffffff);
        } else {
            value = value | (reg & ~0xffffffffUL);
        }
    }
    riscv_cbqri_bc_write(opaque, addr & ~0x7UL, value, 8);
}


static const MemoryRegionOps riscv_cbqri_bc_ops = {
    .read = riscv_cbqri_bc_read_wrapper,
    .write = riscv_cbqri_bc_write_wrapper,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void riscv_cbqri_bc_realize(DeviceState *dev, Error **errp)
{
    RiscvCbqriBandwidthState *bc = RISCV_CBQRI_BC(dev);
    unsigned int bw_alloc_size;

    if (!bc->mmio_base) {
        error_setg(errp, "mmio_base property not set");
        return;
    }

    assert(bc->mon_counters == NULL);
    bc->mon_counters = g_new0(MonitorCounter, bc->nb_mcids);

    assert(bc->bw_allocations == NULL);
    bw_alloc_size = 1 + bc->nb_rcids * get_num_ats(bc);
    bc->bw_allocations = g_new0(BandwidthAllocation, bw_alloc_size);

    assert(bc->rcid_tags == NULL);
    bc->rcid_tags = g_new0(RcidTag, bc->nb_rcids);
    reset_identifier_maps(bc);

    memory_region_init_io(&bc->mmio, OBJECT(dev), &riscv_cbqri_bc_ops,
                          bc, TYPE_RISCV_CBQRI_BC".mmio", 4 * 1024);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &bc->mmio);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, bc->mmio_base);
}

static void riscv_cbqri_bc_reset(DeviceState *dev)
{
    RiscvCbqriBandwidthState *bc = RISCV_CBQRI_BC(dev);

    bc->bc_mon_ctl = 0;
    bc->bc_alloc_ctl = 0;
    bc->bc_rcid_map_ctl = 0;

    reset_identifier_maps(bc);
}

static const Property riscv_cbqri_bc_properties[] = {
    DEFINE_PROP_UINT64("mmio_base", RiscvCbqriBandwidthState, mmio_base, 0),
    DEFINE_PROP_STRING("target", RiscvCbqriBandwidthState, target),

    DEFINE_PROP_UINT16("max_mcids", RiscvCbqriBandwidthState, nb_mcids, 256),
    DEFINE_PROP_UINT16("max_rcids", RiscvCbqriBandwidthState, nb_rcids, 64),
    DEFINE_PROP_UINT16("nbwblks", RiscvCbqriBandwidthState, nbwblks, 1024),
    DEFINE_PROP_UINT16("mrbwb", RiscvCbqriBandwidthState, mrbwb, 819),

    DEFINE_PROP_BOOL("at_data", RiscvCbqriBandwidthState,
                     supports_at_data, true),
    DEFINE_PROP_BOOL("at_code", RiscvCbqriBandwidthState,
                     supports_at_code, true),

    DEFINE_PROP_BOOL("alloc_op_config_limit", RiscvCbqriBandwidthState,
                     supports_alloc_op_config_limit, true),
    DEFINE_PROP_BOOL("alloc_op_read_limit", RiscvCbqriBandwidthState,
                     supports_alloc_op_read_limit, true),

    DEFINE_PROP_BOOL("inactive-entry", RiscvCbqriBandwidthState,
                     supports_inactive_entry, false),
    DEFINE_PROP_BOOL("rcid-map", RiscvCbqriBandwidthState,
                     supports_rcid_map, false),
    DEFINE_PROP_BOOL("mcid-map", RiscvCbqriBandwidthState,
                     supports_mcid_map, false),

    DEFINE_PROP_BOOL("mon_op_config_event", RiscvCbqriBandwidthState,
                     supports_mon_op_config_event, true),
    DEFINE_PROP_BOOL("mon_op_read_counter", RiscvCbqriBandwidthState,
                     supports_mon_op_read_counter, true),

    DEFINE_PROP_BOOL("mon_evt_id_none", RiscvCbqriBandwidthState,
                     supports_mon_evt_id_none, true),
    DEFINE_PROP_BOOL("mon_evt_id_rdwr_count", RiscvCbqriBandwidthState,
                     supports_mon_evt_id_rdwr_count, true),
    DEFINE_PROP_BOOL("mon_evt_id_rdonly_count", RiscvCbqriBandwidthState,
                     supports_mon_evt_id_rdonly_count, true),
    DEFINE_PROP_BOOL("mon_evt_id_wronly_count", RiscvCbqriBandwidthState,
                     supports_mon_evt_id_wronly_count, true),
};

static void riscv_cbqri_bc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = riscv_cbqri_bc_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "RISC-V CBQRI Bandwidth Controller";
    device_class_set_props(dc, riscv_cbqri_bc_properties);
    dc->legacy_reset = riscv_cbqri_bc_reset;
    dc->user_creatable = true;
}

static const TypeInfo riscv_cbqri_bc_info = {
    .name          = TYPE_RISCV_CBQRI_BC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RiscvCbqriBandwidthState),
    .class_init    = riscv_cbqri_bc_class_init,
};

static void riscv_cbqri_bc_register_types(void)
{
    type_register_static(&riscv_cbqri_bc_info);
}

DeviceState *riscv_cbqri_bc_create(hwaddr addr,
                                   const RiscvCbqriBandwidthCaps *caps,
                                   const char *target_name)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_CBQRI_BC);

    qdev_prop_set_uint64(dev, "mmio_base", addr);
    qdev_prop_set_string(dev, "target", target_name);
    qdev_prop_set_uint16(dev, "max_mcids", caps->nb_mcids);
    qdev_prop_set_uint16(dev, "max_rcids", caps->nb_rcids);
    qdev_prop_set_uint16(dev, "mrbwb", caps->mrbwb);
    qdev_prop_set_uint16(dev, "nbwblks", caps->nbwblks);

    qdev_prop_set_bit(dev, "at_data",
                      caps->supports_at_data);
    qdev_prop_set_bit(dev, "at_code",
                      caps->supports_at_code);
    qdev_prop_set_bit(dev, "alloc_op_config_limit",
                      caps->supports_alloc_op_config_limit);
    qdev_prop_set_bit(dev, "alloc_op_read_limit",
                      caps->supports_alloc_op_read_limit);
    qdev_prop_set_bit(dev, "inactive-entry",
                      caps->supports_inactive_entry);
    qdev_prop_set_bit(dev, "rcid-map", caps->supports_rcid_map);
    qdev_prop_set_bit(dev, "mcid-map", caps->supports_mcid_map);
    qdev_prop_set_bit(dev, "mon_op_config_event",
                      caps->supports_mon_op_config_event);
    qdev_prop_set_bit(dev, "mon_op_read_counter",
                      caps->supports_mon_op_read_counter);
    qdev_prop_set_bit(dev, "mon_evt_id_none",
                      caps->supports_mon_evt_id_none);
    qdev_prop_set_bit(dev, "mon_evt_id_rdwr_count",
                      caps->supports_mon_evt_id_rdwr_count);
    qdev_prop_set_bit(dev, "mon_evt_id_rdonly_count",
                      caps->supports_mon_evt_id_rdonly_count);
    qdev_prop_set_bit(dev, "mon_evt_id_wronly_count",
                      caps->supports_mon_evt_id_wronly_count);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    return dev;
}

void get_bc_details(DeviceState *ds, const char *type, RQSC *rqsc)
{
    if (strcmp(type, TYPE_RISCV_CBQRI_BC) == 0) {
        RiscvCbqriBandwidthState *bcs = RISCV_CBQRI_BC(ds);
        (rqsc)->controllerType = 1;
        (rqsc)->mmio_base = bcs->mmio_base;
        (rqsc)->rcidCount = bcs->nb_rcids;
        (rqsc)->mcidCount = bcs->nb_mcids;
    }
}

type_init(riscv_cbqri_bc_register_types)

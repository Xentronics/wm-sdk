/* Copyright 2017 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>   // For PRIu32, PRIu8

#include "msap.h"
#include "function_codes.h"
#include "waps_private.h"
#include "waddr.h"
#include "waps/protocol/waps_protocol.h"
#include "api.h"
#include "lock_bits.h"
#include "persistent.h"

/* Request handlers */
static bool stackStart(waps_item_t * item);
static bool stackStop(waps_item_t * item);

/**
 * \brief   Attempts writing base cost for sink
 * \param   item
 *          Item containing WAPS frame
 * \return  true, if frame was accepted
 */
static bool writeSinkCost(waps_item_t * item);

/**
 * \brief   Attempts reading base cost for sink
 * \param   item
 *          Item containing WAPS frame
 * \return  true, if frame was accepted
 */
static bool readSinkCost(waps_item_t * item);

/**
 * \brief   Write new style app config
 * \param   item
 *          Item containing WAPS frame
 * \return  true, if frame was accepted
 */
static bool writeInterest(waps_item_t * item);

/**
 * \brief   Read new style app config
 * \param   item
 *          Item containing WAPS frame
 * \return  true, if frame was accepted
 */
static bool readInterest(waps_item_t * item);

static bool getNbors(waps_item_t * item);
static bool startScanNbors(waps_item_t * item);
static bool attrReadReq(waps_item_t * item);
static attribute_result_e readAttr(attr_t attr_id,
                                   uint8_t * value,
                                   uint8_t attr_size);
static bool attrWriteReq(waps_item_t * item);
static attribute_result_e writeAttr(attr_t attr_id,
                                    const uint8_t * value,
                                    uint8_t attr_size);
static bool pollRequest(waps_item_t * item);
static bool scratchpadStart(waps_item_t * item);
static bool scratchpadBlock(waps_item_t * item);
static bool scratchpadStatus(waps_item_t * item);
static bool scratchpadSetUpdate(waps_item_t * item);
static bool scratchpadClear(waps_item_t * item);
static bool remoteStatus(waps_item_t * item);
static bool remoteUpdate(waps_item_t * item);
static bool sleep_request(waps_item_t * item);
static bool sleep_stop_request(waps_item_t * item);
static bool sleep_state_request(waps_item_t * item);
static bool sleep_gotosleepinfo_request(waps_item_t * item);
static bool max_msg_queuing_time_write_req(waps_item_t * item);
static bool max_msg_queuing_time_read_req(waps_item_t * item);

/* Map attr id to attr length */
static const uint8_t m_attr_size_lut[] =
{
    MSAP_ATTR_STACK_STATUS_SIZE,
    MSAP_ATTR_PDU_BUFF_USAGE_SIZE,
    MSAP_ATTR_PDU_BUFF_CAP_SIZE,
    MSAP_ATTR_NBOR_COUNT_SIZE,
    MSAP_ATTR_ENERGY_SIZE,
    MSAP_ATTR_AUTOSTART_SIZE,
    MSAP_ATTR_ROUTE_COUNT_SIZE,
    MSAP_ATTR_SYSTEM_TIME_SIZE,
    MSAP_ATTR_AC_RANGE_SIZE,
    MSAP_ATTR_AC_LIMITS_SIZE,
    MSAP_ATTR_CURRENT_AC_SIZE,
    MSAP_ATTR_SCRATCHPAD_BLOCK_MAX_SIZE,
    MSAP_ATTR_MCAST_GROUPS_SIZE,
};

/** App stack state flags */
typedef enum
{
    APP_STACK_STARTED                  = 0,
    APP_STACK_STOPPED                  = 1,
    APP_STACK_RADIO_ADDRESS_NOT_SET    = 2,
    APP_STACK_NODE_ID_NOT_SET          = 4,
    APP_STACK_RADIO_CHANNEL_NOT_SET    = 8,
    APP_STACK_ROLE_NOT_SET             = 16,
    APP_STACK_INTERESTS_MISSING        = 32,
    APP_STACK_ACCESS_DENIED            = 128,
} app_stack_state_flags_e;

/** Msap stack stop result values */
typedef enum
{
    APP_STACK_STOP_RET_OK              = 0,
    APP_STACK_STOP_RET_ALREADY_STOPPED = 1,
    APP_STACK_STOP_RET_ACCESS_DENIED   = 128,
} app_stack_stop_res_e;

/** Msap app sink cost return values */
typedef enum
{
    /** Set and get cost are the same */
    APP_STATE_SINK_COST_OK              = 0,
    /** Only reason to fail set/get cost is wrong role */
    APP_STATE_SINK_COST_INVALID_ROLE    = 1,
    /** The operation is not permitted */
    APP_STATE_SINK_COST_ACCESS_DENIED   = 2,
} app_state_sink_cost_e;

/** Msap app config read result values */
typedef enum
{
    APP_CONFIG_READ_RET_SUCCESS = 0,
    APP_CONFIG_READ_RET_FAILURE,
    APP_CONFIG_READ_RET_ACCESS_DENIED,
} app_config_read_res_e;

/** Msap app config write result values */
typedef enum
{
    APP_CONFIG_WRITE_RET_SUCCESS = 0,
    APP_CONFIG_WRITE_RET_FAILURE_NOT_SINK,
    APP_CONFIG_WRITE_RET_FAILURE_INVALID_INTERVAL,
    APP_CONFIG_WRITE_RET_FAILURE_INVALID_SEQ,
    APP_CONFIG_WRITE_RET_ACCESS_DENIED,
} app_config_write_res_e;

/* Convert read app_lib_data_app_config_res_e to app_config_res_e */
static app_config_read_res_e ReadLibAppCfg2appCfg(app_lib_data_app_config_res_e result)
{
    app_config_read_res_e ret = APP_CONFIG_READ_RET_SUCCESS;

    switch (result)
    {
        case(APP_LIB_DATA_APP_CONFIG_RES_SUCCESS):
            ret = APP_CONFIG_READ_RET_SUCCESS;
            break;
        case(APP_LIB_DATA_APP_CONFIG_RES_INVALID_APP_CONFIG):
            ret = APP_CONFIG_READ_RET_FAILURE;
            break;
        default:
            ret = APP_CONFIG_READ_RET_ACCESS_DENIED;
    }

    return ret;
}


/* Convert write app_lib_data_app_config_res_e to app_config_res_e */
static app_config_write_res_e WriteLibAppCfg2appCfg(app_lib_data_app_config_res_e result)
{
    app_config_write_res_e ret = APP_CONFIG_WRITE_RET_SUCCESS;

    switch (result)
    {
        case(APP_LIB_DATA_APP_CONFIG_RES_SUCCESS):
            ret = APP_CONFIG_WRITE_RET_SUCCESS;
            break;
        case(APP_LIB_DATA_APP_CONFIG_RES_INVALID_ROLE):
            ret = APP_CONFIG_WRITE_RET_FAILURE_NOT_SINK;
            break;
        case(APP_LIB_DATA_APP_CONFIG_RES_INVALID_INTERVAL):
            ret = APP_CONFIG_WRITE_RET_FAILURE_INVALID_INTERVAL;
            break;
        case(APP_LIB_DATA_APP_CONFIG_RES_INVALID_SEQ):
            ret = APP_CONFIG_WRITE_RET_FAILURE_INVALID_SEQ;
            break;
        default:
            ret = APP_CONFIG_WRITE_RET_ACCESS_DENIED;
    }

    return ret;
}

/* Convert app_res_e to attribute_result_e */
static attribute_result_e appRes2attrRes(app_res_e result)
{
    attribute_result_e attr_res = ATTR_SUCCESS;
    switch(result)
    {
        case(APP_RES_OK):
            attr_res = ATTR_SUCCESS;
            break;
        case(APP_RES_UNSPECIFIED_ERROR):
            attr_res = ATTR_UNSUPPORTED_ATTRIBUTE;
            break;
        case(APP_RES_INVALID_VALUE):
            attr_res = ATTR_INV_VALUE;
            break;
        case(APP_RES_INVALID_CONFIGURATION):
            attr_res = ATTR_INV_VALUE;
            break;
        case(APP_RES_INVALID_STACK_STATE):
            attr_res = ATTR_INVALID_STACK_STATE;
            break;
        case(APP_RES_ACCESS_DENIED):
            attr_res = ATTR_ACCESS_DENIED;
            break;
        default:
            attr_res = ATTR_UNSUPPORTED_ATTRIBUTE;
    }

    return attr_res;
}

/* Convert app_res_e to app_state_sink_cost_e */
static app_state_sink_cost_e appRes2appSinkCost(app_res_e result)
{
    app_state_sink_cost_e state;

    switch (result)
    {
        case (APP_RES_OK):
            state = APP_STATE_SINK_COST_OK;
            break;
        case (APP_RES_INVALID_CONFIGURATION):
            state = APP_STATE_SINK_COST_INVALID_ROLE;
            break;
        default:
            state = APP_STATE_SINK_COST_ACCESS_DENIED;
    }

    return state;
}

static app_stack_state_flags_e
    appLibState2SappStackState(app_lib_state_stack_state_e result)
{
    app_stack_state_flags_e state = APP_STACK_STARTED;

    if (result & APP_LIB_STATE_STOPPED)
    {
        state |=  APP_STACK_STOPPED;
    }
    if (result & APP_LIB_STATE_NODE_ADDRESS_NOT_SET)
    {
        state |=  APP_STACK_NODE_ID_NOT_SET;
    }
    if (result & APP_LIB_STATE_NETWORK_ADDRESS_NOT_SET)
    {
        state |=  APP_STACK_RADIO_ADDRESS_NOT_SET;
    }
    if (result & APP_LIB_STATE_NETWORK_CHANNEL_NOT_SET)
    {
        state |=  APP_STACK_RADIO_CHANNEL_NOT_SET;
    }
    if (result & APP_LIB_STATE_ROLE_NOT_SET)
    {
        state |=  APP_STACK_ROLE_NOT_SET;
    }
    if (result & APP_LIB_STATE_APP_CONFIG_DATA_NOT_SET)
    {
        state |=  APP_STACK_INTERESTS_MISSING;
    }
    if (result & APP_LIB_STATE_ACCESS_DENIED)
    {
        state |=  APP_STACK_ACCESS_DENIED;
    }

    return state;
}
bool Msap_handleFrame(waps_item_t * item)
{
    switch (item->frame.sfunc)
    {
        case WAPS_FUNC_MSAP_STACK_START_REQ:
            return stackStart(item);
        case WAPS_FUNC_MSAP_STACK_STOP_REQ:
            return stackStop(item);
        case WAPS_FUNC_MSAP_APP_CONFIG_WRITE_REQ:
            return writeInterest(item);
        case WAPS_FUNC_MSAP_APP_CONFIG_READ_REQ:
            return readInterest(item);
        case WAPS_FUNC_MSAP_ATTR_READ_REQ:
            return attrReadReq(item);
        case WAPS_FUNC_MSAP_ATTR_WRITE_REQ:
            return attrWriteReq(item);
        case WAPS_FUNC_MSAP_INDICATION_POLL_REQ:
            return pollRequest(item);
        case WAPS_FUNC_MSAP_SCRATCHPAD_START_REQ:
            return scratchpadStart(item);
        case WAPS_FUNC_MSAP_SCRATCHPAD_BLOCK_REQ:
            return scratchpadBlock(item);
        case WAPS_FUNC_MSAP_SCRATCHPAD_STATUS_REQ:
            return scratchpadStatus(item);
        case WAPS_FUNC_MSAP_SCRATCHPAD_BOOTABLE_REQ:
            return scratchpadSetUpdate(item);
        case WAPS_FUNC_MSAP_SCRATCHPAD_CLEAR_REQ:
            return scratchpadClear(item);
        case WAPS_FUNC_MSAP_REMOTE_STATUS_REQ:
            return remoteStatus(item);
        case WAPS_FUNC_MSAP_REMOTE_UPDATE_REQ:
            return remoteUpdate(item);
        case WAPS_FUNC_MSAP_GET_NBORS_REQ:
            return getNbors(item);
        case WAPS_FUNC_MSAP_SCAN_NBORS_REQ:
            return startScanNbors(item);
        case WAPS_FUNC_MSAP_SINK_COST_WRITE_REQ:
            return writeSinkCost(item);
        case WAPS_FUNC_MSAP_SINK_COST_READ_REQ:
            return readSinkCost(item);
        case WAPS_FUNC_MSAP_STACK_SLEEP_REQ:
            return sleep_request(item);
        case WAPS_FUNC_MSAP_STACK_SLEEP_STOP_REQ:
            return sleep_stop_request(item);
        case WAPS_FUNC_MSAP_STACK_SLEEP_STATE_GET_REQ:
            return sleep_state_request(item);
        case WAPS_FUNC_MSAP_STACK_SLEEP_GOTOSLEEPINFO_REQ:
            return sleep_gotosleepinfo_request(item);
        case WAPS_FUNC_MSAP_MAX_MSG_QUEUEING_TIME_WRITE_REQ:
            return max_msg_queuing_time_write_req(item);
        case WAPS_FUNC_MSAP_MAX_MSG_QUEUEING_TIME_READ_REQ:
            return max_msg_queuing_time_read_req(item);
        default:
            return false;
    }
}

waps_item_t * Msap_getStackStatusIndication(void)
{
    waps_item_t * item = Waps_itemReserve(WAPS_ITEM_TYPE_INDICATION);
    if (item != NULL)
    {
        Waps_item_init(item,
                       WAPS_FUNC_MSAP_STACK_STATE_IND,
                       sizeof(msap_state_ind_t));
        item->time = 0;
        item->frame.sfid = 0;
        msap_state_ind_t * ind = &item->frame.msap.state_ind;
        ind->result = appLibState2SappStackState(lib_state->getStackState());
        ind->queued_indications = 0;
    }
    return item;
}

void Msap_handleAppConfig(uint8_t seq,
                          const uint8_t * config,
                          uint16_t interval,
                          waps_item_t * item)
{
    if(item == NULL)
    {
        // Invalid parameter
        return;
    }
    // Build new indication
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_APP_CONFIG_RX_IND,
                   sizeof(msap_int_ind_t));
    item->time = 0;
    item->frame.sfid = 0;
    msap_int_ind_t * ind = &item->frame.msap.int_ind;
    ind->interval = interval;
    ind->seq = seq;
    // First clear whole buffer
    memset(&ind->config, 0xFF, APP_CONF_MAX);
    // Then set rest
    memcpy(&ind->config, config, APP_CONF_MAX);
    ind->queued_indications = 0;
}

void Msap_onScannedNbors(waps_item_t * item)
{
    // No existing SCAN_NBORS_IND found, allocate new
    if (item != NULL)
    {
        Waps_item_init(item,
                       WAPS_FUNC_MSAP_SCAN_NBORS_IND,
                       sizeof(msap_on_scanned_nbors_ind_t));
        item->time = 0;
        item->frame.sfid = 0;
        msap_on_scanned_nbors_ind_t * ind = &item->frame.msap.on_scanned_nbors;
        ind->scan_ready = true;
        ind->queued_indications = 0;
    }
}

static void updateIndicationCount(waps_item_t * item)
{
    if (item != NULL)
    {
        /* This is not really queued, but "indications pending" */
        uint8_t pending = queued_indications();
        if(Waps_prot_hasIndication())
        {
            pending = 1;
        }
        item->frame.msap.ind_poll_cnf.queued = pending;
    }
}

static bool pollRequest(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    Waps_item_init(item, WAPS_FUNC_MSAP_INDICATION_POLL_CNF, 1);
    item->pre_cb = updateIndicationCount;
    return true;
}

static bool stackStart(waps_item_t * item)
{
    if (item->frame.splen != sizeof(msap_start_req_t))
    {
        return false;
    }
    app_lib_state_stack_state_e state = APP_LIB_STATE_ACCESS_DENIED;
    /* Check that stack start feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_STACK_START))
    {
        // Check for autostart bit
        bool autostart = item->frame.msap.start_req.start_options;
        // Write autostart bit
        if (Persistent_setAutostart(autostart) != APP_RES_OK)
        {
            return false;
        }
        // Try starting the stack
        if (lib_state->startStack() == APP_RES_OK)
        {
            state = APP_LIB_STATE_STARTED;
        }
        else
        {
            state = appLibState2SappStackState(lib_state->getStackState());
        }
    }
    /* Processing done, build response over request */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_START_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = (uint8_t) state;
    return true;
}

static void reboot_callback(waps_item_t * item)
{
    (void)item;
    /* Wait for uart to be empty */
    Waps_prot_flush_hw();
    /* Reset */
    (void)lib_state->stopStack();
}

static bool stackStop(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    app_stack_stop_res_e result = APP_STACK_STOP_RET_OK;
    /* Check that stack stop feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_STACK_STOP))
    {
        // Clear the autostart bit
        if (Persistent_setAutostart(false) != APP_RES_OK)
        {
            return false;
        }
    }
    else
    {
        result = APP_STACK_STOP_RET_ACCESS_DENIED;
    }
    /* Processing done, build response over request */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_STOP_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = (uint8_t) result;
    if (result == APP_STACK_STOP_RET_OK)
    {
        /* Success, reboot even if stack was already stopped */
        item->post_cb = reboot_callback;
    }

    return true;
}

static void updateTime(waps_item_t * item)
{
    // Time is RTimer_getUs >> 13;
    uint32_t time = lib_time->getTimestampCoarse();
    // Might encounter unaligned pointer => do not insert directly
    memcpy(&item->frame.attr.read_cnf.attr[0], &time, sizeof(uint32_t));
}

static bool attrReadReq(waps_item_t * item)
{
    read_req_t * req_ptr = &item->frame.attr.read_req;
    attr_t attr_id = req_ptr->attr_id;
    attribute_result_e result = ATTR_UNSUPPORTED_ATTRIBUTE;
    uint32_t idx = attr_id - 1;
    uint8_t attr_size = 0;

    if (item->frame.splen != sizeof(read_req_t))
    {
        return false;
    }

    /* Check attribute ID */
    if (idx >= sizeof(m_attr_size_lut))
    {
        result = ATTR_UNSUPPORTED_ATTRIBUTE;
        goto build_response;
    }

    /* Check that MSAP attribute read feature is permitted */
    if (attr_id == MSAP_ATTR_SCRATCHPAD_BLOCK_MAX)
    {
        if (!LockBits_isFeaturePermitted(
                LOCK_BITS_MSAP_SCRATCHPAD_STATUS))
        {
            result = ATTR_ACCESS_DENIED;
            goto build_response;
        }
    }
    else if ((attr_id == MSAP_ATTR_NBOR_COUNT) ||
             (attr_id == MSAP_ATTR_ROUTE_COUNT))
    {
        if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_GET_NBORS))
        {
            result = ATTR_ACCESS_DENIED;
            goto build_response;
        }
    }
    else
    {
        if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_ATTR_READ))
        {
            result = ATTR_ACCESS_DENIED;
            goto build_response;
        }
    }

    /* attribute found in LUT */
    attr_size = m_attr_size_lut[idx];

    /* Read attribute with attribute manager */
    result = readAttr(attr_id, item->frame.attr.read_cnf.attr, attr_size);

    /* Processing done, build response over request */
build_response:
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_ATTR_READ_CNF,
                   FRAME_READ_CNF_HEADER_SIZE);
    if (result == ATTR_SUCCESS)
    {
        item->frame.splen += attr_size;
        item->frame.attr.read_cnf.attr_len = attr_size;
        if (attr_id == MSAP_ATTR_SYSTEM_TIME)
        {
            /* Read system time at last possible moment */
            item->pre_cb = updateTime;
        }
    }
    else
    {
        item->frame.attr.read_cnf.attr_len = 0;
    }
    item->frame.attr.read_cnf.result = (uint8_t)result;
    item->frame.attr.read_cnf.attr_id = attr_id;
    return true;
}

static attribute_result_e readAttr(attr_t attr_id,
                                   uint8_t * value,
                                   uint8_t attr_size)
{
    uint32_t tmp;   // Needed for 32-bit alignment
    app_res_e result = APP_RES_OK;
    switch (attr_id)
    {
        case MSAP_ATTR_STACK_STATUS:
            tmp = appLibState2SappStackState(lib_state->getStackState());
            break;
        case MSAP_ATTR_PDU_BUFF_USAGE:
            {
                size_t free = 0;
                result = lib_data->getNumFreeBuffers(&free);
                tmp = (uint32_t)lib_data->getNumBuffers();
                tmp -= (uint32_t)free;
            }
            break;
        case MSAP_ATTR_PDU_BUFF_CAPA:
            result = lib_data->getNumFreeBuffers((size_t*)&tmp);
            break;
        case MSAP_ATTR_NBOR_COUNT:
            // Deprecated
            result = APP_RES_NOT_IMPLEMENTED;
            break;
        case MSAP_ATTR_ENERGY:
            result = lib_state->getEnergy((uint8_t *)&tmp);
            break;
        case MSAP_ATTR_AUTOSTART:
            lib_storage->readPersistent(&tmp, sizeof(tmp));
            tmp &= MSAP_AUTOSTART;
            break;
        case MSAP_ATTR_ROUTE_COUNT:
            result = lib_state->getRouteCount((size_t*)&tmp);
            break;
        case MSAP_ATTR_SYSTEM_TIME:
            tmp = 0;    /* Not handled here */
            break;
        case MSAP_ATTR_AC_RANGE:
        {
            uint16_t min_ac_ms = 0;
            uint16_t max_ac_ms = 0;
            result = lib_settings->getAcRange(&min_ac_ms, &max_ac_ms);
            tmp = ((uint32_t)min_ac_ms & 0xffff) + ((uint32_t)max_ac_ms << 16);
            break;
        }
        case MSAP_ATTR_AC_LIMITS:
        {
            uint16_t min_ac_ms = 0;
            uint16_t max_ac_ms = 0;
            result = lib_settings->getAcRangeLimits(&min_ac_ms, &max_ac_ms);
            tmp = ((uint32_t)min_ac_ms & 0xffff) + ((uint32_t)max_ac_ms << 16);
            break;
        }
        case MSAP_ATTR_CURRENT_AC:
            result = lib_state->getAccessCycle((uint16_t *)&tmp);
            break;
        case MSAP_ATTR_SCRATCHPAD_BLOCK_MAX:
            tmp = MSAP_SCRATCHPAD_BLOCK_MAX_NUM_BYTES;
            break;
        case MSAP_ATTR_MCAST_GROUPS:
            result = Multicast_getGroups(value);
            break;
        default:
            /* Unsupported attribute */
            result = APP_RES_NOT_IMPLEMENTED;
            break;
    }
    if ((result == APP_RES_OK) && (attr_size <= sizeof(tmp)))
    {
        memcpy(value, &tmp, attr_size);
    }

    return appRes2attrRes(result);
}

static bool attrWriteReq(waps_item_t * item)
{
    write_req_t * req_ptr = &item->frame.attr.write_req;
    attr_t attr_id = req_ptr->attr_id;
    uint32_t idx = attr_id - 1;
    uint8_t attr_size = 0;
    attribute_result_e result = ATTR_SUCCESS;

    if (item->frame.splen != (FRAME_WRITE_REQ_HEADER_SIZE +
                             req_ptr->attr_len))
    {
        return false;
    }

    /* Check attribute ID */
    if (idx >= sizeof(m_attr_size_lut))
    {
        result = ATTR_UNSUPPORTED_ATTRIBUTE;
        goto build_response;
    }


    /* Check that MSAP attribute write feature is permitted */
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_ATTR_WRITE))
    {
        result = ATTR_ACCESS_DENIED;
        goto build_response;
    }

    /* attribute found in LUT */
    attr_size = m_attr_size_lut[idx];

    /* Check length */
    if (attr_size != req_ptr->attr_len)
    {
        result = ATTR_INV_LENGTH;
        goto build_response;
    }

    /* Write attribute with attribute manager */
    result = writeAttr(attr_id, req_ptr->attr, attr_size);

    /* Processing done, build response over request */
build_response:
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_ATTR_WRITE_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = (uint8_t)result;
    return true;
}

static attribute_result_e writeAttr(attr_t attr_id,
                                   const uint8_t * value,
                                   uint8_t attr_size)
{
    app_res_e result = APP_RES_OK;
    uint32_t tmp = 0;
    if (attr_size <= 4)
    {
        memcpy(&tmp, value, attr_size);
    }
    switch (attr_id)
    {
        case MSAP_ATTR_ENERGY:
            result = lib_state->setEnergy(tmp);
            break;
        case MSAP_ATTR_AUTOSTART:
            if(tmp > 1)
            {
                result = APP_RES_INVALID_VALUE;
            }
            else
            {
                result = Persistent_setAutostart(tmp);
            }
            break;
        case MSAP_ATTR_AC_RANGE:
        {
            uint16_t min_ac_ms = tmp & 0xffff;
            uint16_t max_ac_ms = (tmp >> 16) & 0xffff;
            result = lib_settings->setAcRange(min_ac_ms, max_ac_ms);
            break;
        }
        case MSAP_ATTR_MCAST_GROUPS:
            result = Multicast_setGroups(value);
            break;
        default:
            result = APP_RES_NOT_IMPLEMENTED;
            break;
    }

    return appRes2attrRes(result);

}

static bool writeInterest(waps_item_t * item)
{
    if (item->frame.splen != sizeof(msap_int_write_req_t))
    {
        return false;
    }

    app_lib_data_app_config_res_e result =
        APP_LIB_DATA_APP_CONFIG_RES_INVALID_NULL_POINTER;
    /* Check that write app config data feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_APP_CONFIG_WRITE))
    {

        msap_int_write_req_t * req = &item->frame.msap.int_write_req;
        result = lib_data->writeAppConfig(req->config,
                                          req->seq,
                                          req->interval);
    }
    /* Processing done, build response over request */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_APP_CONFIG_WRITE_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = WriteLibAppCfg2appCfg(result);
    return true;
}

static bool readInterest(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    uint16_t seconds = 0;
    uint8_t seq = 0;
    app_lib_data_app_config_res_e result =
        APP_LIB_DATA_APP_CONFIG_RES_INVALID_NULL_POINTER;
    /* Check that read app config data feature is permitted */
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_APP_CONFIG_READ))
    {
        // Access denied
        goto generate_response;
    }

    result = lib_data->readAppConfig(item->frame.msap.int_read_cnf.config,
                                     &seq,
                                     &seconds);

    /* Processing done, build response over request */
generate_response:
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_APP_CONFIG_READ_CNF,
                   sizeof(msap_int_read_cnf_t));
    item->frame.msap.int_read_cnf.interval = seconds;
    item->frame.msap.int_read_cnf.seq = seq;
    item->frame.msap.int_read_cnf.result = ReadLibAppCfg2appCfg(result);
    return true;
}

static bool writeSinkCost(waps_item_t * item)
{
    if(item->frame.splen != sizeof(msap_sink_cost_write_req_t))
    {
        return false;
    }
    app_res_e result = APP_RES_ACCESS_DENIED;
    /* Check that sink cost write feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SINK_COST_WRITE))
    {
        // Get cost from frame
        uint8_t cost = item->frame.msap.cost_write_req.base_cost;
        // Attempt setting  cost
        result = lib_state->setSinkCost(cost);
    }
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SINK_COST_WRITE_CNF,
                   sizeof(simple_cnf_t));

    item->frame.simple_cnf.result = appRes2appSinkCost(result);
    return true;
}

static bool readSinkCost(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    uint8_t cost = 0;
    app_res_e result = APP_RES_ACCESS_DENIED;

    /* Check that sink cost read feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SINK_COST_READ))
    {

        // Attempt setting  cost
        result = lib_state->getSinkCost(&cost);
    }
    // Build response
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SINK_COST_READ_CNF,
                   sizeof(msap_sink_cost_read_cnf_t));

    item->frame.msap.cost_read_cnf.result = appRes2appSinkCost(result);
    item->frame.msap.cost_read_cnf.base_cost = cost;
    return true;
}

static bool getNbors(waps_item_t * item)
{
    // Expected request payload = 0 bytes
    if (item->frame.splen != 0)
    {
        return false;
    }
    // Start building response
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_GET_NBORS_CNF,
                   sizeof(msap_neighbors_cnf_t));
    // Clear entries
    memset(item->frame.msap.nbor_cnf.neighbors,
           0,
           sizeof(msap_neighbor_entry_t) * MSAP_MAX_NBORS);
    // Check that neighbors info feature is permitted
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_GET_NBORS))
    {
        // Access denied, return a block of zeros
        return true;
    }
    app_res_e result = APP_RES_OK;
    app_lib_state_nbor_info_t nbors[MSAP_MAX_NBORS];
    app_lib_state_nbor_list_t nbors_list =
    {
        .number_nbors = MSAP_MAX_NBORS,
        .nbors = &nbors[0],
    };
    // This is for reason that item->frame.msap.nbor_cnf.neighbors is not
    // necessarily correctly aligned
    msap_neighbor_entry_t nbor_entry;
    result = lib_state->getNbors(&nbors_list);
    if (result == APP_RES_OK)
    {
        uint32_t idx = 0;
        app_lib_state_nbor_info_t * info = &nbors[0];
        for(idx = 0; idx < nbors_list.number_nbors; idx++)
        {
            // Clear
            memset(&nbor_entry, 0, sizeof(msap_neighbor_entry_t));
            // Copy
            nbor_entry.addr = Addr_to_Waddr((app_addr_t)info->address);
            nbor_entry.channel = info->channel;
            nbor_entry.cost_0 = info->cost;
            // Convert last update time-stamp to time since last update
            nbor_entry.last_update = info->last_update;
            nbor_entry.link_rel = info->link_reliability;
            nbor_entry.rssi_norm = info->norm_rssi;
            nbor_entry.rx_power_level = info->rx_power;
            nbor_entry.tx_power_level = info->tx_power;
            // Do the switcharoo
            if(info->type == APP_LIB_STATE_NEIGHBOR_IS_NEXT_HOP)
            {
                nbor_entry.type = APP_LIB_STATE_NEIGHBOR_IS_NEXT_HOP;
            }
            else if(info->type == APP_LIB_STATE_NEIGHBOR_IS_MEMBER)
            {
                nbor_entry.type = NEIGHBOR_IS_MEMBER;
            }
            else
            {
                nbor_entry.type = NEIGHBOR_IS_CLUSTER;
            }
            // Copy out
            memcpy(&item->frame.msap.nbor_cnf.neighbors[idx],
                   &nbor_entry,
                   sizeof(msap_neighbor_entry_t));
            info++;
        }

        item->frame.msap.nbor_cnf.result = nbors_list.number_nbors;
    }
    else
    {
        item->frame.msap.nbor_cnf.result = 0;
        return false;
    }
    return true;
}

static bool startScanNbors(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }

    msap_scan_nbors_status_e result = MSAP_SCAN_NBORS_ACCESS_DENIED;
    // Check that scan neighbors feature is permitted
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SCAN_NBORS))
    {
        // Start to scan
        result = lib_state->startScanNbors();
    }
    // Build response
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCAN_NBORS_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;

    return true;
}

/** \brief  Clear the scratchpad */
static bool scratchpadClear(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    msap_scratchpad_clear_e result = MSAP_SCRATCHPAD_CLEAR_SUCCESS;
    /* Check that scratchpad start feature (same bit for clear) is permitted */
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SCRATCHPAD_START))
    {
        result = MSAP_SCRATCHPAD_CLEAR_ACCESS_DENIED;
    }
    else if (lib_state->getStackState() == APP_LIB_STATE_STARTED)
    {
        result = MSAP_SCRATCHPAD_CLEAR_INVALID_STATE;
    }
    else
    {
        /* Clearing the scratchpad may take seconds. We could do this in a
         * callback and send the confirmation immediately, but then the user
         * won't know when the clearing is done.
         */
        lib_otap->clear();
    }
    /* Build response */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCRATCHPAD_CLEAR_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

/** \brief  Write the scratchpad header */
static bool scratchpadStart(waps_item_t * item)
{
    if (item->frame.splen != sizeof(msap_scratchpad_start_req_t))
    {
        return false;
    }
    msap_scratchpad_start_e result = MSAP_SCRATCHPAD_START_SUCCESS;
    /* Check that scratchpad start feature is permitted */
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SCRATCHPAD_START))
    {
        result = MSAP_SCRATCHPAD_START_ACCESS_DENIED;
    }
    else if (lib_state->getStackState() == APP_LIB_STATE_STARTED)
    {
        result = MSAP_SCRATCHPAD_START_INVALID_STATE;
    }
    else
    {
        msap_scratchpad_start_req_t * req =
            &item->frame.msap.scratchpad_start_req;

        /* Start writing to scratchpad. Scratchpad is implicitly cleared,
         * if not already cleared. Again, this may take several seconds.
         */
        if (lib_otap->begin(req->num_bytes, req->seq) != APP_RES_OK)
        {
            result = MSAP_SCRATCHPAD_START_INVALID_NUM_BYTES;
        }
    }
    /* Build response */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCRATCHPAD_START_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

/** \brief  Write a block of words to the scratchpad */
static bool scratchpadBlock(waps_item_t * item)
{
    msap_scratchpad_block_e result = MSAP_SCRATCHPAD_BLOCK_SUCCESS;
    msap_scratchpad_block_req_t * req = &item->frame.msap.scratchpad_block_req;
    if (item->frame.splen != (FRAME_MSAP_SCRATCHPAD_BLOCK_REQ_HEADER_SIZE +
                              req->num_bytes))
    {
        return false;
    }
    /* Store frame id, so we won't lose it later */
    uint8_t sfid = item->frame.sfid;
    if (lib_state->getStackState() == APP_LIB_STATE_STARTED)
    {
        result = MSAP_SCRATCHPAD_BLOCK_INVALID_STATE;
    }
    else
    {
        switch(lib_otap->write(req->start_addr,
                                     req->num_bytes,
                                     req->bytes))
        {
            case APP_LIB_OTAP_WRITE_RES_OK:
                result = MSAP_SCRATCHPAD_BLOCK_SUCCESS;
                break;
            case APP_LIB_OTAP_WRITE_RES_COMPLETED_OK:
                result = MSAP_SCRATCHPAD_BLOCK_COMPLETED_OK;
                break;
            case APP_LIB_OTAP_WRITE_RES_COMPLETED_ERROR:
                result = MSAP_SCRATCHPAD_BLOCK_COMPLETED_ERROR;
                break;
            case APP_LIB_OTAP_WRITE_RES_NOT_ONGOING:
                result = MSAP_SCRATCHPAD_BLOCK_NOT_ONGOING;
                break;
            case APP_LIB_OTAP_WRITE_RES_INVALID_START:
                result = MSAP_SCRATCHPAD_BLOCK_INVALID_START_ADDR;
                break;
            case APP_LIB_OTAP_WRITE_RES_INVALID_NUM_BYTES:
                result = MSAP_SCRATCHPAD_BLOCK_INVALID_NUM_BYTES;
                break;
            case APP_LIB_OTAP_WRITE_RES_INVALID_HEADER:
            case APP_LIB_OTAP_WRITE_RES_INVALID_NULL_BYTES:
                result = MSAP_SCRATCHPAD_BLOCK_INVALID_DATA;
                break;
        }
    }
    /* Build response */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCRATCHPAD_BLOCK_CNF,
                   sizeof(simple_cnf_t));
    item->frame.sfid = sfid;
    item->frame.simple_cnf.result = result;
    return true;
}

/** \brief  Report scratchpad contents */
static bool scratchpadStatus(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCRATCHPAD_STATUS_CNF,
                   sizeof(msap_scratchpad_status_cnf_t));
    /* Check that scratchpad status feature is permitted */
    if (LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SCRATCHPAD_STATUS))
    {
        msap_scratchpad_status_cnf_t * item_cnf =
            &item->frame.msap.scratchpad_status_cnf;
        app_firmware_version_t fw_version;

        item_cnf->num_bytes = lib_otap->getNumBytes();
        item_cnf->crc = lib_otap->getCrc();
        item_cnf->seq = lib_otap->getSeq();
        item_cnf->type = lib_otap->getType();
        item_cnf->status = (uint8_t)(lib_otap->getStatus() & 0xff);
        item_cnf->processed_num_bytes = (uint32_t)lib_otap->getProcessedNumBytes();
        item_cnf->processed_crc = lib_otap->getProcessedCrc();
        item_cnf->processed_seq = lib_otap->getProcessedSeq();
        item_cnf->area_id = lib_otap->getProcessedAreaId();
        fw_version = global_func->getStackFirmwareVersion();
        item_cnf->major_version = fw_version.major;
        item_cnf->minor_version = fw_version.minor;
        item_cnf->maint_version = fw_version.maint;
        item_cnf->devel_version = fw_version.devel;
    }
    else
    {
        /* Access denied, return a block of zeros */
        memset(&(item->frame.msap.scratchpad_status_cnf),
               0,
               sizeof(msap_scratchpad_status_cnf_t));
    }
    return true;
}

/** \brief  Mark stored image as bootable */
static bool scratchpadSetUpdate(waps_item_t * item)
{
    if (item->frame.splen != 0)
    {
        return false;
    }
    msap_scratchpad_bootable_result_e result = MSAP_SCRATCHPAD_BOOTABLE_SUCCESS;
    /* Check that scratchpad start feature (same bit for update) is permitted */
    if (!LockBits_isFeaturePermitted(LOCK_BITS_MSAP_SCRATCHPAD_START))
    {
        result = MSAP_SCRATCHPAD_BOOTABLE_ACCESS_DENIED;
    }
    else if (lib_state->getStackState() == APP_LIB_STATE_STARTED)
    {
        result = MSAP_SCRATCHPAD_BOOTABLE_INVALID_STATE;
    }
    else if (lib_otap->setToBeProcessed() != APP_RES_OK)
    {
        result = MSAP_SCRATCHPAD_BOOTABLE_NO_SCRATCHPAD;
    }
    /* Build response */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_SCRATCHPAD_BOOTABLE_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

static bool remoteStatus(waps_item_t * item)
{
    // Remote status is not implemented by stack anymore
    // Return an existing error code to keep compatibility
    msap_remote_status_e result = MSAP_REMOTE_STATUS_ACCESS_DENIED;

    if (item->frame.splen != sizeof(msap_remote_status_req_t))
    {
        return false;
    }

    /* Build response */
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_REMOTE_STATUS_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

static bool remoteUpdate(waps_item_t * item)
{
    // Remote status is not implemented by stack anymore
    // Return an existing error code to keep compatibility
    msap_remote_update_e result = MSAP_REMOTE_UPDATE_ACCESS_DENIED;

    if (item->frame.splen != sizeof(msap_remote_update_req_t))
    {
        return false;
    }

    Waps_item_init(item,
                   WAPS_FUNC_MSAP_REMOTE_UPDATE_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

static bool sleep_request(waps_item_t * item)
{
    msap_remote_update_e result = MSAP_SLEEP_SUCCESS;

    switch(lib_sleep->sleepStackforTime
          (item->frame.msap.sleep_start_req.seconds,
           item->frame.msap.sleep_start_req.app_config_nrls))
        {
            case APP_RES_OK:
                result = MSAP_SLEEP_SUCCESS;
                break;
            case APP_RES_INVALID_STACK_STATE:
                result = MSAP_SLEEP_INVALID_STATE;
                break;
            case APP_RES_INVALID_CONFIGURATION:
                result = MSAP_SLEEP_INVALID_ROLE;
                break;
            case APP_RES_INVALID_VALUE:
                result = MSAP_SLEEP_INVALID_VALUE;
                break;
            default:
                result = MSAP_SLEEP_ACCESS_DENIED;
                break;
        }
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_SLEEP_REQ_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

static bool sleep_stop_request(waps_item_t * item)
{
    msap_remote_update_e result = MSAP_SLEEP_SUCCESS;

    switch(lib_sleep->wakeupStack())
    {
        case APP_RES_OK:
            result = MSAP_SLEEP_SUCCESS;
            break;
        case APP_RES_INVALID_STACK_STATE:
            result = MSAP_SLEEP_INVALID_STATE;
            break;
        default:
            result = MSAP_SLEEP_ACCESS_DENIED;
            break;
    }
    // Reply works when reset is removed from wakeup in phase2
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_SLEEP_STOP_CNF,
                   sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;
    return true;
}

static bool sleep_state_request(waps_item_t * item)
{
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_SLEEP_STATE_GET_RSP,
                   sizeof(msap_sleep_state_rsp_t));
    if(lib_sleep->getSleepState() == true)
    {
        item->frame.msap.sleep_state_rsp.sleep_started = MSAP_NRLS_SLEEP_ACTIVE;
    }
    else
    {
        item->frame.msap.sleep_state_rsp.sleep_started = MSAP_NRLS_SLEEP_NOT_STARTED;
    }
    item->frame.msap.sleep_state_rsp.sleep_seconds =
        lib_sleep->getStackWakeup();
    return true;
}

static bool sleep_gotosleepinfo_request(waps_item_t * item)
{
    Waps_item_init(item,
                   WAPS_FUNC_MSAP_STACK_SLEEP_GOTOSLEEPINFO_RSP,
                   sizeof(msap_sleep_latest_gotosleep_rsp_t));
    item->frame.msap.sleep_gotosleep_rsp_t.gotoNRSL_seconds = lib_sleep->getSleepLatestGotosleep();
    return true;
}

static bool max_msg_queuing_time_write_req(waps_item_t * item)
{
    bool status                 = false;
    app_res_e result            = APP_RES_INVALID_VALUE;
    waps_func_e conf            = WAPS_FUNC_MSAP_MAX_MSG_QUEUEING_TIME_WRITE_CNF;

    if (item->frame.splen == sizeof (msap_max_msg_queuing_time_req_t))
    {
        app_lib_data_qos_e prio = item->
                                  frame.msap.max_msg_queuing_time_req.priority;
        uint16_t time = item->frame.msap.max_msg_queuing_time_req.time;
        result = lib_data->setMaxMsgQueuingTime(prio,time);
        status = true;
    }
    Waps_item_init(item, conf, sizeof(simple_cnf_t));
    item->frame.simple_cnf.result = result;

    return status;
}

static bool max_msg_queuing_time_read_req(waps_item_t * item)
{
    bool status                 = false;
    app_res_e result            = APP_RES_INVALID_VALUE;
    waps_func_e conf            = WAPS_FUNC_MSAP_MAX_MSG_QUEUEING_TIME_READ_CNF;
    uint16_t time = 0;

    if (item->frame.splen == sizeof(item->
                                 frame.msap.max_msg_queuing_time_req.priority))
    {
        app_lib_data_qos_e prio = item->
                                  frame.msap.max_msg_queuing_time_req.priority;

        result = lib_data->getMaxMsgQueuingTime(prio,&time);
        status = true;
    }
    Waps_item_init(item, conf, sizeof(msap_max_msg_queuing_time_read_cnf_t));
    item->frame.msap.max_msg_queuing_time_read_cnf.time = time;
    item->frame.msap.max_msg_queuing_time_read_cnf.result = result;
    item->frame.splen = sizeof(item->frame.msap.max_msg_queuing_time_read_cnf);

    return status;
}

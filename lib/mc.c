/*
 * mc.c
 *
 * MontaVista IPMI code for handling management controllers
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <OpenIPMI/ipmi_conn.h>
#include <OpenIPMI/ipmiif.h>
#include <OpenIPMI/ipmi_domain.h>
#include <OpenIPMI/ipmi_mc.h>
#include <OpenIPMI/ipmi_sdr.h>
#include <OpenIPMI/ipmi_sel.h>
#include <OpenIPMI/ipmi_entity.h>
#include <OpenIPMI/ipmi_sensor.h>
#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_int.h>
#include <OpenIPMI/ipmi_oem.h>
#include <OpenIPMI/locked_list.h>
#include <OpenIPMI/opq.h>

/* Timer structure for rereading the SEL. */
typedef struct mc_reread_sel_s
{
    int                 cancelled;
    ipmi_mc_t           *mc;
    ipmi_domain_t       *domain;
    ipmi_sels_fetched_t handler;
    void                *cb_data;
} mc_reread_sel_t;

typedef struct mc_devid_data_s
{    
    uint8_t device_id;

    uint8_t device_revision;

    unsigned int provides_device_sdrs : 1;
    unsigned int device_available : 1;

    unsigned int chassis_support : 1;
    unsigned int bridge_support : 1;
    unsigned int IPMB_event_generator_support : 1;
    unsigned int IPMB_event_receiver_support : 1;
    unsigned int FRU_inventory_support : 1;
    unsigned int SEL_device_support : 1;
    unsigned int SDR_repository_support : 1;
    unsigned int sensor_device_support : 1;

    uint8_t major_fw_revision;
    uint8_t minor_fw_revision;

    uint8_t major_version;
    uint8_t minor_version;

    uint32_t manufacturer_id;
    uint16_t product_id;

    uint8_t  aux_fw_revision[4];
} mc_devid_data_t;

typedef struct domain_up_info_s
{
    ipmi_mcid_t mcid;
} domain_up_info_t;

struct ipmi_mc_s
{
    unsigned int usecount;
    ipmi_lock_t *lock;

    ipmi_domain_t *domain;
    long          seq;
    ipmi_addr_t   addr;
    int           addr_len;

    /* Are we currently in need of a cleanup? */
    int cleanup;

    /* If we have any external users that do not have direct
       references, we increment the usercount.  This is primarily the
       internal uses in the active_handlers list, but we cannot use
       that list being empty because it also may have external
       users. */
    int usercount;

    /* If the MC is known to be good in the system, then active is
       true.  If active is false, that means that there are sensors
       that refer to this MC, but the MC is not currently in the
       system. */
    int active;

    /* This is the "real" current active value.  We wait until the MC
       is not in use to change the active value, this keeps track of
       the current value and the number of times is has changed while
       the mc was in use. */
    int curr_active;
    unsigned int active_transitions;

    /* Used to generate unique numbers for the MC. */
    unsigned int uniq_num;

    /* The device SDRs on the MC. */
    ipmi_sdr_info_t *sdrs;

    /* The sensors that came from the device SDR on this MC. */
    ipmi_sensor_t **sensors_in_my_sdr;
    unsigned int  sensors_in_my_sdr_count;

    /* The entities that came from the device SDR on this MC are
       somehow stored in this data structure. */
    void *entities_in_my_sdr;

    /* Sensors that this MC owns (you message this MC to talk to this
       sensor, and events report the MC as the owner. */
    ipmi_sensor_info_t  *sensors;

    ipmi_control_info_t *controls;

    unsigned int in_domain_list : 1; /* Tells if we are in the list of
					our domain yet. */

    /* The system event log, for querying and storing events. */
    ipmi_sel_info_t *sel;

    /* The handler to call for add/delete event operations.  This is NULL
       normally and is only used if the MC has a special delete event
       handler. */
    ipmi_mc_del_event_cb sel_del_event_handler;
    ipmi_mc_add_event_cb sel_add_event_handler;

    /* Timer for rescanning the sel periodically. */
    os_hnd_timer_id_t *sel_timer;
    mc_reread_sel_t   *sel_timer_info;
    unsigned int      sel_scan_interval; /* seconds between SEL scans */

    /* Is the global events enable for the MC enabled? */
    int events_enabled;

    /* The SEL time when the connection first came up.  Only used at
       startup, after the SEL has been read the first time this will
       be set to zero. */
    ipmi_time_t startup_SEL_time;

    void *oem_data;

    ipmi_mc_oem_fixup_sdrs_cb fixup_sdrs_handler;
    void                      *fixup_sdrs_cb_data;

    ipmi_mc_oem_new_sensor_cb new_sensor_handler;
    void                      *new_sensor_cb_data;

    ipmi_oem_event_handler_cb oem_event_handler;
    void                      *oem_event_cb_data;

    ipmi_oem_event_handler_cb sel_oem_event_handler;
    void                      *sel_oem_event_cb_data;

    ipmi_mc_ptr_cb sdrs_first_read_handler;
    void           *sdrs_first_read_cb_data;
    ipmi_mc_ptr_cb sels_first_read_handler;
    void           *sels_first_read_cb_data;

    /* Call these when the MC is destroyed. */
    locked_list_t *removed_handlers;

    /* Call these when the MC changes from active to inactive. */
    locked_list_t *active_handlers;

    /* The following are for waiting until a domain is up before
       starting the SEL query, so that the domain will be registered
       before events are fetched. */
    domain_up_info_t *conup_info;

    /* Set if we are treating main SDRs like device SDRs. */
    int treat_main_as_device_sdrs;

    /* The rest is the actual data from the get device id and SDRs.
       There's the normal version, the pending version, and the
       version.  The real version is the one from the get device id
       response, and normal version may have been adjusted by the OEM
       code.  The pending version is used to hold the data until the
       usecount goes to 0; we don't change the user data until no one
       else is using it. */

    mc_devid_data_t devid;
    mc_devid_data_t real_devid;
    mc_devid_data_t pending_devid;
    int pending_devid_data;
    int pending_new_mc;

    /* Name used for reporting.  We add a ' ' onto the end, thus
       the +1. */
    char name[IPMI_MC_NAME_LEN+1];
};

static void mc_sel_new_event_handler(ipmi_sel_info_t *sel,
				     ipmi_mc_t       *mc,
				     ipmi_event_t    *event,
				     void            *cb_data);

static void sels_fetched_start_timer(ipmi_sel_info_t *sel,
				     int             err,
				     int             changed,
				     unsigned int    count,
				     void            *cb_data);

static void con_up_handler(ipmi_domain_t *domain,
			   int           err,
			   unsigned int  conn_num,
			   unsigned int  port_num,
			   int           still_connected,
			   void          *cb_data);

static void call_active_handlers(ipmi_mc_t *mc);

typedef struct mc_name_info
{
    char name[IPMI_MC_NAME_LEN];
} mc_name_info_t;

/***********************************************************************
 *
 * Routines for creating and destructing MCs.
 *
 **********************************************************************/

static void
mc_set_name(ipmi_mc_t *mc)
{
    int         length;
    ipmi_mcid_t id = ipmi_mc_convert_to_id(mc);

    ipmi_lock(mc->lock);
    length = ipmi_domain_get_name(mc->domain, mc->name, sizeof(mc->name)-3);
    mc->name[length] = '(';
    length++;
    length += snprintf(mc->name+length, IPMI_MC_NAME_LEN-length-3, "%x.%x",
		       id.channel, id.mc_num);
    mc->name[length] = ')';
    length++;
    mc->name[length] = ' ';
    length++;
    mc->name[length] = '\0';
    length++;
    ipmi_unlock(mc->lock);
}

int
ipmi_mc_get_name(ipmi_mc_t *mc, char *name, int length)
{
    int  slen;

    if (length <= 0)
	return 0;

    /* Never changes, no lock needed. */
    slen = strlen(mc->name);
    if (slen == 0) {
	if (name)
	    *name = '\0';
	goto out;
    }

    slen -= 1; /* Remove the trailing ' ' */
    if (slen >= length)
	slen = length - 1;

    if (name)
	memcpy(name, mc->name, slen);
    name[slen] = '\0';
 out:
    return slen;
}

char *
_ipmi_mc_name(ipmi_mc_t *mc)
{
    return mc->name;
}

int
_ipmi_create_mc(ipmi_domain_t *domain,
		ipmi_addr_t   *addr,
		unsigned int  addr_len,
		ipmi_mc_t     **new_mc)
{
    ipmi_mc_t *mc;
    int       rv = 0;

    if (addr_len > sizeof(ipmi_addr_t))
	return EINVAL;

    mc = ipmi_mem_alloc(sizeof(*mc));
    if (!mc)
	return ENOMEM;
    memset(mc, 0, sizeof(*mc));

    mc->usecount = 1; /* Require a release */

    mc->domain = domain;

    mc->seq = ipmi_get_seq();

    mc->events_enabled = 1;

    mc->active = 0; /* Start assuming inactive. */

    mc->sensors = NULL;
    mc->sensors_in_my_sdr = NULL;
    mc->sensors_in_my_sdr_count = 0;
    mc->entities_in_my_sdr = NULL;
    mc->controls = NULL;
    mc->new_sensor_handler = NULL;
    rv = ipmi_create_lock(domain, &mc->lock);
    if (rv)
	goto out_err;
    mc->removed_handlers = locked_list_alloc(ipmi_domain_get_os_hnd(domain));
    if (!mc->removed_handlers) {
	rv = ENOMEM;
	goto out_err;
    }
    mc->active_handlers = locked_list_alloc(ipmi_domain_get_os_hnd(domain));
    if (!mc->active_handlers) {
	rv = ENOMEM;
	goto out_err;
    }
    mc->sel = NULL;
    mc->sel_timer_info = NULL;
    mc->sel_scan_interval = ipmi_domain_get_sel_rescan_time(domain);

    memcpy(&(mc->addr), addr, addr_len);
    mc->addr_len = addr_len;
    mc->sdrs = NULL;

    mc->conup_info = ipmi_mem_alloc(sizeof(*(mc->conup_info)));
    if (!mc->conup_info) {
	rv = ENOMEM;
	goto out_err;
    }

    rv = ipmi_sensors_alloc(mc, &(mc->sensors));
    if (rv)
	goto out_err;

    rv = ipmi_controls_alloc(mc, &(mc->controls));
    if (rv)
	goto out_err;

    rv = ipmi_sel_alloc(mc, 0, &(mc->sel));
    if (rv)
	goto out_err;

    rv = ipmi_sdr_info_alloc(domain, mc, 0, 1, &(mc->sdrs));
    if (rv)
	goto out_err;

    /* When we get new logs, handle them. */
    ipmi_sel_set_new_event_handler(mc->sel,
				   mc_sel_new_event_handler,
				   domain);

    mc_set_name(mc);
 out_err:
    if (rv)
	_ipmi_cleanup_mc(mc);
    else
	*new_mc = mc;

    return rv;
}

static os_handler_t *
mc_get_os_hnd(ipmi_mc_t *mc)
{
    ipmi_domain_t *domain = mc->domain;
    return ipmi_domain_get_os_hnd(domain);
}

static int
call_removed_handler(void *cb_data, void *item1, void *item2)
{
    ipmi_mc_oem_removed_cb handler = item1;
    ipmi_mc_t              *mc = cb_data;

    ipmi_mc_remove_oem_removed_handler(mc, handler, item2);
    handler(mc->domain, mc, item2);
    return LOCKED_LIST_ITER_CONTINUE;
}

static int
check_mc_destroy(ipmi_mc_t *mc)
{
    ipmi_domain_t *domain = mc->domain;

    if (!mc->active
	&& (ipmi_controls_get_count(mc->controls) == 0)
	&& (ipmi_sensors_get_count(mc->sensors) == 0)
	&& (mc->usercount == 0))
    {
	/* There are no sensors associated with this MC, so it's safe
           to delete it.  If there are sensors that still reference
           this MC (such as from another MC's SDR repository, or the
           main SDR repository) we have to leave it inactive but not
           delete it.  The active handlers come from MCDLR and FRUDLR
           SDRs that monitor the MC. */
	_ipmi_remove_mc_from_domain(domain, mc);

	if (mc->conup_info)
	    ipmi_mem_free(mc->conup_info);
	if (mc->removed_handlers)
	    locked_list_destroy(mc->removed_handlers);
    	if (mc->active_handlers)
	    locked_list_destroy(mc->active_handlers);
	if (mc->sensors)
	    ipmi_sensors_destroy(mc->sensors);
	if (mc->controls)
	    ipmi_controls_destroy(mc->controls);
	if (mc->sdrs)
	    ipmi_sdr_info_destroy(mc->sdrs, NULL, NULL);
	if (mc->sel)
	    ipmi_sel_destroy(mc->sel, NULL, NULL);
	if (mc->lock)
	    ipmi_destroy_lock(mc->lock);

	ipmi_mem_free(mc);
	return 1;
    }
    return 0;
}

void
_ipmi_cleanup_mc(ipmi_mc_t *mc)
{
    mc->cleanup = 1;
}

static void
cleanup_mc(ipmi_mc_t *mc)
{
    int           i;
    int           rv;
    ipmi_domain_t *domain = mc->domain;
    os_handler_t  *os_hnd = ipmi_domain_get_os_hnd(domain);

    /* Call the OEM handlers for removal, if it has been registered. */
    locked_list_iterate(mc->removed_handlers, call_removed_handler, mc);
    
    /* First the device SDR sensors, since they can be there for any
       MC. */
    if (mc->sensors_in_my_sdr) {
	for (i=0; i<mc->sensors_in_my_sdr_count; i++) {
	    ipmi_sensor_t *sensor;
	    _ipmi_domain_entity_lock(domain);
	    sensor = mc->sensors_in_my_sdr[i];
	    if (sensor) {
		ipmi_entity_t *entity = ipmi_sensor_get_entity(sensor);
		_ipmi_entity_get(entity);
		_ipmi_sensor_get(sensor);
		_ipmi_domain_entity_unlock(domain);
		ipmi_sensor_destroy(mc->sensors_in_my_sdr[i]);
		_ipmi_sensor_put(sensor);
		_ipmi_entity_put(entity);
	    } else {
		_ipmi_domain_entity_unlock(domain);
	    }
	}
	ipmi_mem_free(mc->sensors_in_my_sdr);
	mc->sensors_in_my_sdr = NULL;
    }

    if (mc->entities_in_my_sdr) {
	ipmi_sdr_entity_destroy(mc->entities_in_my_sdr);
	mc->entities_in_my_sdr = NULL;
    }

    /* Make sure the timer stops. */
    if (mc->sel_timer_info) {
	mc->sel_timer_info->cancelled = 1;
	rv = os_hnd->stop_timer(os_hnd, mc->sel_timer);
	if (!rv) {
	    /* If we can stop the timer, free it and it's info.
	       If we can't stop the timer, that means that the
	       code is currently in the timer handler, so we let
	       the "cancelled" value do this for us. */
	    os_hnd->free_timer(os_hnd, mc->sel_timer);
	    mc->sel_timer = NULL;
	    ipmi_mem_free(mc->sel_timer_info);
	}
	mc->sel_timer_info = NULL;
    }

    if (mc->conup_info) {
	ipmi_domain_remove_connect_change_handler(domain,
						  con_up_handler,
						  mc->conup_info);
    }

    mc->cleanup = 0;

    _ipmi_mc_set_active(mc, 0);
}

/***********************************************************************
 *
 * Reset routines for MCs.
 *
 **********************************************************************/

typedef struct mc_reset_info_s
{
    ipmi_mc_done_cb done;
    void            *cb_data;
} mc_reset_info_t;

static void
mc_reset_done(ipmi_mc_t  *mc,
	      ipmi_msg_t *rsp,
	      void       *rsp_data)
{
    int             err = 0;
    mc_reset_info_t *info = rsp_data;

    if (rsp->data[0] != 0)
	err = IPMI_IPMI_ERR_VAL(rsp->data[0]);

    if (info->done)
	info->done(mc, err, info->cb_data);

    ipmi_mem_free(info);
}

int
ipmi_mc_reset(ipmi_mc_t       *mc,
	      int             reset_type,
	      ipmi_mc_done_cb done,
	      void            *cb_data)
{
    int             rv;
    ipmi_msg_t      msg;
    mc_reset_info_t *info;

    CHECK_MC_LOCK(mc);

    if (reset_type == IPMI_MC_RESET_COLD)
	msg.cmd = IPMI_COLD_RESET_CMD;
    else if (reset_type == IPMI_MC_RESET_WARM)
	msg.cmd = IPMI_WARM_RESET_CMD;
    else
	return EINVAL;

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;
    info->done = done;
    info->cb_data = cb_data;

    msg.netfn = IPMI_APP_NETFN;
    msg.data = NULL;
    msg.data_len = 0;
    rv = ipmi_mc_send_command(mc, 0, &msg, mc_reset_done, info);
    if (rv)
	ipmi_mem_free(info);

    return rv;
}

/***********************************************************************
 *
 * Event handling.
 *
 **********************************************************************/

/* Got a new event in the system event log that we didn't have before. */
static void
mc_sel_new_event_handler(ipmi_sel_info_t *sel,
			 ipmi_mc_t       *mc,
			 ipmi_event_t    *event,
			 void            *cb_data)
{
    _ipmi_domain_system_event_handler(cb_data, mc, event);
}

int
_ipmi_mc_check_oem_event_handler(ipmi_mc_t *mc, ipmi_event_t *event)
{
    if (mc->oem_event_handler)
	return (mc->oem_event_handler(mc, event, mc->oem_event_cb_data));
    else
	return 0;
}

int
_ipmi_mc_check_sel_oem_event_handler(ipmi_mc_t *mc, ipmi_event_t *event)
{
    if (mc->sel_oem_event_handler)
	return (mc->sel_oem_event_handler(mc, event,
					  mc->sel_oem_event_cb_data));
    else
	return 0;
}


/***********************************************************************
 *
 * SEL handling.
 *
 **********************************************************************/

int
_ipmi_mc_sel_event_add(ipmi_mc_t *mc, ipmi_event_t *event)
{
    return ipmi_sel_event_add(mc->sel, event);
}

ipmi_time_t
ipmi_mc_get_startup_SEL_time(ipmi_mc_t *mc)
{
    return mc->startup_SEL_time;
}

void
ipmi_mc_set_del_event_handler(ipmi_mc_t            *mc,
			      ipmi_mc_del_event_cb handler)
{
    mc->sel_del_event_handler = handler;
}

void
ipmi_mc_set_add_event_handler(ipmi_mc_t            *mc,
			      ipmi_mc_add_event_cb handler)
{
    mc->sel_add_event_handler = handler;
}

void
ipmi_mc_set_sel_rescan_time(ipmi_mc_t *mc, unsigned int seconds)
{
    unsigned int old_time;
    CHECK_MC_LOCK(mc);

    if (mc->sel_scan_interval == seconds)
	return;

    old_time = mc->sel_scan_interval;

    mc->sel_scan_interval = seconds;
    if (old_time == 0) {
	/* The old time was zero, so we must restart the timer. */
	sels_fetched_start_timer(mc->sel, 0, 0, 0, mc->sel_timer_info);
    }
}

unsigned int
ipmi_mc_get_sel_rescan_time(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);

    return mc->sel_scan_interval;
}

typedef struct sel_op_done_info_s
{
    ipmi_mc_t  *mc;
    ipmi_mc_cb done;
    void       *cb_data;
} sel_op_done_info_t;

static void
sel_op_done(ipmi_sel_info_t *sel,
	    void            *cb_data,
	    int             err)
{
    sel_op_done_info_t *info = cb_data;

    /* No need to refcount, the domain/mc should already be locked. */
    if (info->done)
        info->done(info->mc, err, info->cb_data);
    ipmi_mem_free(info);
}

int
ipmi_mc_del_event(ipmi_mc_t                 *mc,
		  ipmi_event_t              *event, 
		  ipmi_mc_del_event_done_cb handler,
		  void                      *cb_data)
{
    sel_op_done_info_t *sel_info;
    int                rv;

    if (!mc->devid.SEL_device_support)
	return EINVAL;

    /* If we have an OEM handler, call it instead. */
    if (mc->sel_del_event_handler) {
	rv = mc->sel_del_event_handler(mc, event, handler, cb_data);
	return rv;
    }

    sel_info = ipmi_mem_alloc(sizeof(*sel_info));
    if (!sel_info)
	return ENOMEM;

    sel_info->mc = mc;
    sel_info->done = handler;
    sel_info->cb_data = cb_data;

    rv = ipmi_sel_del_event(mc->sel, event, sel_op_done, sel_info);
    if (rv)
	ipmi_mem_free(sel_info);

    return rv;
}

typedef struct sel_add_op_done_info_s
{
    ipmi_mc_t                 *mc;
    ipmi_mc_add_event_done_cb done;
    void                      *cb_data;
} sel_add_op_done_info_t;

static void sel_add_op_done(ipmi_sel_info_t *sel,
			    void            *cb_data,
			    int             err,
			    unsigned int    record_id)
{
    sel_add_op_done_info_t *info = cb_data;

    /* No need to lock, the domain/mc should already be locked. */
    if (info->done)
        info->done(info->mc, record_id, err, info->cb_data);
    ipmi_mem_free(info);
}

int
ipmi_mc_add_event_to_sel(ipmi_mc_t                 *mc,
			 ipmi_event_t              *event,
			 ipmi_mc_add_event_done_cb handler,
			 void                      *cb_data)
{
    sel_add_op_done_info_t *sel_info;
    int                    rv;

    if (!mc->devid.SEL_device_support)
	return EINVAL;

    /* If we have an OEM handler, call it instead. */
    if (mc->sel_add_event_handler) {
	rv = mc->sel_add_event_handler(mc, event, handler, cb_data);
	return rv;
    }

    sel_info = ipmi_mem_alloc(sizeof(*sel_info));
    if (!sel_info)
	return ENOMEM;

    sel_info->mc = mc;
    sel_info->done = handler;
    sel_info->cb_data = cb_data;

    rv = ipmi_sel_add_event_to_sel(mc->sel, event, sel_add_op_done, sel_info);
    if (rv)
	ipmi_mem_free(sel_info);

    return rv;
}

ipmi_event_t *
ipmi_mc_next_event(ipmi_mc_t *mc, ipmi_event_t *event)
{
    return ipmi_sel_get_next_event(mc->sel, event);
}

ipmi_event_t *
ipmi_mc_prev_event(ipmi_mc_t *mc, ipmi_event_t *event)
{
    return ipmi_sel_get_prev_event(mc->sel, event);
}

ipmi_event_t *
ipmi_mc_last_event(ipmi_mc_t *mc)
{
    return ipmi_sel_get_last_event(mc->sel);
}

ipmi_event_t *
ipmi_mc_first_event(ipmi_mc_t *mc)
{
    return ipmi_sel_get_first_event(mc->sel);
}

ipmi_event_t *
ipmi_mc_event_by_recid(ipmi_mc_t    *mc,
                       unsigned int record_id)
{
    return ipmi_sel_get_event_by_recid(mc->sel, record_id);
}

int
ipmi_mc_sel_count(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_get_sel_count(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_entries_used(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_get_sel_entries_used(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_major_version(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_major_version(mc->sel, &val);
    return val;
}

int 
ipmi_mc_sel_get_minor_version(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_minor_version(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_num_entries(ipmi_mc_t *mc)
{
    unsigned int val = 0;
    
    ipmi_sel_get_num_entries(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_free_bytes(ipmi_mc_t *mc)
{
    unsigned int val = 0;
    
    ipmi_sel_get_free_bytes(mc->sel, &val);
    return val;
}

int 
ipmi_mc_sel_get_overflow(ipmi_mc_t *mc)
{
    unsigned int val = 0;
    
    ipmi_sel_get_overflow(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_supports_delete_sel(ipmi_mc_t *mc)
{
    unsigned int val = 0;
    
    ipmi_sel_get_supports_delete_sel(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_supports_partial_add_sel(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_supports_partial_add_sel(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_supports_reserve_sel(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_supports_reserve_sel(mc->sel, &val);
    return val;
}

int 
ipmi_mc_sel_get_supports_get_sel_allocation(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_supports_get_sel_allocation(mc->sel, &val);
    return val;
}

int
ipmi_mc_sel_get_last_addition_timestamp(ipmi_mc_t *mc)
{
    unsigned int val = 0;

    ipmi_sel_get_last_addition_timestamp(mc->sel, &val);
    return val;
}

int
ipmi_mc_set_oem_event_handler(ipmi_mc_t                 *mc,
			      ipmi_oem_event_handler_cb handler,
			      void                      *cb_data)
{
    mc->oem_event_handler = handler;
    mc->oem_event_cb_data = cb_data;
    return 0;
}

int
ipmi_mc_set_sel_oem_event_handler(ipmi_mc_t                 *mc,
				  ipmi_oem_event_handler_cb handler,
				  void                      *cb_data)
{
    mc->sel_oem_event_handler = handler;
    mc->sel_oem_event_cb_data = cb_data;
    return 0;
}

static void mc_reread_sel_timeout(void *cb_data, os_hnd_timer_id_t *id);

static void
sels_fetched_start_timer(ipmi_sel_info_t *sel,
			 int             err,
			 int             changed,
			 unsigned int    count,
			 void            *cb_data)
{
    mc_reread_sel_t *info = cb_data;
    ipmi_mc_t       *mc = info->mc;
    os_handler_t    *os_hnd;
    struct timeval  timeout;

    if (info->cancelled) {
	ipmi_mem_free(info);
	return;
    }

    /* After the first SEL fetch, disable looking at the timestamp, in
       case someone messes with the SEL time. */
    mc->startup_SEL_time = 0;

    if (info->handler) {
	ipmi_sels_fetched_t handler;
	void                *mcb_data;

	handler = info->handler;
	mcb_data = info->cb_data;
	info->handler = NULL;
	handler(sel, err, changed, count, mcb_data);
    }

    if (mc->sel_scan_interval != 0) {
	os_hnd = mc_get_os_hnd(mc);
	timeout.tv_sec = mc->sel_scan_interval;
	timeout.tv_usec = 0;
	os_hnd->start_timer(os_hnd,
			    mc->sel_timer,
			    &timeout,
			    mc_reread_sel_timeout,
			    info);
    }
}

static void
mc_reread_sel_timeout(void *cb_data, os_hnd_timer_id_t *id)
{
    mc_reread_sel_t *info = cb_data;
    ipmi_mc_t       *mc = info->mc;
    int             rv = EINVAL;

    if (info->cancelled) {
	ipmi_mem_free(info);
	return;
    }

    if (_ipmi_domain_get(info->domain))
	return;

    /* Only fetch the SEL if we know the connection is up. */
    if (ipmi_domain_con_up(mc->domain))
	rv = ipmi_sel_get(mc->sel, sels_fetched_start_timer, info);

    /* If we couldn't run the SEL get, then restart the timer now. */
    if (rv)
	sels_fetched_start_timer(mc->sel, 0, 0, 0, info);

    _ipmi_domain_put(info->domain);
}

typedef struct sel_reread_s
{
    ipmi_mc_done_cb handler;
    void            *cb_data;
    ipmi_mcid_t     mcid;
    int             err;
} sel_reread_t;

static void
mc_reread_sel_cb(ipmi_mc_t *mc, void *cb_data)
{
    sel_reread_t *info = cb_data;

    info->handler(mc, info->err, info->cb_data);
}

static void
reread_sel_done(ipmi_sel_info_t *sel,
		int             err,
		int             changed,
		unsigned int    count,
		void            *cb_data)
{
    sel_reread_t *info = cb_data;
    int          rv;

    if (info->handler) {
	if (!sel) {
	    info->handler(NULL, ECANCELED, info->cb_data);
	    goto out;
	}

	rv = ipmi_mc_pointer_cb(info->mcid, mc_reread_sel_cb, info);
	if (rv) {
	    info->handler(NULL, ECANCELED, info->cb_data);
	    goto out;
	}
    }
 out:
    ipmi_mem_free(info);
}

static int start_sel_ops(ipmi_mc_t           *mc,
			 int                 fail_if_down,
			 ipmi_sels_fetched_t handler,
			 void                *cb_data);

int
ipmi_mc_reread_sel(ipmi_mc_t       *mc,
		   ipmi_mc_done_cb handler,
		   void            *cb_data)
{
    sel_reread_t        *info = NULL;
    ipmi_sels_fetched_t cb = NULL;
    int                 rv;

    if (handler) {
	info = ipmi_mem_alloc(sizeof(*info));
	if (!info)
	    return ENOMEM;

	info->handler = handler;
	info->cb_data = cb_data;
	info->mcid = ipmi_mc_convert_to_id(mc);
	info->err = 0;
	cb = reread_sel_done;
    }

    ipmi_lock(mc->lock);
    if (mc->sel_timer_info) {
	/* SEL is already set up, just do a request. */
	rv = ipmi_sel_get(mc->sel, cb, info);
    } else {
	/* SEL is not set up, start it. */
	rv = start_sel_ops(mc, 1, cb, info);
    }
    ipmi_unlock(mc->lock);

    if (rv && info)
	ipmi_mem_free(info);

    return rv;
}

typedef struct sel_get_time_s
{
    sel_get_time_cb handler;
    void            *cb_data;
    char            name[IPMI_MC_NAME_LEN];
} sel_get_time_t;

static void
get_sel_time(ipmi_mc_t  *mc,
	     ipmi_msg_t *rsp,
	     void       *rsp_data)
{
    sel_get_time_t *info = rsp_data;

    if (!mc) {
	/* The MC went away, deliver an error. */
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(get_sel_time): "
		 "MC went away during SEL time fetch.",
		 info->name);
	if (info->handler)
	    info->handler(mc, ENXIO, 0, info->cb_data);
	goto out;
    }

    if (rsp->data[0] != 0) {
	/* Error setting the event receiver, report it. */
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(get_sel_time): "
		 "Could not get SEL time for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
	if (info->handler)
	    info->handler(mc, IPMI_IPMI_ERR_VAL(rsp->data[0]), 0,
			  info->cb_data);
	goto out;
    }

    if (rsp->data_len < 5) {
	/* Not enough data? */
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(get_sel_time): "
		 "Get SEL time response too short for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
	if (info->handler)
	    info->handler(mc, EINVAL, 0, info->cb_data);
	goto out;
    }

    if (info->handler)
	info->handler(mc, 0, ipmi_get_uint32(rsp->data+1), info->cb_data);

 out:
    ipmi_mem_free(info);
}

int
ipmi_mc_get_current_sel_time(ipmi_mc_t       *mc,
			     sel_get_time_cb handler,
			     void            *cb_data)
{
    ipmi_msg_t     msg;
    sel_get_time_t *info;
    int            rv;

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;

    info->handler = handler;
    info->cb_data = cb_data;
    strncpy(info->name, mc->name, sizeof(info->name));

    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_GET_SEL_TIME_CMD;
    msg.data = NULL;
    msg.data_len = 0;
    rv = ipmi_mc_send_command(mc, 0, &msg, get_sel_time, info);
    if (rv)
	ipmi_mem_free(info);
    return rv;
}

typedef struct set_sel_time_s
{
    ipmi_mc_done_cb handler;
    void            *cb_data;
    char            name[IPMI_MC_NAME_LEN];
} set_sel_time_t;

static void
set_sel_time(ipmi_mc_t  *mc,
	     ipmi_msg_t *rsp,
	     void       *rsp_data)
{
    set_sel_time_t *info = rsp_data;

    if (!mc) {
	/* The MC went away, deliver an error. */
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(set_sel_time): "
		 "MC went away during SEL time fetch.",
		 info->name);
	if (info->handler)
	    info->handler(mc, ENXIO, info->cb_data);
	goto out;
    }

    if (rsp->data[0] != 0) {
	/* Error setting the event receiver, report it. */
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(set_sel_time): "
		 "Could not get SEL time for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
	if (info->handler)
	    info->handler(mc, IPMI_IPMI_ERR_VAL(rsp->data[0]), info->cb_data);
	goto out;
    }

    if (info->handler)
	info->handler(mc, 0, info->cb_data);

 out:
    ipmi_mem_free(info);
}

int
ipmi_mc_set_current_sel_time(ipmi_mc_t             *mc,
			     const struct timeval  *time,
			     ipmi_mc_done_cb       handler,
			     void                  *cb_data)
{
    ipmi_msg_t     msg;
    int            rv;
    unsigned char  data[4];
    set_sel_time_t *info;


    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;

    info->handler = handler;
    info->cb_data = cb_data;
    strncpy(info->name, mc->name, sizeof(info->name));

    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_SET_SEL_TIME_CMD;
    msg.data = data;
    msg.data_len = 4;
    ipmi_set_uint32(data, time->tv_sec);
    mc->startup_SEL_time = ipmi_timeval_to_time(*time);
    rv = ipmi_mc_send_command(mc, 0, &msg, set_sel_time, info);
    if (rv)
	ipmi_mem_free(info);
    return rv;
}


/***********************************************************************
 *
 * Handling startup of a new MC
 *
 **********************************************************************/

typedef struct set_event_rcvr_info_s
{
    ipmi_mc_done_cb done;
    void            *cb_data;
} set_event_rcvr_info_t;

static void
set_event_rcvr_done(ipmi_mc_t  *mc,
		    ipmi_msg_t *rsp,
		    void       *rsp_data)
{
    ipmi_mc_done_cb done = NULL;
    void            *cb_data = NULL;
    int             rv = 0;

    if (rsp_data) {
	set_event_rcvr_info_t *info = rsp_data;
	done = info->done;
	cb_data = info->cb_data;
	ipmi_mem_free(info);
    }

    if (!mc) {
	rv = ECANCELED;
	goto out; /* The MC went away, no big deal. */
    }

    if (rsp->data[0] != 0) {
	/* Error setting the event receiver, report it. */
	ipmi_log(IPMI_LOG_WARNING,
		 "%smc.c(set_event_rcvr_done): "
		 "Could not set event receiver for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
	rv = IPMI_IPMI_ERR_VAL(rsp->data[0]);
    }

 out:
    if (done)
	done(mc, rv, cb_data);
}

static int
send_set_event_rcvr(ipmi_mc_t       *mc,
		    unsigned int    addr,
		    ipmi_mc_done_cb done,
		    void            *cb_data)
{
    ipmi_msg_t            msg;
    unsigned char         data[2];
    set_event_rcvr_info_t *info = NULL;

    if (done) {
	info = ipmi_mem_alloc(sizeof(*info));
	if (!info)
	    return ENOMEM;
	info->done = done;
	info->cb_data = cb_data;
    }
    
    msg.netfn = IPMI_SENSOR_EVENT_NETFN;
    msg.cmd = IPMI_SET_EVENT_RECEIVER_CMD;
    msg.data = data;
    msg.data_len = 2;
    data[0] = addr;
    data[1] = 0; /* LUN is 0 per the spec (section 7.2 of 1.5 spec). */
    return ipmi_mc_send_command(mc, 0, &msg, set_event_rcvr_done, info);
    /* No care about return values, if this fails it will be done
       again later. */
}

static void
get_event_rcvr_done(ipmi_mc_t  *mc,
		    ipmi_msg_t *rsp,
		    void       *rsp_data)
{
    if (!mc)
	return; /* The MC went away, no big deal. */

    if (rsp->data[0] != 0) {
	/* Error getting the event receiver, report it. */
	ipmi_log(IPMI_LOG_WARNING,
		 "%smc.c(get_event_rcvr_done): "
		 "Could not get event receiver for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
    } else if (rsp->data_len < 2) {
	ipmi_log(IPMI_LOG_WARNING,
		 "%smc.c(get_event_rcvr_done): "
		 "Get event receiver length invalid for MC at 0x%x",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
    } else if ((rsp->data[1] == 0) && (!mc->events_enabled))  {
	/* Nothing to do, our event receiver is disabled. */
    } else {
	ipmi_domain_t    *domain = ipmi_mc_get_domain(mc);
	ipmi_mc_t        *destmc;
	ipmi_ipmb_addr_t ipmb;

	ipmb.addr_type = IPMI_IPMB_ADDR_TYPE;
	ipmb.channel = ipmi_mc_get_channel(mc);
	ipmb.slave_addr = rsp->data[1];
	ipmb.lun = 0;

	if (mc->events_enabled) {
	    destmc = _ipmi_find_mc_by_addr(domain, (ipmi_addr_t *) &ipmb,
					   sizeof(ipmb));
	    if (!destmc || !ipmi_mc_ipmb_event_receiver_support(destmc)) {
		/* The current event receiver doesn't exist or cannot
		   receive events, change it. */
		unsigned int event_rcvr = ipmi_domain_get_event_rcvr(mc->domain);
		if (event_rcvr)
		    send_set_event_rcvr(mc, event_rcvr, NULL, NULL);
	    }
	    if (destmc)
		_ipmi_mc_put(destmc);
	} else {
	    send_set_event_rcvr(mc, 0, NULL, NULL);
	}
    }
}

static void
send_get_event_rcvr(ipmi_mc_t *mc)
{
    ipmi_msg_t    msg;
    
    msg.netfn = IPMI_SENSOR_EVENT_NETFN;
    msg.cmd = IPMI_GET_EVENT_RECEIVER_CMD;
    msg.data = NULL;
    msg.data_len = 0;
    ipmi_mc_send_command(mc, 0, &msg, get_event_rcvr_done, NULL);
    /* No care about return values, if this fails it will be done
       again later. */
}

void
_ipmi_mc_check_event_rcvr(ipmi_mc_t *mc)
{
    if (mc && mc->devid.IPMB_event_generator_support) {
	/* We have an MC that is live (or still live) and generates
	   events, make sure the event receiver is set properly. */
	unsigned int event_rcvr = ipmi_domain_get_event_rcvr(mc->domain);

	/* Don't bother if we have no possible event receivers.*/
	if (event_rcvr) {
	    send_get_event_rcvr(mc);
	}
    }
}

static void
startup_set_sel_time(ipmi_mc_t  *mc,
		     ipmi_msg_t *rsp,
		     void       *rsp_data)
{
    mc_name_info_t *info = rsp_data;

    if (!mc) {
	ipmi_log(IPMI_LOG_WARNING,
		 "%smc.c(startup_set_sel_time): "
		 "MC went away during SEL time set", info->name);
	goto out;
    }

    if (mc->sels_first_read_handler) {
	mc->sels_first_read_handler(mc, mc->sels_first_read_cb_data);
	mc->sels_first_read_handler = NULL;
    }

    if (rsp->data[0] != 0) {
	ipmi_log(IPMI_LOG_WARNING,
		 "%smc.c(startup_set_sel_time): "
		 "Unable to set the SEL time due to error: %x",
		 mc->name, rsp->data[0]);
	mc->startup_SEL_time = 0;
    }

    ipmi_sel_get(mc->sel, sels_fetched_start_timer, mc->sel_timer_info);

 out:
    ipmi_mem_free(info);
}

static void
first_sel_op(ipmi_mc_t *mc)
{
    ipmi_msg_t     msg;
    int            rv;
    unsigned char  data[4];
    struct timeval now;
    mc_name_info_t *info;

    /* Set the current system event log time.  We do this here so
       we can be sure that the entities are all there before
       reporting events. */
    msg.netfn = IPMI_STORAGE_NETFN;
    msg.cmd = IPMI_SET_SEL_TIME_CMD;
    msg.data = data;
    msg.data_len = 4;
    gettimeofday(&now, NULL);
    ipmi_set_uint32(data, now.tv_sec);
    mc->startup_SEL_time = ipmi_seconds_to_time(now.tv_sec);
    info = ipmi_mem_alloc(sizeof(*info));
    if (!info) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(first_sel_op): "
		 "Unable to start SEL time, out of memory",
		 mc->name);
	goto cont_op;
    }
    rv = ipmi_mc_send_command(mc, 0, &msg, startup_set_sel_time, info);
    if (rv) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(first_sel_op): "
		 "Unable to start SEL time set due to error: %x",
		 mc->name, rv);
	ipmi_mem_free(info);
	goto cont_op;
    }
    return;

 cont_op:
    mc->startup_SEL_time = 0;
    rv = ipmi_sel_get(mc->sel, sels_fetched_start_timer,
		      mc->sel_timer_info);
    if (rv) {
	sels_fetched_start_timer(mc->sel, 0, 0, 0, mc->sel_timer_info);
	if (mc->sels_first_read_handler) {
	    mc->sels_first_read_handler(mc, mc->sels_first_read_cb_data);
	    mc->sels_first_read_handler = NULL;
	}
    }
}

static void
con_up_mc(ipmi_mc_t *mc, void *cb_data)
{
    first_sel_op(mc);
}

static void
con_up_handler(ipmi_domain_t *domain,
	       int           err,
	       unsigned int  conn_num,
	       unsigned int  port_num,
	       int           still_connected,
	       void          *cb_data)
{
    domain_up_info_t *info = cb_data;

    if (!still_connected)
	return;

    ipmi_domain_remove_connect_change_handler(domain,
					      con_up_handler,
					      info);
    ipmi_mc_pointer_cb(info->mcid, con_up_mc, info);
}

static int
start_sel_ops(ipmi_mc_t           *mc,
	      int                 fail_if_down,
	      ipmi_sels_fetched_t handler,
	      void                *cb_data)
{
    ipmi_domain_t   *domain = ipmi_mc_get_domain(mc);
    mc_reread_sel_t *info;
    int             rv;
    os_handler_t    *os_hnd = mc_get_os_hnd(mc);

    /* Allocate the system event log fetch timer. */
    info = ipmi_mem_alloc(sizeof(*info));
    if (!info) {
	ipmi_log(IPMI_LOG_SEVERE,
		 "%smc.c(sensors_reread): "
		 "Unable to allocate info for system event log timer."
		 " System event log will not be queried",
		 mc->name);
	rv = ENOMEM;
	goto sel_failure;
    }
    info->mc = mc;
    info->domain = mc->domain;
    info->cancelled = 0;
    info->handler = handler;
    info->cb_data = cb_data;
    rv = os_hnd->alloc_timer(os_hnd, &(mc->sel_timer));
    if (rv) {
	ipmi_mem_free(info);
	ipmi_log(IPMI_LOG_SEVERE,
		 "%smc.c(sensors_reread): "
		 "Unable to allocate the system event log timer."
		 " System event log will not be queried",
		 mc->name);
	goto sel_failure;
    } else {
	mc->sel_timer_info = info;
    }

    if (ipmi_domain_con_up(domain)) {
	/* The domain is already up, just start the process. */
	first_sel_op(mc);
    } else if (fail_if_down) {
	rv = EAGAIN;
	ipmi_mem_free(info);
	mc->sel_timer_info = NULL;
	goto sel_failure;
    } else {
	/* The domain is not up yet, wait for it to come up then start
           the process. */
	mc->conup_info->mcid = ipmi_mc_convert_to_id(mc);
	rv = ipmi_domain_add_connect_change_handler(domain, con_up_handler,
						    mc->conup_info);
	if (rv) {
	    ipmi_log(IPMI_LOG_SEVERE,
		     "%smc.c(start_sel_ops): "
		     "Unable to add a connection change handler for the"
		     " delayed SEL timer start, starting it now, but some"
		     " events may come in before the connection is up.",
		     mc->name);
	    first_sel_op(mc);
	}
    }
    return 0;

 sel_failure:
    /* SELs not started, just call the handler. */
    if (mc->sels_first_read_handler) {
	mc->sels_first_read_handler(mc, mc->sels_first_read_cb_data);
	mc->sels_first_read_handler = NULL;
    }
    return rv;
}

/* This is called after the first sensor scan for the MC, we start up
   timers and things like that here. */
static void
sensors_reread(ipmi_mc_t *mc, int err, void *cb_data)
{
    unsigned int event_rcvr = 0;

    if (!mc)
	return;

    /* See if any presence has changed with the new sensors. */ 
    ipmi_detect_domain_presence_changes(mc->domain, 0);

    /* We set the event receiver here, so that we know all the SDRs
       are installed.  That way any incoming events from the device
       will have the proper sensor set. */
    if (mc->devid.IPMB_event_generator_support
	&& ipmi_option_set_event_rcvr(mc->domain))
    {
	event_rcvr = ipmi_domain_get_event_rcvr(mc->domain);
    }

    if (event_rcvr)
	send_set_event_rcvr(mc, event_rcvr, NULL, NULL);

    if (mc->sdrs_first_read_handler) {
	mc->sdrs_first_read_handler(mc, mc->sdrs_first_read_cb_data);
	mc->sdrs_first_read_handler = NULL;
    }

    if (mc->devid.SEL_device_support && ipmi_option_SEL(mc->domain))
	/* If the MC supports an SEL, start scanning its SEL. */
	start_sel_ops(mc, 0, NULL, NULL);
}

int
_ipmi_mc_handle_new(ipmi_mc_t *mc)
{
    int rv = 0;

    /* Apply any pending updates now, so we can get things that the
       OEM handler installed. */
    if (mc->pending_devid_data) {
	mc->pending_devid_data = 0;
	mc->devid = mc->pending_devid;
    }

   _ipmi_mc_set_active(mc, 1);

    if (mc->devid.chassis_support && (ipmi_mc_get_address(mc) == 0x20)) {
        rv = _ipmi_chassis_create_controls(mc);
	if (rv)
	    return rv;
    }

    if (((mc->devid.provides_device_sdrs) || (mc->treat_main_as_device_sdrs))
	&& ipmi_option_SDRs(ipmi_mc_get_domain(mc)))
	rv = ipmi_mc_reread_sensors(mc, sensors_reread, NULL);
    else
	sensors_reread(mc, 0, NULL);

    return rv;
}

/***********************************************************************
 *
 * MC ID handling
 *
 **********************************************************************/

void
_ipmi_mc_use(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    mc->usercount++;
}

void
_ipmi_mc_release(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    mc->usercount--;
}

/* Must be holding the domain->mc_lock to call these. */
int
_ipmi_mc_get(ipmi_mc_t *mc)
{
    mc->usecount++;
    return 0;
}

void
_ipmi_mc_put(ipmi_mc_t *mc)
{
    _ipmi_domain_mc_lock(mc->domain);
    if (mc->usecount == 1) {
	/* Install any pending change from device id changes. */
	if (mc->pending_devid_data) {
	    mc->devid = mc->pending_devid;
	    mc->pending_devid_data = 0;
	    if (mc->pending_new_mc) {
		_ipmi_mc_handle_new(mc);
		mc->pending_new_mc = 0;
	    }
	}

	/* If we need to cleanup the MC, do so. */
	if (mc->cleanup)
	    cleanup_mc(mc);

	/* Do as many active transitions as necessary to bring the MC
	   to the proper state. */
	while (mc->active_transitions != 0) {
	    mc->active_transitions--;
	    mc->active = !mc->active;
	    _ipmi_domain_mc_unlock(mc->domain);
	    call_active_handlers(mc);
	    _ipmi_domain_mc_lock(mc->domain);

	    /* Something grabbed the MC while we were out, don't do
	       any more transitions. */
	    if (mc->usecount != 1)
		goto out;
	}

	/* Destroy the MC if it is ready.  Note that if this actually
	   does the destroy, it will release the MC lock and return
	   true, so we can just return. */
	if (check_mc_destroy(mc))
	    return;
    }
 out:
    mc->usecount--;
    _ipmi_domain_mc_unlock(mc->domain);
}

ipmi_mcid_t
ipmi_mc_convert_to_id(ipmi_mc_t *mc)
{
    ipmi_mcid_t val;

    CHECK_MC_LOCK(mc);

    val.domain_id = ipmi_domain_convert_to_id(mc->domain);
    val.mc_num = ipmi_mc_get_address(mc);
    val.channel = ipmi_mc_get_channel(mc);
    val.seq = mc->seq;
    return val;
}

typedef struct mc_ptr_info_s
{
    int            err;
    int            cmp_seq;
    ipmi_mcid_t    id;
    ipmi_mc_ptr_cb handler;
    void           *cb_data;
} mc_ptr_info_t;

static void
mc_ptr_cb(ipmi_domain_t *domain, void *cb_data)
{
    mc_ptr_info_t *info = cb_data;
    ipmi_addr_t   addr;
    unsigned int  addr_len;
    ipmi_mc_t     *mc;

    if (info->id.channel == IPMI_BMC_CHANNEL) {
	ipmi_system_interface_addr_t *si = (void *) &addr;

	si->addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	si->channel = info->id.mc_num;
	si->lun = 0;
	addr_len = sizeof(*si);
    } else {
	ipmi_ipmb_addr_t *ipmb = (void *) &addr;

	ipmb->addr_type = IPMI_IPMB_ADDR_TYPE;
	ipmb->channel = info->id.channel;
	ipmb->slave_addr = info->id.mc_num;
	ipmb->lun = 0;
	addr_len = sizeof(*ipmb);
    }

    mc = _ipmi_find_mc_by_addr(domain, &addr, addr_len);
    if (mc) {
	if (info->cmp_seq && (mc->seq != info->id.seq)) {
	    _ipmi_mc_put(mc);
	    return;
	}

	info->err = 0;
	info->handler(mc, info->cb_data);
	_ipmi_mc_put(mc);
    }
}

int
ipmi_mc_pointer_cb(ipmi_mcid_t id, ipmi_mc_ptr_cb handler, void *cb_data)
{
    int           rv;
    mc_ptr_info_t info;

    info.err = EINVAL;
    info.id = id;
    info.handler = handler;
    info.cb_data = cb_data;
    info.cmp_seq = 1;
    rv = ipmi_domain_pointer_cb(id.domain_id, mc_ptr_cb, &info);
    if (!rv)
	rv = info.err;
    return rv;
}

int
ipmi_mc_pointer_noseq_cb(ipmi_mcid_t    id,
			 ipmi_mc_ptr_cb handler,
			 void           *cb_data)
{
    int           rv;
    mc_ptr_info_t info;

    info.err = EINVAL;
    info.id = id;
    info.handler = handler;
    info.cb_data = cb_data;
    info.cmp_seq = 0;
    rv = ipmi_domain_pointer_cb(id.domain_id, mc_ptr_cb, &info);
    if (!rv)
	rv = info.err;
    return rv;
}

int
ipmi_cmp_mc_id_noseq(ipmi_mcid_t id1, ipmi_mcid_t id2)
{
    int d;

    d = ipmi_cmp_domain_id(id1.domain_id, id2.domain_id);
    if (d)
	return d;

    if (id1.mc_num > id2.mc_num)
	return 1;
    if (id1.mc_num < id2.mc_num)
	return -1;
    if (id1.channel > id2.channel)
	return 1;
    if (id1.channel < id2.channel)
	return -1;
    return 0;
}

int
ipmi_cmp_mc_id(ipmi_mcid_t id1, ipmi_mcid_t id2)
{
    int d;

    d = ipmi_cmp_mc_id_noseq(id1, id2);
    if (d)
	return d;

    if (id1.seq > id2.seq)
	return 1;
    if (id1.seq < id2.seq)
	return -1;
    return 0;
}

void
ipmi_mc_id_set_invalid(ipmi_mcid_t *id)
{
    memset(id, 0, sizeof(*id));
}

int
ipmi_mc_id_is_invalid(ipmi_mcid_t *id)
{
    return (id->domain_id.domain == NULL);
}

/***********************************************************************
 *
 * Handle sending commands and getting responses.
 *
 **********************************************************************/

static int
addr_rsp_handler(ipmi_domain_t *domain, ipmi_msgi_t *rspi)
{
    ipmi_addr_t                *addr = &rspi->addr;
    unsigned int               addr_len = rspi->addr_len;
    ipmi_msg_t                 *msg = &rspi->msg;
    ipmi_mc_response_handler_t rsp_handler = rspi->data2;
    ipmi_mc_t                  *mc;

    if (rsp_handler) {
	if (domain)
	    mc = _ipmi_find_mc_by_addr(domain, addr, addr_len);
	else
	    mc = NULL;
	rsp_handler(mc, msg, rspi->data1);
	if (mc)
	    _ipmi_mc_put(mc);
    }
    return IPMI_MSG_ITEM_NOT_USED;
}

int
ipmi_mc_send_command(ipmi_mc_t                  *mc,
		     unsigned int               lun,
		     ipmi_msg_t                 *msg,
		     ipmi_mc_response_handler_t rsp_handler,
		     void                       *rsp_data)
{
    int           rv;
    ipmi_addr_t   addr = mc->addr;
    ipmi_domain_t *domain;

    CHECK_MC_LOCK(mc);

    rv = ipmi_addr_set_lun(&addr, lun);
    if (rv)
	return rv;

    domain = ipmi_mc_get_domain(mc);

    rv = ipmi_send_command_addr(domain,
				&addr, mc->addr_len,
				msg,
				addr_rsp_handler,
				rsp_data,
				rsp_handler);
    return rv;
}

/***********************************************************************
 *
 * Handle global OEM callbacks for new MCs.
 *
 **********************************************************************/

typedef struct oem_handlers_s {
    unsigned int                 manufacturer_id;
    unsigned int                 first_product_id;
    unsigned int                 last_product_id;
    ipmi_oem_mc_match_handler_cb handler;
    ipmi_oem_shutdown_handler_cb shutdown;
    void                         *cb_data;
} oem_handlers_t;

static locked_list_t *oem_handlers;

int
ipmi_register_oem_handler(unsigned int                 manufacturer_id,
			  unsigned int                 product_id,
			  ipmi_oem_mc_match_handler_cb handler,
			  ipmi_oem_shutdown_handler_cb shutdown,
			  void                         *cb_data)
{
    oem_handlers_t *new_item;
    int            rv;

    /* This might be called before initialization, so be 100% sure. */
    rv = _ipmi_mc_init();
    if (rv)
	return rv;

    new_item = ipmi_mem_alloc(sizeof(*new_item));
    if (!new_item)
	return ENOMEM;

    new_item->manufacturer_id = manufacturer_id;
    new_item->first_product_id = product_id;
    new_item->last_product_id = product_id;
    new_item->handler = handler;
    new_item->shutdown = shutdown;
    new_item->cb_data = cb_data;

    if (! locked_list_add(oem_handlers, new_item, NULL)) {
	ipmi_mem_free(new_item);
	return ENOMEM;
    }

    return 0;
}

int
ipmi_register_oem_handler_range(unsigned int                 manufacturer_id,
				unsigned int                 first_product_id,
				unsigned int                 last_product_id,
				ipmi_oem_mc_match_handler_cb handler,
				ipmi_oem_shutdown_handler_cb shutdown,
				void                         *cb_data)
{
    oem_handlers_t *new_item;
    int            rv;

    /* This might be called before initialization, so be 100% sure. */
    rv = _ipmi_mc_init();
    if (rv)
	return rv;

    new_item = ipmi_mem_alloc(sizeof(*new_item));
    if (!new_item)
	return ENOMEM;

    new_item->manufacturer_id = manufacturer_id;
    new_item->first_product_id = first_product_id;
    new_item->last_product_id = last_product_id;
    new_item->handler = handler;
    new_item->shutdown = shutdown;
    new_item->cb_data = cb_data;

    if (! locked_list_add(oem_handlers, new_item, NULL)) {
	ipmi_mem_free(new_item);
	return ENOMEM;
    }

    return 0;
}

typedef struct handler_cmp_s
{
    int          rv;
    unsigned int manufacturer_id;
    unsigned int first_product_id;
    unsigned int last_product_id;
    ipmi_mc_t    *mc;
} handler_cmp_t;

static int
oem_handler_cmp_dereg(void *cb_data, void *item1, void *item2)
{
    oem_handlers_t *hndlr = item1;
    handler_cmp_t  *cmp = cb_data;

    if ((hndlr->manufacturer_id == cmp->manufacturer_id)
	&& (hndlr->first_product_id <= cmp->first_product_id)
	&& (hndlr->last_product_id >= cmp->last_product_id))
    {
	cmp->rv = 0;
	locked_list_remove(oem_handlers, item1, item2);
	ipmi_mem_free(hndlr);
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
ipmi_deregister_oem_handler(unsigned int manufacturer_id,
			    unsigned int product_id)
{
    handler_cmp_t  tmp;

    tmp.rv = ENOENT;
    tmp.manufacturer_id = manufacturer_id;
    tmp.first_product_id = product_id;
    tmp.last_product_id = product_id;
    locked_list_iterate(oem_handlers, oem_handler_cmp_dereg, &tmp);
    return tmp.rv;
}

int
ipmi_deregister_oem_handler_range(unsigned int manufacturer_id,
				  unsigned int first_product_id,
				  unsigned int last_product_id)
{
    handler_cmp_t  tmp;

    tmp.rv = ENOENT;
    tmp.manufacturer_id = manufacturer_id;
    tmp.first_product_id = first_product_id;
    tmp.last_product_id = last_product_id;
    locked_list_iterate(oem_handlers, oem_handler_cmp_dereg, &tmp);
    return tmp.rv;
}

static int
oem_handler_call(void *cb_data, void *item1, void *item2)
{
    oem_handlers_t *hndlr = item1;
    handler_cmp_t  *cmp = cb_data;

    if ((hndlr->manufacturer_id == cmp->manufacturer_id)
	&& (hndlr->first_product_id <= cmp->first_product_id)
	&& (hndlr->last_product_id >= cmp->last_product_id))
    {
	cmp->rv = hndlr->handler(cmp->mc, hndlr->cb_data);
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

static int
check_oem_handlers(ipmi_mc_t *mc)
{
    handler_cmp_t  tmp;

    tmp.rv = 0;
    tmp.manufacturer_id = mc->pending_devid.manufacturer_id;
    tmp.first_product_id = mc->pending_devid.product_id;
    tmp.last_product_id = mc->pending_devid.product_id;
    tmp.mc = mc;
    locked_list_iterate(oem_handlers, oem_handler_call, &tmp);
    return tmp.rv;
}


/***********************************************************************
 *
 * device SDR handling.
 *
 **********************************************************************/

typedef struct sdr_fetch_info_s
{
    ipmi_domain_t    *domain;
    ipmi_mcid_t      source_mc; /* This is used to scan the SDRs. */
    ipmi_mc_done_cb  done;
    void             *done_data;
    opq_t            *sensor_wait_q;
    int              err;
    int              changed;
    ipmi_sdr_info_t  *sdrs;
} sdr_fetch_info_t;

int
ipmi_mc_set_main_sdrs_as_device(ipmi_mc_t *mc)
{
    int             rv;
    ipmi_sdr_info_t *new_sdrs;

    rv = ipmi_sdr_info_alloc(ipmi_mc_get_domain(mc), mc, 0, 0, &new_sdrs);
    if (rv)
	return rv;

    mc->treat_main_as_device_sdrs = 1;
    if (mc->sdrs)
	ipmi_sdr_info_destroy(mc->sdrs, NULL, NULL);
    mc->sdrs = new_sdrs;

    /* Note that we don't reread the sensors, so this must be done
       before the sensor read operation. */
    return 0;
}

static void
sdr_reread_done(sdr_fetch_info_t *info, ipmi_mc_t *mc, int err, int mc_valid)
{
    if (info->done)
	info->done(mc, err, info->done_data);
    if (mc_valid)
	opq_op_done(info->sensor_wait_q);
    ipmi_mem_free(info);
}

static void
sdrs_fetched_mc_cb(ipmi_mc_t *mc, void *cb_data)
{
    sdr_fetch_info_t *info = (sdr_fetch_info_t *) cb_data;
    int              rv = 0;

    if (info->err) {
	sdr_reread_done(info, mc, info->err, 1);
	return;
    }

    if (mc->fixup_sdrs_handler)
	mc->fixup_sdrs_handler(mc, info->sdrs, mc->fixup_sdrs_cb_data);

    if (info->changed) {
	ipmi_entity_scan_sdrs(info->domain, mc,
			      ipmi_domain_get_entities(info->domain),
			      info->sdrs);
	rv = ipmi_sensor_handle_sdrs(info->domain, mc, info->sdrs);

	if (!rv)
	    ipmi_detect_domain_presence_changes(info->domain, 0);
    }

    sdr_reread_done(info, mc, rv, 1);
}

static void
sdrs_fetched(ipmi_sdr_info_t *sdrs,
	     int             err,
	     int             changed,
	     unsigned int    count,
	     void            *cb_data)
{
    sdr_fetch_info_t *info = (sdr_fetch_info_t *) cb_data;
    int              rv = 0;

    info->err = err;
    info->changed = changed;
    info->sdrs = sdrs;
    rv = ipmi_mc_pointer_cb(info->source_mc, sdrs_fetched_mc_cb, info);
    if (rv)
	sdr_reread_done(info, NULL, ECANCELED, 0);
}

static void
sensor_read_mc_cb(ipmi_mc_t *mc, void *cb_data)
{
    sdr_fetch_info_t *info = (sdr_fetch_info_t *) cb_data;
    int              rv;

    rv = ipmi_sdr_fetch(ipmi_mc_get_sdrs(mc), sdrs_fetched, info);
    if (rv == ENOSYS)
	/* ENOSYS means that the sensor population is not dyanmic. */
	sdr_reread_done(info, mc, 0, 1);
    else if (rv)
	sdr_reread_done(info, mc, rv, 1);
}

static void
sensor_read_handler(void *cb_data, int shutdown)
{
    sdr_fetch_info_t *info = (sdr_fetch_info_t *) cb_data;
    int              rv;

    if (shutdown) {
	sdr_reread_done(info, NULL, ECANCELED, 0);
	return;
    }

    rv = ipmi_mc_pointer_cb(info->source_mc, sensor_read_mc_cb, info);
    if (rv)
	sdr_reread_done(info, NULL, ECANCELED, 0);
}

int ipmi_mc_reread_sensors(ipmi_mc_t       *mc,
			   ipmi_mc_done_cb done,
			   void            *done_data)
{
    sdr_fetch_info_t   *info;
    int                rv = 0;
    ipmi_sensor_info_t *sensors;

    CHECK_MC_LOCK(mc);

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;

    sensors = _ipmi_mc_get_sensors(mc);

    info->source_mc = ipmi_mc_convert_to_id(mc);
    info->domain = ipmi_mc_get_domain(mc);
    info->done = done;
    info->done_data = done_data;
    info->sensor_wait_q = _ipmi_sensors_get_waitq(sensors);

    if (! opq_new_op(info->sensor_wait_q,
		     sensor_read_handler, info, 0))
	rv = ENOMEM;

    if (rv)
	ipmi_mem_free(info);

    return rv;
}

/***********************************************************************
 *
 * Checking for the validity and currentness of MC data.
 *
 **********************************************************************/

/* Check the MC, we reread the SDRs and check the event receiver. */
void
_ipmi_mc_check_mc(ipmi_mc_t *mc)
{
    if ((mc->devid.provides_device_sdrs) || (mc->treat_main_as_device_sdrs))
	ipmi_mc_reread_sensors(mc, NULL, NULL);
    _ipmi_mc_check_event_rcvr(mc);
}



/***********************************************************************
 *
 * Handle the boatloads of information from a get device id.
 *
 **********************************************************************/

int
_ipmi_mc_get_device_id_data_from_rsp(ipmi_mc_t *mc, ipmi_msg_t *rsp)
{
    unsigned char *rsp_data = rsp->data;
    int           rv = 0;

    if (rsp_data[0] != 0) {
	return IPMI_IPMI_ERR_VAL(rsp_data[0]);
    }

    if (rsp->data_len < 12) {
	if ((rsp->data[0] == 0) && (rsp->data_len >= 6)) {
	    int major_version = rsp->data[5] & 0xf;
	    int minor_version = (rsp->data[5] >> 4) & 0xf;

	    if (major_version < 1) {
		ipmi_log(IPMI_LOG_ERR_INFO,
			 "%smc.c(_ipmi_mc_get_device_id_data_from_rsp): "
			 "IPMI version of the MC at address 0x%2.2x is %d.%d,"
			 " which is older than OpenIPMI supports",
			 mc->name, ipmi_addr_get_slave_addr(&mc->addr),
			 major_version, minor_version);
		return EINVAL;
	    }
	}
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%smc.c(_ipmi_mc_get_device_id_data_from_rsp): "
		 "Invalid return from IPMI Get Device ID from address 0x%2.2x,"
		 " something is seriously wrong with the MC",
		 mc->name, ipmi_addr_get_slave_addr(&mc->addr));
	return EINVAL;
    }

    _ipmi_domain_mc_lock(mc->domain);

    /* Pend these to be installed when nobody is using them. */
    mc->pending_devid.device_id = rsp_data[1];
    mc->pending_devid.device_revision = rsp_data[2] & 0xf;
    mc->pending_devid.provides_device_sdrs = (rsp_data[2] & 0x80) == 0x80;
    mc->pending_devid.device_available = (rsp_data[3] & 0x80) == 0x80;
    mc->pending_devid.major_fw_revision = rsp_data[3] & 0x7f;
    mc->pending_devid.minor_fw_revision = rsp_data[4];
    mc->pending_devid.major_version = rsp_data[5] & 0xf;
    mc->pending_devid.minor_version = (rsp_data[5] >> 4) & 0xf;
    mc->pending_devid.chassis_support = (rsp_data[6] & 0x80) == 0x80;
    mc->pending_devid.bridge_support = (rsp_data[6] & 0x40) == 0x40;
    mc->pending_devid.IPMB_event_generator_support
	= (rsp_data[6] & 0x20) == 0x20;
    mc->pending_devid.IPMB_event_receiver_support
	= (rsp_data[6] & 0x10) == 0x10;
    mc->pending_devid.FRU_inventory_support = (rsp_data[6] & 0x08) == 0x08;
    mc->pending_devid.SEL_device_support = (rsp_data[6] & 0x04) == 0x04;
    mc->pending_devid.SDR_repository_support = (rsp_data[6] & 0x02) == 0x02;
    mc->pending_devid.sensor_device_support = (rsp_data[6] & 0x01) == 0x01;
    mc->pending_devid.manufacturer_id = (rsp_data[7]
				 | (rsp_data[8] << 8)
				 | (rsp_data[9] << 16));
    mc->pending_devid.product_id = rsp_data[10] | (rsp_data[11] << 8);

    if (rsp->data_len < 16) {
	/* no aux revision. */
	memset(mc->pending_devid.aux_fw_revision, 0, 4);
    } else {
	memcpy(mc->pending_devid.aux_fw_revision, rsp_data + 12, 4);
    }

    /* Copy these to the version we use for comparison. */
    mc->real_devid = mc->pending_devid;

    /* Either copy it or mark it to be copied. */
    if (mc->usecount == 1) {
	mc->devid = mc->pending_devid;
	mc->pending_devid_data = 0;
	mc->pending_new_mc = 0;

	/* OEM handlers set the pending data. */
	rv = check_oem_handlers(mc);
    } else {
	mc->pending_devid_data = 1;
	mc->pending_new_mc = 1;
	rv = EBUSY; /* Tell the user that they must call the OEM
		       handlers check later when the MC is
		       released. */
    }
    _ipmi_domain_mc_unlock(mc->domain);

    return rv;
}

int
_ipmi_mc_device_data_compares(ipmi_mc_t  *mc,
			      ipmi_msg_t *rsp)
{
    unsigned char *rsp_data = rsp->data;

    if (rsp->data_len < 12) {
	return EINVAL;
    }

    if (mc->real_devid.device_id != rsp_data[1])
	return 0;

    if (mc->real_devid.device_revision != (rsp_data[2] & 0xf))
	return 0;
    
    if (mc->real_devid.provides_device_sdrs != ((rsp_data[2] & 0x80) == 0x80))
	return 0;

    if (mc->real_devid.device_available != ((rsp_data[3] & 0x80) == 0x80))
	return 0;

    if (mc->real_devid.major_fw_revision != (rsp_data[3] & 0x7f))
	return 0;

    if (mc->real_devid.minor_fw_revision != (rsp_data[4]))
	return 0;

    if (mc->real_devid.major_version != (rsp_data[5] & 0xf))
	return 0;

    if (mc->real_devid.minor_version != ((rsp_data[5] >> 4) & 0xf))
	return 0;

    if (mc->real_devid.chassis_support != ((rsp_data[6] & 0x80) == 0x80))
	return 0;

    if (mc->real_devid.bridge_support != ((rsp_data[6] & 0x40) == 0x40))
	return 0;

    if (mc->real_devid.IPMB_event_generator_support
	!= ((rsp_data[6] & 0x20)==0x20))
	return 0;

    if (mc->real_devid.IPMB_event_receiver_support
	!= ((rsp_data[6] & 0x10) == 0x10))
	return 0;

    if (mc->real_devid.FRU_inventory_support != ((rsp_data[6] & 0x08) == 0x08))
	return 0;

    if (mc->real_devid.SEL_device_support != ((rsp_data[6] & 0x04) == 0x04))
	return 0;

    if (mc->real_devid.SDR_repository_support
	!= ((rsp_data[6] & 0x02) == 0x02))
	return 0;

    if (mc->real_devid.sensor_device_support != ((rsp_data[6] & 0x01) == 0x01))
	return 0;

    if (mc->real_devid.manufacturer_id != (rsp_data[7]
					   | (rsp_data[8] << 8)
					   | (rsp_data[9] << 16)))
	return 0;

    if (mc->real_devid.product_id != (rsp_data[10] | (rsp_data[11] << 8)))
	return 0;

    if (rsp->data_len < 16) {
	/* no aux revision, it should be all zeros. */
	if ((mc->real_devid.aux_fw_revision[0] != 0)
	    || (mc->real_devid.aux_fw_revision[1] != 0)
	    || (mc->real_devid.aux_fw_revision[2] != 0)
	    || (mc->real_devid.aux_fw_revision[3] != 0))
	    return 0;
    } else {
	if (memcmp(mc->real_devid.aux_fw_revision, rsp_data + 12, 4) != 0)
	    return 0;
    }

    /* Everything's the same. */
    return 1;
}

/***********************************************************************
 *
 * Get/set the information for an MC.
 *
 **********************************************************************/

void
_ipmi_mc_get_sdr_sensors(ipmi_mc_t     *mc,
			 ipmi_sensor_t ***sensors,
			 unsigned int  *count)
{
    *sensors = mc->sensors_in_my_sdr;
    *count = mc->sensors_in_my_sdr_count;
}

void
_ipmi_mc_set_sdr_sensors(ipmi_mc_t     *mc,
			 ipmi_sensor_t **sensors,
			 unsigned int  count)
{
    mc->sensors_in_my_sdr = sensors;
    mc->sensors_in_my_sdr_count = count;
}

void *
_ipmi_mc_get_sdr_entities(ipmi_mc_t *mc)
{
    return mc->entities_in_my_sdr;
}

void
_ipmi_mc_set_sdr_entities(ipmi_mc_t *mc, void *entities)
{
    mc->entities_in_my_sdr = entities;
}

int
ipmi_mc_provides_device_sdrs(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.provides_device_sdrs;
}

int
ipmi_mc_set_sdrs_first_read_handler(ipmi_mc_t      *mc,
				    ipmi_mc_ptr_cb handler,
				    void           *cb_data)
{
    CHECK_MC_LOCK(mc);
    mc->sdrs_first_read_handler = handler;
    mc->sdrs_first_read_cb_data = cb_data;
    return 0;
}

int ipmi_mc_set_sels_first_read_handler(ipmi_mc_t      *mc,
					ipmi_mc_ptr_cb handler,
					void           *cb_data)
{
    CHECK_MC_LOCK(mc);
    mc->sels_first_read_handler = handler;
    mc->sels_first_read_cb_data = cb_data;
    return 0;
}

static int
call_active_handler(void *cb_data, void *item1, void *item2)
{
    ipmi_mc_active_cb handler = item1;
    ipmi_mc_t         *mc = cb_data;

    handler(mc, mc->active, item2);
    return LOCKED_LIST_ITER_CONTINUE;
}

static void
call_active_handlers(ipmi_mc_t *mc)
{
    locked_list_iterate(mc->active_handlers, call_active_handler, mc);
}

int
ipmi_mc_add_active_handler(ipmi_mc_t         *mc,
			   ipmi_mc_active_cb handler,
			   void              *cb_data)
{
    CHECK_MC_LOCK(mc);

    if (locked_list_add(mc->active_handlers, handler, cb_data))
	return 0;
    else
	return ENOMEM;
}

int
ipmi_mc_remove_active_handler(ipmi_mc_t         *mc,
			      ipmi_mc_active_cb handler,
			      void              *cb_data)
{
    CHECK_MC_LOCK(mc);

    if (locked_list_remove(mc->active_handlers, handler, cb_data))
	return 0;
    else
	return EINVAL;
}

int
ipmi_mc_is_active(ipmi_mc_t *mc)
{
    return mc->active;
}

void
_ipmi_mc_set_active(ipmi_mc_t *mc, int val)
{
    ipmi_lock(mc->lock);
    if (mc->curr_active != val) {
	mc->curr_active = val;
	mc->active_transitions++;
    }
    ipmi_unlock(mc->lock);
}

void
ipmi_mc_set_provides_device_sdrs(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.provides_device_sdrs = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_device_available(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.device_available;
}

void
ipmi_mc_set_device_available(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.device_available = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_chassis_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.chassis_support;
}

void
ipmi_mc_set_chassis_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.chassis_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_bridge_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.bridge_support;
}

void
ipmi_mc_set_bridge_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.bridge_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_ipmb_event_generator_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.IPMB_event_generator_support;
}

void
ipmi_mc_set_ipmb_event_generator_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.IPMB_event_generator_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_ipmb_event_receiver_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.IPMB_event_receiver_support;
}

void
ipmi_mc_set_ipmb_event_receiver_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.IPMB_event_receiver_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_fru_inventory_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.FRU_inventory_support;
}

void
ipmi_mc_set_fru_inventory_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.FRU_inventory_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_sel_device_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.SEL_device_support;
}

void
ipmi_mc_set_sel_device_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.SEL_device_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_sdr_repository_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.SDR_repository_support;
}

void
ipmi_mc_set_sdr_repository_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.SDR_repository_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_sensor_device_support(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.sensor_device_support;
}

void
ipmi_mc_set_sensor_device_support(ipmi_mc_t *mc, int val)
{
    CHECK_MC_LOCK(mc);
    _ipmi_domain_mc_lock(mc->domain);
    mc->pending_devid.sensor_device_support = val;
    mc->pending_devid_data = 1;
    _ipmi_domain_mc_unlock(mc->domain);
}

int
ipmi_mc_device_id(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.device_id;
}

int
ipmi_mc_device_revision(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.device_revision;
}

int
ipmi_mc_major_fw_revision(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.major_fw_revision;
}

int
ipmi_mc_minor_fw_revision(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.minor_fw_revision;
}

int
ipmi_mc_major_version(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.major_version;
}

int
ipmi_mc_minor_version(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.minor_version;
}

int
ipmi_mc_manufacturer_id(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.manufacturer_id;
}

int
ipmi_mc_product_id(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->devid.product_id;
}

void
ipmi_mc_aux_fw_revision(ipmi_mc_t *mc, unsigned char val[])
{
    CHECK_MC_LOCK(mc);
    memcpy(val, mc->devid.aux_fw_revision, sizeof(mc->devid.aux_fw_revision));
}

void
ipmi_mc_set_oem_data(ipmi_mc_t *mc, void *data)
{
    CHECK_MC_LOCK(mc);
    mc->oem_data = data;
}

void *
ipmi_mc_get_oem_data(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->oem_data;
}

ipmi_sensor_info_t *
_ipmi_mc_get_sensors(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->sensors;
}

ipmi_control_info_t *
_ipmi_mc_get_controls(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->controls;
}

ipmi_sdr_info_t *
ipmi_mc_get_sdrs(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    return mc->sdrs;
}

unsigned int
ipmi_mc_get_address(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    if (mc->addr.addr_type == IPMI_IPMB_ADDR_TYPE) {
	ipmi_ipmb_addr_t *ipmb = (ipmi_ipmb_addr_t *) &(mc->addr);
	return ipmb->slave_addr;
    } else if (mc->addr.addr_type == IPMI_SYSTEM_INTERFACE_ADDR_TYPE) {
	ipmi_system_interface_addr_t *si = (void *) &(mc->addr);
	return si->channel;
    }

    /* Address is ignore for other types. */
    return 0;
}

void
ipmi_mc_get_ipmi_address(ipmi_mc_t    *mc,
			 ipmi_addr_t  *addr,
			 unsigned int *addr_len)
{
    /* We don't check the lock here because this is used as part of
       the lock grabbing. */
    if (addr)
	memcpy(addr, &mc->addr, mc->addr_len);
    if (addr_len)
	*addr_len = mc->addr_len;
}

unsigned int
ipmi_mc_get_channel(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);
    if (mc->addr.addr_type == IPMI_SYSTEM_INTERFACE_ADDR_TYPE)
	return IPMI_BMC_CHANNEL;
    else
	return mc->addr.channel;
}

ipmi_domain_t *ipmi_mc_get_domain(ipmi_mc_t *mc)
{
    return mc->domain;
}

unsigned int
ipmi_mc_get_unique_num(ipmi_mc_t *mc)
{
    unsigned int rv;

    ipmi_lock(mc->lock);
    rv = mc->uniq_num;
    mc->uniq_num++;
    ipmi_unlock(mc->lock);
    return rv;
}

int
_ipmi_mc_new_sensor(ipmi_mc_t     *mc,
		    ipmi_entity_t *ent,
		    ipmi_sensor_t *sensor,
		    void          *link)
{
    int rv = 0;

    CHECK_MC_LOCK(mc);

    if (mc->new_sensor_handler)
	rv = mc->new_sensor_handler(mc, ent, sensor, link,
				    mc->new_sensor_cb_data);
    return rv;
}

int
ipmi_mc_set_oem_new_sensor_handler(ipmi_mc_t                 *mc,
				   ipmi_mc_oem_new_sensor_cb handler,
				   void                      *cb_data)
{
    CHECK_MC_LOCK(mc);
    mc->new_sensor_handler = handler;
    mc->new_sensor_cb_data = cb_data;
    return 0;
}

int
ipmi_mc_set_sdrs_fixup_handler(ipmi_mc_t                 *mc,
			       ipmi_mc_oem_fixup_sdrs_cb handler,
			       void                      *cb_data)
{
    CHECK_MC_LOCK(mc);
    mc->fixup_sdrs_handler = handler;
    mc->fixup_sdrs_cb_data = cb_data;
    return 0;
}

int
ipmi_mc_add_oem_removed_handler(ipmi_mc_t              *mc,
				ipmi_mc_oem_removed_cb handler,
				void                   *cb_data)
{
    CHECK_MC_LOCK(mc);

    if (locked_list_add(mc->removed_handlers, handler, cb_data))
	return 0;
    else
	return ENOMEM;
}

int
ipmi_mc_remove_oem_removed_handler(ipmi_mc_t              *mc,
				   ipmi_mc_oem_removed_cb handler,
				   void                   *cb_data)
{
    CHECK_MC_LOCK(mc);

    if (locked_list_remove(mc->removed_handlers, handler, cb_data))
	return 0;
    else
	return EINVAL;
}

int
ipmi_mc_get_events_enable(ipmi_mc_t *mc)
{
    CHECK_MC_LOCK(mc);

    return mc->events_enabled;
}

int
ipmi_mc_set_events_enable(ipmi_mc_t       *mc,
			  int             val,
			  ipmi_mc_done_cb done,
			  void            *cb_data)
{
    int rv;

    CHECK_MC_LOCK(mc);

    if (!ipmi_mc_ipmb_event_generator_support(mc))
	return ENOTSUP;

    val = val != 0;

    ipmi_lock(mc->lock);
    if (val == mc->events_enabled) {
	/* Didn't changed, just finish the operation. */
	ipmi_unlock(mc->lock);
	done(mc, 0, cb_data);
	return 0;
    }

    mc->events_enabled = val;
    
    if (val) {
	unsigned int event_rcvr = ipmi_domain_get_event_rcvr(mc->domain);
	rv = send_set_event_rcvr(mc, event_rcvr, done, cb_data);
    } else {
	rv = send_set_event_rcvr(mc, 0, done, cb_data);
    }
    ipmi_unlock(mc->lock);

    return rv;
}


/***********************************************************************
 *
 * Global initialization and shutdown
 *
 **********************************************************************/

static int mc_initialized = 0;

int
_ipmi_mc_init(void)
{
    if (mc_initialized)
	return 0;

    oem_handlers = locked_list_alloc(ipmi_get_global_os_handler());
    if (!oem_handlers)
	return ENOMEM;

    mc_initialized = 1;

    return 0;
}

static int
oem_handler_free(void *cb_data, void *item1, void *item2)
{
    oem_handlers_t *hndlr = item1;

    locked_list_remove(oem_handlers, item1, item2);
    ipmi_mem_free(hndlr);
    return LOCKED_LIST_ITER_CONTINUE;
}

void
_ipmi_mc_shutdown(void)
{
    if (mc_initialized) {
	/* Destroy the members of the OEM list. */
	locked_list_iterate(oem_handlers, oem_handler_free, NULL);
	locked_list_destroy(oem_handlers);
	oem_handlers = NULL;
	mc_initialized = 0;
    }
}

/***********************************************************************
 *
 * Lock checking
 *
 **********************************************************************/

void
__ipmi_check_mc_lock(ipmi_mc_t *mc)
{
    if (!mc)
	return;

    if (!DEBUG_LOCKS)
	return;

    if (mc->usecount == 0)
	ipmi_report_lock_error(ipmi_domain_get_os_hnd(mc->domain),
			       "MC not locked when it should have been");
	
}

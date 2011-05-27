/*
 * Copyright (c) 2011 Jan Vesely
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** @addtogroup drvusbohcihc
 * @{
 */
/** @file
 * @brief OHCI Host controller driver routines
 */
#include <errno.h>
#include <str_error.h>
#include <adt/list.h>
#include <libarch/ddi.h>

#include <usb/debug.h>
#include <usb/usb.h>
#include <usb/ddfiface.h>

#include "hc.h"
#include "hcd_endpoint.h"

#define OHCI_USED_INTERRUPTS \
    (I_SO | I_WDH | I_UE | I_RHSC)
static int interrupt_emulator(hc_t *instance);
static void hc_gain_control(hc_t *instance);
static int hc_init_transfer_lists(hc_t *instance);
static int hc_init_memory(hc_t *instance);
/*----------------------------------------------------------------------------*/
/** Announce OHCI root hub to the DDF
 *
 * @param[in] instance OHCI driver intance
 * @param[in] hub_fun DDF fuction representing OHCI root hub
 * @return Error code
 */
int hc_register_hub(hc_t *instance, ddf_fun_t *hub_fun)
{
	assert(instance);
	assert(hub_fun);

	const usb_address_t hub_address =
	    device_keeper_get_free_address(&instance->manager, USB_SPEED_FULL);
	if (hub_address <= 0) {
		usb_log_error("Failed(%d) to get OHCI root hub address.\n",
		    hub_address);
		return hub_address;
	}
	instance->rh.address = hub_address;
	usb_device_keeper_bind(
	    &instance->manager, hub_address, hub_fun->handle);

#define CHECK_RET_RELEASE(ret, message...) \
if (ret != EOK) { \
	usb_log_error(message); \
	hc_remove_endpoint(instance, hub_address, 0, USB_DIRECTION_BOTH); \
	usb_device_keeper_release(&instance->manager, hub_address); \
	return ret; \
} else (void)0

	int ret = hc_add_endpoint(instance, hub_address, 0, USB_SPEED_FULL,
	    USB_TRANSFER_CONTROL, USB_DIRECTION_BOTH, 64, 0, 0);
	CHECK_RET_RELEASE(ret, "Failed(%d) to add OHCI rh endpoint 0.\n", ret);

	char *match_str = NULL;
	/* DDF needs heap allocated string */
	ret = asprintf(&match_str, "usb&class=hub");
	ret = ret > 0 ? 0 : ret;
	CHECK_RET_RELEASE(ret, "Failed(%d) to create match-id string.\n", ret);

	ret = ddf_fun_add_match_id(hub_fun, match_str, 100);
	CHECK_RET_RELEASE(ret, "Failed(%d) add root hub match-id.\n", ret);

	ret = ddf_fun_bind(hub_fun);
	CHECK_RET_RELEASE(ret, "Failed(%d) to bind root hub function.\n", ret);

	return EOK;
#undef CHECK_RET_RELEASE
}
/*----------------------------------------------------------------------------*/
/** Initialize OHCI hc driver structure
 *
 * @param[in] instance Memory place for the structure.
 * @param[in] regs Address of the memory mapped I/O registers.
 * @param[in] reg_size Size of the memory mapped area.
 * @param[in] interrupts True if w interrupts should be used
 * @return Error code
 */
int hc_init(hc_t *instance, uintptr_t regs, size_t reg_size, bool interrupts)
{
	assert(instance);
	int ret = EOK;
#define CHECK_RET_RETURN(ret, message...) \
if (ret != EOK) { \
	usb_log_error(message); \
	return ret; \
} else (void)0

	ret = pio_enable((void*)regs, reg_size, (void**)&instance->registers);
	CHECK_RET_RETURN(ret,
	    "Failed(%d) to gain access to device registers: %s.\n",
	    ret, str_error(ret));

	list_initialize(&instance->pending_batches);
	usb_device_keeper_init(&instance->manager);
	ret = usb_endpoint_manager_init(&instance->ep_manager,
	    BANDWIDTH_AVAILABLE_USB11);
	CHECK_RET_RETURN(ret, "Failed to initialize endpoint manager: %s.\n",
	    str_error(ret));

	ret = hc_init_memory(instance);
	CHECK_RET_RETURN(ret, "Failed to create OHCI memory structures: %s.\n",
	    str_error(ret));
#undef CHECK_RET_RETURN

	fibril_mutex_initialize(&instance->guard);
	hc_gain_control(instance);

	rh_init(&instance->rh, instance->registers);

	if (!interrupts) {
		instance->interrupt_emulator =
		    fibril_create((int(*)(void*))interrupt_emulator, instance);
		fibril_add_ready(instance->interrupt_emulator);
	}

	return EOK;
}
/*----------------------------------------------------------------------------*/
/** Create end register endpoint structures
 *
 * @param[in] instance OHCI driver structure.
 * @param[in] address USB address of the device.
 * @param[in] endpoint USB endpoint number.
 * @param[in] speed Communication speeed of the device.
 * @param[in] type Endpoint's transfer type.
 * @param[in] direction Endpoint's direction.
 * @param[in] mps Maximum packet size the endpoint accepts.
 * @param[in] size Maximum allowed buffer size.
 * @param[in] interval Time between transfers(interrupt transfers only).
 * @return Error code
 */
int hc_add_endpoint(
    hc_t *instance, usb_address_t address, usb_endpoint_t endpoint,
    usb_speed_t speed, usb_transfer_type_t type, usb_direction_t direction,
    size_t mps, size_t size, unsigned interval)
{
	endpoint_t *ep = malloc(sizeof(endpoint_t));
	if (ep == NULL)
		return ENOMEM;
	int ret =
	    endpoint_init(ep, address, endpoint, direction, type, speed, mps);
	if (ret != EOK) {
		free(ep);
		return ret;
	}

	hcd_endpoint_t *hcd_ep = hcd_endpoint_assign(ep);
	if (hcd_ep == NULL) {
		endpoint_destroy(ep);
		return ENOMEM;
	}

	ret = usb_endpoint_manager_register_ep(&instance->ep_manager, ep, size);
	if (ret != EOK) {
		hcd_endpoint_clear(ep);
		endpoint_destroy(ep);
		return ret;
	}

	/* Enqueue hcd_ep */
	switch (ep->transfer_type) {
	case USB_TRANSFER_CONTROL:
		instance->registers->control &= ~C_CLE;
		endpoint_list_add_ep(
		    &instance->lists[ep->transfer_type], hcd_ep);
		instance->registers->control_current = 0;
		instance->registers->control |= C_CLE;
		break;
	case USB_TRANSFER_BULK:
		instance->registers->control &= ~C_BLE;
		endpoint_list_add_ep(
		    &instance->lists[ep->transfer_type], hcd_ep);
		instance->registers->control |= C_BLE;
		break;
	case USB_TRANSFER_ISOCHRONOUS:
	case USB_TRANSFER_INTERRUPT:
		instance->registers->control &= (~C_PLE & ~C_IE);
		endpoint_list_add_ep(
		    &instance->lists[ep->transfer_type], hcd_ep);
		instance->registers->control |= C_PLE | C_IE;
		break;
	default:
		break;
	}

	return EOK;
}
/*----------------------------------------------------------------------------*/
/** Dequeue and delete endpoint structures
 *
 * @param[in] instance OHCI hc driver structure.
 * @param[in] address USB address of the device.
 * @param[in] endpoint USB endpoint number.
 * @param[in] direction Direction of the endpoint.
 * @return Error code
 */
int hc_remove_endpoint(hc_t *instance, usb_address_t address,
    usb_endpoint_t endpoint, usb_direction_t direction)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	endpoint_t *ep = usb_endpoint_manager_get_ep(&instance->ep_manager,
	    address, endpoint, direction, NULL);
	if (ep == NULL) {
		usb_log_error("Endpoint unregister failed: No such EP.\n");
		fibril_mutex_unlock(&instance->guard);
		return ENOENT;
	}

	hcd_endpoint_t *hcd_ep = hcd_endpoint_get(ep);
	if (hcd_ep) {
		/* Dequeue hcd_ep */
		switch (ep->transfer_type) {
		case USB_TRANSFER_CONTROL:
			instance->registers->control &= ~C_CLE;
			endpoint_list_remove_ep(
			    &instance->lists[ep->transfer_type], hcd_ep);
			instance->registers->control_current = 0;
			instance->registers->control |= C_CLE;
			break;
		case USB_TRANSFER_BULK:
			instance->registers->control &= ~C_BLE;
			endpoint_list_remove_ep(
			    &instance->lists[ep->transfer_type], hcd_ep);
			instance->registers->control |= C_BLE;
			break;
		case USB_TRANSFER_ISOCHRONOUS:
		case USB_TRANSFER_INTERRUPT:
			instance->registers->control &= (~C_PLE & ~C_IE);
			endpoint_list_remove_ep(
			    &instance->lists[ep->transfer_type], hcd_ep);
			instance->registers->control |= C_PLE | C_IE;
			break;
		default:
			break;
		}
		hcd_endpoint_clear(ep);
	} else {
		usb_log_warning("Endpoint without hcd equivalent structure.\n");
	}
	int ret = usb_endpoint_manager_unregister_ep(&instance->ep_manager,
	    address, endpoint, direction);
	fibril_mutex_unlock(&instance->guard);
	return ret;
}
/*----------------------------------------------------------------------------*/
/** Get access to endpoint structures
 *
 * @param[in] instance OHCI hc driver structure.
 * @param[in] address USB address of the device.
 * @param[in] endpoint USB endpoint number.
 * @param[in] direction Direction of the endpoint.
 * @param[out] bw Reserved bandwidth.
 * @return Error code
 */
endpoint_t * hc_get_endpoint(hc_t *instance, usb_address_t address,
    usb_endpoint_t endpoint, usb_direction_t direction, size_t *bw)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	endpoint_t *ep = usb_endpoint_manager_get_ep(&instance->ep_manager,
	    address, endpoint, direction, bw);
	fibril_mutex_unlock(&instance->guard);
	return ep;
}
/*----------------------------------------------------------------------------*/
/** Add USB transfer to the schedule.
 *
 * @param[in] instance OHCI hc driver structure.
 * @param[in] batch Batch representing the transfer.
 * @return Error code.
 */
int hc_schedule(hc_t *instance, usb_transfer_batch_t *batch)
{
	assert(instance);
	assert(batch);
	assert(batch->ep);

	/* Check for root hub communication */
	if (batch->ep->address == instance->rh.address) {
		return rh_request(&instance->rh, batch);
	}

	fibril_mutex_lock(&instance->guard);
	list_append(&batch->link, &instance->pending_batches);
	batch_commit(batch);

	/* Control and bulk schedules need a kick to start working */
	switch (batch->ep->transfer_type)
	{
	case USB_TRANSFER_CONTROL:
		instance->registers->command_status |= CS_CLF;
		break;
	case USB_TRANSFER_BULK:
		instance->registers->command_status |= CS_BLF;
		break;
	default:
		break;
	}
	fibril_mutex_unlock(&instance->guard);
	return EOK;
}
/*----------------------------------------------------------------------------*/
/** Interrupt handling routine
 *
 * @param[in] instance OHCI hc driver structure.
 * @param[in] status Value of the status register at the time of interrupt.
 */
void hc_interrupt(hc_t *instance, uint32_t status)
{
	assert(instance);
	usb_log_debug("OHCI(%p) interrupt: %x.\n", instance, status);
	if ((status & ~I_SF) == 0) /* ignore sof status */
		return;
	if (status & I_RHSC)
		rh_interrupt(&instance->rh);

	if (status & I_WDH) {
		fibril_mutex_lock(&instance->guard);
		usb_log_debug2("HCCA: %p-%#" PRIx32 " (%p).\n", instance->hcca,
		    instance->registers->hcca,
		    (void *) addr_to_phys(instance->hcca));
		usb_log_debug2("Periodic current: %#" PRIx32 ".\n",
		    instance->registers->periodic_current);

		link_t *current = instance->pending_batches.next;
		while (current != &instance->pending_batches) {
			link_t *next = current->next;
			usb_transfer_batch_t *batch =
			    usb_transfer_batch_from_link(current);

			if (batch_is_complete(batch)) {
				list_remove(current);
				usb_transfer_batch_finish(batch);
			}
			current = next;
		}
		fibril_mutex_unlock(&instance->guard);
	}

	if (status & I_UE) {
		hc_start_hw(instance);
	}

}
/*----------------------------------------------------------------------------*/
/** Check status register regularly
 *
 * @param[in] instance OHCI hc driver structure.
 * @return Error code
 */
int interrupt_emulator(hc_t *instance)
{
	assert(instance);
	usb_log_info("Started interrupt emulator.\n");
	while (1) {
		const uint32_t status = instance->registers->interrupt_status;
		instance->registers->interrupt_status = status;
		hc_interrupt(instance, status);
		async_usleep(10000);
	}
	return EOK;
}
/*----------------------------------------------------------------------------*/
/** Turn off any (BIOS)driver that might be in control of the device.
 *
 * @param[in] instance OHCI hc driver structure.
 */
void hc_gain_control(hc_t *instance)
{
	assert(instance);
	usb_log_debug("Requesting OHCI control.\n");
	/* Turn off legacy emulation */
	volatile uint32_t *ohci_emulation_reg =
	    (uint32_t*)((char*)instance->registers + 0x100);
	usb_log_debug("OHCI legacy register %p: %x.\n",
	    ohci_emulation_reg, *ohci_emulation_reg);
	/* Do not change A20 state */
	*ohci_emulation_reg &= 0x100;
	usb_log_debug("OHCI legacy register %p: %x.\n",
	    ohci_emulation_reg, *ohci_emulation_reg);

	/* Interrupt routing enabled => smm driver is active */
	if (instance->registers->control & C_IR) {
		usb_log_debug("SMM driver: request ownership change.\n");
		instance->registers->command_status |= CS_OCR;
		while (instance->registers->control & C_IR) {
			async_usleep(1000);
		}
		usb_log_info("SMM driver: Ownership taken.\n");
		instance->registers->control &= (C_HCFS_RESET << C_HCFS_SHIFT);
		async_usleep(50000);
		return;
	}

	const unsigned hc_status =
	    (instance->registers->control >> C_HCFS_SHIFT) & C_HCFS_MASK;
	/* Interrupt routing disabled && status != USB_RESET => BIOS active */
	if (hc_status != C_HCFS_RESET) {
		usb_log_debug("BIOS driver found.\n");
		if (hc_status == C_HCFS_OPERATIONAL) {
			usb_log_info("BIOS driver: HC operational.\n");
			return;
		}
		/* HC is suspended assert resume for 20ms */
		instance->registers->control &= (C_HCFS_RESUME << C_HCFS_SHIFT);
		async_usleep(20000);
		usb_log_info("BIOS driver: HC resumed.\n");
		return;
	}

	/* HC is in reset (hw startup) => no other driver
	 * maintain reset for at least the time specified in USB spec (50 ms)*/
	usb_log_info("HC found in reset.\n");
	async_usleep(50000);
}
/*----------------------------------------------------------------------------*/
/** OHCI hw initialization routine.
 *
 * @param[in] instance OHCI hc driver structure.
 */
void hc_start_hw(hc_t *instance)
{
	/* OHCI guide page 42 */
	assert(instance);
	usb_log_debug2("Started hc initialization routine.\n");

	/* Save contents of fm_interval register */
	const uint32_t fm_interval = instance->registers->fm_interval;
	usb_log_debug2("Old value of HcFmInterval: %x.\n", fm_interval);

	/* Reset hc */
	usb_log_debug2("HC reset.\n");
	size_t time = 0;
	instance->registers->command_status = CS_HCR;
	while (instance->registers->command_status & CS_HCR) {
		async_usleep(10);
		time += 10;
	}
	usb_log_debug2("HC reset complete in %zu us.\n", time);

	/* Restore fm_interval */
	instance->registers->fm_interval = fm_interval;
	assert((instance->registers->command_status & CS_HCR) == 0);

	/* hc is now in suspend state */
	usb_log_debug2("HC should be in suspend state(%x).\n",
	    instance->registers->control);

	/* Use HCCA */
	instance->registers->hcca = addr_to_phys(instance->hcca);

	/* Use queues */
	instance->registers->bulk_head =
	    instance->lists[USB_TRANSFER_BULK].list_head_pa;
	usb_log_debug2("Bulk HEAD set to: %p (%#" PRIx32 ").\n",
	    instance->lists[USB_TRANSFER_BULK].list_head,
	    instance->lists[USB_TRANSFER_BULK].list_head_pa);

	instance->registers->control_head =
	    instance->lists[USB_TRANSFER_CONTROL].list_head_pa;
	usb_log_debug2("Control HEAD set to: %p (%#" PRIx32 ").\n",
	    instance->lists[USB_TRANSFER_CONTROL].list_head,
	    instance->lists[USB_TRANSFER_CONTROL].list_head_pa);

	/* Enable queues */
	instance->registers->control |= (C_PLE | C_IE | C_CLE | C_BLE);
	usb_log_debug2("All queues enabled(%x).\n",
	    instance->registers->control);

	/* Enable interrupts */
	instance->registers->interrupt_enable = OHCI_USED_INTERRUPTS;
	usb_log_debug2("Enabled interrupts: %x.\n",
	    instance->registers->interrupt_enable);
	instance->registers->interrupt_enable = I_MI;

	/* Set periodic start to 90% */
	uint32_t frame_length = ((fm_interval >> FMI_FI_SHIFT) & FMI_FI_MASK);
	instance->registers->periodic_start = (frame_length / 10) * 9;
	usb_log_debug2("All periodic start set to: %x(%u - 90%% of %d).\n",
	    instance->registers->periodic_start,
	    instance->registers->periodic_start, frame_length);

	instance->registers->control &= (C_HCFS_OPERATIONAL << C_HCFS_SHIFT);
	usb_log_info("OHCI HC up and running(%x).\n",
	    instance->registers->control);
}
/*----------------------------------------------------------------------------*/
/** Initialize schedule queues
 *
 * @param[in] instance OHCI hc driver structure
 * @return Error code
 */
int hc_init_transfer_lists(hc_t *instance)
{
	assert(instance);
#define SETUP_ENDPOINT_LIST(type) \
do { \
	const char *name = usb_str_transfer_type(type); \
	int ret = endpoint_list_init(&instance->lists[type], name); \
	if (ret != EOK) { \
		usb_log_error("Failed(%d) to setup %s endpoint list.\n", \
		    ret, name); \
		endpoint_list_fini(&instance->lists[USB_TRANSFER_ISOCHRONOUS]);\
		endpoint_list_fini(&instance->lists[USB_TRANSFER_INTERRUPT]); \
		endpoint_list_fini(&instance->lists[USB_TRANSFER_CONTROL]); \
		endpoint_list_fini(&instance->lists[USB_TRANSFER_BULK]); \
		return ret; \
	} \
} while (0)

	SETUP_ENDPOINT_LIST(USB_TRANSFER_ISOCHRONOUS);
	SETUP_ENDPOINT_LIST(USB_TRANSFER_INTERRUPT);
	SETUP_ENDPOINT_LIST(USB_TRANSFER_CONTROL);
	SETUP_ENDPOINT_LIST(USB_TRANSFER_BULK);
#undef SETUP_ENDPOINT_LIST
	endpoint_list_set_next(&instance->lists[USB_TRANSFER_INTERRUPT],
	    &instance->lists[USB_TRANSFER_ISOCHRONOUS]);

	return EOK;
}
/*----------------------------------------------------------------------------*/
/** Initialize memory structures used by the OHCI hcd.
 *
 * @param[in] instance OHCI hc driver structure.
 * @return Error code.
 */
int hc_init_memory(hc_t *instance)
{
	assert(instance);

	bzero(&instance->rh, sizeof(instance->rh));
	/* Init queues */
	const int ret = hc_init_transfer_lists(instance);
	if (ret != EOK) {
		return ret;
	}

	/*Init HCCA */
	instance->hcca = malloc32(sizeof(hcca_t));
	if (instance->hcca == NULL)
		return ENOMEM;
	bzero(instance->hcca, sizeof(hcca_t));
	usb_log_debug2("OHCI HCCA initialized at %p.\n", instance->hcca);

	unsigned i = 0;
	for (; i < 32; ++i) {
		instance->hcca->int_ep[i] =
		    instance->lists[USB_TRANSFER_INTERRUPT].list_head_pa;
	}
	usb_log_debug2("Interrupt HEADs set to: %p (%#" PRIx32 ").\n",
	    instance->lists[USB_TRANSFER_INTERRUPT].list_head,
	    instance->lists[USB_TRANSFER_INTERRUPT].list_head_pa);

	/* Init interrupt code */
	instance->interrupt_code.cmds = instance->interrupt_commands;
	instance->interrupt_code.cmdcount = OHCI_NEEDED_IRQ_COMMANDS;
	{
		/* Read status register */
		instance->interrupt_commands[0].cmd = CMD_MEM_READ_32;
		instance->interrupt_commands[0].dstarg = 1;
		instance->interrupt_commands[0].addr =
		    (void*)&instance->registers->interrupt_status;

		/* Test whether we are the interrupt cause */
		instance->interrupt_commands[1].cmd = CMD_BTEST;
		instance->interrupt_commands[1].value =
		    OHCI_USED_INTERRUPTS;
		instance->interrupt_commands[1].srcarg = 1;
		instance->interrupt_commands[1].dstarg = 2;

		/* Predicate cleaning and accepting */
		instance->interrupt_commands[2].cmd = CMD_PREDICATE;
		instance->interrupt_commands[2].value = 2;
		instance->interrupt_commands[2].srcarg = 2;

		/* Write-clean status register */
		instance->interrupt_commands[3].cmd = CMD_MEM_WRITE_A_32;
		instance->interrupt_commands[3].srcarg = 1;
		instance->interrupt_commands[3].addr =
		    (void*)&instance->registers->interrupt_status;

		/* Accept interrupt */
		instance->interrupt_commands[4].cmd = CMD_ACCEPT;
	}

	return EOK;
}
/**
 * @}
 */

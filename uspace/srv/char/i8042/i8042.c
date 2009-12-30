/*
 * Copyright (c) 2001-2004 Jakub Jermar
 * Copyright (c) 2006 Josef Cejka
 * Copyright (c) 2009 Jiri Svoboda
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

/** @addtogroup kbd_port
 * @ingroup kbd
 * @{
 */ 
/** @file
 * @brief i8042 PS/2 port driver.
 */

#include <ddi.h>
#include <libarch/ddi.h>
#include <devmap.h>
#include <ipc/ipc.h>
#include <async.h>
#include <unistd.h>
#include <sysinfo.h>
#include <stdio.h>
#include <errno.h>

#include "i8042.h"

#define NAME "i8042"
#define NAMESPACE "char"

/* Interesting bits for status register */
#define i8042_OUTPUT_FULL	0x01
#define i8042_INPUT_FULL	0x02
#define i8042_AUX_DATA		0x20

/* Command constants */
#define i8042_CMD_WRITE_CMDB	0x60	/**< write command byte */
#define i8042_CMD_WRITE_AUX	0xd4	/**< write aux device */

/* Command byte fields */
#define i8042_KBD_IE		0x01
#define i8042_AUX_IE		0x02
#define i8042_KBD_DISABLE	0x10
#define i8042_AUX_DISABLE	0x20
#define i8042_KBD_TRANSLATE	0x40

/* Mouse constants */
#define MOUSE_OUT_INIT  0xf4
#define MOUSE_ACK       0xfa

enum {
	DEVID_PRI = 0, /**< primary device */
        DEVID_AUX = 1, /**< AUX device */
	MAX_DEVS  = 2
};

static irq_cmd_t i8042_cmds[] = {
	{
		.cmd = CMD_PIO_READ_8,
		.addr = NULL,	/* will be patched in run-time */
		.dstarg = 1
	},
	{
		.cmd = CMD_BTEST,
		.value = i8042_OUTPUT_FULL,
		.srcarg = 1,
		.dstarg = 3
	},
	{
		.cmd = CMD_PREDICATE,
		.value = 2,
		.srcarg = 3
	},
	{
		.cmd = CMD_PIO_READ_8,
		.addr = NULL,	/* will be patched in run-time */
		.dstarg = 2
	},
	{
		.cmd = CMD_ACCEPT
	}
};

static irq_code_t i8042_kbd = {
	sizeof(i8042_cmds) / sizeof(irq_cmd_t),
	i8042_cmds
};

static uintptr_t i8042_physical;
static uintptr_t i8042_kernel;
static i8042_t * i8042;

static i8042_port_t i8042_port[MAX_DEVS];

static void wait_ready(void)
{
	while (pio_read_8(&i8042->status) & i8042_INPUT_FULL);
}

static void i8042_irq_handler(ipc_callid_t iid, ipc_call_t *call);
static void i8042_connection(ipc_callid_t iid, ipc_call_t *icall);
static int i8042_init(void);
static void i8042_port_write(int devid, uint8_t data);


int main(int argc, char *argv[])
{
	char name[16];
	int i, rc;
	char dchar[MAX_DEVS] = { 'a', 'b' };

	printf(NAME ": i8042 PS/2 port driver\n");

	rc = devmap_driver_register(NAME, i8042_connection);
	if (rc < 0) {
		printf(NAME ": Unable to register driver.\n");
		return rc;
	}

	if (i8042_init() != EOK)
		return -1;

	for (i = 0; i < MAX_DEVS; i++) {
		i8042_port[i].client_phone = -1;

		snprintf(name, 16, "%s/ps2%c", NAMESPACE, dchar[i]);
		rc = devmap_device_register(name, &i8042_port[i].dev_handle);
		if (rc != EOK) {
			devmap_hangup_phone(DEVMAP_DRIVER);
			printf(NAME ": Unable to register device %s.\n", name);
			return rc;
		}
		printf(NAME ": Registered device %s\n", name);
	}

	printf(NAME ": Accepting connections\n");
	task_retval(0);
	async_manager();

	/* Not reached */
	return 0;
}

static int i8042_init(void)
{
	void *vaddr;

	i8042_physical = sysinfo_value("kbd.address.physical");
	i8042_kernel = sysinfo_value("kbd.address.kernel");
	if (pio_enable((void *) i8042_physical, sizeof(i8042_t), &vaddr) != 0)
		return -1;
	i8042 = vaddr;

	async_set_interrupt_received(i8042_irq_handler);

	/* Disable kbd, enable mouse */
	pio_write_8(&i8042->status, i8042_CMD_WRITE_CMDB);
	wait_ready();
	pio_write_8(&i8042->status, i8042_CMD_WRITE_CMDB);
	wait_ready();
	pio_write_8(&i8042->data, i8042_KBD_DISABLE);
	wait_ready();

	/* Flush all current IO */
	while (pio_read_8(&i8042->status) & i8042_OUTPUT_FULL)
		(void) pio_read_8(&i8042->data);

	i8042_port_write(DEVID_AUX, MOUSE_OUT_INIT);

	i8042_kbd.cmds[0].addr = (void *) &((i8042_t *) i8042_kernel)->status;
	i8042_kbd.cmds[3].addr = (void *) &((i8042_t *) i8042_kernel)->data;
	ipc_register_irq(sysinfo_value("kbd.inr"), device_assign_devno(), 0, &i8042_kbd);
	ipc_register_irq(sysinfo_value("mouse.inr"), device_assign_devno(), 0, &i8042_kbd);

	pio_write_8(&i8042->status, i8042_CMD_WRITE_CMDB);
	wait_ready();
	pio_write_8(&i8042->data, i8042_KBD_IE | i8042_KBD_TRANSLATE |
	    i8042_AUX_IE);
	wait_ready();

	return 0;
}

/** Character device connection handler */
static void i8042_connection(ipc_callid_t iid, ipc_call_t *icall)
{
	void *fs_va = NULL;
	ipc_callid_t callid;
	ipc_call_t call;
	ipcarg_t method;
	dev_handle_t dh;
	int flags;
	int retval;
	size_t cnt;
	int dev_id, i;

	printf(NAME ": connection handler\n");

	/* Get the device handle. */
	dh = IPC_GET_ARG1(*icall);

	/* Determine which disk device is the client connecting to. */
	dev_id = -1;
	for (i = 0; i < MAX_DEVS; i++)
		if (i8042_port[i].dev_handle == dh)
			dev_id = i;

	if (dev_id < 0/* || disk[dev_id].present == false*/) {
		ipc_answer_0(iid, EINVAL);
		return;
	}

	/* Answer the IPC_M_CONNECT_ME_TO call. */
	ipc_answer_0(iid, EOK);

	printf(NAME ": accepted connection\n");

	while (1) {
		callid = async_get_call(&call);
		method = IPC_GET_METHOD(call);
		switch (method) {
		case IPC_M_PHONE_HUNGUP:
			/* The other side has hung up. */
			ipc_answer_0(callid, EOK);
			return;
		case IPC_M_CONNECT_TO_ME:
			printf(NAME ": creating callback connection\n");
			if (i8042_port[dev_id].client_phone != -1) {
				retval = ELIMIT;
				break;
			}
			i8042_port[dev_id].client_phone = IPC_GET_ARG5(call);
			retval = 0;
			break;
		case IPC_FIRST_USER_METHOD:
			printf(NAME ": write %d to devid %d\n",
			    IPC_GET_ARG1(call), dev_id);
			i8042_port_write(dev_id, IPC_GET_ARG1(call));
			retval = 0;
			break;
		default:
			retval = EINVAL;
			break;
		}
		ipc_answer_0(callid, retval);
	}
}

void i8042_port_write(int devid, uint8_t data)
{
	if (devid == DEVID_AUX) {
		pio_write_8(&i8042->status, i8042_CMD_WRITE_AUX);
		wait_ready();
	}
	pio_write_8(&i8042->data, data);
	wait_ready();
}

static void i8042_irq_handler(ipc_callid_t iid, ipc_call_t *call)
{
	int status, data;
	int devid;

	status = IPC_GET_ARG1(*call);
	data = IPC_GET_ARG2(*call);

	if ((status & i8042_AUX_DATA)) {
		devid = DEVID_AUX;
	} else {
		devid = DEVID_PRI;
	}

	if (i8042_port[devid].client_phone != -1) {
		async_msg_1(i8042_port[devid].client_phone,
		    IPC_FIRST_USER_METHOD, data);
	}
}

/**
 * @}
 */
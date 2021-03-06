/*
 * Copyright (c) 2006 Ondrej Palkovsky
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

/** @addtogroup genericipc
 * @{
 */
/** @file
 */

/* IPC resources management
 *
 * The goal of this source code is to properly manage IPC resources and allow
 * straight and clean clean-up procedure upon task termination.
 *
 * The pattern of usage of the resources is:
 * - allocate a capability and phone kernel object (do not publish yet),
 *   connect to the answerbox, and finally publish the capability
 * - disconnect connected phone (some messages might be on the fly)
 * - find phone capability and send a message using phone
 * - answer message to phone
 * - hangup phone (the caller has hung up)
 * - hangup phone (the answerbox is exiting)
 *
 * Locking strategy
 *
 * - To use a phone, disconnect a phone etc., the phone must be first locked and
 *   then checked that it is connected
 * - To connect an allocated phone it need not be locked (assigning pointer is
 *   atomic on all platforms)
 *
 * - To answer a message, the answerbox must be locked
 * - The locking of phone and answerbox is done at the ipc_ level.
 *   It is perfectly correct to pass unconnected phone to these functions and
 *   proper reply will be generated.
 *
 * Locking order
 *
 * - first phone, then answerbox
 *   + Easy locking on calls
 *   - Very hard traversing list of phones when disconnecting because the phones
 *     may disconnect during traversal of list of connected phones. The only
 *     possibility is try_lock with restart of list traversal.
 *
 * Destroying is less frequent, this approach is taken.
 *
 * Phone call
 *
 * *** Connect_me_to ***
 * The caller sends IPC_M_CONNECT_ME_TO to an answerbox. The server receives
 * 'phoneid' of the connecting phone as an ARG5. If it answers with RETVAL=EOK,
 * the phone call is accepted, otherwise it is refused.
 *
 * *** Connect_to_me ***
 * The caller sends IPC_M_CONNECT_TO_ME.
 * The server receives an automatically opened phoneid. If it accepts
 * (RETVAL=EOK), it can use the phoneid immediately. Possible race condition can
 * arise, when the client receives messages from new connection before getting
 * response for connect_to_me message. Userspace should implement handshake
 * protocol that would control it.
 *
 * Phone hangup
 *
 * *** The caller hangs up (sys_ipc_hangup) ***
 * - The phone is disconnected (no more messages can be sent over this phone),
 *   all in-progress messages are correctly handled. The answerbox receives
 *   IPC_M_PHONE_HUNGUP call from the phone that hung up. When all async calls
 *   are answered, the phone is deallocated.
 *
 * *** The answerbox hangs up (ipc_answer(EHANGUP))
 * - The phone is disconnected. EHANGUP response code is sent to the calling
 *   task. All new calls through this phone get a EHUNGUP error code, the task
 *   is expected to call sys_ipc_hangup after cleaning up its internal
 *   structures.
 *
 *
 * Call forwarding
 *
 * The call can be forwarded, so that the answer to call is passed directly to
 * the original sender. However, this poses special problems regarding routing
 * of hangup messages.
 *
 * sys_ipc_hangup -> IPC_M_PHONE_HUNGUP
 * - this message CANNOT be forwarded
 *
 * EHANGUP during forward
 * - The *forwarding* phone will be closed, EFORWARD is sent to receiver.
 *
 * EHANGUP, ENOENT during forward
 * - EFORWARD is sent to the receiver, ipc_forward returns error code EFORWARD
 *
 * Cleanup strategy
 *
 * 1) Disconnect all our phones ('ipc_phone_hangup').
 *
 * 2) Disconnect all phones connected to answerbox.
 *
 * 3) Answer all messages in 'calls' and 'dispatched_calls' queues with
 *    appropriate error code (EHANGUP, EFORWARD).
 *
 * 4) Wait for all async answers to arrive and dispose of them.
 *
 */

#include <synch/spinlock.h>
#include <ipc/ipc.h>
#include <arch.h>
#include <proc/task.h>
#include <ipc/ipcrsc.h>
#include <assert.h>
#include <abi/errno.h>
#include <cap/cap.h>
#include <mm/slab.h>

static void phone_destroy(void *arg)
{
	phone_t *phone = (phone_t *) arg;
	slab_free(phone_cache, phone);
}

static kobject_ops_t phone_kobject_ops = {
	.destroy = phone_destroy
};


/** Allocate new phone in the specified task.
 *
 * @param[in]  task     Task for which to allocate a new phone.
 * @param[in]  publish  If true, the new capability will be published.
 * @param[out] phandle  New phone capability handle.
 * @param[out] kobject  New phone kobject.
 *
 * @return  An error code if a new capability cannot be allocated.
 */
errno_t phone_alloc(task_t *task, bool publish, cap_phone_handle_t *phandle,
    kobject_t **kobject)
{
	cap_handle_t handle;
	errno_t rc = cap_alloc(task, &handle);
	if (rc == EOK) {
		phone_t *phone = slab_alloc(phone_cache, FRAME_ATOMIC);
		if (!phone) {
			cap_free(TASK, handle);
			return ENOMEM;
		}
		kobject_t *kobj = malloc(sizeof(kobject_t), FRAME_ATOMIC);
		if (!kobj) {
			cap_free(TASK, handle);
			slab_free(phone_cache, phone);
			return ENOMEM;
		}

		ipc_phone_init(phone, task);
		phone->state = IPC_PHONE_CONNECTING;

		kobject_initialize(kobj, KOBJECT_TYPE_PHONE, phone,
		    &phone_kobject_ops);
		phone->kobject = kobj;

		if (publish)
			cap_publish(task, handle, kobj);

		*phandle = handle;
		if (kobject)
			*kobject = kobj;
	}
	return rc;
}

/** Free slot from a disconnected phone.
 *
 * All already sent messages will be correctly processed.
 *
 * @param handle Phone capability handle of the phone to be freed.
 *
 */
void phone_dealloc(cap_phone_handle_t handle)
{
	kobject_t *kobj = cap_unpublish(TASK, handle, KOBJECT_TYPE_PHONE);
	if (!kobj)
		return;

	kobject_put(kobj);
	cap_free(TASK, handle);
}

/** @}
 */

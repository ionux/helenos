/*
 * Copyright (c) 2009 Lukas Mejdrech
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

/** @addtogroup nildummy
 * @{
 */

/** @file
 *  Dummy network interface layer module stub.
 *  @see module.c
 */

#include <async.h>
#include <stdio.h>

#include <ipc/ipc.h>
#include <ipc/services.h>

#include "../../err.h"
#include "../../modules.h"

#include "../../include/net_interface.h"

#include "../../structures/packet/packet.h"

#include "../nil_module.h"

#include "nildummy.h"

/** The module name.
 */
#define NAME	"Dummy nil protocol"

/** Prints the module name.
 */
void	module_print_name( void );

/** Starts the dummy nil module.
 *  Initializes the client connection serving function, initializes the module, registers the module service and starts the async manager, processing IPC messages in an infinite loop.
 *  @param[in] client_connection The client connection processing function. The module skeleton propagates its own one.
 *  @returns EOK on success.
 *  @returns Other error codes as defined for the pm_init() function.
 *  @returns Other error codes as defined for the nil_initialize() function.
 *  @returns Other error codes as defined for the REGISTER_ME() macro function.
 */
int	module_start( async_client_conn_t client_connection );

/** Passes the parameters to the module specific nil_message() function.
 *  @param[in] callid The message identifier.
 *  @param[in] call The message parameters.
 *  @param[out] answer The message answer parameters.
 *  @param[out] answer_count The last parameter for the actual answer in the answer parameter.
 *  @returns EOK on success.
 *  @returns ENOTSUP if the message is not known.
 *  @returns Other error codes as defined for each specific module message function.
 */
int	module_message( ipc_callid_t callid, ipc_call_t * call, ipc_call_t * answer, int * answer_count );

void module_print_name( void ){
	printf( "%s", NAME );
}

int module_start( async_client_conn_t client_connection ){
	ERROR_DECLARE;

	ipcarg_t	phonehash;
	int			net_phone;

	async_set_client_connection( client_connection );
	net_phone = net_connect_module( SERVICE_NETWORKING );
	ERROR_PROPAGATE( pm_init());
	if( ERROR_OCCURRED( nil_initialize( net_phone ))
	|| ERROR_OCCURRED( REGISTER_ME( SERVICE_NILDUMMY, & phonehash ))){
		pm_destroy();
		return ERROR_CODE;
	}

	async_manager();

	pm_destroy();
	return EOK;
}

int	module_message( ipc_callid_t callid, ipc_call_t * call, ipc_call_t * answer, int * answer_count ){
	return nil_message( callid, call, answer, answer_count );
}

/** @}
 */
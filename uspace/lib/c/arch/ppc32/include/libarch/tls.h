/*
 * Copyright (c) 2006 Martin Decky
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

/** @addtogroup libcppc32
 * @{
 */
/** @file
 */

#ifndef LIBC_ppc32_TLS_H_
#define LIBC_ppc32_TLS_H_

#define CONFIG_TLS_VARIANT_1

#define PPC_TP_OFFSET 0x7000

typedef struct {
	void *fibril_data;
} tcb_t;

static inline void __tcb_set(tcb_t *tcb)
{
	void *tp = tcb;
	tp += PPC_TP_OFFSET + sizeof(tcb_t);

	asm volatile (
	    "mr %%r2, %0\n"
	    :
	    : "r" (tp)
	);
}

static inline tcb_t *__tcb_get(void)
{
	void *retval;

	asm volatile (
	    "mr %0, %%r2\n"
	    : "=r" (retval)
	);

	return (tcb_t *)(retval - PPC_TP_OFFSET - sizeof(tcb_t));
}

#endif

/** @}
 */

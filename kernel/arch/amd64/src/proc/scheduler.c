/*
 * Copyright (c) 2005 Ondrej Palkovsky
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

/** @addtogroup amd64proc
 * @{
 */
/** @file
 */

#include <proc/scheduler.h>
#include <cpu.h>
#include <proc/task.h>
#include <proc/thread.h>
#include <arch.h>
#include <arch/context.h>
#include <arch/asm.h>
#include <print.h>
#include <arch/pm.h>
#include <arch/ddi/ddi.h>

/** Perform amd64 specific tasks needed before the new task is run.
 *
 * Interrupts are disabled.
 */
void before_task_runs_arch(void)
{
	io_perm_bitmap_install();
}

/** Perform amd64 specific tasks needed before the new thread is scheduled. */
void before_thread_runs_arch(void)
{
	CPU->arch.tss->rsp0 =
	    (uintptr_t) &THREAD->kstack[THREAD_STACK_SIZE - SP_DELTA];
	
	/*
	 * Syscall support.
	 */
	swapgs();
	write_msr(AMD_MSR_GS, (uintptr_t) THREAD->arch.syscall_rsp);
	swapgs();
	
	/* TLS support - set FS to thread local storage */
	write_msr(AMD_MSR_FS, THREAD->arch.tls);
}

void after_thread_ran_arch(void)
{
}

/** @}
 */

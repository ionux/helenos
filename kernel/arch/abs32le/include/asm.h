/*
 * Copyright (c) 2010 Martin Decky
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

/** @addtogroup abs32le
 * @{
 */
/** @file
 */

#ifndef KERN_abs32le_ASM_H_
#define KERN_abs32le_ASM_H_

#include <arch/types.h>
#include <typedefs.h>
#include <config.h>

extern void interrupt_handlers(void);

extern void enable_l_apic_in_msr(void);


extern void asm_delay_loop(uint32_t);
extern void asm_fake_loop(uint32_t);


static inline __attribute__((noreturn)) void cpu_halt(void)
{
	/* On real hardware this should stop processing further
	   instructions on the CPU (and possibly putting it into
	   low-power mode) without any possibility of exitting
	   this function. */
	
	while (true);
}

static inline void cpu_sleep(void)
{
	/* On real hardware this should put the CPU into low-power
	   mode. However, the CPU is free to continue processing
	   futher instructions any time. The CPU also wakes up
	   upon an interrupt. */
}

static inline void pio_write_8(ioport8_t *port, uint8_t val)
{
}

/** Word to port
 *
 * Output word to port
 *
 * @param port Port to write to
 * @param val Value to write
 *
 */
static inline void pio_write_16(ioport16_t *port, uint16_t val)
{
}

/** Double word to port
 *
 * Output double word to port
 *
 * @param port Port to write to
 * @param val Value to write
 *
 */
static inline void pio_write_32(ioport32_t *port, uint32_t val)
{
}

/** Byte from port
 *
 * Get byte from port
 *
 * @param port Port to read from
 * @return Value read
 *
 */
static inline uint8_t pio_read_8(ioport8_t *port)
{
	return 0;
}

/** Word from port
 *
 * Get word from port
 *
 * @param port Port to read from
 * @return Value read
 *
 */
static inline uint16_t pio_read_16(ioport16_t *port)
{
	return 0;
}

/** Double word from port
 *
 * Get double word from port
 *
 * @param port Port to read from
 * @return Value read
 *
 */
static inline uint32_t pio_read_32(ioport32_t *port)
{
	return 0;
}

static inline ipl_t interrupts_enable(void)
{
	/* On real hardware this unconditionally enables preemption
	   by internal and external interrupts.
	   
	   The return value stores the previous interrupt level. */
	
	return 0;
}

static inline ipl_t interrupts_disable(void)
{
	/* On real hardware this disables preemption by the usual
	   set of internal and external interrupts. This does not
	   apply to special non-maskable interrupts and sychronous
	   CPU exceptions.
	   
	   The return value stores the previous interrupt level. */
	
	return 0;
}

static inline void interrupts_restore(ipl_t ipl)
{
	/* On real hardware this either enables or disables preemption
	   according to the interrupt level value from the argument. */
}

static inline ipl_t interrupts_read(void)
{
	/* On real hardware the return value stores the current interrupt
	   level. */
	
	return 0;
}

static inline uintptr_t get_stack_base(void)
{
	/* On real hardware this returns the address of the bottom
	   of the current CPU stack. The the_t structure is stored
	   on the bottom of stack and this is used to identify the
	   current CPU, current task, current thread and current
	   address space. */
	
	return 0;
}

static inline uintptr_t *get_ip()
{
	/* On real hardware this returns the current instruction
	   pointer value. The value certainly changes with each
	   instruction, but it can be still used to identify
	   a specific function. */
	
	return 0;
}

#endif

/** @}
 */
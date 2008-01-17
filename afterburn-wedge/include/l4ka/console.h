/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/include/l4ka/console.h
 * Description:   The file to be included by any source file that wants
 *                console output.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 ********************************************************************/
#ifndef __AFTERBURN_WEDGE__INCLUDE__L4KA__CONSOLE_H__
#define __AFTERBURN_WEDGE__INCLUDE__L4KA__CONSOLE_H__

#define L4_TRACEBUFFER
#define L4_PERFMON_ENERGY
#define L4_PERFMON
#define L4_CONFIG_CPU_IA32_P4

#include <l4/kdebug.h>
#include <l4/tracebuffer.h>

extern "C" int trace_printf(const char* format, ...);		
extern "C" int dbg_printf(const char* format, ...);		
extern bool l4_tracebuffer_enabled;

#define dprintf(n,a...)						\
    do								\
    {								\
	if(DBG_LEVEL>n)						\
	    dbg_printf(a);					\
	if (TRACE_LEVEL>n && l4_tracebuffer_enabled)		\
	    trace_printf(a, L4_TRACEBUFFER_MAGIC);		\
    } while(0)



#endif	/* __AFTERBURN_WEDGE__INCLUDE__L4KA__CONSOLE_H__ */

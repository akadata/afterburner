/*********************************************************************
 *
 * Copyright (C) 2003-2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/l4-common/l4thread.h
 * Description:   
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
 * $Id: l4thread.h,v 1.7 2005/07/18 08:42:09 joshua Exp $
 *
 ********************************************************************/
#ifndef __L4_COMMON__HTHREAD_H__
#define __L4_COMMON__HTHREAD_H__

#include <l4/thread.h>
#include INC_ARCH(types.h)
#include INC_WEDGE(config.h)
#include INC_WEDGE(user.h)
#include INC_ARCH(sync.h)

class l4thread_t;
typedef void (*l4thread_func_t)( void *, l4thread_t * );


/* Thread support data.
 */
class l4thread_t
{
private:
    L4_ThreadId_t local_tid;

    void *tlocal_data;

    void *start_param;
    l4thread_func_t start_func;

    void arch_prepare_start();

    static void self_halt();
    static void self_start();

public:
    L4_Word_t start_sp;
    L4_Word_t start_ip;

    friend class l4thread_manager_t;

    L4_ThreadId_t get_local_tid()
	{ return this->local_tid; }
    L4_ThreadId_t get_global_tid()
	{ return L4_GlobalId(this->local_tid); }

    void *get_tlocal_data()
	{ return this->tlocal_data; }

    void start()
	{ L4_Start( this->get_local_tid() ); }
};


class l4thread_manager_t
{
private:
    L4_Word_t thread_space_start;
    L4_Word_t thread_space_len;
    L4_Word_t utcb_size;
    L4_Word_t utcb_base;
    static const L4_Word_t max_local_threads = CONFIG_NR_VCPUS*3 + 5;
    static const L4_Word_t max_threads = 4096;

    volatile word_t utcb_mask[ max_local_threads / sizeof(L4_Word_t) + 1 ];
    volatile word_t tid_mask[ max_threads / sizeof(L4_Word_t) + 1 ];

public:
    void init( L4_Word_t tid_space_start, L4_Word_t tid_space_len );
    
    l4thread_t * create_thread( 
	vcpu_t *vcpu,
	L4_Word_t stack_bottom, 
	L4_Word_t stack_size,
	L4_Word_t prio, 
	l4thread_func_t start_func,  
	L4_ThreadId_t pager_tid,
	void *param=NULL, 
	void *tlocal_data=NULL, 
	L4_Word_t tlocal_size=0 );

    L4_ThreadId_t thread_id_allocate();
    void thread_id_release( L4_ThreadId_t tid );

    L4_Word_t utcb_allocate();
    void utcb_release( L4_Word_t utcb );

    void terminate_thread( L4_ThreadId_t tid );
};

extern inline l4thread_manager_t * get_l4thread_manager()
{
    extern l4thread_manager_t l4thread_manager;
    return &l4thread_manager;
}


#endif	/* __L4_COMMON__HTHREAD_H__ */

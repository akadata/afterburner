/*********************************************************************
 *
 * Copyright (C) 2007,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/include/device/ide.h
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
 ********************************************************************/

#ifndef __QEMU_DM_H__
#define __QEMU_DM_H__

#include INC_ARCH(types.h)
#include <debug.h>
#include INC_WEDGE(l4thread.h)
#include <console.h>
#include <string.h>

#include <qemu-dm_idl_client.h>
#include <qemu-dm_pager_idl_server.h>

typedef u64_t uint64_t;
typedef u32_t uint32_t;
typedef u16_t uint16_t;
typedef u8_t  uint8_t;

#define IOREQ_READ      1
#define IOREQ_WRITE     0

#define STATE_IOREQ_NONE        0
#define STATE_IOREQ_READY       1
#define STATE_IOREQ_INPROCESS   2
#define STATE_IORESP_READY      3

#define IOREQ_TYPE_PIO          0 /* pio */
#define IOREQ_TYPE_COPY         1 /* mmio ops */
#define IOREQ_TYPE_AND          2
#define IOREQ_TYPE_OR           3
#define IOREQ_TYPE_XOR          4
#define IOREQ_TYPE_XCHG         5
#define IOREQ_TYPE_ADD          6
#define IOREQ_TYPE_TIMEOFFSET   7
#define IOREQ_TYPE_INVALIDATE   8 /* mapcache */
#define IOREQ_TYPE_SUB          9

/*
 * VMExit dispatcher should cooperate with instruction decoder to
 * prepare this structure and notify service OS and DM by sending
 * virq
 */
struct ioreq {
    uint64_t addr;          /*  physical address            */
    uint64_t size;          /*  size in bytes               */
    uint64_t count;         /*  for rep prefixes            */
    uint64_t data;          /*  data (or paddr of data)     */
    uint8_t state:4;
    uint8_t data_is_ptr:1;  /*  if 1, data above is the guest paddr 
                             *   of the real data to use.   */
    uint8_t dir:1;          /*  1=read, 0=write             */
    uint8_t df:1;
    uint8_t pad:1;
    uint8_t type;           /* I/O type                     */
    uint8_t _pad0[6];
    uint64_t io_count;      /* How many IO done on a vcpu   */
};
typedef struct ioreq ioreq_t;

struct vcpu_iodata {
    struct ioreq vp_ioreq;
};
typedef struct vcpu_iodata vcpu_iodata_t;

struct shared_iopage {
    struct vcpu_iodata   vcpu_iodata[1];
};
typedef struct shared_iopage shared_iopage_t;

struct buf_ioreq {
    uint8_t  type;   /* I/O type                    */
    uint8_t  pad:1;
    uint8_t  dir:1;  /* 1=read, 0=write             */
    uint8_t  size:2; /* 0=>1, 1=>2, 2=>4, 3=>8. If 8, use two buf_ioreqs */
    uint32_t addr:20;/* physical address            */
    uint32_t data;   /* data                        */
};
typedef struct buf_ioreq buf_ioreq_t;

#define IOREQ_BUFFER_SLOT_NUM     511 /* 8 bytes each, plus 2 4-byte indexes */
struct buffered_iopage {
    unsigned int read_pointer;
    unsigned int write_pointer;
    buf_ioreq_t buf_ioreq[IOREQ_BUFFER_SLOT_NUM];
}; /* NB. Size of this structure must be no greater than one page. */
typedef struct buffered_iopage buffered_iopage_t;

#if defined(__i386__) || defined(__x86_64__)
#define ACPI_PM1A_EVT_BLK_ADDRESS           0x0000000000001f40
#define ACPI_PM1A_CNT_BLK_ADDRESS           (ACPI_PM1A_EVT_BLK_ADDRESS + 0x04)
#define ACPI_PM_TMR_BLK_ADDRESS             (ACPI_PM1A_EVT_BLK_ADDRESS + 0x08)
#endif /* defined(__i386__) || defined(__x86_64__) */



class qemu_dm_t
{
public:
    struct {
        __attribute__((aligned(4096))) shared_iopage_t sio_page;
        __attribute__((aligned(4096))) buffered_iopage_t bio_page;
    } s_pages;

    L4_ThreadId_t qemu_dm_server_id;
    L4_ThreadId_t pager_id;
    L4_ThreadId_t irq_server_id;

    void init(void);


    unsigned char qemu_dm_pager_stack[KB(16)] ALIGNED(CONFIG_STACK_ALIGN);
    void pager_server_loop(void);

    void init_shared_pages(void) {
	memset(&s_pages.sio_page,0,PAGE_SIZE);
	memset(&s_pages.bio_page,0,PAGE_SIZE);
    }

    ioreq_t * get_ioreq_page(void) { return &s_pages.sio_page.vcpu_iodata[0].vp_ioreq; }

    L4_Word_t send_pio(unsigned long port, unsigned long count, L4_Word_t size,
		       L4_Word_t value, L4_Word_t dir, L4_Word_t df, L4_Word_t value_is_ptr);
    
    L4_Word_t raise_event(L4_Word_t event);

};

#endif /* __QEMU_DM_H__ */

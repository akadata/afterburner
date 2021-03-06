/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:	l4ka-drivers/net/server.c
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

#include <l4/kip.h>

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>

#include <linux/if_bridge.h>
#if defined(DO_LINUX_BRIDGE)
#include <br_private.h>
#endif

#include <asm/io.h>

#include <glue/thread.h>
#include <glue/bottomhalf.h>
#include <glue/vmmemory.h>
#include <glue/wedge.h>

#if defined(CONFIG_X86_IO_APIC)
#include <acpi/acpi.h>
#include <linux/acpi.h>
#endif

#if defined(PREFIX)
#undef PREFIX
#endif
#define PREFIX "L4VMnet server: "


#include "server.h"
#include "lanaddress.h"

#define TXDP (0x20/4)
#define ISR  (0x10/4)
#define IMR  (0x14/4)
#define IER  (0x18/4)

#if !defined(CONFIG_DRIVERS_NET_OPTIMIZE)
int L4VMnet_debug_level = 3;
MODULE_PARM( L4VMnet_debug_level, "i" );
#endif

int L4VMnet_server_irq = 0;
MODULE_PARM( L4VMnet_server_irq, "i" );

L4VMnet_server_t L4VMnet_server = { 
    L4_nilthread, L4_nilthread, L4_nilthread
};

static void L4VMnet_deliver_irq(L4_Word_t irq_flags)
{
    L4VM_server_deliver_irq(L4VMnet_server.my_irq_tid, L4VMnet_server_irq,  irq_flags,
			    &L4VMnet_server.irq_status, L4VMnet_server.irq_mask, &L4VMnet_server.irq_pending);
    
}

static L4_Word16_t L4VMnet_cmd_allocate_bottom(void)
{
    return L4VM_server_cmd_allocate( &L4VMnet_server.bottom_half_cmds, L4VMnet_server.my_irq_tid, 
				     L4VMnet_server_irq, L4VMNET_IRQ_BOTTOM_HALF_CMD, 
				     &L4VMnet_server.irq_status, L4VMnet_server.irq_mask,
				     &L4VMnet_server.irq_pending);

}

static L4_Word16_t L4VMnet_cmd_allocate_top(void)
{
    return L4VM_server_cmd_allocate( &L4VMnet_server.top_half_cmds, L4VMnet_server.my_irq_tid, 
				     L4VMnet_server_irq, L4VMNET_IRQ_TOP_HALF_CMD, 
				     &L4VMnet_server.irq_status, L4VMnet_server.irq_mask,
				     &L4VMnet_server.irq_pending);

}


static int L4VMnet_absorb_frame( struct sk_buff *skb );
static void L4VMnet_flush_tasklet_handler( unsigned long unused );

/*
 * Our L4 clients hand us unreadable packets.  The packets are not mapped
 * into the address space.  So no routing decisions by Linux are possible.
 * This driver must perform the routing
 * decisions.  It shouldn't be a problem in our experimentation.
 *
 * The driver must watch for notifier events: NETDEV_UP and
 * NETDEV_DOWN.  Thus we can detect if our target device goes down.  We
 * mustn't send a packet once a device goes down, or we'd give the target
 * device driver unexpected behavior from Linux.  Likewise, if the target
 * device driver is paused, we mustn't try to deliver packets.
 */

DECLARE_TASKLET( L4VMnet_flush_tasklet, L4VMnet_flush_tasklet_handler, 0 );

/***************************************************************************
 *
 * Interface handle mapper.
 *
 ***************************************************************************/

#define L4VMNET_MAX_CLIENTS	16

static L4VMnet_client_info_t *L4VMnet_client_list[ L4VMNET_MAX_CLIENTS ];

static atomic_t dirty_tx_pkt_cnt = ATOMIC_INIT(0);

extern void
L4VMnet_client_list_init( void )
{
    int i;

    for( i = 0; i < L4VMNET_MAX_CLIENTS; i++ )
	L4VMnet_client_list[i] = NULL;
}

#if 0
static void
L4VMnet_client_handle_free( IVMnet_handle_t handle )
{
    L4VMnet_client_list[ handle ] = NULL;
}
#endif

extern int L4VMnet_client_handle_set(
	IVMnet_handle_t handle,
	L4VMnet_client_info_t *client )
{
    static spinlock_t lock = SPIN_LOCK_UNLOCKED;

    if( handle >= L4VMNET_MAX_CLIENTS )
	return FALSE;

    spin_lock( &lock );
    {
	if( L4VMnet_client_list[handle] != NULL )
	{
	    spin_unlock( &lock );
	    return FALSE;
	}
	L4VMnet_client_list[ handle ] = client;
    }
    spin_unlock( &lock );

    return TRUE;
}

extern inline L4VMnet_client_info_t *
L4VMnet_client_handle_lookup( IVMnet_handle_t handle )
{
    if( handle < L4VMNET_MAX_CLIENTS )
	return L4VMnet_client_list[ handle ];
    return NULL;
}

/***************************************************************************
 *
 * Receiving packets.
 *
 ***************************************************************************/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,9)
static int L4VMnet_bridge_handler( struct net_bridge_port *p, struct sk_buff **pskb )
{
    struct sk_buff *skb = *pskb;

    dprintk(4, PREFIX "inspect packet %x\n", skb);

    skb_push( skb, ETH_HLEN ); // Uncover the LAN address.
    if( L4VMnet_absorb_frame(skb) )
	return 1;

    skb_pull( skb, ETH_HLEN ); // Cover the LAN address back up.
    return 0;	// We didn't handle the packet.
}
#else
static int L4VMnet_bridge_handler( struct sk_buff *skb )
{
    dprintk(2, PREFIX "inspect packet %x\n", skb);

    skb_push( skb, ETH_HLEN ); // Uncover the LAN address.
    if( L4VMnet_absorb_frame(skb) )
	return 0;

    skb_pull( skb, ETH_HLEN ); // Cover the LAN address back up.
    return 1;	// We didn't handle the packet.
}
#endif
/***************************************************************************
 *
 * Sending packets outbound from the client.
 *
 ***************************************************************************/

/*
 * When deallocating memory, Linux uses skb->head.
 * When accessing the shared skb info (skb_shinfo), Linux uses skb->end.
 * When transmitting data, Linux uses skb->data.
 *
 * skb->head points to the beginning of allocated space.
 * skb->data points to the beginning of valid data.
 * skb->tail points to the end of valid data.
 * skb->end points to the maximum valid address for tail.
 */

typedef struct L4VMnet_skb_shadow
{
    struct sk_buff *skb;
    L4VMnet_desc_ring_t *ring;
    L4_Word16_t start_frag;
    L4_Word16_t frag_count;
} L4VMnet_skb_shadow_t;

#if !defined(CONFIG_AFTERBURN_DRIVERS_NET_DP83820)

static void
L4VMnet_skb_destructor( struct sk_buff *skb )
{
    L4VMnet_skb_shadow_t *shadow = (L4VMnet_skb_shadow_t *)skb->head;
    IVMnet_ring_descriptor_t *desc, *frag_desc;

    ASSERT(shadow);
    ASSERT(shadow->skb == skb);
    ASSERT(shadow->start_frag < shadow->ring->cnt);

    desc = &shadow->ring->desc_ring[ shadow->start_frag ];
    ASSERT( desc->status.X.server_owned && desc->status.X.dropped );

    // Handle the fragments first.  The last fragment in the list has
    // its fragment bit cleared.
    if( desc->status.X.fragment )
    {
	L4_Word16_t idx = (shadow->start_frag + 1) % shadow->ring->cnt;
	while( shadow->frag_count )
	{
	    // Release the fragment descriptor back to the client.
	    frag_desc = &shadow->ring->desc_ring[ idx ];
	    ASSERT( frag_desc->status.X.server_owned );
	    frag_desc->status.X.server_owned = 0;
	    idx = (idx + 1) % shadow->ring->cnt;
	    shadow->frag_count--;
	}
    }

    // Handle the head fragment last.
    desc->status.X.server_owned = 0;

    // Clean-up the skb.
    ASSERT( !skb_shinfo(skb)->frag_list );
    skb_shinfo(skb)->nr_frags = 0;
    skb->data = skb->head;
    skb->tail = skb->data;
    skb->h.raw = skb->data;
    skb->len = 0;
    skb->data_len = 0;

    dprintk(0, PREFIX "skb destructor\n" );
}

static struct sk_buff *
L4VMnet_skb_wrap_packet( L4VMnet_client_info_t *client )
{
    // TODO: validate that buffers + fragments together create valid
    // skb buffs.  Lots of overlapping state must be validated.
    L4VMnet_desc_ring_t *ring;
    IVMnet_ring_descriptor_t *desc, *frag_desc;
    struct sk_buff *skb;
    L4VMnet_skb_shadow_t *skb_shadow;
    int fragment;

    ring = &client->send_ring;

    // Grab the first packet, and move to the next.
    desc = &ring->desc_ring[ ring->start_free ];
    if( !desc->status.X.server_owned )
	return NULL;
    if( unlikely(desc->status.X.dropped) )
    {
	dprintk(1,  PREFIX "send wrap-around.\n" );
	return NULL;
    }

    skb = alloc_skb( sizeof(L4VMnet_skb_shadow_t), GFP_ATOMIC );
    if( unlikely(skb == NULL) )
	return NULL;

    skb_shadow = (L4VMnet_skb_shadow_t *)skb->head;
    skb_shadow->ring = ring;
    skb_shadow->start_frag = ring->start_free;
    skb_shadow->frag_count = 0;
    skb_shadow->skb = skb;

    ring->start_free = (ring->start_free + 1) % ring->cnt;

    // TODO: validate that the packet lives in the client's address space.
    ASSERT( desc->buffer && desc->buffer_len );
    ASSERT( desc->buffer + client->client_space->bus_start < num_physpages * PAGE_SIZE );
    skb->data = bus_to_virt(desc->buffer + client->client_space->bus_start);
    skb->len  = desc->buffer_len;
    ASSERT( skb->len && skb->data );
    skb->tail = skb->data + skb->len;

    skb->destructor = L4VMnet_skb_destructor;
    skb->ip_summed  = CHECKSUM_NONE;
    skb->protocol   = desc->protocol;
    skb->pkt_type   = desc->pkt_type;

    // Where the device must start the checksum.
    skb->h.raw = desc->csum.tx.start + skb->data;
    // Where the device must store the checksum.
    skb->csum = desc->csum.tx.offset + skb->data - skb->h.raw;

    if( likely(desc->status.X.csum) )
    {
	if( (skb->h.raw >= skb->data) && (skb->h.raw <= skb->tail) &&
		((skb->csum + skb->h.raw) <= skb->tail) )
	    skb->ip_summed = CHECKSUM_HW;
	else
	    dprintk(1, PREFIX "bad hardware checksum request.\n" );
    }

    dprintk(4, PREFIX "skb protocol %x, pkt_type %x, "
	    "data %p (bus %p/%p)\n",
	    skb->protocol, skb->pkt_type, skb->data, (void *)desc->buffer,
	    (void *)virt_to_bus(skb->data) );

    fragment = 0;
    frag_desc = desc;
    while( frag_desc->status.X.fragment )
    {
	L4_Word_t paddr;

	if( unlikely(fragment >= MAX_SKB_FRAGS) )
	    break;
	frag_desc = &ring->desc_ring[ ring->start_free ];
	if( unlikely(!frag_desc->status.X.server_owned) )
	    break;
	ring->start_free = (ring->start_free + 1) % ring->cnt;

	skb_shinfo(skb)->frags[fragment].size = frag_desc->buffer_len;
	skb_shinfo(skb)->frags[fragment].page_offset =
	    (unsigned long)frag_desc->buffer & (PAGE_SIZE-1);
	ASSERT(frag_desc->buffer);
	ASSERT( frag_desc->buffer + client->client_space->bus_start < num_physpages * PAGE_SIZE );
	paddr = virt_to_phys(bus_to_virt(frag_desc->buffer + client->client_space->bus_start));
	skb_shinfo(skb)->frags[fragment].page = mem_map + (paddr >> PAGE_SHIFT);
	ASSERT( paddr && frag_desc->buffer_len );
	skb->len += frag_desc->buffer_len;
	skb->data_len += frag_desc->buffer_len;
	dprintk(2, PREFIX "frag %d @ %x + %x, %u\n",
		fragment,
		(L4_Word_t)skb_shinfo(skb)->frags[fragment].page,
		(L4_Word_t)skb_shinfo(skb)->frags[fragment].page_offset,
		skb_shinfo(skb)->frags[fragment].size );
	ASSERT( page_to_bus(skb_shinfo(skb)->frags[fragment].page) == (frag_desc->buffer & PAGE_MASK) );

	fragment++;
    }
    skb_shinfo(skb)->nr_frags = fragment;
    skb_shadow->frag_count = fragment;

    ASSERT( skb_headlen(skb) == desc->buffer_len );
    if( fragment )
	dprintk(2, PREFIX "fragments %d, tot len %d, fragment len %d\n",
		fragment, skb->len, skb->data_len );

    desc->status.X.dropped = 1; // Remember that we have submitted this packet.

    return skb;
}

static int L4VMnet_dev_queue_client_pkt( L4VMnet_client_info_t *client )
{
    struct sk_buff *skb;

    skb = L4VMnet_skb_wrap_packet( client );
    if( skb == NULL )
	return FALSE;

    // TODO: if the skb exceeds the device's queue length, it will call
    // kfree_skb(skb) at interrupt time, which isn't compatible with
    // desctructors.  And it will drop our packets.
    ASSERT( client->real_dev );
    skb->dev = client->real_dev;
    dev_queue_xmit( skb );	// Can be called from an interrupt.

    return TRUE;
}

static void
L4VMnet_transmit_client_pkts( void )
{
    L4VMnet_client_info_t *client;
    L4_Word_t pkts_sent, total_pkts;

    total_pkts = 0;
    do {
	pkts_sent = 0;
	for( client = L4VMnet_server.client_list; client; client = client->next)
	{
	    if( !client->operating )
		continue;

	    if( L4VMnet_dev_queue_client_pkt(client) )
		pkts_sent++;
	}

	total_pkts += pkts_sent;
    } while( pkts_sent );

    dprintk(2, PREFIX "%u packets transmitted\n", total_pkts );
}
#endif


/***************************************************************************
 *
 * Sending packets outbound from the client via the dp83820 interface.
 *
 ***************************************************************************/

static void notify_dp83820_client( L4VMnet_client_info_t *client )
{
    IVMnet_client_shared_t *shared = client->client_shared;

    ASSERT( L4_MyLocalId().raw == get_vcpu()->main_ltid.raw );
    
    dprintk(2, PREFIX "notify client tid %x irq pending %d isr %x imr %x ier %x\n",
	    shared->client_irq_tid,
	    shared->client_irq_pending, shared->dp83820_regs[ISR], shared->dp83820_regs[IMR],
	    shared->dp83820_regs[IER]);

    if (!shared->client_irq_pending 
	&& (shared->dp83820_regs[ISR] & shared->dp83820_regs[IMR])
	&& shared->dp83820_regs[IER] )
    {
	unsigned long irq_flags;
	L4_MsgTag_t tag;
	
	dprintk(0, PREFIX "delivering virq %d to client tid %x\n",
		shared->client_irq_no, shared->client_irq_tid  );
	local_irq_save(irq_flags); 
	ASSERT( !vcpu_interrupts_enabled() );
	tag = l4ka_wedge_send_virtual_irq(shared->client_irq_no, shared->client_irq_tid, L4_ZeroTime);
	
	local_irq_restore(irq_flags);

	if( L4_IpcFailed(tag) ) {
	    dprintk(1, PREFIX "skb destructor notify failed\n" );
	    shared->client_irq_pending = 0;
	}
    }
}

typedef struct L4VMnet_skb_dp83820_shadow
{
    struct sk_buff *skb;
    L4VMnet_client_info_t *client;
    L4_Word_t link;
} L4VMnet_skb_dp83820_shadow_t;


extern inline IVMnet_dp83820_descriptor_t *
get_dp83820_desc( L4VMnet_client_info_t *client, L4_Word_t link )
{
    return (IVMnet_dp83820_descriptor_t *)
	(link - client->dp83820_tx.first + client->dp83820_tx.ring_start);
}

extern inline IVMnet_dp83820_descriptor_t *
get_dp83820_current_desc( L4VMnet_client_info_t *client )
{
    // TODO: make SMP safe
    return get_dp83820_desc( client, client->client_shared->dp83820_regs[TXDP] );
}

static int reignite_transmit( L4VMnet_client_info_t *client )
{
    IVMnet_dp83820_descriptor_t *desc;

    desc = get_dp83820_current_desc( client );
    if( !desc->cmd_status.tx.own )
	return 0;

    L4VMnet_server.server_status->irq_status |= 1;
    L4VMnet_server.server_status->irq_pending = 1;

    l4ka_wedge_raise_irq( client->client_shared->server_irq_no );
    return 1;
}


static void
L4VMnet_skb_dp83820_destructor( struct sk_buff *skb )
{
    L4VMnet_skb_dp83820_shadow_t *shadow = 
	(L4VMnet_skb_dp83820_shadow_t *)skb->head;
    IVMnet_dp83820_descriptor_t *desc, *frag_desc;
    int do_irq = 0, delay;
    L4_Word_t link;

    ASSERT(shadow);
    ASSERT(shadow->skb == skb);
    ASSERT(shadow->client);
    ASSERT(shadow->link);
    ASSERT( L4_MyLocalId().raw == get_vcpu()->main_ltid.raw );

    desc = get_dp83820_desc( shadow->client, shadow->link );
    ASSERT( desc->cmd_status.tx.own && desc->cmd_status.tx.ok );
    if( desc->cmd_status.tx.intr )
	do_irq = 1;
    link = desc->cmd_status.tx.more ? desc->link : 0;
    desc->cmd_status.tx.own = 0;
    atomic_dec( &dirty_tx_pkt_cnt );

    while( link )
    {
	// Release the fragment descriptor back to the client.
	ASSERT(link);
	frag_desc = get_dp83820_desc( shadow->client, link );
    	ASSERT(frag_desc->cmd_status.tx.own && frag_desc->cmd_status.tx.ok);
	if( frag_desc->cmd_status.tx.intr )
	     do_irq = 1;
	link = frag_desc->cmd_status.tx.more ? frag_desc->link : 0;
	frag_desc->cmd_status.tx.own = 0;
    }

    delay = reignite_transmit( shadow->client );
    set_bit( 6, (volatile unsigned long *)&shadow->client->client_shared->dp83820_regs[ISR] );
    if( do_irq )
	set_bit( 7, (volatile unsigned long *)&shadow->client->client_shared->dp83820_regs[ISR] );
    if( !delay )
	notify_dp83820_client( shadow->client );

    // Clean-up the skb.
    ASSERT( !skb_shinfo(skb)->frag_list );
    skb_shinfo(skb)->nr_frags = 0;
    skb->data = skb->head;
    skb->tail = skb->data;
    skb->h.raw = skb->data;
    skb->len = 0;
    skb->data_len = 0;

    dprintk(0, PREFIX "skb destructor done\n");
}

static struct sk_buff *
L4VMnet_skb_wrap_dp83820_packet( L4VMnet_client_info_t *client )
{
    // TODO: validate that buffers + fragments together create valid
    // skb buffs.  Lots of overlapping state must be validated.
    IVMnet_dp83820_descriptor_t *desc, *frag_desc;
    struct sk_buff *skb;
    L4VMnet_skb_dp83820_shadow_t *skb_shadow;
    int fragment;

    // Grab the first packet.
    ASSERT( client->client_shared->dp83820_regs[TXDP] );
    desc = get_dp83820_current_desc( client );
    if( !desc->cmd_status.tx.own )
	return NULL;
    if( unlikely(desc->cmd_status.tx.ok) )
    {
	dprintk(1,  PREFIX "send wrap-around.\n" );
	return NULL;
    }

    skb = alloc_skb( sizeof(L4VMnet_skb_dp83820_shadow_t), GFP_ATOMIC );
    if( unlikely(skb == NULL) )
	return NULL;

    skb_shadow = (L4VMnet_skb_dp83820_shadow_t *)skb->head;
    skb_shadow->client = client;
    skb_shadow->link = client->client_shared->dp83820_regs[TXDP];
    skb_shadow->skb = skb;

    // Move to the next packet.
    client->client_shared->dp83820_regs[TXDP]= desc->link;

    // TODO: validate that the packet lives in the client's address space.
    ASSERT( desc->buffer && desc->buffer_len );
    skb->data = bus_to_virt(desc->buffer + client->client_space->bus_start);
    skb->len  = desc->buffer_len;
    skb->tail = skb->data + skb->len;

    skb->destructor = L4VMnet_skb_dp83820_destructor;
    skb->ip_summed  = CHECKSUM_NONE;
    skb->pkt_type   = PACKET_OUTGOING;

    // Where the device must start the checksum.
    skb->h.raw = 34 + skb->data;
    // Where the device must store the checksum.
    if( desc->extended_status.udp_pkt ) {
	skb->csum = 40 + skb->data - skb->h.raw;
	skb->ip_summed = CHECKSUM_HW;
    }
    else if( desc->extended_status.tcp_pkt ) {
	skb->csum = 50 + skb->data - skb->h.raw;
	skb->ip_summed = CHECKSUM_HW;
    }
    else if( desc->extended_status.ip_pkt )
	L4_KDB_Enter("oops, ip csum not configured");

    dprintk(2, PREFIX "skb protocol %x, pkt_type %x, "
	    "data %p (bus %p/%p)\n",
	    skb->protocol, skb->pkt_type, skb->data, (void *)desc->buffer,
	    (void *)virt_to_bus(skb->data) );

    desc->cmd_status.tx.ok = 1; // Remember that we have submitted this packet.

    fragment = 0;
    frag_desc = desc;
    while( frag_desc->cmd_status.tx.more )
    {
	L4_Word_t paddr;
	IVMnet_dp83820_descriptor_t *last = frag_desc;

	if( unlikely(fragment >= MAX_SKB_FRAGS) ) {
	    last->cmd_status.tx.more = 0;
	    break;
	}
	ASSERT( client->client_shared->dp83820_regs[TXDP] );
	frag_desc = get_dp83820_current_desc( client );
	if( unlikely(!frag_desc->cmd_status.tx.own) ) {
	    last->cmd_status.tx.more = 0;
	    break;
	}
	frag_desc->cmd_status.tx.ok = 1;
	client->client_shared->dp83820_regs[TXDP] = frag_desc->link;

	ASSERT( frag_desc->buffer && frag_desc->buffer_len );
	skb_shinfo(skb)->frags[fragment].size = frag_desc->buffer_len;
	skb_shinfo(skb)->frags[fragment].page_offset =
	    (unsigned long)frag_desc->buffer & (PAGE_SIZE-1);
	paddr = l4ka_wedge_bus_to_phys(frag_desc->buffer + client->client_space->bus_start);
	skb_shinfo(skb)->frags[fragment].page = mem_map + (paddr >> PAGE_SHIFT);
	skb->len += frag_desc->buffer_len;
	skb->data_len += frag_desc->buffer_len;
	dprintk(2, PREFIX "frag %d @ %x + %x, %u\n",
		fragment,
		(L4_Word_t)skb_shinfo(skb)->frags[fragment].page,
		(L4_Word_t)skb_shinfo(skb)->frags[fragment].page_offset,
		skb_shinfo(skb)->frags[fragment].size );
	ASSERT( page_to_bus(skb_shinfo(skb)->frags[fragment].page) == ((frag_desc->buffer + client->client_space->bus_start) & PAGE_MASK) );

	fragment++;
    }
    skb_shinfo(skb)->nr_frags = fragment;

    ASSERT( skb_headlen(skb) == desc->buffer_len );
    if( fragment )
	dprintk(2, PREFIX "fragments %d, tot len %d, fragment len %d\n",
		fragment, skb->len, skb->data_len );

    return skb;
}


static int L4VMnet_dev_queue_dp83820_pkt( L4VMnet_client_info_t *client )
{
    struct sk_buff *skb;

    skb = L4VMnet_skb_wrap_dp83820_packet( client );
    if( skb == NULL ) {
	set_bit( 9 /* TXIDLE */, (volatile unsigned long *)&client->client_shared->dp83820_regs[ISR] );
//	notify_dp83820_client( client );
	return FALSE;
    }

    // TODO: if the skb exceeds the device's queue length, it will call
    // kfree_skb(skb) at interrupt time, which isn't compatible with
    // desctructors.  And it will drop our packets.
    ASSERT( client->real_dev );
    skb->dev = client->real_dev;
    dev_queue_xmit( skb );	// Can be called from an interrupt.
    atomic_inc( &dirty_tx_pkt_cnt );

    return TRUE;
}

static void
L4VMnet_dp83820_tx_pkts( void )
{
    L4VMnet_client_info_t *client;
    L4_Word_t pkts_sent, total_pkts;

    ASSERT( L4_MyLocalId().raw == get_vcpu()->main_ltid.raw );
    total_pkts = 0;
    do {
	pkts_sent = 0;
	for( client = L4VMnet_server.client_list; client; client = client->next)
	{
	    if( !client->operating )
		continue;

	    if( L4VMnet_dev_queue_dp83820_pkt(client) )
		pkts_sent++;
	}

	total_pkts += pkts_sent;
    } while( pkts_sent );

    dprintk(2, PREFIX "%u packets transmitted\n", total_pkts );

}

/***************************************************************************
 *
 * Server handlers for executing in top halves and bottom halves.
 *
 ***************************************************************************/


static void L4VMnet_attach_handler( L4VM_server_cmd_t *cmd, void *data )
{
    IVMnet_handle_t handle;
    idl4_server_environment ipc_env;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;
    L4VMnet_client_info_t *client;
    L4VM_client_space_info_t *client_space;
    idl4_fpage_t idl4_fp, idl4_fp2;
    L4VM_alligned_alloc_t alloc_info;
    int log2size;
    unsigned long flags;

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    // Prepare the reply.
    ipc_env._action = 0;

    // Derive the client's handle from its LAN address.
    handle = lanaddress_get_handle(
	    (lanaddress_t *)params->attach.lan_address );

    dprintk(0, PREFIX "attach request from handle %d tid %t, lan address ",
	    handle, cmd->reply_to_tid);
    L4VMnet_print_lan_address( params->attach.lan_address );
    dprintk(0, ", for device %s\n", params->attach.dev_name );

    // Get access to the client's pages.  This is a HACK.
    client_space = L4VM_get_client_space_info( cmd->reply_to_tid );
    if( client_space == NULL )
	goto err_client_space;

    // Allocate memory for the datastructures to be mapped into the client.
    log2size = L4_SizeLog2( L4_Fpage(0,sizeof(IVMnet_client_shared_t)) );
    if( L4VM_fpage_alloc(log2size, &alloc_info) < 0 )
    {
	CORBA_exception_set( &ipc_env, ex_IVMnet_no_memory, NULL );
	goto err_shared_region_alloc;
    }
    dprintk(2,  "client shared region at 0x%p, size %u, "
	    "requested size %d\n", (void *)L4_Address(alloc_info.fpage),
	    L4_Size(alloc_info.fpage), 1 << log2size );

    // Allocate a client structure.
    client = kmalloc( sizeof(L4VMnet_client_info_t), GFP_KERNEL );
    if( client == NULL )
    {
	CORBA_exception_set( &ipc_env, ex_IVMnet_no_memory, NULL );
	goto err_client_info_alloc;
    }

    // Initialize the client structure.
    client->ivmnet_handle = handle;
    memcpy( client->lan_address, params->attach.lan_address, ETH_ALEN );
    client->shared_alloc_info = alloc_info;
    client->client_space = client_space;
    memset( &client->rcv_ring, 0, sizeof(L4VMnet_skb_ring_t) );
    client->rcv_ring.cnt = L4VMNET_SKB_RING_LEN;

    // Initialize the pointers to the shared region.
    client->client_shared = (IVMnet_client_shared_t *)
	L4_Address(client->shared_alloc_info.fpage);
    // Page-in the client shared region (zero all the pages)
    memset( client->client_shared, 0, L4_Size(client->shared_alloc_info.fpage) );
    client->send_ring.desc_ring = client->client_shared->snd_desc_ring;
    client->send_ring.start_free = 0;
    client->send_ring.start_dirty = 0;
    client->send_ring.cnt = IVMnet_snd_descriptor_ring_cnt;

    client->client_shared->server_irq_no = L4VMnet_server_irq;
    client->client_shared->server_irq_tid = L4VMnet_server.my_irq_tid;
    client->client_shared->server_main_tid = L4VMnet_server.my_main_tid;

    client->operating = FALSE;

    strcpy( client->real_dev_name, params->attach.dev_name );
    client->real_dev = dev_get_by_name( client->real_dev_name );
    if( client->real_dev ) {
	// Enable bridging on the device.  This is never disabled,
	// because we don't maintain reference counts to the real devices.
	client->real_dev->br_port = (struct net_bridge_port *)&L4VMnet_server;
    }

    // Enable the client.
    if( !L4VMnet_client_handle_set(handle, client) )
    {
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_client, NULL );
	goto err_client_handle;
    }

    client->next = L4VMnet_server.client_list;
    L4VMnet_server.client_list = client;

    // Reply to the client.
    idl4_fpage_set_base( &idl4_fp, 0 );
    idl4_fpage_set_mode( &idl4_fp, IDL4_MODE_MAP );
    idl4_fpage_set_page( &idl4_fp, client->shared_alloc_info.fpage );
    idl4_fpage_set_permissions( &idl4_fp, IDL4_PERM_WRITE | IDL4_PERM_READ );

    idl4_fpage_set_base( &idl4_fp2, L4_Size(client->shared_alloc_info.fpage) );
    idl4_fpage_set_mode( &idl4_fp2, IDL4_MODE_MAP );
    idl4_fpage_set_page( &idl4_fp2, L4VMnet_server.status_alloc_info.fpage );
    idl4_fpage_set_permissions( &idl4_fp2, IDL4_PERM_WRITE | IDL4_PERM_READ );

    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    
    IVMnet_Control_attach_propagate_reply( cmd->reply_to_tid,
	    &handle, &idl4_fp, &idl4_fp2, &ipc_env );
    local_irq_restore(flags);
    // TODO: check for IPC error, and free resources if necessary.
    
    return;

err_client_handle:
    kfree( client );
err_client_info_alloc:
    L4VM_fpage_dealloc( &alloc_info );
err_shared_region_alloc:
err_client_space:
    printk( KERN_ERR PREFIX "error attaching client.\n" );
    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    IVMnet_Control_attach_propagate_reply( cmd->reply_to_tid,
	    NULL, NULL, NULL, &ipc_env );
    local_irq_restore(flags);
    return;
}

static void L4VMnet_reattach_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    unsigned long flags;
    L4VMnet_client_info_t *client;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;

    dprintk(1, PREFIX "reattach request from 0x%x\n",
	    cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    client = L4VMnet_client_handle_lookup( params->reattach.handle );
    if( client == NULL )
    {
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
	local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
	L4_Set_VirtualSender( L4VMnet_server.server_tid );
	IVMnet_Control_reattach_propagate_reply( cmd->reply_to_tid,
		NULL, NULL, &ipc_env );
	local_irq_restore(flags);
    }
    else
    {
	idl4_fpage_t idl4_fp, idl4_fp2;

	ipc_env._action = 0;

	L4VM_page_in( client->shared_alloc_info.fpage, PAGE_SIZE );
	L4VM_page_in( L4VMnet_server.status_alloc_info.fpage, PAGE_SIZE );

	idl4_fpage_set_base( &idl4_fp, 0 );
	idl4_fpage_set_mode( &idl4_fp, IDL4_MODE_MAP );
	idl4_fpage_set_page( &idl4_fp, client->shared_alloc_info.fpage );
	idl4_fpage_set_permissions( &idl4_fp, 
		IDL4_PERM_WRITE | IDL4_PERM_READ );

	idl4_fpage_set_base( &idl4_fp2, 
		L4_Size(client->shared_alloc_info.fpage) );
	idl4_fpage_set_mode( &idl4_fp2, IDL4_MODE_MAP );
	idl4_fpage_set_page( &idl4_fp2, 
		L4VMnet_server.status_alloc_info.fpage );
	idl4_fpage_set_permissions( &idl4_fp2, 
		IDL4_PERM_WRITE | IDL4_PERM_READ );

	local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
	L4_Set_VirtualSender( L4VMnet_server.server_tid );
	IVMnet_Control_reattach_propagate_reply( cmd->reply_to_tid,
		&idl4_fp, &idl4_fp2, &ipc_env );
	local_irq_restore(flags);
    }
}

static void L4VMnet_detach_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    unsigned long flags;
    L4VMnet_client_info_t *client;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;
    ipc_env._action = 0;

    dprintk(1, PREFIX "detach request from 0x%x\n",
	    cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    client = L4VMnet_client_handle_lookup( params->start.handle );
    if( client == NULL )
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
    else
    {
	ipc_env._action = 0;
	client->operating = FALSE;
	if( client->real_dev ) {
	    dev_put( client->real_dev );
	    client->real_dev = NULL;
	}
    }

    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    IVMnet_Control_detach_propagate_reply( cmd->reply_to_tid, &ipc_env );
    local_irq_restore(flags);
}

static void L4VMnet_start_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    L4VMnet_client_info_t *client;
    unsigned long flags;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;

    dprintk(1, PREFIX "start request from 0x%x\n", cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    client = L4VMnet_client_handle_lookup( params->start.handle );
    if( client == NULL )
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
    else
    {
	ipc_env._action = 0;
	if( client->real_dev ) {
	    client->operating = TRUE;

	    rtnl_lock();
	    dev_change_flags( client->real_dev, 
		    IFF_UP | IFF_BROADCAST | IFF_MULTICAST | IFF_PROMISC );
	    rtnl_unlock();
	}
    }

    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    IVMnet_Control_start_propagate_reply( cmd->reply_to_tid, &ipc_env );
    local_irq_restore(flags);
}

static void L4VMnet_stop_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    L4VMnet_client_info_t *client;
    unsigned long flags;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;

    dprintk(1, PREFIX "stop request from 0x%x\n",
	    cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    client = L4VMnet_client_handle_lookup( params->start.handle );
    if( client == NULL )
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
    else
    {
	ipc_env._action = 0;
	client->operating = FALSE;
	// TODO: wait for client operations to quiesce.
    }

    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    IVMnet_Control_stop_propagate_reply( cmd->reply_to_tid, &ipc_env );
    local_irq_restore(flags);
}

static void L4VMnet_update_stats_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    L4VMnet_client_info_t *client;
    unsigned long flags;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;

    dprintk(2, PREFIX "update stats request from 0x%x\n",
	    cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );
    ASSERT( sizeof( IVMnet_stats_t ) == sizeof( struct net_device_stats ) );

    client = L4VMnet_client_handle_lookup( params->start.handle );
    if( client == NULL )
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
    else {
	struct net_device *netdev;

	//stg: reporting physical instead of virtual device stats
	ipc_env._action = 0;
	netdev = dev_get_by_name( "eth0" );
	memcpy( &L4VMnet_server.server_status->stats, netdev->get_stats( netdev ), sizeof( struct net_device_stats ) );
	dev_put( netdev );
    }

    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    IVMnet_Control_update_stats_propagate_reply( cmd->reply_to_tid, &ipc_env );
    local_irq_restore(flags);
}

static void L4VMnet_register_dp83820_tx_ring_handler( L4VM_server_cmd_t *cmd, void *data )
{
    idl4_server_environment ipc_env;
    L4VMnet_client_info_t *client;
    unsigned long flags;
    L4VM_server_cmd_params_t *params = (L4VM_server_cmd_params_t *)cmd->data;

    dprintk(2, PREFIX "register dp83820 tx ring from 0x%x\n",
	    cmd->reply_to_tid.raw );

    ASSERT( cmd->reply_to_tid.raw );
    ASSERT( !L4_IsNilThread(L4VMnet_server.server_tid) );

    client = L4VMnet_client_handle_lookup( params->register_dp83820_tx_ring.handle );
    if( client == NULL )
	CORBA_exception_set( &ipc_env, ex_IVMnet_invalid_handle, NULL );
    else 
    {
	// Extract the client parameters.
	L4_Word_t client_paddr = 
	    params->register_dp83820_tx_ring.client_paddr;
	L4_Word_t client_size = 
	    params->register_dp83820_tx_ring.ring_size_bytes;
	L4_Word_t has_extended_status = 
	    params->register_dp83820_tx_ring.has_extended_status;
	L4_Fpage_t fp_req = L4_Fpage( client_paddr, client_size );
	CORBA_Environment req_env = idl4_default_environment;
	idl4_fpage_t fp;
	int err;

	ipc_env._action = 0;

	if( !has_extended_status ) {
	    printk( KERN_ERR PREFIX "The client must use the dp83820 extended status.  Fatal error.\n" );
	    CORBA_exception_set( &ipc_env, ex_IVMnet_no_memory, NULL );
	    goto out;
	}

	// Request a virtual address region from Linux.
	err = L4VM_fpage_vmarea_get( L4_SizeLog2(fp_req), 
		&client->dp83820_tx.vmarea );
	if( err < 0 ) {
	    CORBA_exception_set( &ipc_env, ex_IVMnet_no_memory, NULL );
	    goto out;
	}

	// Request the client pages from the resourcemon.
	local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
	idl4_set_rcv_window( &req_env, client->dp83820_tx.vmarea.fpage );
	IResourcemon_request_client_pages( resourcemon_shared.resourcemon_tid,
		&cmd->reply_to_tid, fp_req.raw, &fp, &req_env );
	local_irq_restore(flags);

	if( req_env._major != CORBA_NO_EXCEPTION ) {
	    CORBA_exception_free( &req_env );
	    CORBA_exception_set( &ipc_env, ex_IVMnet_no_memory, NULL );
	    goto out;
	}

	// Point at our local address for the descriptor ring.
	client->dp83820_tx.first = client_paddr;
	client->dp83820_tx.ring_start =
	    (L4_Address(client->dp83820_tx.vmarea.fpage) +
	     ((L4_Size(client->dp83820_tx.vmarea.fpage)-1) & client_paddr) );
  	dprintk(1, PREFIX "dp83820 tx ring, client paddr %08x, DD/OS %x\n",
	  	 (unsigned int) client_paddr, (unsigned int) client->dp83820_tx.ring_start);

    }

out:
    local_irq_save(flags); ASSERT( !vcpu_interrupts_enabled() );
    L4_Set_VirtualSender( L4VMnet_server.server_tid );
    IVMnet_Control_register_dp83820_tx_ring_propagate_reply( cmd->reply_to_tid, &ipc_env );
    local_irq_restore(flags);
}



/***************************************************************************
 *
 * Server thread.
 *
 ***************************************************************************/
IDL4_INLINE void IVMnet_Control_attach_implementation(
	CORBA_Object _caller,
	const char *dev_name,
	const char *lan_address,
	IVMnet_handle_t *handle,
	idl4_fpage_t *shared_window,
	idl4_fpage_t *server_status,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_bottom();
    cmd = &server->bottom_half_cmds.cmds[ cmd_idx ];
    params = &server->bottom_half_params[ cmd_idx ];
    cmd->data = params;

    cmd->reply_to_tid = _caller;
    memcpy( params->attach.dev_name, dev_name, IFNAMSIZ );
    params->attach.dev_name[IFNAMSIZ-1] = '\0';
    memcpy( params->attach.lan_address, lan_address, ETH_ALEN );
    cmd->handler = L4VMnet_attach_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_BOTTOM_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_ATTACH(IVMnet_Control_attach_implementation);

IDL4_INLINE void IVMnet_Control_reattach_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_fpage_t *shared_window,
	idl4_fpage_t *server_status,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_bottom();
    cmd = &server->bottom_half_cmds.cmds[ cmd_idx ];
    params = &server->bottom_half_params[ cmd_idx ];
    cmd->data = params;
    
    cmd->reply_to_tid = _caller;
    params->reattach.handle = handle;
    cmd->handler = L4VMnet_reattach_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_BOTTOM_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_REATTACH(IVMnet_Control_reattach_implementation);


IDL4_INLINE void IVMnet_Control_detach_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_bottom();
    cmd = &server->bottom_half_cmds.cmds[ cmd_idx ];
    params = &server->bottom_half_params[ cmd_idx ];
    cmd->data = params;
    
    cmd->reply_to_tid = _caller;
    params->detach.handle = handle;
    cmd->handler = L4VMnet_detach_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_BOTTOM_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_DETACH(IVMnet_Control_detach_implementation);


IDL4_INLINE void IVMnet_Control_start_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_top();
    cmd = &server->top_half_cmds.cmds[ cmd_idx ];
    params = &server->top_half_params[ cmd_idx ];
    cmd->data = params;

    cmd->reply_to_tid = _caller;
    params->start.handle = handle;
    cmd->handler = L4VMnet_start_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_TOP_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_START(IVMnet_Control_start_implementation);


IDL4_INLINE void IVMnet_Control_stop_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_top();
    cmd = &server->top_half_cmds.cmds[ cmd_idx ];
    params = &server->top_half_params[ cmd_idx ];
    cmd->data = params;
    
    cmd->reply_to_tid = _caller;
    params->stop.handle = handle;
    cmd->handler = L4VMnet_stop_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_TOP_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_STOP(IVMnet_Control_stop_implementation);


IDL4_INLINE void IVMnet_Control_update_stats_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_top();
    cmd = &server->top_half_cmds.cmds[ cmd_idx ];
    params = &server->top_half_params[ cmd_idx ];
    cmd->data = params;
    
    cmd->reply_to_tid = _caller;
    params->update_stats.handle = handle;
    cmd->handler = L4VMnet_update_stats_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_TOP_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_UPDATE_STATS(IVMnet_Control_update_stats_implementation);


IDL4_INLINE void IVMnet_Control_run_dispatcher_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	idl4_server_environment *_env)
{
    L4VMnet_deliver_irq( L4VMNET_IRQ_DISPATCH );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_RUN_DISPATCHER(IVMnet_Control_run_dispatcher_implementation);



IDL4_INLINE void IVMnet_Control_register_dp83820_tx_ring_implementation(
	CORBA_Object _caller,
	const IVMnet_handle_t handle,
	const L4_Word_t client_paddr,
	const L4_Word_t ring_size_bytes,
	const L4_Word_t has_extended_status,
	idl4_server_environment *_env)
{
    L4VMnet_server_t *server = &L4VMnet_server;
    L4VM_server_cmd_t *cmd;
    L4VM_server_cmd_params_t *params;
    L4_Word16_t cmd_idx;

    cmd_idx = L4VMnet_cmd_allocate_bottom();
    cmd = &server->bottom_half_cmds.cmds[ cmd_idx ];
    params = &server->bottom_half_params[ cmd_idx ];
    cmd->data = params;
    cmd->reply_to_tid = _caller;
    params->register_dp83820_tx_ring.handle = handle;
    params->register_dp83820_tx_ring.client_paddr = client_paddr;
    params->register_dp83820_tx_ring.ring_size_bytes = ring_size_bytes;
    params->register_dp83820_tx_ring.has_extended_status = has_extended_status;
    cmd->handler = L4VMnet_register_dp83820_tx_ring_handler;
    L4VMnet_deliver_irq( L4VMNET_IRQ_BOTTOM_HALF_CMD );

    idl4_set_no_response( _env );
}
IDL4_PUBLISH_ATTR
IDL4_PUBLISH_IVMNET_CONTROL_REGISTER_DP83820_TX_RING(IVMnet_Control_register_dp83820_tx_ring_implementation);


void L4VMnet_server_thread( void *param )
{
    L4_ThreadId_t partner;
    L4_MsgTag_t msgtag;
    idl4_msgbuf_t msgbuf;
    long cnt;
    char dev_name[IFNAMSIZ];
    char addr_buf[ETH_ALEN];
    static void *IVMnet_Control_vtable[IVMNET_CONTROL_DEFAULT_VTABLE_SIZE] = IVMNET_CONTROL_DEFAULT_VTABLE;

    idl4_msgbuf_init(&msgbuf);
    idl4_msgbuf_add_buffer( &msgbuf, dev_name, sizeof(dev_name) );
    idl4_msgbuf_add_buffer( &msgbuf, addr_buf, sizeof(addr_buf) );

    while( 1 )
    {
	partner = L4_nilthread;
	msgtag.raw = 0;
	cnt = 0;

	while( 1 )
	{
	    idl4_msgbuf_sync(&msgbuf);
	    idl4_reply_and_wait(&partner, &msgtag, &msgbuf, &cnt);
	    if (idl4_is_error(&msgtag))
		break;

	    dprintk(0, PREFIX "server thread request from %x\n", partner.raw);
	    idl4_process_request(&partner, &msgtag, &msgbuf, &cnt,
		    IVMnet_Control_vtable[
		      idl4_get_function_id(&msgtag) & IVMNET_CONTROL_FID_MASK]);
	}
    }
}

void IVMnet_Control_discard(void)
{
    // Invoked in response to invalid IPC messages.
}

/***************************************************************************
 *
 * IRQ and bottom half handling.
 *
 ***************************************************************************/

static void
L4VMnet_bottom_half_handler( void * param )
{
    L4VMnet_server_t *server = &L4VMnet_server;
    dprintk(1, PREFIX "bottom half handler, in interrupt: %u\n",  in_interrupt() );
    L4VM_server_cmd_dispatcher( &L4VMnet_server.bottom_half_cmds, server, server->server_tid );
}

static irqreturn_t
L4VMnet_irq_handler( int irq, void *data, struct pt_regs *regs )
{
    while( 1 )
    {
	// Outstanding events?  Read them and reset without losing events.
	unsigned events, client_events;

	events = irq_status_reset(&L4VMnet_server.irq_status);
	client_events = irq_status_reset((volatile unsigned *)&L4VMnet_server.server_status->irq_status);
	
	if( !events && !client_events )
	    return IRQ_HANDLED;
	
	dprintk(0, PREFIX "irq handler: 0x%x, 0x%x\n", events, client_events );

	// No one needs to deliver IRQ messages.  They would be superfluous.
	L4VMnet_server.irq_pending = TRUE;
	L4VMnet_server.server_status->irq_pending = TRUE;

#if !defined(CONFIG_AFTERBURN_DRIVERS_NET_DP83820)
	if( (events & L4VMNET_IRQ_DISPATCH) || client_events )
	{
	    // TODO: can we do this at IRQ time?
	    L4VMnet_transmit_client_pkts();
	}
#else
	if( (events & L4VMNET_IRQ_DP83820_TX) || client_events )
	{
	    L4VMnet_dp83820_tx_pkts();
	}
#endif

	if( (events & L4VMNET_IRQ_TOP_HALF_CMD) != 0 )
	    L4VM_server_cmd_dispatcher( &L4VMnet_server.top_half_cmds, &L4VMnet_server,  L4VMnet_server.server_tid );

	if( (events & L4VMNET_IRQ_BOTTOM_HALF_CMD) != 0 )
	{
	    // Make sure that we tackle bottom halves outside the
	    // interrupt context!!
	    schedule_task( &L4VMnet_server.bottom_half_task );
	}

	// Enable interrupt message delivery.
	L4VMnet_server.irq_pending = FALSE;
	L4VMnet_server.server_status->irq_pending = FALSE;
    }
}

#if 0

static int
L4VMnet_irq_pending( void *data )
{
    return (L4VMnet_server.irq_status & L4VMnet_server.irq_mask) ||
	   L4VMnet_server.server_status->irq_status;
}

#endif
/***************************************************************************
 *
 * Packet send functions.
 *
 ***************************************************************************/

static int L4VMnet_xmit_packets_to_client_thread(
	L4VMnet_client_info_t *client,
	int receiver )
{
    L4VMnet_skb_ring_t *ring = &client->rcv_ring;
    L4_Word_t idx;
    L4_Word_t string_items = 0;
    L4_MsgTag_t tag;
    L4_StringItem_t str_item;
    L4_ThreadId_t receiver_tid;
    unsigned long flags;
    L4_Word_t len, transferred_bytes = 0;
    L4_Word_t cnt=0;

    dprintk(0, PREFIX "transmit incoming packets to client %p\n", client);
    
    if( receiver >= client->client_shared->receiver_cnt )
    {
	dprintk(2, PREFIX "inbound packets for unavailable client\n" );
	return FALSE;
    }
    receiver_tid = client->client_shared->receiver_tids[ receiver ];

    local_irq_save(flags); // Protect our message registers.
    ASSERT( !vcpu_interrupts_enabled() );

    // Install string items for the message.
    idx = ring->start_dirty;
    while( string_items < IVMnet_rcv_buffer_cnt )
    {
	struct sk_buff *skb = ring->skb_ring[ idx ];
	if( skb == NULL )
	    break;	// No more packets.

	if( string_items > 0 )
	{
	    // Store the string item from the last iteration.  We delay
	    // by one iteration to update the C bit.
	    str_item.X.C = 1;
	    L4_LoadMR( 2*string_items-1, str_item.raw[0] );
	    L4_LoadMR( 2*string_items,   str_item.raw[1] );
	}

	len = skb_headlen(skb);
	str_item = L4_StringItem( len, skb->data );
	transferred_bytes += len;

	string_items++;
	idx = (idx + 1) % ring->cnt;
	if( idx == ring->start_dirty )
	    break;	// Wrap-around.
    }
    dprintk(2, PREFIX "%u inbound packets, %u bytes to transfer, tid %t.\n", 
	    string_items, transferred_bytes, receiver_tid );

    if( string_items == 0 )
    {
	local_irq_restore(flags);
	return FALSE;
    }

    // Write the last string item.
    L4_LoadMR( 2*string_items-1, str_item.raw[0] );
    L4_LoadMR( 2*string_items,   str_item.raw[1] );

    // Initialize the IPC message tag.
    tag.raw = 0;
    tag.X.t = 2*string_items;
    L4_Set_MsgTag( tag );
    client->client_shared->receiver_tids[receiver] = L4_nilthread;

    // Deliver the IPC.
    L4_Set_XferTimeouts( L4_Timeouts(L4_Never, L4_Never) );
    tag = l4ka_wedge_notify_thread( receiver_tid, L4_ZeroTime);
    
    if( unlikely(L4_IpcFailed(tag)) )
    {
	L4_Word_t err = L4_ErrorCode();
	
	if( ((err >> 1) & 7) <= 3 ) 
	{
	    // Send-phase error.
	    dprintk(2, PREFIX "send-phase error %x %x.\n", (unsigned int) receiver_tid.raw, err);
	    client->client_shared->receiver_tids[receiver] = receiver_tid;
	    transferred_bytes = 0;
	}
	else 
	{
	    // Message overflow.
	    dprintk(2, PREFIX "receive-phase error or msg overflow %x.\n", (unsigned int) receiver_tid.raw );
	    transferred_bytes = err >> 4;
	}
    }

    dprintk(2, PREFIX "%u bytes transferred to client.\n", transferred_bytes );

    // Commit the mappings.  We may have had partial transfer.  We resend
    // any skbuffs that had partial transfer.
    for( idx = 0; (idx < string_items) && transferred_bytes; idx++ )
    {
	struct sk_buff *skb = ring->skb_ring[ ring->start_dirty ];
	len = skb_headlen(skb);
	if( transferred_bytes >= len ) {
	    cnt++;
	    transferred_bytes -= len;
	    dev_kfree_skb_any( ring->skb_ring[ ring->start_dirty ] );
	    ring->skb_ring[ ring->start_dirty ] = NULL;
	    ring->start_dirty = (ring->start_dirty + 1) % ring->cnt;
	}
	else 
	    transferred_bytes = 0;
    }

    local_irq_restore(flags);

    return TRUE;
}

static void L4VMnet_xmit_packets_to_client( L4VMnet_client_info_t *client )
{
#if !defined(CONFIG_AFTERBURN_DRIVERS_NET_DP83820)
    int receiver = 0;

    while( (receiver < client->client_shared->receiver_cnt) &&
	    (receiver < IVMnet_max_receiver_cnt) )
    {
	if( L4_IsNilThread( client->client_shared->receiver_tids[receiver] ) )
	{
	    receiver++;
	    continue;
	}
	if( !L4VMnet_xmit_packets_to_client_thread(client, receiver) )
	    return;
	receiver++;
    }
    dprintk(2, PREFIX "wow, not enough client receiver threads!\n" );
    
#else
    if( !L4_IsNilThread( client->client_shared->receiver_tids[0] ) )
	L4VMnet_xmit_packets_to_client_thread( client, 0 );
    else if (client)
	dprintk(2, PREFIX "client receiver thread is nilthread, wait.\n");
	
#endif
}

static void L4VMnet_queue_pkt_to_client(
	L4VMnet_client_info_t *client,
	struct sk_buff *skb )
{
    L4VMnet_skb_ring_t *ring;

    if( !client->operating )
    {
	dev_kfree_skb_any( skb );
	return;
    }

    ring = &client->rcv_ring;
    dprintk(4, PREFIX "queue packet %p to client %p ring %p\n", skb, client, ring);
    
    // Make room on the queue.
    if( ring->skb_ring[ring->start_free] != NULL )
    {
	// Ring is full.  So drop the packet.
	dev_kfree_skb_any( skb );
	tasklet_hi_schedule( &L4VMnet_flush_tasklet );
	dprintk(1, PREFIX "wow, too many packets for client!\n" );
	return;
    }

    ring->skb_ring[ ring->start_free ] = skb;
    ring->start_free = (ring->start_free + 1) % ring->cnt;

    if( L4VMnet_skb_ring_pending(&client->rcv_ring) > 2 || 
	    (atomic_read(&dirty_tx_pkt_cnt) == 0) )
	tasklet_hi_schedule( &L4VMnet_flush_tasklet );
}


static int
L4VMnet_absorb_frame( struct sk_buff *skb )
    // Returns TRUE if the packet has been handled.
    // Returns FALSE if the packet is supposed to be handled by Linux.
{
    L4VMnet_client_info_t *client;
    IVMnet_handle_t handle;
    lanaddress_t *lanaddr = (lanaddress_t *)skb->mac.raw;
    lanaddress_t *linux_lanaddr;

    // Is the packet destined for the local Linux?
    linux_lanaddr = (lanaddress_t *)skb->dev->dev_addr;
    if( (linux_lanaddr->align4.lsb == lanaddr->align4.lsb) &&
	    (linux_lanaddr->align4.msb == lanaddr->align4.msb) )
	return FALSE;
    

    if( unlikely(skb_shinfo(skb)->nr_frags > 0) )
    {
	dprintk(4, PREFIX "fragmented packet, linearize\n");
	skb_linearize(skb, GFP_ATOMIC);
	// We don't yet support fragmentation, so drop the packet.
	//dev_kfree_skb_any( skb );
	//return TRUE;
    }
    
    // Fast map the destination LAN address into a handle, and then lookup
    // the client structure.
    handle = lanaddress_get_handle( lanaddr );
    client = L4VMnet_client_handle_lookup( handle );
    if( client ) {
	L4VMnet_queue_pkt_to_client( client, skb );
	return TRUE;
    }


    for( client = L4VMnet_server.client_list; client; client = client->next )
    {
	struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);
	if( clone )
	    L4VMnet_queue_pkt_to_client( client, clone );
    }

    return FALSE;	// Return broadcasts to Linux too.
}

static void
L4VMnet_flush_tasklet_handler( unsigned long unused )
{
    L4VMnet_client_info_t *client;
    dprintk(2, PREFIX "flush tasklet client list %p (server %p)\n",
	    L4VMnet_server.client_list, &L4VMnet_server);

    for( client = L4VMnet_server.client_list; client; client = client->next )
	if( client->operating )
	    L4VMnet_xmit_packets_to_client( client );
	else
	    dprintk(2, PREFIX "flush tasklet  client %p not operating\n", client);

}


/***************************************************************************
 *
 * Linux Module functions.
 *
 ***************************************************************************/

static int __init
L4VMnet_server_alloc( void )
{
    int err, log2size;

    // Initialize the server.
    L4VMnet_server.server_tid = L4_nilthread;
    L4VMnet_server.irq_status = 0;
    L4VMnet_server.irq_mask = ~0;
    L4VMnet_server.irq_pending = FALSE;
    L4VMnet_server.my_irq_tid = get_vcpu()->irq_gtid;
    L4VMnet_server.my_main_tid = get_vcpu()->main_gtid;


    SET_MODULE_OWNER( L4VMnet_server );

    INIT_TQUEUE( &L4VMnet_server.bottom_half_task,
	    L4VMnet_bottom_half_handler, NULL );

    // Skip server cmd ring init ... already zeroed.

    // Allocate a status page, shared by all clients.
    log2size = L4_SizeLog2( L4_Fpage(0,sizeof(IVMnet_server_shared_t)) );
    err = L4VM_fpage_alloc( log2size, &L4VMnet_server.status_alloc_info );
    if( err < 0 )
	goto err_server_status;
    L4VMnet_server.server_status = (IVMnet_server_shared_t *)
	L4_Address( L4VMnet_server.status_alloc_info.fpage );
    L4VMnet_server.server_status->irq_status = 0;
    L4VMnet_server.server_status->irq_pending = 0;

    // Some IRQ init.
    L4VMnet_server.irq_status = 0;
    L4VMnet_server.irq_mask = ~0;
    L4VMnet_server.irq_pending = FALSE;
    L4VMnet_server.my_irq_tid = get_vcpu()->irq_gtid;
    L4VMnet_server.my_main_tid = get_vcpu()->main_gtid;

    if (L4VMnet_server_irq == 0)
    {
#if defined(CONFIG_X86_IO_APIC)
        L4_KernelInterfacePage_t *kip = (L4_KernelInterfacePage_t *) L4_GetKernelInterface();
        L4VMnet_server_irq = L4_ThreadIdSystemBase(kip) + 3;	
	acpi_register_gsi(L4VMnet_server_irq, ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_LOW);
#else
	L4VMnet_server_irq = 9;
#endif
    }
    printk(PREFIX "L4VMnet server irq %d\n", L4VMnet_server_irq );

    // TODO: in case of error, we don't want the irq to remain virtual.
    l4ka_wedge_add_virtual_irq( L4VMnet_server_irq );

    err = request_irq( L4VMnet_server_irq, L4VMnet_irq_handler, 0,
	    "l4ka_net_server", &L4VMnet_server );
    if( err < 0 )
    {
	    printk( KERN_ERR PREFIX "unable to allocate virtual interrupt %d, err: %d\n", 
		    L4VMnet_server_irq, err);
	goto err_request_irq;
    }

    // Start the server loop.
    L4VMnet_server.server_tid = L4VM_thread_create( GFP_KERNEL, 
	    L4VMnet_server_thread, l4ka_wedge_get_irq_prio(), 
	    smp_processor_id(), NULL, 0 );
    if( L4_IsNilThread(L4VMnet_server.server_tid) )
    {
	printk( KERN_ERR PREFIX "failed to start the server thread.\n" );
	err = -ENOMEM;
	goto err_thread_start;
    }

    return 0;

err_thread_start:
    free_irq( L4VMnet_server_irq, &L4VMnet_server );
err_request_irq:
    L4VM_fpage_dealloc( &L4VMnet_server.status_alloc_info );
err_server_status:
    return -ENODEV;
}

static int __init
L4VMnet_server_init_module( void )
{
    int result;

    if( br_handle_frame_hook ) {
	printk( KERN_ERR "Error: The bridging module is already loaded!  This\n"
		"module is incompatible with the bridging module.\n" );
	return -ENODEV;
    }

    br_handle_frame_hook = L4VMnet_bridge_handler;
    L4VMnet_client_list_init();

    // TODO: register device notifiers!!

    result = L4VMnet_server_alloc();
    if( result )
	return result;

    // Register with the locator.
    L4VM_server_register_location( UUID_IVMnet, L4VMnet_server.server_tid );

    printk(PREFIX "L4VMnet server initialized.\n" );

    return 0;
}

static void __exit
L4VMnet_server_exit_module( void )
{
    if( !L4_IsNilThread(L4VMnet_server.server_tid) )
	L4VM_thread_delete( L4VMnet_server.server_tid );
    free_irq( L4VMnet_server_irq, &L4VMnet_server );
    L4VM_fpage_dealloc( &L4VMnet_server.status_alloc_info );
    br_handle_frame_hook = NULL;
}

MODULE_AUTHOR( "Joshua LeVasseur <jtl@ira.uka.de>" );
MODULE_DESCRIPTION( "L4 network device driver server stub" );
MODULE_LICENSE( "Dual BSD/GPL" );
MODULE_SUPPORTED_DEVICE( "L4 VM net" );
MODULE_VERSION( "yoda" );

module_init( L4VMnet_server_init_module );
module_exit( L4VMnet_server_exit_module );

#if defined(MODULE)
// Define a .afterburn_wedge_module section, to activate symbol resolution
// for this module against the wedge's symbols.
__attribute__ ((section(".afterburn_wedge_module")))
burn_wedge_header_t burn_wedge_header = {
    BURN_WEDGE_MODULE_INTERFACE_VERSION
};
#endif


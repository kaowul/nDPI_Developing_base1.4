/*
 *	xt_ndpi - Netfilter module to match nDPI-detected sessions
 *
 *	(C) 2013 Luca Deri <deri@ntop.org>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 */

#define pr_fmt( fmt ) KBUILD_MODNAME ": " fmt
#include <linux/version.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/netfilter/x_tables.h>
#include <linux/types.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_ecache.h>
#include <linux/proc_fs.h>
#include "../include/xt_ndpi.h"
#include "../include/xt_ndpi_cb.h"

#include "ndpi.h"
#include "lru.h"

#define MATCH_PASS          0
#define MATCH_BLOCK         1
#define MATCH_DFL_VERDICT   MATCH_PASS

/*
 * #undef NDPI_ENABLE_DEBUG_MESSAGES
 * #define NDPI_ENABLE_DEBUG_MESSAGES 1
 * #define DEBUG 1
 */
/* Enable debug tracings */
const u_int8_t debug = 0;


#define PROC_REMOVE( pde, net ) proc_net_remove( net, dir_name )
#define PDE_ROOT	"xt_ndpi"
#define PDE_PROTO	"proto"
static struct proc_dir_entry *pde, *pde_proto;


/*
 * Enable protocol guess based on ports for those sessions that
 * have not been discovered with DPI
 */
const u_int8_t guess_protocol = 1;

/* prototype define */
static int ndpi_process_packet(const struct sk_buff *_skb,
				 const struct xt_ndpi_protocols *match_info,
                 const struct xt_ndpi_tginfo    *target_info,
                 struct nf_conn *ct);

#define trace_print(...) do { \
    if (unlikely(debug)) { \
        pr_debug(__VA_ARGS__); \
    } \
} while (0)

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
# define debug_print(...)    pr_debug(__VA_ARGS__)
#else
# define debug_print(...)    ((void)0)
#endif

/**
 * hash of address
 * @return LruKey (aka. uint64_t)
 * Ref: http://www.cnblogs.com/napoleon_liu/archive/2010/12/29/1920839.html
 */
static inline LruKey toLruKey(struct nf_conn *ct)
{
    LruKey key = (LruKey)ct;
    key = (key>>3)*2654435761;  /* 2654435761 2^32 */
    /* use the high bits */
    return ((key>>32)|(key<<32));
}

/**
 * Update match and judge verdict
 * @above in iptables
 * @entry:
 * @proto_cmp_result: 0 (protocol matches)
 * @return : 0 is match
 */
static  u_int64_t GET_MATCH_ABOVE(const struct xt_ndpi_protocols *info, struct LruCacheEntryValue *entry , u_int64_t proto_cmp_result){


 	#if 0
	if (proto_cmp_result)
		pr_info("GET_MATCH_ABOVE0: cmp_result (%lld) above(%d) entry->above[%lld](%d)\n", proto_cmp_result, info->match_above, info->pool,entry->above[info->pool]);
	#endif
	if (!proto_cmp_result || info->match_above < 0){
		/* do nothing */
		return proto_cmp_result;
	}else{
		/* custom above( max is 32766)*/
		if (entry->above[info->pool] <= info->match_above && likely(entry->above[info->pool] < 32767 - 1 ))
			entry->above[info->pool]++;	
 		#if 0
		if (proto_cmp_result)
			pr_info("GET_MATCH_ABOVE: cmp_result (%lu) above(%d) entry->above[%d](%d)\n", proto_cmp_result, info->match_above, info->pool,entry->above[info->pool]);
		#endif
		return proto_cmp_result && entry->above[info->pool] <= info->match_above;
	}
}

/* Dump configuration ane restore it later on */
static void  ndpi_print_bitmask( const struct xt_ndpi_protocols *info, char * str )
{
#ifdef NDPI_ENABLE_DEBUG_MESSAGES

	u_int i, flag;
	u_int num_supported_protocols = ndpi_get_num_supported_protocols( ndpi_struct ) + 1 /* NOT_YET protocol */;
	pr_debug( "[NDPI] Protos: %s", str );
	for ( i = 0; i < num_supported_protocols; i++ )
	{
		if ( NDPI_COMPARE_PROTOCOL_TO_BITMASK( info->protocols, i ) != 0 )
		{
			if ( flag == 1 )
			{
				pr_debug( ",%s",
					 (i == NOT_YET_PROTOCOL) ? "NOT_YET" : ndpi_get_proto_by_id( ndpi_struct, i ) );
			}else{
				pr_debug( "--n-protos %s",
					 (i == NOT_YET_PROTOCOL) ? "NOT_YET" : ndpi_get_proto_by_id( ndpi_struct, i ) );
				flag = 1;
			}
		}
	}
#endif
}


/*
 * TODO IPv6 support
 */

char* intoaV4( unsigned int addr, char* buf, u_short bufLen )
{
	char	*cp, *retStr;
	uint	byte;
	int	n;

	cp	= &buf[bufLen];
	*--cp	= '\0';

	n = 4;
	do
	{
		byte	= addr & 0xff;
		*--cp	= byte % 10 + '0';
		byte	/= 10;
		if ( byte > 0 )
		{
			*--cp	= byte % 10 + '0';
			byte	/= 10;
			if ( byte > 0 )
				*--cp = byte + '0';
		}
		*--cp	= '.';
		addr	>>= 8;
	}
	while ( --n > 0 );

	/* Convert the string to lowercase */
	retStr = (char *) (cp + 1);

	return(retStr);
}


/* ********************************************* */

char* protoname( u_int8_t id, char *buf, u_int buf_len )
{
	switch ( id )
	{
	case IPPROTO_TCP: return("TCP");
	case IPPROTO_UDP: return("UDP");
	case IPPROTO_ICMP: return("ICMP");
	default:
		snprintf( buf, buf_len, "%d", id );
		return(buf);
	}
}


/* ********************************************* */

static char* print_lru_ct_entry( struct LruCacheEntryValue *entry, char *buff, u_int buff_len )
{
	char buf0[24], buf1[24], buf2[24];

	snprintf( buff, buff_len,
		  "[%s] %s:%u <-> %s:%u (%u pkts) [Proto: %s]",
		  protoname( entry->proto, buf0, sizeof(buf0) - 1 ),
		  intoaV4( ntohl( entry->src_ip ), buf1, sizeof(buf1) - 1 ), ntohs( entry->sport ),
		  intoaV4( ntohl( entry->dst_ip ), buf2, sizeof(buf2) - 1 ), ntohs( entry->dport ),
		  entry->num_packets_processed,
		  (entry->ndpi_proto == NOT_YET_PROTOCOL)
		  ? "NotYet" : ndpi_get_proto_name( ndpi_struct, entry->ndpi_proto )
		  );
	return(buff);
}


/* ********************************************* */

static void ndpi_flow_end_notify( struct LruCacheEntryValue *entry )
{
	if( unlikely(debug)){
		char buff[256];
		pr_warning( "[NDPI] Exporting dead flow %s\n", print_lru_ct_entry( entry, buff, sizeof(buff) ) );
	}
}


/* ********************************************* */

static int init_entry_with_ct( struct LruCacheEntryValue *entry, struct nf_conn *ct )
{
    int ret = 0;
    debug_print("call %s\n", __FUNCTION__ );
    if(!entry->src)   entry->src  = kmalloc( ndpi_proto_size, GFP_ATOMIC );
    if(!entry->dst)   entry->dst  = kmalloc( ndpi_proto_size, GFP_ATOMIC );
    if(!entry->flow)  entry->flow = kmalloc( ndpi_flow_struct_size, GFP_ATOMIC );

    if (entry->src == NULL || entry->dst == NULL || entry->flow == NULL) {
        ret = -1;
        goto init_entry_alloc_error;
    }

    pr_debug("%s:%d: set protocol_detected = 0\n", __FUNCTION__, __LINE__);
    /* init by declare order */
    entry->num_packets_processed = 0;
    entry->protocol_detected     = 0;
    entry->host_name_checked     = 0;
    entry->ndpi_proto            = NDPI_PROTOCOL_UNKNOWN;
    memset(entry->flow, 0, ndpi_flow_struct_size);
    memset(entry->src,  0, ndpi_proto_size);
    memset(entry->dst,  0, ndpi_proto_size);
    entry->ct = ct;
    entry->last_processed_skb = NULL;
    entry->last_stamp = 0;
    entry->src_ip = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip;
    entry->dst_ip = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3.ip;
    entry->sport  = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.all;
    entry->dport  = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u.all;
    memset(entry->above, 0, sizeof(entry->above));
    entry->proto  = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.l3num;

    return 0;

    /* allocate error */
init_entry_alloc_error:
    pr_err("%s:%d: INNER ERROR return -1\n", __FUNCTION__, __LINE__);
    kfree(entry->src);  entry->src = NULL;
    kfree(entry->dst);  entry->dst = NULL;
    kfree(entry->flow); entry->flow = NULL;
    return ret;
}


/* ********************************************* */

static inline void dumpLruCacheEntryValue( struct LruCacheEntryValue *entry, bool verdict )
{
	if ( unlikely(debug))
	{
		char buf0[24], buf1[24], buf2[24];
		pr_debug( "[NDPI] DUMP [%s][%s][%s:%u <-> %s:%u][%s]\n",
				 (entry->ndpi_proto == NOT_YET_PROTOCOL)
				 ? "NotYet" : ndpi_get_proto_name( ndpi_struct, entry->ndpi_proto ),
				 protoname( entry->proto, buf0, sizeof(buf0) - 1 ),
				 intoaV4( ntohl( entry->src_ip ), buf1, sizeof(buf1) - 1 ), ntohs( entry->sport ),
				 intoaV4( ntohl( entry->dst_ip ), buf2, sizeof(buf2) - 1 ), ntohs( entry->dport ),
				 verdict ? "DROP" : "PASS" );
	}
}


/**
 * process a packet
 * _skb: indicate a packet
 * match_info:  if not NULL, this function is invoked by `ndpi_match()`
 * target_info: if not NULL, this function is invoked by `ndpi_tg()`
 * ct:   indicate a fllow containing the packet
 * @return: MATCH_PASS:  pass the packet
 *          MATCH_BLOCK: block the packet
 * NOTE match_info and target_info are that only one is not NULL
 */
static int ndpi_process_packet(const struct sk_buff *_skb,
				 const struct xt_ndpi_protocols *match_info,
                 const struct xt_ndpi_tginfo    *target_info,
                 struct nf_conn *ct)
{
	LruKey                    key = toLruKey(ct);
	struct LruCacheEntryValue *entry;
	struct LruCacheNode       *node;
	u_int64_t                 time;
	struct timeval			  tv;
	const struct iphdr        *iph;
	u_int16_t                 ip_len;
	u_int8_t                  *ip;
	struct sk_buff            *copied_skb;
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	char buff[256];
#endif
	int verdict = MATCH_DFL_VERDICT;
    struct xt_ndpi_protocols dummy_info;
    struct xt_ndpi_protocols const *info;

    if ((match_info && target_info) || (!match_info && !target_info)) {
        pr_err("%s:%d: INEER ERROR, match_info and target_info are that only one is not NULL",
                __FUNCTION__, __LINE__);
        return MATCH_DFL_VERDICT;
    }
    /* Init the dummy xt_ndpi_protocols info for ndpi_tg function to avoid error */
    if (!match_info && target_info) {
        debug_print("%s:%d: Init dummy_info for ndpi_tg\n", __FUNCTION__, __LINE__);
        memset(&dummy_info, 0, sizeof(dummy_info));
        match_info = &dummy_info;
    }
    info = match_info;

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info( "[NDPI] Begin function ndpi_process_packet(%lu)  nfctinfo:%u\n", (long unsigned int)key, _skb->nfctinfo);
	ndpi_print_bitmask( info, "--------1) START------------" );
#endif

	/*
	 * We need that as two chains (e.g. INPUT and OUTPUT) can receive
	 * packet for the same flow
	 */
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info( "[NDPI] add_to_lru#2\n" );
#endif
	node = add_to_lru_cache( lru_cache, key );
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info("[NDPI] add_to_lru over#2\n" );
#endif

	if (node == NULL) {
		pr_warning("%s:%d: add_to_lru_cache() returned NULL\n", __FUNCTION__, __LINE__);
		return MATCH_DFL_VERDICT;
	}

    entry = &node->node.value;

    /* New entry just created */
	if (entry->ct == NULL) {
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
		pr_info( "[NDPI] New entry just created\n" );
#endif
        /* init the new entry */
		if (init_entry_with_ct(entry, ct) != 0) {
			/*
			 * Not enough memory: we let the LRU polish the cache
			 * without explicitly deleting the entry
			 */
			pr_warning("%s:%d Found NEW flow but NOT ENOUGH MEMORY!\n", __FUNCTION__, __LINE__);
			return MATCH_DFL_VERDICT;
        }
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
        if ((htons( entry->sport ) != 22) && (htons( entry->dport ) != 22))
            pr_info( "[NDPI] Found NEW flow [%s]\n", print_lru_ct_entry( entry, buff, sizeof(buff) ) );
#endif

    /* The existing entry */
	} else {
        /* Looks like netfilter recycles stuff */
		if (
			( (entry->src_ip == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip)
			  && (entry->dst_ip == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3.ip)
			  && (entry->sport == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.all)
			  && (entry->dport == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u.all)
			  && (entry->proto == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.l3num) )
			||
			( (entry->src_ip == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3.ip)
			  && (entry->dst_ip == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip)
			  && (entry->sport == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u.all)
			  && (entry->dport == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.all)
			  && (entry->proto == ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.l3num) ) )
		{
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
			if ( (htons( entry->sport ) != 22) && (htons( entry->dport ) != 22) )
                pr_info( "[NDPI] Found EXISTING flow (ct!=null and 5meta similar) [%s]\n", print_lru_ct_entry( entry, buff, sizeof(buff) ) );
#endif
		} else {

			/* In this case we need to reset the bucket and start over */
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
			pr_info( "[NDPI] ct!=null and 5meta dont like RECYCLED %u:%u <-> %u:%u\n ",
                    ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip, ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.all,
                    ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3.ip, ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u.all );
#endif
            /* Export all data */
            ndpi_flow_end_notify( entry );

            /* Reset data and start over */
			if (init_entry_with_ct( entry, ct ) != 0) {
                pr_warning("%s:%d Fail will cause flow is NULL\n", __FUNCTION__, __LINE__);
                return MATCH_DFL_VERDICT;
			}
        }
	}

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	if ( !entry->protocol_detected ) {
        if (unlikely( debug ))
            pr_info( "[NDPI]  Found existing not detected flow [key: %lu][num_packets_processed: %u]\n",
                    (long unsigned int) key, entry->num_packets_processed );
	}
#endif

    if ( entry->protocol_detected ) {
		/* Just in case the host has not been checked yet as the cache was empty */

		verdict = GET_MATCH_ABOVE(info, entry, NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->protocols, entry->ndpi_proto))? MATCH_BLOCK: MATCH_PASS;

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
		ndpi_print_bitmask( info, "--" );

		if (unlikely( debug ))
			pr_info( "[NDPI] line:%d after compare verdict %d [Proto: %s]\n", __LINE__, verdict,
                    (entry->ndpi_proto == NOT_YET_PROTOCOL)? "NotYet": ndpi_get_proto_name(ndpi_struct, entry->ndpi_proto) );
		if (unlikely( debug ))
			pr_info( "[NDPI] Found existing detected flow detected  [key: %lu][num_packets_processed: %u]. Returning verdict %d \n",
				 (long unsigned int) key, entry->num_packets_processed, verdict );
#endif
		dumpLruCacheEntryValue( entry, verdict );
		NDPI_CB_RECORD( _skb, entry );

		return verdict;
	}

	/* PT:here my some influence */
	if ( entry->last_processed_skb == _skb && entry ->last_stamp == _skb -> tstamp.tv64) {
		/*
		 * This looks a duplicated packet, so let's discard it as it was probably
		 * processed by another nDPI-based rule
		 */
        debug_print( "[NDPI] Duplicated packet, discard it\n" );
		NDPI_CB_RECORD( _skb, entry );

        /* FTP_CONTROL never be mark as detected */
		verdict = GET_MATCH_ABOVE(info, entry, NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->protocols, entry->ndpi_proto))? MATCH_BLOCK: MATCH_PASS;
        return verdict;
	}

    entry->last_processed_skb = _skb;
	entry->last_stamp = _skb->tstamp.tv64;

	copied_skb = skb_copy( _skb, GFP_ATOMIC );
	
	if ( copied_skb == NULL ) {
		if (unlikely( debug ))
			pr_info( "[NDPI] skb_copy() failed.\n" );
		return verdict;
	}

	iph	= ip_hdr(copied_skb);
	ip_len	= copied_skb->len, ip = (u_int8_t *) iph;
	if (unlikely( debug ))
		pr_info( "[NDPI] ndpi_process_packet(%p, ip_len=%u)\n", _skb, copied_skb->len );

	do_gettimeofday( &tv );
	time = ( (u_int64_t) tv.tv_sec) * ndpi_detection_tick_resolution + tv.tv_usec / (1000000 / ndpi_detection_tick_resolution);

	entry->num_packets_processed++;

	entry->ndpi_proto = ndpi_detection_process_packet( ndpi_struct, entry->flow, ip, ip_len, time, entry->src, entry->dst );

    if (entry->ndpi_proto != NDPI_PROTOCOL_FTP_CONTROL   /* always check ftp_control */
            && (   (entry->ndpi_proto == NDPI_PROTOCOL_HTTP && entry->flow->packet_counter >= 5)  /* give up after some counts */
                || (iph->protocol == IPPROTO_UDP && entry->num_packets_processed >= 20)
                || (iph->protocol == IPPROTO_TCP && entry->num_packets_processed >= 20)
                || (entry->ndpi_proto != NDPI_PROTOCOL_UNKNOWN && entry->ndpi_proto != NDPI_PROTOCOL_HTTP))) {
		entry->protocol_detected = 1;   /* We have made a decision */
		if (unlikely( debug ))
			pr_info( "[NDPI][NDPI2] set protocol_detected=1" );
		if ( (entry->ndpi_proto == NDPI_PROTOCOL_UNKNOWN) && guess_protocol )
		{
			entry->ndpi_proto = ndpi_guess_undetected_protocol( ndpi_struct, iph->protocol,
									    ntohl( entry->src_ip ), ntohs( entry->sport ),
									    ntohl( entry->dst_ip ), ntohs( entry->dport ) );
			if (unlikely( debug ))
				pr_info( "[NDPI][NDPI2] process dont find, guessed \n" );
		}

		NDPI_CB_RECORD( _skb, entry );
		verdict = GET_MATCH_ABOVE(info, entry, NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->protocols, entry->ndpi_proto))? MATCH_BLOCK: MATCH_PASS;
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
		pr_info( "[NDPI] line:374 after guessed, verdict %d [Proto: %s]\n", verdict, (entry->ndpi_proto == NOT_YET_PROTOCOL)
			 ? "NotYet" : ndpi_get_proto_name( ndpi_struct, entry->ndpi_proto ) );

#endif
		free_LruCacheEntryValue( entry ); /* Free nDPI memory */

	} else {
		/*
		 * In this case we have not yet detected the protocol but the user has specified unknown as protocol
		 */
        if (entry->ndpi_proto == NDPI_PROTOCOL_UNKNOWN)
            entry->ndpi_proto = NOT_YET_PROTOCOL;
		verdict = GET_MATCH_ABOVE(info, entry, NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->protocols, entry->ndpi_proto))? MATCH_BLOCK: MATCH_PASS;
		NDPI_CB_RECORD(_skb,entry);
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
		pr_info( "[NDPI] line:%d force set proto=NOT_YET_PROTOCOL  skip compare verdict %d [Proto: %s] num_packets_processed:%u\n",
                __LINE__, verdict, (entry->ndpi_proto == NOT_YET_PROTOCOL)
			 ? "NotYet" : ndpi_get_proto_name( ndpi_struct, entry->ndpi_proto ), entry->num_packets_processed);
#endif
	}

	kfree_skb( copied_skb );

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info( "[NDPI] Returning verdict %d [Proto: %s]\n", verdict, (entry->ndpi_proto == NOT_YET_PROTOCOL)
		 ? "NotYet" : ndpi_get_proto_name( ndpi_struct, entry->ndpi_proto ) );
#endif

	dumpLruCacheEntryValue( entry, verdict );

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info( "-----------1) END-----------\n" );
#endif
	return verdict;
}


/* ********************************************* */

#if LINUX_VERSION_CODE < KERNEL_VERSION( 2, 6, 35 )
static bool ndpi_match( const struct sk_buff *skb, const struct xt_match_param *par )
#else
static bool ndpi_match( const struct sk_buff *skb, struct xt_action_param *par )
#endif
{
	int verdict;
	struct nf_conn *ct;
    enum ip_conntrack_info ctinfo;
	const struct xt_ndpi_protocols *match_info = par->matchinfo;
	if (unlikely( debug ))
		pr_info( "[NDPI] ndpi_match fragoff:%d thoff:%u hooknum:%u family:%u hotdrop:%d\n", par->fragoff, par->thoff, par->hooknum, par->family, *par->hotdrop );
	ct = nf_ct_get( skb, &ctinfo );
	if ((ct == NULL) || (skb == NULL)) {
		if (unlikely( debug ))
			pr_notice( "[NDPI] ignoring NULL untracked sk_buff.\n" );
		return MATCH_PASS;

#if LINUX_VERSION_CODE < KERNEL_VERSION( 3, 0, 0 )
	} else if (nf_ct_is_untracked(skb)) {
#else
	} else if (nf_ct_is_untracked(ct)) {
#endif
        pr_notice("%s:%d: ignoring untracked sk_buff.\n", __FUNCTION__, __LINE__);
		return MATCH_PASS;
	}

	/* process the packet */
    spin_lock_bh(&ndpi_lock);
	verdict = ndpi_process_packet(skb, match_info, NULL, ct);
    spin_unlock_bh(&ndpi_lock);

	if (unlikely(debug) &&  verdict == MATCH_BLOCK)
		pr_debug( "[NDPI] Dropping ...\n" );

	return verdict;
}


/* ********************************************************** */

static struct xt_match ndpi_regs[] __read_mostly = {
	{
		.name		= "ndpi",
		.revision	= 0,
		.family		= NFPROTO_IPV4,
		.match		= ndpi_match,
		.matchsize	= sizeof(struct xt_ndpi_protocols),
		.me		= THIS_MODULE,
	}
};

static int nproto_proc_read( char *page, char **start, off_t off, int count, int *eof, void *data )
{
	u_int	num_supported_protocols = ndpi_get_num_supported_protocols( ndpi_struct ) + 1 /* NOT_YET protocol */;
	u_int	i;
	int	len = 0;
	char	*tmp;
	for ( i = 0; i < num_supported_protocols; i++ )
	{
		tmp = ndpi_get_proto_by_id( ndpi_struct, i );
		if ( !!tmp )
			len += sprintf( page + len, "%u,%s\n", i, tmp );
	}
	return(len);
}


static int init_proc_engine( void )
{
	pde = proc_mkdir( PDE_ROOT, NULL );
	if ( pde == NULL )
		goto out_pde;
	pde_proto = create_proc_entry( PDE_PROTO, S_IRUGO | S_IWUSR, pde );
	if ( pde_proto == NULL )
		goto out_pde_proto;
	pde_proto->read_proc = nproto_proc_read;
	return(0);
out_pde_proto:
	remove_proc_entry( PDE_ROOT, NULL );
out_pde:
	return(-ENOMEM);
}


static void term_proc_engine( void )
{
	remove_proc_entry( PDE_PROTO, pde );
	remove_proc_entry( PDE_ROOT, NULL );
}

static unsigned int ndpi_tg( struct sk_buff *skb, const struct xt_target_param *par )
{
    const struct xt_ndpi_tginfo *target_info;
    struct nf_conn              *ct;
    enum ip_conntrack_info      ctinfo;
    target_info = par->targinfo;
    ct   = nf_ct_get( skb, &ctinfo );
    if ( (ct == NULL) || (skb == NULL) ) {

        return XT_CONTINUE;
#if LINUX_VERSION_CODE < KERNEL_VERSION( 3, 0, 0 )
    } else if ( nf_ct_is_untracked( skb ) ) {
#else
    } else if ( nf_ct_is_untracked( ct ) ) {
#endif
        return XT_CONTINUE;
    }
    spin_lock_bh(&ndpi_lock);
    /* just check and update lrucache, therefore, I ignore the return value */
    ndpi_process_packet(skb, NULL, target_info, ct);
    spin_unlock_bh(&ndpi_lock);

    return XT_CONTINUE;
}


static struct xt_target ndpi_tg_regs[] __read_mostly = {
	{
		.name		= "NDPI",
		.revision	= 0,
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		.family = NFPROTO_UNSPEC,
#else
		.family = NFPROTO_IPV4,
#endif
		.target		= ndpi_tg,
		.targetsize	= sizeof(struct xt_ndpi_tginfo),
		.me		= THIS_MODULE,
	}
};

/* tg end*/

/* ********************************************************** */

static int __init ndpi_init( void )
{
	int rc;

#if defined( DEBUG ) ||  defined (ENABLE_DEBUG_MESSAGES)
	pr_debug("Initializing nDPI module in DEBUG Mode...\n" );
#else
	pr_info("Initializing nDPI module...\n" );
#endif

	if ( (rc = init_lru_engine() ) < 0 )
		goto out_lru;
	if ( (rc = init_ndpi_engine() ) < 0 )
		goto out_ndpi;
	if ( (rc = init_proc_engine() ) < 0 )
		goto out_proc;
	if ( (rc = xt_register_matches( ndpi_regs, ARRAY_SIZE( ndpi_regs ) ) ) < 0 )
		goto out_mt;
	if ( (rc = xt_register_targets( ndpi_tg_regs, ARRAY_SIZE( ndpi_tg_regs ) ) ) < 0 )
		goto out_tg;
	pr_info("nDPI module initialized succesfully\n" );
	return(rc);
out_tg:
	xt_unregister_matches( ndpi_regs, ARRAY_SIZE( ndpi_regs ) );
out_mt:
	term_proc_engine();
out_proc:
	term_ndpi_engine();
out_ndpi:
	term_lru_engine();
out_lru:
	pr_err("nDPI module initialized FAILED\n" );

//打开skb时间戳
	net_enable_timestamp();
	return(rc);
}


/* ********************************************************** */

static void __exit ndpi_exit( void )
{
	term_proc_engine();
	term_ndpi_engine();
	term_lru_engine();

	xt_unregister_matches( ndpi_regs, ARRAY_SIZE( ndpi_regs ) );
	xt_unregister_targets( ndpi_tg_regs, ARRAY_SIZE( ndpi_tg_regs ) );

//关闭skb时间戳
	net_disable_timestamp();
	pr_info("nDPI module terminated\n" );
}


/* ********************************************************** */

module_init( ndpi_init );
module_exit( ndpi_exit );
MODULE_LICENSE( "GPL" );
MODULE_AUTHOR( "Luca Deri <deri@ntop.org>" );
MODULE_DESCRIPTION( "Match nDPI-discovered sessions (ver: "VER_DEV")" );
MODULE_ALIAS( "ipt_ndpi" );

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_byteorder.h>
#include <rte_branch_prediction.h>
#include <rte_sched.h>
#include <rte_ether.h>
#include "main.h"
#include <rte_arp.h>

#include <global_main_var.h>
#include <TCP/tcp_var.h>
#include <TCP/MemPool.h>
//#include <TCP/sockvar.h>
#include <TCP/mbuf.h>

#include <rohc/common/rohc_common.h>
//#include <rohc/comp/rohc_comp.h>
//#include <rohc/decomp/rohc_decomp.h>
#include <pkt_coalesce/packet_coalesce.h>
#include <TCP/module1.h>

#include "ip_frag_reasm.h"                              /* ← ADD */

/*
 * Inner IP fragment reassembly tables  — PRE-TUNNELING support
 *
 * These are separate from the outer-IP tables (g_frag_tbl / g_frag_death_row
 * declared in ip_frag_reasm.h) so that inner and outer flow keys never
 * collide.  They are allocated and freed in ip_frag_reasm_init() /
 * ip_frag_reasm_cleanup() — see ip_frag_reasm.c.
 *
 * Declaration: the definitions live in ip_frag_reasm.c; we only need
 * extern references here so app_thread.c can use them.
 */
extern struct rte_ip_frag_tbl    *g_inner_frag_tbl[RTE_MAX_ETHPORTS];
extern struct rte_ip_frag_death_row g_inner_frag_death_row[RTE_MAX_ETHPORTS];

/*
 * inner_ip_is_fragment() — returns non-zero when the IPv4 header indicates
 * that this is a fragment (MF bit set OR non-zero fragment offset).
 *
 * Mirrors outer_ip_is_fragment() from ip_frag_reasm.h but operates on the
 * inner IP header of a GRE-encapsulated packet (pre-tunneling case).
 */
static inline int
inner_ip_is_fragment(const struct rte_ipv4_hdr *iph)
{
	uint16_t frag = rte_be_to_cpu_16(iph->fragment_offset);
	return (frag & RTE_IPV4_HDR_MF_FLAG) ||
	       (frag & RTE_IPV4_HDR_OFFSET_MASK);
}


#define PKT_RX_IP_CKSUM_MASK RTE_MBUF_F_RX_IP_CKSUM_MASK
#define PKT_RX_L4_CKSUM_MASK RTE_MBUF_F_RX_L4_CKSUM_MASK
#define PKT_TX_IP_CKSUM RTE_MBUF_F_TX_IP_CKSUM
#define PKT_TX_IPV4 RTE_MBUF_F_TX_IPV4
#define PKT_TX_TCP_CKSUM RTE_MBUF_F_TX_TCP_CKSUM
#define PKT_TX_UDP_CKSUM RTE_MBUF_F_TX_UDP_CKSUM
#define PKT_TX_OUTER_IPV4 RTE_MBUF_F_TX_OUTER_IPV4
#define PKT_TX_OUTER_IP_CKSUM RTE_MBUF_F_TX_OUTER_IP_CKSUM
#define PKT_TX_TUNNEL_GRE RTE_MBUF_F_TX_TUNNEL_GRE

//added by sayyad


Firewalltable firewall_lut[MAX_FIREWALL_RULES];
uint16_t firewall_entries_cnt;

struct site_data_I site_data[MAX_SITE_AVAILABLE];

struct infwinfo firewallinfo;

/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

uint64_t timer1mscnt;
uint64_t timer1mscnt_2;
unsigned char apptxbuf[2100];
int apptxlen;
unsigned short ipiden;

unsigned char *apprxbuf;
int apprxlen;
uint8_t capture_mac_entry = 0;

//struct sockaddr_nam nam1;

unsigned int print_enable = 0;
unsigned int print_enable1 = 0;
unsigned int drop_packet = 0;
unsigned char drop_toggle = 0;

// unsigned int HC_RTT_VAL = (600 * 1000);
// unsigned int HC_PKT_PERIOD_VAL = 20000; /* one packet every 20ms for VoIP */

unsigned int PKT_CS_TIMER;

/* tunnel reachable is high when either the gateway or tunnel destination
   arp entry is resolved
   Based on this entry, periodic arp request packet is generated
   if tunnel is not reachable, arp request is sent once in 5 seconds
   if tunnel is reachable, arp request is sent once in every 100 seconds */
uint8_t hc_lan_reachable,hc_wan_reachable;

// #define ENABLE_PAYLOAD_COMP 0
// #define INSERT_PLC_BYTE 0

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

/* 
	index 0 - LAN 
   	index 1 - WAN
*/
struct port_stats rx_port_stats[2];
struct port_stats tx_port_stats[2];

struct port_error_stats error_stats[2];

uint32_t arp_timer;

void send_etherpkt(int lanif);
uint32_t qlen_max_seen = 1;

/*
 * QoS parameters are encoded as follows:
 *		Outer VLAN ID defines subport
 *		Inner VLAN ID defines pipe
 *		Destination IP host (0.0.0.XXX) defines queue
 * Values below define offset to each field from start of frame
 */
#define SUBPORT_OFFSET	7
#define PIPE_OFFSET		9
#define QUEUE_OFFSET	20
#define COLOR_OFFSET	19

unsigned int find_tunnel_ip(unsigned int daddr)
{
	
	for (int i = 0; i < MAX_SITE_AVAILABLE; i++)
	{
		if (site_data[i].valid == 1 && site_data[i].site_number != site_number) // It should not take Destination IP from the local site. 
		{ // Check if the site is valid
			// Check all Local_ASL entries for a matching IP
			for (int j = 0; j < NUM_ASL; j++)
			{
				// printf("ip %x mask %x daddr %x \n",site_data[i].Local_ASL[j].IP,site_data[i].Local_ASL[j].mask,daddr);
				if ((site_data[i].Local_ASL[j].IP & site_data[i].Local_ASL[j].mask)  == 
					(daddr & site_data[i].Local_ASL[j].mask) )
				{
					// printf("i %x \n",i);
					return i; // Return the corresponding index for which tunnel IP is found
				}
			}
		}
	}
	return 0; // Return 0 if no matching valid entry is found
}

int
in_fwlookup_search(struct rte_mbuf *m, int *site_index)
{
	struct rte_ether_hdr *eth_hdr;
	struct rte_vlan_hdr *vlan_hdr;
	uint16_t ether_type;
	void *l3, *l4;
	int hdr_len;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_ipv6_hdr *ipv6_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct rte_ipv4_hdr *inner_ipv4_hdr;
	// struct rte_flow_item_gre *gre_hdr;
	struct rte_gre_hdr *gre_hdr;
	int gre_len;
	uint8_t vlan_tagged = 0;
	uint16_t vlan_id;
	int i, index;
	uint16_t total_num;
	uint8_t proto_match, l4_match, dscp_match;
	uint8_t *pdata1 = rte_pktmbuf_mtod(m, uint8_t *);
	uint8_t Proto_Id;
	uint32_t Src_IP, Dst_IP;
	uint8_t dscp;

	eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	ether_type = ntohs(eth_hdr->ether_type);						
	
	if(ether_type == RTE_ETHER_TYPE_ARP)
	{
		/* For ARP pacekts, default queue */
		return -1;
	}
	else if(ether_type == RTE_ETHER_TYPE_VLAN)
	{
		vlan_tagged = 1;
		vlan_hdr = (struct rte_vlan_hdr *)((char *)eth_hdr + 
													sizeof(struct rte_ether_hdr));
		vlan_id = ntohs(vlan_hdr->vlan_tci) & 0xFFF;
		l3 = (uint8_t *)vlan_hdr + sizeof(struct rte_vlan_hdr);
		ipv4_hdr = (struct rte_ipv4_hdr *)l3;
		l4 = (uint8_t *)ipv4_hdr + sizeof(struct rte_ipv4_hdr);
		udp_hdr = (struct rte_udp_hdr *)l4;	

		Proto_Id = ipv4_hdr->next_proto_id;
		Src_IP   = ipv4_hdr->src_addr;
		Dst_IP   = ipv4_hdr->dst_addr;
		
	}
	// else if( (ether_type == RTE_ETHER_TYPE_QINQ) ||
	// 		 (ether_type == RTE_ETHER_TYPE_QINQ1) ||
	// 		 (ether_type == RTE_ETHER_TYPE_QINQ2) ||
	// 		 (ether_type == RTE_ETHER_TYPE_QINQ3)
	// 	   )
	// {
	// 	vlan_hdr = (struct rte_vlan_hdr *)((char *)eth_hdr + 
	// 												sizeof(struct rte_ether_hdr));
	// 	l3 = (uint8_t *)vlan_hdr + (2*sizeof(struct rte_vlan_hdr));
	// 	ipv4_hdr = (struct rte_ipv4_hdr *)l3;
	// 	l4 = (uint8_t *)ipv4_hdr + sizeof(struct rte_ipv4_hdr);
	// }
	else if(ether_type == RTE_ETHER_TYPE_IPV4)
	{
		vlan_tagged = 0;
		vlan_id = 1;
		l3 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
		ipv4_hdr = (struct rte_ipv4_hdr *)l3;
		l4 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
		udp_hdr = (struct rte_udp_hdr *)l4;

		if(ipv4_hdr->next_proto_id == 0x2F)
		{
			/* GRE header */
			// gre_hdr = (struct rte_flow_item_gre *)l4;
			// inner_ipv4_hdr = (struct rte_ipv4_hdr *)(l4 + sizeof(struct rte_flow_item_gre));
			// udp_hdr = (struct rte_udp_hdr *)(l4 + sizeof(struct rte_flow_item_gre) + sizeof(struct rte_ipv4_hdr));		

			gre_hdr = (struct rte_gre_hdr *)l4;
			gre_len = 4;
			if(gre_hdr->c)gre_len += 4; /* checksum present bit */
			if(gre_hdr->k)gre_len += 4; /* key present bit */
			if(gre_hdr->s)gre_len += 4; /* sequence number present bit */
			inner_ipv4_hdr = (struct rte_ipv4_hdr *)(l4 + gre_len);
			udp_hdr = (struct rte_udp_hdr *)(l4 + gre_len + sizeof(struct rte_ipv4_hdr));
		}
		else
		{
			gre_hdr = NULL;
			inner_ipv4_hdr = ipv4_hdr;
			// udp_hdr remains same....
		}
		
		/*
		 * First   : Use internal IP address and internal protocol type
		 * second  : Use GRE ip address and internal protocol type(any value)
		 * default : Use GRE ip address and GRE protocol type(0x2F)
		 */
		if(Use_gre_hdr_for_qos == 0)
		{
			Proto_Id = inner_ipv4_hdr->next_proto_id;
			Src_IP   = inner_ipv4_hdr->src_addr;
			Dst_IP   = inner_ipv4_hdr->dst_addr;
			dscp     = inner_ipv4_hdr->type_of_service & 0xFC;  // dscp is 6bits
		}
		else if(Use_gre_hdr_for_qos == 1)
		{
			Proto_Id = inner_ipv4_hdr->next_proto_id;
			Src_IP   = ipv4_hdr->src_addr;
			Dst_IP   = ipv4_hdr->dst_addr;
			dscp     = ipv4_hdr->type_of_service & 0xFC;  // dscp is 6bits
		}
		else
		{
			Proto_Id = ipv4_hdr->next_proto_id;
			Src_IP   = ipv4_hdr->src_addr;
			Dst_IP   = ipv4_hdr->dst_addr;
			dscp     = ipv4_hdr->type_of_service & 0xFC;  // dscp is 6bits
		}		
	}
	else
	{
		/* default queue */
		return -1;
	}

	//  printf("Proto_Id %x Src_IP %x, Dst_IP %x \n",Proto_Id, Src_IP, Dst_IP);

	index = -1;
	total_num = 0;
	proto_match = 0;
	l4_match = 0;
	dscp_match = 0;
	
	for(i=0;i<MAX_FIREWALL_RULES;i++)
	{
		proto_match = 0;
		l4_match = 0;
		dscp_match = 0;
		
		/* If Rule is disabled, skip the rule */
		if(firewall_lut[i].Enbl == 0)
			continue;		
		
		/* for normal pkt, vlan_id is 1 */
		if(firewall_lut[i].VLAN == vlan_id)
		{
			if( (firewall_lut[i].Proto == Proto_Id) ||
				(firewall_lut[i].Proto == 0))
			{
				proto_match = 1;						  
			}				
		}
		if( ((firewall_lut[i].SrcIP & firewall_lut[i].SrcMask) ==
			(Src_IP & firewall_lut[i].SrcMask)) ||
			(firewall_lut[i].SrcIP == 0x0)
		  )
		{
			if( ((firewall_lut[i].DstIP & firewall_lut[i].DstMask) ==
				(Dst_IP & firewall_lut[i].DstMask)) ||
				(firewall_lut[i].DstIP == 0x0)
			  )
			{
				*site_index = i;
				if((firewall_lut[i].SrcPort == udp_hdr->src_port) ||  
					(firewall_lut[i].SrcPort == 0))
				{
					if((firewall_lut[i].DstPort == udp_hdr->dst_port) ||
						(firewall_lut[i].DstPort == 0))
					{
						l4_match = 1;
						
						if((firewall_lut[i].DSCP_In == dscp) ||
						(firewall_lut[i].DSCP_In == 0))
						{
							dscp_match = 1;
						}

					}
				}
			}						
		}

		if(proto_match == 1 && l4_match == 1 && dscp_match == 1)
		{			
			index = i;
			break;
		}

		total_num++;	
		if(total_num >= firewall_entries_cnt)
			break;		
	}

	return index;
}

static inline int
get_pkt_sched(struct rte_mbuf *m, uint32_t *subport, uint32_t *pipe,
			uint32_t *traffic_class, uint32_t *queue, uint32_t *color)
{
	uint16_t *pdata = rte_pktmbuf_mtod(m, uint16_t *);
	uint8_t *pdata1 = rte_pktmbuf_mtod(m, uint8_t *);
	uint16_t pipe_queue;
	int index, site_index;
	uint8_t be_queue_cnt;
	
	site_index = -1;
	/* Find the matching firewall rule */
	index = in_fwlookup_search(m, &site_index);
	if(index == -1)
	{
		/* If no index matches, set to default site (site 0) */
		/* If IP address matches to one of the site's IP, assign that site */
		if(site_index ==  -1)
			*pipe = 0;
		else
			*pipe = firewall_lut[site_index].SiteNum;

		*traffic_class = DEFAULT_TC_PER_SITE; //12;
		*queue = 0;
	}
	else
	{
		*pipe = firewall_lut[index].SiteNum;
		*traffic_class = firewall_lut[index].tcNum;
		*queue = 0;
	}

	*subport = 0;
	*color = 0;

	// if(pdata1[23] == 0x2f)
	// {
	// printf("index %d site number %d, traffic class %d \n",index, *pipe,*traffic_class);
	// }
	
	// return 0;
	return index;
}

int
tcp_qos_queue(uint8_t dscp, uint32_t sip, uint32_t dip, uint16_t sport,uint16_t dport, uint8_t next_proto, uint32_t *Rate_config)
{
	
	uint16_t ether_type;	
	int hdr_len;	
	uint8_t vlan_tagged = 0;
	uint16_t vlan_id;
	int i, index;
	uint16_t total_num;
	uint8_t proto_match, l4_match, dscp_match;
	int site_index = -1;

	uint32_t flipped_sip; 
	uint32_t flipped_dip;
	uint16_t flipped_sport;
	uint16_t flipped_dport;

	uint32_t subport, pipe, traffic_class, queue, color;
	uint32_t queue_id;

	flipped_sip   = ntohl(sip);
	flipped_dip   = ntohl(dip);
	flipped_sport = ntohs(sport);
	flipped_dport = ntohs(dport);
	uint8_t dscp1 = dscp & 0xFC;
	
	vlan_tagged = 0;
	vlan_id = 1;		

	index = -1;
	total_num = 0;
	proto_match = 0;
	l4_match = 0;
	dscp_match = 0;
	

	// printf("sip %x dip %x sport %x dport %x \n",flipped_sip, flipped_dip, flipped_sport, flipped_dport);
	
	for(i=0;i<MAX_FIREWALL_RULES;i++)
	{
		proto_match = 0;
		l4_match = 0;
		dscp_match = 0;

		// printf("firewall sip %x dip %x sport %x dport %x \n",firewall_lut[i].SrcIP, firewall_lut[i].DstIP, firewall_lut[i].SrcPort, firewall_lut[i].DstPort);
		
		/* If Rule is disabled, skip the rule */
		if(firewall_lut[i].Enbl == 0)
			continue;		
		
		/* for normal pkt, vlan_id is 1 */
		if((firewall_lut[i].VLAN == 0)||(firewall_lut[i].VLAN == vlan_id))
		{
			if( (firewall_lut[i].Proto == next_proto) ||
			    (firewall_lut[i].Proto == 0)
			  )
			{
				proto_match = 1;
				// printf("proto match %d \n",i);						  
			}				
		}

		// printf("s1 %x \n",firewall_lut[i].SrcIP & firewall_lut[i].SrcMask);
		// printf("s2 %x \n",sip & firewall_lut[i].SrcMask);
		if( ((firewall_lut[i].SrcIP & firewall_lut[i].SrcMask) ==
			(flipped_sip & firewall_lut[i].SrcMask)) || 
			(firewall_lut[i].SrcIP == 0x0)
		)
		{
			// printf("sIP match %d \n",i);	
			if( ((firewall_lut[i].DstIP & firewall_lut[i].DstMask) ==
				(flipped_dip & firewall_lut[i].DstMask)) || 
				(firewall_lut[i].DstIP == 0x0)
			)
			{
				site_index = i;
				// printf("dIP match %d \n",i);	
				if((firewall_lut[i].SrcPort == flipped_sport) || (firewall_lut[i].SrcPort == 0))
				{
					if((firewall_lut[i].DstPort == flipped_dport) || (firewall_lut[i].DstPort == 0))
					{
						l4_match = 1;
						// printf("port match %d \n",i);	

						if((firewall_lut[i].DSCP_In == dscp1) || (firewall_lut[i].DSCP_In == 0))
						{
							dscp_match = 1;
						}
					}
				}
			}						
		}

		if(proto_match == 1 && l4_match == 1 && dscp_match == 1)
		{			
			index = i;
			break;
		}

		total_num++;	
		if(total_num >= firewall_entries_cnt)
			break;		
	}

	if(index == -1)
	{
		int16_t low_index,high_index;
		/* If no index matches, set to default site (site 0) */
		/* If IP address matches to one of the site's IP, assign that site */
		if(site_index ==  -1)
		{
			pipe = 0;
			// low_index = 0;
			// high_index = 13;
		}
		else
		{
			pipe = firewall_lut[site_index].SiteNum;
			
			/* find the protocol matching queue within the pipe
			* We have to search -13 to +13 from site_index
			*/
			// low_index = site_index - 13;
			// if(low_index < 0) low_index = 0;
			// high_index = site_index + 13;
			// if(high_index > MAX_FIREWALL_RULES) high_index = MAX_FIREWALL_RULES;
		}
				
		/* If no protocol match, default tc is 12 */
		traffic_class = DEFAULT_TC_PER_SITE; // 12;
		// for(i=low_index;i<high_index;i++)
		// {
		// 	if((firewall_lut[i].SiteNum == pipe) &&
		// 	   (firewall_lut[i].Proto == next_proto))
		// 	{
		// 		traffic_class = firewall_lut[i].tcNum;
		// 		break;
		// 	}
		// }

		queue = 0;

		/* bytes per second to kbps */
		*Rate_config = (firewall_lut[0].Rate_Configured * 8) / 1000;
	}
	else
	{
		pipe = firewall_lut[index].SiteNum;  // 3
		traffic_class = firewall_lut[index].tcNum; // 1
		queue = 0;

		/* bytes per second to kbps */
		*Rate_config = (firewall_lut[index].Rate_Configured * 8) / 1000;
		// printf("Rate configured %d \n",firewall_lut[index].Rate_Configured);	
	}

	subport = 0;
	color = 0;

	// printf("index %d pipe %d traffic_class %d \n",index, pipe, traffic_class);

	queue_id = (pipe * RTE_SCHED_QUEUES_PER_PIPE) + traffic_class;
	// printf("queue_id %d \n",queue_id);

	 //printf("Rate configured1 %d \n",*Rate_config);	

	return queue_id;

}


static bool is_rtp_hdr(struct rte_udp_hdr *udp_hdr,
                       struct rtphdr1 *rtp_hdr)
{
	// const uint8_t *remain_data = packet;
	// size_t remain_len = packet_len;
	// const uint8_t *udp_payload;
	// unsigned int udp_payload_size;
	// const struct rtphdr1 *rtp;
	bool is_rtp = false;	
	uint16_t i;

	const uint16_t max_well_known_port = 1024;
	const uint16_t sip_port = 5060;

	// const size_t default_rtp_ports_nr = 7;
	// unsigned int default_rtp_ports[] = { 1234, 36780, 33238, 5020, 5002, 5006, 10042 };
	uint16_t udp_sport;
	uint16_t udp_dport;
	uint16_t udp_len;
	uint8_t rtp_pt;

	/* retrieve UDP source and destination ports and UDP length */
	// memcpy(&udp_sport, remain_data, sizeof(uint16_t));
	// memcpy(&udp_dport, remain_data + 2, sizeof(uint16_t));
	// memcpy(&udp_len, remain_data + 4, sizeof(uint16_t));

	udp_sport = ntohs(udp_hdr->src_port);
	udp_dport = ntohs(udp_hdr->dst_port);
	udp_len   = ntohs(udp_hdr->dgram_len);

	// printf("Going to check udp hdr udp_dport %d udp_len %d\n",udp_dport,udp_len);

	/* RTP streams do not use well known ports */
	if((udp_sport) <= max_well_known_port ||
	   (udp_dport) <= max_well_known_port)
	{
		goto not_rtp;
	}

	/* SIP (UDP/5060) is not RTP */
	if((udp_sport) == sip_port && (udp_dport) == sip_port)
	{
		goto not_rtp;
	}

	/* the UDP destination port of RTP packet is even (the RTCP destination
	 * port are RTP destination port + 1, so it is odd) */
	if(((udp_dport) % 2) != 0)
	{
		goto not_rtp;
	}

	/* UDP Length shall not be too large */
	if((udp_len) > 200)
	{
		goto not_rtp;
	}

	// remain_data += sizeof(struct udphdr);
	// remain_len -= sizeof(struct udphdr);

	uint16_t udp_payloadlen;
	udp_payloadlen = udp_len - sizeof(struct udphdr);

	/* UDP payload shall be large enough for RTP header  */
	if(udp_payloadlen < sizeof(struct rtphdr1))
	{		
		goto unsupported_rtp_hdr;
	}

	// udp_payload = (uint8_t *) remain_data;
	// udp_payload_size = remain_len;
	// rtp = (const struct rtphdr *) udp_payload;

	// printf("Going to check rtp hdr version %d pt %d cc %d \n",rtp_hdr->version,rtp_hdr->pt,rtp_hdr->cc);
	// printf("rtp hdr SN %d ts %x ssrc %x \n",ntohs(rtp_hdr->sn),ntohl(rtp_hdr->timestamp),ntohl(rtp_hdr->ssrc));

	/* RTP version bits shall be 2 */
	if(rtp_hdr->version != 0x2)
	{
		goto not_rtp;
	}

	


	/* RTP payload type shall be GSM (0x03), ITU-T G.723 (0x04),
	 * ITU-T G.729 (0x12), dynamic RTP type 97 (0x61),
	 * telephony-event (0x65), Speex (0x72),
	 * or dynamic RTP type 125 (0x7d) */
	rtp_pt = rtp_hdr->pt;
	if(rtp_pt != 0x03 && rtp_pt != 0x04 && rtp_pt != 0x12 && rtp_pt != 0x61 &&
	   rtp_pt != 0x65 && rtp_pt != 0x72 && rtp_pt != 0x7d)
	{
		goto not_rtp;
	}

	/* RTP packets with CSRC items can not be compressed by this RTP profile */
	if(rtp_hdr->cc != 0)
	{
		goto unsupported_rtp_hdr;
	}

	is_rtp = true;

not_rtp:
unsupported_rtp_hdr:
	return is_rtp;
}


// static inline int
// get_pkt_sched(struct rte_mbuf *m, uint32_t *subport, uint32_t *pipe,
// 			uint32_t *traffic_class, uint32_t *queue, uint32_t *color)
// {
// 	uint16_t *pdata = rte_pktmbuf_mtod(m, uint16_t *);
// 	uint16_t pipe_queue;

// 	/* Outer VLAN ID*/
// 	*subport = (rte_be_to_cpu_16(pdata[SUBPORT_OFFSET]) & 0x0FFF) &
// 		(port_params.n_subports_per_port - 1);

// 	/* Inner VLAN ID */
// 	*pipe = (rte_be_to_cpu_16(pdata[PIPE_OFFSET]) & 0x0FFF) &
// 		(subport_params[*subport].n_pipes_per_subport_enabled - 1);

// 	pipe_queue = active_queues[(pdata[QUEUE_OFFSET] >> 8) % n_active_queues];

// 	/* Traffic class (Destination IP) */
// 	*traffic_class = pipe_queue > RTE_SCHED_TRAFFIC_CLASS_BE ?
// 			RTE_SCHED_TRAFFIC_CLASS_BE : pipe_queue;

// 	/* Traffic class queue (Destination IP) */			
// 	*queue = pipe_queue - *traffic_class;

// 	/* Color (Destination IP) */
// 	*color = pdata[COLOR_OFFSET] & 0x03;

// 	subport_debug	=	*subport;
// 	pipe_debug		=	*pipe;
// 	traffic_class_debug = *traffic_class;
// 	queue_debug		=	*queue;
// 	color_debug		=	*color;

// 	// printf("subport %d, pipe %d, traffic class %d, queue %d, color %d\n",
// 	// 		*subport, *pipe, *traffic_class, *queue, *color);

// 	return 0;
// }

// static inline int
// get_pkt_sched(struct rte_mbuf *m, uint32_t *subport, uint32_t *pipe,
// 			uint32_t *traffic_class, uint32_t *queue, uint32_t *color)
// {
// 	uint16_t *pdata = rte_pktmbuf_mtod(m, uint16_t *);
// 	uint16_t pipe_queue;

// 	/* Outer VLAN ID*/
// 	*subport = 0; //(rte_be_to_cpu_16(pdata[SUBPORT_OFFSET]) & 0x0FFF) &
// 		//(port_params.n_subports_per_port - 1);

// 	/* Inner VLAN ID */
// 	//*pipe = 0; //(rte_be_to_cpu_16(pdata[PIPE_OFFSET]) & 0x0FFF) &
// 		//(subport_params[*subport].n_pipes_per_subport_enabled - 1);

// 	pipe_queue = active_queues[(pdata[QUEUE_OFFSET] >> 8) % n_active_queues];

// 	if(rte_be_to_cpu_16(pdata[18]) == 5001)
// 	{

// 		*pipe = 0;

// 		/* Traffic class (Destination IP) */
// 		*traffic_class = 0; //pipe_queue > RTE_SCHED_TRAFFIC_CLASS_BE ?
// 				//RTE_SCHED_TRAFFIC_CLASS_BE : pipe_queue;

// 		/* Traffic class queue (Destination IP) */			
// 		*queue = 0; //pipe_queue - *traffic_class;
// 	}
// 	else if(rte_be_to_cpu_16(pdata[18]) == 5002)
// 	{
// 		*pipe = 1;

// 		/* Traffic class (Destination IP) */
// 		*traffic_class = 0; //1; //pipe_queue > RTE_SCHED_TRAFFIC_CLASS_BE ?
// 				//RTE_SCHED_TRAFFIC_CLASS_BE : pipe_queue;

// 		/* Traffic class queue (Destination IP) */			
// 		*queue = 0; //pipe_queue - *traffic_class;
// 	}
// 	else
// 	{
// 		*pipe = 0;
		
// 		/* Traffic class (Destination IP) */
// 		*traffic_class = 2; //pipe_queue > RTE_SCHED_TRAFFIC_CLASS_BE ?
// 				//RTE_SCHED_TRAFFIC_CLASS_BE : pipe_queue;

// 		/* Traffic class queue (Destination IP) */			
// 		*queue = 0; //pipe_queue - *traffic_class;
// 	}

// 	/* Color (Destination IP) */
// 	*color = 0; //pdata[COLOR_OFFSET] & 0x03;
	
// 	return 0;
// }

void fillheaders_send(int comp_status, int lanif)
{
	uint32_t i;
	unsigned long sum;
	unsigned short tcpcs,ipcs;
	uint32_t temp32;
	uint16_t temp16;
	uint16_t inner_iplen, outer_iplen;
	unsigned int tcp_payload_len;
	uint8_t gre_len, tcphlen;
	uint16_t tcplen;

	if(apptxbuf[23] == 0x2F)
	{
		gre_len = 24;
		if((apptxbuf[34] & 0x80) == 0x80)gre_len += 4; /* checksum bit */
		if((apptxbuf[34] & 0x20) == 0x20)gre_len += 4; /* Key bit */
		if((apptxbuf[34] & 0x10) == 0x10)gre_len += 4; /* Seq num bit */
	}
	else
		gre_len = 0;

	tcphlen = ((apptxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
	tcp_payload_len = apptxlen - (gre_len+34+tcphlen) ;	

	if( (INSERT_PLC_BYTE) && (comp_status < 0))
	{
		memcpy(apptxbuf+gre_len+34+tcphlen+1,apptxbuf+gre_len+34+tcphlen, tcp_payload_len);

		//if(comp_status < 0)
		{
			/* compression failed */					
			apptxbuf[gre_len+34+tcphlen] = 0x56;						
		}
		
		tcp_payload_len++;
		apptxlen++;

		if(gre_len == 0)
		{
			/* non-GRE packet */
			inner_iplen = (((apptxbuf[16]<<8)&0xFF00) | apptxbuf[17]);
			inner_iplen++;
			
			apptxbuf[16] = inner_iplen >> 8;
			apptxbuf[17] = inner_iplen >> 0;	
		}
		else
		{
			/* GRE packet */
			outer_iplen = (((apptxbuf[16]<<8)&0xFF00) | apptxbuf[17]);
			inner_iplen = (((apptxbuf[gre_len+16]<<8)&0xFF00) | apptxbuf[gre_len+17]);
			outer_iplen++;
			inner_iplen++;
			
			apptxbuf[16] = outer_iplen >> 8;
			apptxbuf[17] = outer_iplen >> 0;

			apptxbuf[gre_len+16] = inner_iplen >> 8;
			apptxbuf[gre_len+17] = inner_iplen >> 0;
		}
	}
	else
	{
		inner_iplen = (((apptxbuf[gre_len+16]<<8)&0xFF00) | apptxbuf[gre_len+17]);
	}

	tcplen = inner_iplen - 20;


	/* IP Checksum calculation */
	// sum = 0;
	// sum = sum + (((apptxbuf[14]<<8)&0xFF00) | apptxbuf[15]);
	// sum = sum + (((apptxbuf[16]<<8)&0xFF00) | apptxbuf[17]);
	// sum = sum + (((apptxbuf[18]<<8)&0xFF00) | apptxbuf[19]);
	// sum = sum + (((apptxbuf[20]<<8)&0xFF00) | apptxbuf[21]);
	// sum = sum + (((apptxbuf[22]<<8)&0xFF00) | apptxbuf[23]);
	// sum = sum + (((apptxbuf[26]<<8)&0xFF00) | apptxbuf[27]);
	// sum = sum + (((apptxbuf[28]<<8)&0xFF00) | apptxbuf[29]);
	// sum = sum + (((apptxbuf[30]<<8)&0xFF00) | apptxbuf[31]);
	// sum = sum + (((apptxbuf[32]<<8)&0xFF00) | apptxbuf[33]);

	// while(sum>>16)
	// 	sum = (sum & 0xFFFF) + (sum >> 16);
	
	// ipcs = ~sum;

	// apptxbuf[24] = (ipcs>>8) & 0xFF;
	// apptxbuf[25] = (ipcs) & 0xFF;

	// if(gre_len)
	// {
	// 	sum = 0;
	// 	sum = sum + (((apptxbuf[14+gre_len]<<8)&0xFF00) | apptxbuf[15+gre_len]);
	// 	sum = sum + (((apptxbuf[16+gre_len]<<8)&0xFF00) | apptxbuf[17+gre_len]);
	// 	sum = sum + (((apptxbuf[18+gre_len]<<8)&0xFF00) | apptxbuf[19+gre_len]);
	// 	sum = sum + (((apptxbuf[20+gre_len]<<8)&0xFF00) | apptxbuf[21+gre_len]);
	// 	sum = sum + (((apptxbuf[22+gre_len]<<8)&0xFF00) | apptxbuf[23+gre_len]);
	// 	sum = sum + (((apptxbuf[26+gre_len]<<8)&0xFF00) | apptxbuf[27+gre_len]);
	// 	sum = sum + (((apptxbuf[28+gre_len]<<8)&0xFF00) | apptxbuf[29+gre_len]);
	// 	sum = sum + (((apptxbuf[30+gre_len]<<8)&0xFF00) | apptxbuf[31+gre_len]);
	// 	sum = sum + (((apptxbuf[32+gre_len]<<8)&0xFF00) | apptxbuf[33+gre_len]);

	// 	while(sum>>16)
	// 		sum = (sum & 0xFFFF) + (sum >> 16);
		
	// 	ipcs = ~sum;

	// 	apptxbuf[24+gre_len] = (ipcs>>8) & 0xFF;
	// 	apptxbuf[25+gre_len] = (ipcs) & 0xFF;
	// }

	// /* TCP Checksum calculation */	
	// sum = 0;
	// apptxbuf[50+gre_len] = 0x00;
	// apptxbuf[51+gre_len] = 0x00;
	// for(i=0;i<tcplen/2;i++)
	// {
	// 	sum = sum + (((apptxbuf[2*i+34+gre_len]<<8)&0xFF00) | apptxbuf[2*i+35+gre_len]);
	// }
	// if((tcplen%2) == 1)

	// 	sum = sum + (((apptxbuf[tcplen+33+gre_len]<<8)&0xFF00) | 0x00);  //apptxbuf[tcplen-1+34]

	// temp32 = (apptxbuf[26+gre_len]<<24) | (apptxbuf[27+gre_len]<<16) | (apptxbuf[28+gre_len]<<8) | (apptxbuf[29+gre_len]);
	// sum = sum + ((temp32 >> 16) & 0xFFFF);
	// sum = sum + (temp32 & 0xFFFF);
	
	// temp32 = (apptxbuf[30+gre_len]<<24) | (apptxbuf[31+gre_len]<<16) | (apptxbuf[32+gre_len]<<8) | (apptxbuf[33+gre_len]);
	// sum = sum + ((temp32 >> 16) & 0xFFFF);
	// sum = sum + (temp32 & 0xFFFF);
	
	// sum = sum + apptxbuf[23+gre_len];
	// temp16 = inner_iplen;
	// sum = sum + (temp16 - 20);
	
	// while(sum>>16)
	// 	sum = (sum & 0xFFFF) + (sum >> 16);
	
	// tcpcs = ~sum;
	
	// apptxbuf[50+gre_len] = (tcpcs>>8) & 0xFF;
	// apptxbuf[51+gre_len] = (tcpcs) & 0xFF;

	send_etherpkt(lanif);	

	// if( (apptxlen != (inner_iplen+14)) &&
	// 				(inner_iplen > 46)  ) 
	// {
	// 	/* data length mismatch */
	// 	printf("TX Pkt len and ip len mismatch %d %d portid %d\n",apptxlen,(inner_iplen+14),lanif);		
	// }
}

/*
 * This function called, after TCP processing and before sending over the PHY port
 * 1) if sending over LAN i/f, do not compress
 * 2) if sending over WAN i/f
 *    a) if payload compression is enabled, goto compress the data
 *       if failed,
 * 			add 0x56 to the TCP payload and send over WAN i/f
 *       if success.
 *          add 0x65 to the TCP paylaod and send over WAN i/f
 *    b) if payload compression is disabled, send plain data over WAN i/f

*/
void check_tcp_compressible(int lanif, uint8_t gre_len, uint8_t session_pl_enabled)
{
	unsigned int tcplen,tcp_payload_len;
	uint8_t tcphlen;
	int comp_status = 0;	

	if(lanif == 0)
		send_etherpkt(lanif);
	else
	{
		//printf("session_pl_enbaled %d \n",session_pl_enbaled);
		if(session_pl_enabled == 0)
		{
			send_etherpkt(lanif);
		}
		else
		{
			//printf("in comp_decomp \n");
			//tcplen = apptxlen - 14 - 20;
			tcplen = ((apptxbuf[gre_len+16]<<8) | apptxbuf[gre_len+17]) - 20;
			tcphlen = ((apptxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
			
			tcp_payload_len = tcplen - tcphlen;

			//printf("apptxlen %d, tcphlen %d tcplen %d\n",apptxlen,tcphlen,tcplen);

			if(tcp_payload_len == 0)
			{
				send_etherpkt(lanif);
				// tcpstat.tcps_test1++;
			}
			else
			{
				unsigned char *buf_offset;
				buf_offset = apptxbuf+(34+gre_len+tcphlen);			
				comp_status = test_compressdev_deflate_stateless_fixed(0,apptxbuf,apptxlen,34+gre_len+tcphlen,lanif,
																	   &buf_offset);

				if(comp_status < 0)
				{
					fillheaders_send(comp_status, lanif);
					// tcpstat.tcps_test2++;
				}
				else 
				{
					send_etherpkt(lanif);
					// tcpstat.tcps_test3++;
				}	

				// if(apptxbuf[23] == 0x2F)
				// 	gre_len = 24;
				// else
				// 	gre_len = 0;

				// tcphlen = ((apptxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
				// tcp_payload_len = apptxlen - gre_len+34+tcphlen ;	

				// //memcpy(apptxbuf, det_inp->apprxbuf, gre_len+34+tcphlen);	
				// memcpy(apptxbuf+gre_len+34+tcphlen+1,apptxbuf+gre_len+34+tcphlen, tcp_payload_len);

				// if(comp_status < 0)
				// {
				// 	/* compression failed */					
				// 	apptxbuf[gre_len+34+tcphlen] = 0x56;						
				// }
				// else
				// {
				// 	/* compression success */
				// 	apptxbuf[gre_len+34+tcphlen] = 0x65;	
				// }
				// apptxlen = apptxlen + 1;
				// fillheaders_send(lanif, gre_len, tcp_payload_len);
			}
		}
	}
}
/*
 * This function is called once the packet is received,after processing gives to TCP stack
 * 1) If received over LAN, no need to decompress
 * 2) If received over WAN
 *    a) if payload compression is disabled, simply give the packet to TCP process
 * 	  b) if payload compression is enabled
 *       if first TCP payload byte is 0x56
 * 			goto decompressor.
 * 				if fail, drop the packet at TCP deparsing logic
 * 				if success, give it to TCP engine
 * 		if first TCP payload byte is 0x65,
 * 			remove 0x65 and give it to TCP engine
 * 		
*/
/* If packet rcvd on WAN, decompress the packet and give it to TCP stack
   if rcvd on LAN, first give it to TCP stack and then to compressor */	
uint8_t check_tcp_decompressable(unsigned char *rxbuf, int txlen, unsigned int lanif, uint8_t gre_len)
{
	unsigned int tcplen,tcp_payload_len;
	uint8_t tcphlen;
	uint16_t inner_iplen, outer_iplen;
	uint8_t decomp_en = 0;
	uint8_t decomp_status;
	// unsigned char tempbuf[2000];

	// printf("lanif %d ENABLE_PAYLOAD_COMP %d INSERT_PLC_BYTE %d \n",lanif,ENABLE_PAYLOAD_COMP,INSERT_PLC_BYTE);
	
	if(lanif == 0)
		return 0;
	else
	{
		if(ENABLE_PAYLOAD_COMP == 0)
		{
			return 0;
		}
		else
		{
			/* Fetch the  */
			//tcplen = txlen - 14 - 20;
			tcplen = ((rxbuf[gre_len+16]<<8) | rxbuf[gre_len+17]) - 20;
			tcphlen = ((rxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
			
			tcp_payload_len = tcplen - tcphlen;			

			if(tcp_payload_len == 0)
			{
				//send_etherpkt(lanif);
				// printf("In tcp decompression1 \n");
				// tcpstat.tcps_test4++;
				return 0;
			}
			else
			{
				
				if(INSERT_PLC_BYTE == 0)
				{
					/* Always decomp is enabled, if no byte is appended */
					decomp_en = 1;
				}
				else
				{
					// printf("In tcp decompression %x \n",rxbuf[gre_len+34+tcphlen]);
				
					if(rxbuf[gre_len+34+tcphlen] == 0x56)
					{
						/* no decompression */
						decomp_en = 0;
						// tcpstat.tcps_test5++;
					}
					else if(rxbuf[gre_len+34+tcphlen] == 0x65)
					{
						/* decompression */
						decomp_en = 1;
						// tcpstat.tcps_test6++;
					}
					// rte_memcpy(tempbuf, rxbuf, gre_len+34+tcphlen);
					// rte_memcpy(tempbuf+gre_len+34+tcphlen, rxbuf+gre_len+34+tcphlen+1, tcp_payload_len-1);
					// apptxlen = apptxlen -1;
					// if(gre_len == 0)
					// {
					// 	/* non-GRE packet */
					// 	inner_iplen = (((tempbuf[16]<<8)&0xFF00) | tempbuf[17]);
					// 	inner_iplen--;
						
					// 	tempbuf[16] = inner_iplen >> 8;
					// 	tempbuf[17] = inner_iplen >> 0;	
					// }
					// else
					// {
					// 	/* GRE packet */
					// 	outer_iplen = (((tempbuf[16]<<8)&0xFF00) | tempbuf[17]);
					// 	inner_iplen = (((tempbuf[gre_len+16]<<8)&0xFF00) | tempbuf[gre_len+17]);
					// 	outer_iplen--;
					// 	inner_iplen--;
						
					// 	tempbuf[16] = outer_iplen >> 8;
					// 	tempbuf[17] = outer_iplen >> 0;

					// 	tempbuf[gre_len+16] = inner_iplen >> 8;
					// 	tempbuf[gre_len+17] = inner_iplen >> 0;
					// }
				}				

				/*
				 * return values:
				 * 0 - no decompression
				 * 1 - decompression success
				 * 2 - decompression fail
				 * 
				*/
				if(decomp_en)
				{
					unsigned char *buf_offset;
					buf_offset = rxbuf + (34+gre_len+tcphlen+1);		
					// decomp_status = test_compressdev_deflate_stateless_fixed(1,tempbuf,apptxlen,34+gre_len+tcphlen,lanif);
					decomp_status = test_compressdev_deflate_stateless_fixed(1,rxbuf,apptxlen,34+gre_len+tcphlen+1,lanif,
																			 &buf_offset);
					if(decomp_status < 0)
					{
						// tcpstat.tcps_test7++;
						/* decompression failed */					
						return 2;
												
					}
					else
					{
						// tcpstat.tcps_test8++;
						/* deompression success */						
						return 1;	
					}
				}
				else
				{
					/* Remove the extra byte added.... */

					rte_memcpy(apptxbuf, rxbuf, gre_len+34+tcphlen);
					rte_memcpy(apptxbuf+gre_len+34+tcphlen, rxbuf+gre_len+34+tcphlen+1, tcp_payload_len-1);
					apptxlen = apptxlen -1;
					if(gre_len == 0)
					{
						/* non-GRE packet */
						inner_iplen = (((apptxbuf[16]<<8)&0xFF00) | apptxbuf[17]);
						inner_iplen--;
						
						apptxbuf[16] = inner_iplen >> 8;
						apptxbuf[17] = inner_iplen >> 0;	
					}
					else
					{
						/* GRE packet */
						outer_iplen = (((apptxbuf[16]<<8)&0xFF00) | apptxbuf[17]);
						inner_iplen = (((apptxbuf[gre_len+16]<<8)&0xFF00) | apptxbuf[gre_len+17]);
						outer_iplen--;
						inner_iplen--;
						
						apptxbuf[16] = outer_iplen >> 8;
						apptxbuf[17] = outer_iplen >> 0;

						apptxbuf[gre_len+16] = inner_iplen >> 8;
						apptxbuf[gre_len+17] = inner_iplen >> 0;
					}
					// tcpstat.tcps_test9++;
					
					/* Just removed the PLC byte, no decompression */
					return 1;
				}
			}
		}
	}

	return 0;
}

uint8_t check_tcp_decompressable_ip(unsigned char *rxbuf, int txlen, unsigned int lanif, uint8_t gre_len,
									uint8_t session_pl_enabled)
{
	unsigned int tcplen,tcp_payload_len;
	uint8_t tcphlen;
	uint16_t inner_iplen, outer_iplen;
	uint8_t decomp_en = 0;
	uint8_t decomp_status;
	// unsigned char tempbuf[2000];

	// printf("lanif %d ENABLE_PAYLOAD_COMP %d INSERT_PLC_BYTE %d \n",lanif,ENABLE_PAYLOAD_COMP,INSERT_PLC_BYTE);
	
	if(lanif == 0)
		return 0;
	else
	{
		if(session_pl_enabled == 0)
		{
			return 0;
		}
		else
		{
			/* Fetch the  */
			//tcplen = txlen - 14 - 20;
			tcplen = ((rxbuf[gre_len+3]<<8) | rxbuf[gre_len+2]) - 20;
			tcphlen = ((rxbuf[gre_len+32] & 0xF0)	>> 4 ) << 2;
			
			tcp_payload_len = tcplen - tcphlen;		
			// printf("tcplen %x tcphlen %x tcp_payload_len %x lanif %d \n",tcplen,tcphlen,tcp_payload_len,lanif);	

			if(tcp_payload_len == 0)
			{
				//send_etherpkt(lanif);
				// printf("In tcp decompression1 \n");
				// tcpstat.tcps_test4++;
				return 0;
			}
			else
			{
				
				if(INSERT_PLC_BYTE == 0)
				{
					/* Always decomp is enabled, if no byte is appended */
					decomp_en = 1;
				}
				else
				{
					// printf("In tcp decompression %x \n",rxbuf[gre_len+34+tcphlen]);
				
					if(rxbuf[gre_len+20+tcphlen] == 0x56)
					{
						/* no decompression */
						decomp_en = 0;
						// tcpstat.tcps_test5++;
					}
					else if(rxbuf[gre_len+20+tcphlen] == 0x65)
					{
						/* decompression */
						decomp_en = 1;
						// tcpstat.tcps_test6++;
					}
					// rte_memcpy(tempbuf, rxbuf, gre_len+34+tcphlen);
					// rte_memcpy(tempbuf+gre_len+34+tcphlen, rxbuf+gre_len+34+tcphlen+1, tcp_payload_len-1);
					// apptxlen = apptxlen -1;
					// if(gre_len == 0)
					// {
					// 	/* non-GRE packet */
					// 	inner_iplen = (((tempbuf[16]<<8)&0xFF00) | tempbuf[17]);
					// 	inner_iplen--;
						
					// 	tempbuf[16] = inner_iplen >> 8;
					// 	tempbuf[17] = inner_iplen >> 0;	
					// }
					// else
					// {
					// 	/* GRE packet */
					// 	outer_iplen = (((tempbuf[16]<<8)&0xFF00) | tempbuf[17]);
					// 	inner_iplen = (((tempbuf[gre_len+16]<<8)&0xFF00) | tempbuf[gre_len+17]);
					// 	outer_iplen--;
					// 	inner_iplen--;
						
					// 	tempbuf[16] = outer_iplen >> 8;
					// 	tempbuf[17] = outer_iplen >> 0;

					// 	tempbuf[gre_len+16] = inner_iplen >> 8;
					// 	tempbuf[gre_len+17] = inner_iplen >> 0;
					// }
				}				

				/*
				 * return values:
				 * 0 - no decompression
				 * 1 - decompression success
				 * 2 - decompression fail
				 * 
				*/
				if(decomp_en)
				{
					unsigned char *buf_offset;
					buf_offset = rxbuf + (20+gre_len+tcphlen+1);		
					// decomp_status = test_compressdev_deflate_stateless_fixed(1,tempbuf,apptxlen,34+gre_len+tcphlen,lanif);
					decomp_status = test_compressdev_deflate_stateless_fixed(1,rxbuf,apptxlen,20+gre_len+tcphlen+1,lanif,
																			 &buf_offset);
					if(decomp_status < 0)
					{
						// tcpstat.tcps_test7++;
						/* decompression failed */					
						return 2;
												
					}
					else
					{
						// tcpstat.tcps_test8++;
						/* deompression success */						
						return 1;	
					}
				}
				else
				{
					/* Remove the extra byte added.... */

					rte_memcpy(apptxbuf, rxbuf, gre_len+20+tcphlen);
					rte_memcpy(apptxbuf+gre_len+20+tcphlen, rxbuf+gre_len+20+tcphlen+1, tcp_payload_len-1);
					apptxlen = apptxlen -1;
					if(gre_len == 0)
					{
						/* non-GRE packet */
						inner_iplen = (((apptxbuf[2]<<8)&0xFF00) | apptxbuf[3]);
						inner_iplen--;
						
						apptxbuf[2] = inner_iplen >> 8;
						apptxbuf[3] = inner_iplen >> 0;	
					}
					else
					{
						/* GRE packet */
						outer_iplen = (((apptxbuf[2]<<8)&0xFF00) | apptxbuf[3]);
						inner_iplen = (((apptxbuf[gre_len+2]<<8)&0xFF00) | apptxbuf[gre_len+3]);
						outer_iplen--;
						inner_iplen--;
						
						apptxbuf[2] = outer_iplen >> 8;
						apptxbuf[3] = outer_iplen >> 0;

						apptxbuf[gre_len+2] = inner_iplen >> 8;
						apptxbuf[gre_len+3] = inner_iplen >> 0;
					}
					// tcpstat.tcps_test9++;
					
					/* Just removed the PLC byte, no decompression */
					return 1;
				}
			}
		}
	}

	return 0;
}



uint8_t
verify_tcp_checksum(uint8_t *rxbuf, struct ipv4_hdr *ip, int iplen)
{
	uint8_t match = 0;
	uint16_t tcprxlen;
	uint32_t temp32;
	uint16_t temp16;
	unsigned int i;	
	unsigned long sum;
	unsigned short tcpcs;

	tcprxlen = iplen - 20;

	sum = 0;
	for(i=0;i<tcprxlen/2;i++)
	{
		sum = sum + (((rxbuf[2*i]<<8)&0xFF00) | rxbuf[2*i+1]);
	}
	if((tcprxlen%2) == 1)
		sum = sum + (((rxbuf[tcprxlen-1]<<8)&0xFF00) | 0x00);  //apptxbuf[tcprxlen-1+34]

	temp32 = htonl(ip->saddr);
	sum = sum + ((temp32 >> 16) & 0xFFFF);
	sum = sum + (temp32 & 0xFFFF);

	temp32 = htonl(ip->daddr);
	sum = sum + ((temp32 >> 16) & 0xFFFF);
	sum = sum + (temp32 & 0xFFFF);

	sum = sum + ip->protocol;
	temp16 = htons(ip->tot_len);
	sum = sum + (temp16 - 20);

	while(sum>>16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	
	//printf("sum %x \n",sum);
	tcpcs = ~sum;

	if(tcpcs == 0)
	{
		match = 1;

	}
	else
	{
		match = 0;
	}		

	return match;
}

extern TCPlookuptable TCP_lut[MAX_TCP_CONN];

void print_tcpdebug()
{
	int i,j;
	struct tcpcb *tp;
	struct socket *so;
	unsigned int loop_start;
	int error,len;
	int idle_time = 0;
	int run_cnt = 0;

	loop_start = 2;

	calculate_tcp_clock();

	for(i=loop_start;i<MAX_TCP_CONN;i++)
	{
		if(TCP_lut[i].isStored==1)
		{
			tp = TCP_lut[i].tp;
			idle_time = tcp_now - tp->t_rcvtime;
	
			if (idle_time >= 7000)
			{
				//printf("index %x, seq %x ack %x win %x \n",i,tp->dummy_lastseq,tp->dummy_lastack,tp->dummy_lastwin);
				//printf("index1 %x, ack %x win %x adv %x\n",i,tp->dummy_lastack,tp->rcv_nxt,tp->rcv_adv);
			}
		}
		else
		{
			break;
		}
	}
}

int Qos_queue_len(uint32_t queue_id)
{
	struct rte_sched_queue_stats stats;
	//rte_sched_queue_read_stats(qos_conf[0].sched_port, 49, &stats, qlen);	
	return (qos_conf[0].wt_thread.qlen[queue_id]);
}


void selipmac(int lanif)
{
	if(lanif == LAN_IF_ID)
	{
		Sourceipaddrsel = LAN_IPADDR;
		memcpy(SRC_MACADDR, LAN_MACADDR, RTE_ETHER_ADDR_LEN);		
	}
	else
	{
		Sourceipaddrsel = TUNNEL_IPADDR;
		memcpy(SRC_MACADDR, TUNNEL_MACADDR, RTE_ETHER_ADDR_LEN);		
	}
}

void SendArpRequestPkt(int lanif)
{
	struct rte_mbuf *created_pkt = NULL;
	struct rte_ether_hdr *eth_hdr;
	struct rte_arp_hdr *arp_hdr;

	size_t pkt_size;
	int ret;

	selipmac(lanif);

	created_pkt = rte_pktmbuf_alloc(qos_conf[0].mbuf_pool);
	if (created_pkt == NULL) {
		printf("Failed to allocate mbuf in send ARP pkt\n");
		return;
	}

	pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
	created_pkt->data_len = pkt_size;
	created_pkt->pkt_len = pkt_size;	

	eth_hdr = rte_pktmbuf_mtod(created_pkt, struct rte_ether_hdr *);
	//rte_ether_addr_copy(&bond_mac_addr, &eth_hdr->s_addr);
	memcpy(&eth_hdr->src_addr, SRC_MACADDR, RTE_ETHER_ADDR_LEN);	
	memset(&eth_hdr->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

	arp_hdr = (struct rte_arp_hdr *)(
		(char *)eth_hdr + sizeof(struct rte_ether_hdr));

	arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
	arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
	arp_hdr->arp_plen = sizeof(uint32_t);
	arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);

	//rte_ether_addr_copy(&bond_mac_addr, &arp_hdr->arp_data.arp_sha);
	memcpy(&arp_hdr->arp_data.arp_sha, SRC_MACADDR, RTE_ETHER_ADDR_LEN);
	//arp_hdr->arp_data.arp_sip = bond_ip;
	arp_hdr->arp_data.arp_sip = htonl(Sourceipaddrsel);
	memset(&arp_hdr->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);
	// arp_hdr->arp_data.arp_tip =
	// 		  ((unsigned char *)&res->ip.addr.ipv4)[0]        |
	// 		 (((unsigned char *)&res->ip.addr.ipv4)[1] << 8)  |
	// 		 (((unsigned char *)&res->ip.addr.ipv4)[2] << 16) |
	// 		 (((unsigned char *)&res->ip.addr.ipv4)[3] << 24);
	
	/* send ARP request to remote ip, if in same subnet else to gateway IP */
	// if((TUNNEL_IPADDR & TUNNEL_MASK) == 
	// 		(TUNNEL_REMOTE_IPADDR & TUNNEL_MASK))
	// {
	// 	/* same subnet */
	// 	arp_hdr->arp_data.arp_tip = htonl(TUNNEL_REMOTE_IPADDR);
	// }
	// else
	// {
	// 	/* not in the same subnet */
	// 	arp_hdr->arp_data.arp_tip = htonl(TUNNEL_GW);
	// }
	arp_hdr->arp_data.arp_tip = htonl(Destipaddrsel);
	
	int portid = l2fwd_dst_ports[lanif];
	send_schedule_pkt(portid, created_pkt);
	//printf("Sent the ARP Request packet src = %x dst = %x \n",htonl(arp_hdr->arp_data.arp_sip),
	 //														  htonl(arp_hdr->arp_data.arp_tip));

	arp_timer = OFFSET_FROM_START(NULL, 20);
}

void SendArpReplyPkt(struct rte_mbuf *m, int lanif)
{
	struct rte_ether_hdr *eth_hdr;
	struct rte_arp_hdr *arp_hdr;
	struct rte_ether_addr d_addr;

	selipmac(lanif);

	eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);	

	arp_hdr = (struct rte_arp_hdr *)(
		(char *)eth_hdr + sizeof(struct rte_ether_hdr));

	arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
	rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
	// rte_ether_addr_copy(&bond_mac_addr, &eth_hdr->s_addr);
	memcpy(&eth_hdr->src_addr, SRC_MACADDR, RTE_ETHER_ADDR_LEN);	
	rte_ether_addr_copy(&arp_hdr->arp_data.arp_sha,
								&arp_hdr->arp_data.arp_tha);
	arp_hdr->arp_data.arp_tip = arp_hdr->arp_data.arp_sip;
	// rte_ether_addr_copy(&bond_mac_addr, &d_addr);
	memcpy(&d_addr, SRC_MACADDR, RTE_ETHER_ADDR_LEN);	
	rte_ether_addr_copy(&d_addr, &arp_hdr->arp_data.arp_sha);
	arp_hdr->arp_data.arp_sip = htonl(Sourceipaddrsel);

	int portid = l2fwd_dst_ports[lanif];
	send_schedule_pkt(portid, m);
	// printf("Sent the ARP Reply packet \n");

	// printf("Sent the ARP Reply packet src = %x dst = %x \n",htonl(arp_hdr->arp_data.arp_sip),
	// 														  htonl(arp_hdr->arp_data.arp_tip));
					

}


int arptipmatch(int lanif, uint32_t arp_tip)
{
	int match = 0;

	if(lanif == LAN_IF_ID)
	{
		if(arp_tip == LAN_IPADDR)
			match = 1;
	}
	else
	{
		if(arp_tip == TUNNEL_IPADDR)
			match = 1;
	}

	return match;
}

/// @brief Processing of ARP packets
/// @param m Holds the ARP data received
/// @param portid port over which ARP packet has received

void ProcessArpPkt(struct rte_mbuf *m, unsigned portid)
{
	struct rte_ether_hdr *eth_hdr;
	struct rte_arp_hdr *arp_hdr;

	eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	arp_hdr = (struct rte_arp_hdr *)((char *)eth_hdr + 
													sizeof(struct rte_ether_hdr));
	
	if(!tunnel_enable)
	{
		/* Fill the mac table and bypass the packet */
		checkandfillmacmem(arp_hdr);
		goto bypass_arppkt;
	}

	/* tunnel enabled case.... */	
	uint32_t arp_tip = ntohl(arp_hdr->arp_data.arp_tip);
	if(arptipmatch(portid, arp_tip) == 1)
	{
		uint16_t arp_opcode = ntohs(arp_hdr->arp_opcode);
		if(arp_opcode == RTE_ARP_OP_REPLY)
		{
			/* update the mac table and drop the packet */
			checkandfillmacmem(arp_hdr);
			// printf("Received the ARP Reply packet src = %x dst = %x \n",htonl(arp_hdr->arp_data.arp_sip),
			// 															htonl(arp_hdr->arp_data.arp_tip));
			if(portid == LAN_IF_ID) hc_lan_reachable = 1;
			else hc_wan_reachable = 1;

			goto arp_freembuf;
		}
		else if(arp_opcode == RTE_ARP_OP_REQUEST)
		{
			/* update the mac table, send the ARP reply and free the request mbuf */
			// printf("Received the ARP Request packet src = %x dst = %x \n",htonl(arp_hdr->arp_data.arp_sip),
			// 															  htonl(arp_hdr->arp_data.arp_tip));
			checkandfillmacmem(arp_hdr);
			SendArpReplyPkt(m, portid);
			// tunnel_reachable = 1;
			
			if(portid == LAN_IF_ID) hc_lan_reachable = 1;
			else hc_wan_reachable = 1;

			/* received mbuf is used for send mbuf, don't free it */
			return;			
			// goto arp_freembuf;
		}
		else
		{
			/* Just drop the packet */
			goto arp_freembuf;
		}					
	}
	else
	{
		/* TODO: this case to be seen.... */
		/* to be bypassed */
		checkandfillmacmem(arp_hdr);
		goto bypass_arppkt;
	}
	

	bypass_arppkt:
		apprxbuf = rte_pktmbuf_mtod(m, unsigned char *);
		apptxlen = m->pkt_len;
		rte_memcpy(apptxbuf, apprxbuf, apptxlen);					
		send_etherpkt(l2fwd_dst_ports[portid]);		
	
	arp_freembuf:
		rte_pktmbuf_free(m);
	
}

void arptimercheck()
{
	calculate_tcp_clock();
	
	int32_t diff = timer_diff(tcp_now, 0, arp_timer,0);
	Destipaddrsel = TUNNEL_GW;
	if(diff > 0)
	 SendArpRequestPkt(WAN_IF_ID);
}



int clientSocket;
struct sockaddr_in serverAddress;

static struct timeval last_time = {0, 0};
static uint64_t last_rx_bytes[2] = {0, 0};  // LAN, WAN
static uint64_t last_tx_bytes[2] = {0, 0};  // LAN, WAN
static double rx_throughput_mbps[2] = {0.0, 0.0};
static double tx_throughput_mbps[2] = {0.0, 0.0};

// Function to calculate throughput
static void calculate_throughput(void) {
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    
    if (last_time.tv_sec != 0) {
        double time_diff = (current_time.tv_sec - last_time.tv_sec) + 
                          (current_time.tv_usec - last_time.tv_usec) / 1000000.0;
        
        if (time_diff > 0) {
            // Calculate LAN throughput
            rx_throughput_mbps[0] = ((rx_port_stats[0].total_bytes - last_rx_bytes[0]) * 8.0) / 
                                   (time_diff * 1000000.0);
            tx_throughput_mbps[0] = ((tx_port_stats[0].total_bytes - last_tx_bytes[0]) * 8.0) / 
                                   (time_diff * 1000000.0);
            
            // Calculate WAN throughput
            rx_throughput_mbps[1] = ((rx_port_stats[1].total_bytes - last_rx_bytes[1]) * 8.0) / 
                                   (time_diff * 1000000.0);
            tx_throughput_mbps[1] = ((tx_port_stats[1].total_bytes - last_tx_bytes[1]) * 8.0) / 
                                   (time_diff * 1000000.0);
        }
    }
    
    // Update last values
    last_time = current_time;
    last_rx_bytes[0] = rx_port_stats[0].total_bytes;
    last_rx_bytes[1] = rx_port_stats[1].total_bytes;
    last_tx_bytes[0] = tx_port_stats[0].total_bytes;
    last_tx_bytes[1] = tx_port_stats[1].total_bytes;
}

// Function to ensure directory exists
static int ensure_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            printf("Error creating directory %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void fill_stats() {
	// by manikant
    // Calculate error stats
    error_stats[0].rx_err = rx_port_stats[0].total_pkts - rx_port_stats[0].good_pkts;
    error_stats[0].tx_err = tx_port_stats[0].total_pkts - tx_port_stats[0].good_pkts;
    error_stats[0].rx_missed_err = 0;
    error_stats[0].rx_crc_err = 0;
    error_stats[0].rx_fragment_err = 0;
    error_stats[0].tx_tso_err = 0;

    error_stats[1].rx_err = rx_port_stats[1].total_pkts - rx_port_stats[1].good_pkts;
    error_stats[1].tx_err = tx_port_stats[1].total_pkts - tx_port_stats[1].good_pkts;
    error_stats[1].rx_missed_err = 0;
    error_stats[1].rx_crc_err = 0;
    error_stats[1].rx_fragment_err = 0;
    error_stats[1].tx_tso_err = 0;

    // Calculate throughput
    calculate_throughput();

    // Ensure directory exists
    if (ensure_directory("/usr/local/dpdk") != 0) {
        return;
    }

    // Write to JSON file
    FILE *fp = fopen("/usr/local/dpdk/dpdk_stats.json", "w");
    if (!fp) {
        printf("Error opening file: %s\n", strerror(errno));
        return;
    }

    // Get current timestamp
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // Write JSON data
    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp\": \"%s\",\n", timestamp);
	fprintf(fp, "  \"firmware_version\": \"%s\",\n", "1.0.10");
	fprintf(fp, "  \"lan_gui_status\": %u,\n", lan_gui_status);
	fprintf(fp, "  \"wan_gui_status\": %u,\n", wan_gui_status);
	fprintf(fp, "  \"reset_gui_status\": %u,\n", reset_gui_status);
    fprintf(fp, "  \"lan_port\": {\n");
    fprintf(fp, "    \"rx_stats\": {\n");
    fprintf(fp, "      \"total_pkts\": %u,\n", rx_port_stats[0].total_pkts);
    fprintf(fp, "      \"good_pkts\": %u,\n", rx_port_stats[0].good_pkts);
    fprintf(fp, "      \"total_bytes\": %u,\n", rx_port_stats[0].total_bytes);
    fprintf(fp, "      \"good_bytes\": %u,\n", rx_port_stats[0].good_bytes);
    fprintf(fp, "      \"mcast_pkts\": %u,\n", rx_port_stats[0].mcast_pkts);
    fprintf(fp, "      \"throughput_mbps\": %.2f\n", rx_throughput_mbps[0]);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"tx_stats\": {\n");
    fprintf(fp, "      \"total_pkts\": %u,\n", tx_port_stats[0].total_pkts);
    fprintf(fp, "      \"good_pkts\": %u,\n", tx_port_stats[0].good_pkts);
    fprintf(fp, "      \"total_bytes\": %u,\n", tx_port_stats[0].total_bytes);
    fprintf(fp, "      \"good_bytes\": %u,\n", tx_port_stats[0].good_bytes);
    fprintf(fp, "      \"mcast_pkts\": %u,\n", tx_port_stats[0].mcast_pkts);
    fprintf(fp, "      \"throughput_mbps\": %.2f\n", tx_throughput_mbps[0]);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"error_stats\": {\n");
    fprintf(fp, "      \"rx_err\": %u,\n", error_stats[0].rx_err);
    fprintf(fp, "      \"tx_err\": %u,\n", error_stats[0].tx_err);
    fprintf(fp, "      \"rx_missed_err\": %u,\n", error_stats[0].rx_missed_err);
    fprintf(fp, "      \"rx_crc_err\": %u,\n", error_stats[0].rx_crc_err);
    fprintf(fp, "      \"rx_fragment_err\": %u,\n", error_stats[0].rx_fragment_err);
    fprintf(fp, "      \"tx_tso_err\": %u\n", error_stats[0].tx_tso_err);
    fprintf(fp, "    }\n");
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"wan_port\": {\n");
    fprintf(fp, "    \"rx_stats\": {\n");
    fprintf(fp, "      \"total_pkts\": %u,\n", rx_port_stats[1].total_pkts);
    fprintf(fp, "      \"good_pkts\": %u,\n", rx_port_stats[1].good_pkts);
    fprintf(fp, "      \"total_bytes\": %u,\n", rx_port_stats[1].total_bytes);
    fprintf(fp, "      \"good_bytes\": %u,\n", rx_port_stats[1].good_bytes);
    fprintf(fp, "      \"mcast_pkts\": %u,\n", rx_port_stats[1].mcast_pkts);
    fprintf(fp, "      \"throughput_mbps\": %.2f\n", rx_throughput_mbps[1]);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"tx_stats\": {\n");
    fprintf(fp, "      \"total_pkts\": %u,\n", tx_port_stats[1].total_pkts);
    fprintf(fp, "      \"good_pkts\": %u,\n", tx_port_stats[1].good_pkts);
    fprintf(fp, "      \"total_bytes\": %u,\n", tx_port_stats[1].total_bytes);
    fprintf(fp, "      \"good_bytes\": %u,\n", tx_port_stats[1].good_bytes);
    fprintf(fp, "      \"mcast_pkts\": %u,\n", tx_port_stats[1].mcast_pkts);
    fprintf(fp, "      \"throughput_mbps\": %.2f\n", tx_throughput_mbps[1]);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"error_stats\": {\n");
    fprintf(fp, "      \"rx_err\": %u,\n", error_stats[1].rx_err);
    fprintf(fp, "      \"tx_err\": %u,\n", error_stats[1].tx_err);
    fprintf(fp, "      \"rx_missed_err\": %u,\n", error_stats[1].rx_missed_err);
    fprintf(fp, "      \"rx_crc_err\": %u,\n", error_stats[1].rx_crc_err);
    fprintf(fp, "      \"rx_fragment_err\": %u,\n", error_stats[1].rx_fragment_err);
    fprintf(fp, "      \"tx_tso_err\": %u\n", error_stats[1].tx_tso_err);
    fprintf(fp, "    }\n");
    fprintf(fp, "  }\n");
    fprintf(fp, "}\n");

    fclose(fp);
	reset_gui_status = 0;
    // Console output (optional - remove for production if not needed)
    // printf("Stats written to /usr/local/dpdk/dpdk_stats.json\n");
    // printf("LAN RX: %.2f Mbps, TX: %.2f Mbps\n", rx_throughput_mbps[0], tx_throughput_mbps[0]);
    // printf("WAN RX: %.2f Mbps, TX: %.2f Mbps\n", rx_throughput_mbps[1], tx_throughput_mbps[1]);
}

void socket_init()
{
	
	clientSocket = socket(AF_INET, SOCK_DGRAM,0);
	
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
	serverAddress.sin_port = htons(6002);
				
}

void tcp_tc_init()
{
	uint32_t i;
	Num_tcp_tc = 0;
	printf("Default tc per site %d \n",DEFAULT_TC_PER_SITE);
	for(i=0;i<MAX_FIREWALL_RULES;i++)
	{		
		/* If Rule is disabled, skip the rule */
		if(firewall_lut[i].Enbl == 0)
			continue;

		/* Stores TCP matched queues and default queue */
		if( (firewall_lut[i].Proto == 0x06) || 
			(firewall_lut[i].Proto == 0x00) ||
			(firewall_lut[i].tcNum == DEFAULT_TC_PER_SITE)
		  )
		{
			TCP_TC[Num_tcp_tc] = (firewall_lut[i].SiteNum * 16) + firewall_lut[i].tcNum;
			Num_tcp_tc++;
		}	
	}

	printf("TCP TC's = %d \n",Num_tcp_tc);	
	for(i=0;i<Num_tcp_tc;i++)
	{
		printf("TCP_TC[%d] = %d \n",i,TCP_TC[i]);	
	}
}


void
app_rx_thread(struct thread_conf **confs)
{
	uint32_t i, nb_rx;
	struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned;
	struct thread_conf *conf;
	int conf_idx = 0;
	int conf_idx1;

	uint32_t subport;
	uint32_t pipe;
	uint32_t traffic_class;
	uint32_t queue;
	uint32_t color;

	// struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	// struct rte_mbuf *pkts_burst_rx[MAX_PKT_BURST];
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned j, portid;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US_RX;
	struct rte_eth_dev_tx_buffer *buffer;

	struct rte_ether_hdr *eth_hdr;
	struct rte_arp_hdr *arp_pkt;
	uint32_t packet_type = RTE_PTYPE_UNKNOWN;
	uint16_t ether_type;
	void *l3, *l4, *l5;
	int hdr_len;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_ipv4_hdr *inner_ipv4_hdr;
	// struct rte_flow_item_gre *gre_hdr;
	struct rte_gre_hdr *gre_hdr;
	struct rte_ipv6_hdr *ipv6_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct rtphdr1 *rtp_hdr;
	//unsigned char *apprxbuf;
	struct mbuf *m_read;
	unsigned initial_print_en = 1;
	uint32_t d_loop = 0;
	// const uint64_t mscnt = rte_get_timer_hz()/1000; /* around 1ms */	
	const uint64_t mscnt = rte_get_timer_hz()/1000; /* around 10us */	
	uint64_t prev_tsc_ms, diff_tsc_ms;
	uint64_t ol_flags;
	uint8_t pkt_integrity_fail;
	uint8_t match = 0;
	uint16_t ip_len = 0;
	uint16_t loop1;

	uint16_t frag_field_early;                          /* ← ADD */
    int      is_frag_early;                             /* ← ADD */

	uint32_t timerrcvd=0;
	uint32_t prev_timerrcvd=0;
	uint32_t num_iterations = 0;
	struct rte_sched_queue_stats stats;
	uint32_t qlen;
	uint16_t l4_offset;
	uint8_t gre_len;
	uint8_t retrieve_asmuch_rxd;
	uint32_t clk1;
	uint32_t clk2;
	uint64_t pkt_rx4_cntr = 0;
	uint64_t pkt_rx5_cntr = 0;

	prev_tsc = 0;
	timer_tsc = 0;
	timer1mscnt = 0;
	prev_tsc_ms = 0;
	
	tcp_tc_init();
	socket_init();	
	tcp_init();
	Rohc_init();	
	usleep(1000);

	if (ip_frag_reasm_init() != 0) {                    /* ← ADD */
        printf("[FRAG] Fatal: fragment table init failed.\n"); /* ← ADD */
        return;                                         /* ← ADD */
    }                                                   /* ← ADD */



	//uint8_t cdev_id = PL_comp_decomp_init();
	//if(cdev_id <= 0)
	//{
	//	printf("Payload compression initialization failed \n");
	//}
	
	// Initialize the mac memory to 0.
	Init_macmem();

	// printf("In app rx thread function mscnt 25-09-25 %d\n",mscnt);

	printf("Firmware version 1.0.12 dated: 07-05-2026\n");
	//return;	


	while ((conf = confs[conf_idx]) ) 
	{
		again:
		if(qos_conf[0].wt_thread.profile_change == 1)
		{
			// printf("Going to skip the rx thread processing \n");
			goto skip_rx_thread;
		}
		// if(profile_change_thread_launch == 1)
		// {
		// 	/* when profile is changed, close this function */
		// 	/* Main core will launch the function once again... */
		// 	printf("***************************** Closing the app rx thread \n");
		// 	break;
		// }
		cur_tsc = rte_rdtsc();
		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) 
		{		
			prev_tsc = cur_tsc;
		}

		diff_tsc_periodic = cur_tsc - prev_tsc_periodic;
		if(diff_tsc_periodic >= 5000*mscnt)
		{		
			// print_enable = 1;	
			// calculate_tcp_clock();
			// printf("periodic timer1 %d timerlistcnt %d datacheckcnt %d \n",(tcp_now-clk2),timerlist_cnt,
			//  														datacheck_cnt);						
			// printf("\n");	
			// TCP_highwatt();														
			clk2 = tcp_now;
			timerlist_cnt = 0;
			datacheck_cnt = 0;
			lanrx_cnt = 0;
			wanrx_cnt = 0;
			lantx_cnt = 0;
			wantx_cnt = 0;
			debug_cnt1 = 0;
			debug_cnt2 = 0;
			debug_cnt3 = 0;
			debug_cnt4 = 0;

			fill_stats();
			// send_l2_pkt();

			prev_tsc_periodic = cur_tsc;			
		}

		diff_tsc_timerlist = cur_tsc - prev_tsc_timerlist;		
		if(diff_tsc_timerlist >= ((TIMER_RESOLUTION*1)*mscnt))
		{			
			tcp_run_timerlist(TIMER_RESOLUTION);
			prev_tsc_timerlist = cur_tsc;	
			timerlist_cnt++;		
		}

		diff_tsc_ms = cur_tsc - prev_tsc_ms;
		if(diff_tsc_ms > mscnt)
		{
			timer1mscnt++;

			ip_frag_reasm_drain_deathrow();             /* ← ADD */

			/* 1 means - once in every 10us */
			if(!(timer1mscnt%5000))
			{
				/* send ARP request once in every 5sec, 
				   if tunnel disabled, dont send 
				   if tunnel is reachable dont send here 
				*/
				if(tunnel_enable && (!hc_lan_reachable) )
				{
					Destipaddrsel = LAN_GW;
					SendArpRequestPkt(LAN_IF_ID);
					// display_mac_table();
				}
				if(tunnel_enable && (!hc_wan_reachable) )
				{
					Destipaddrsel = TUNNEL_GW;
					SendArpRequestPkt(WAN_IF_ID);
					// display_mac_table();
				}
			}
			if(!(timer1mscnt%60000))
			{
				/* Periodic ARP request once in every 60 sec, 
				   if tunnel disabled, dont send 
				*/
				if(tunnel_enable)
				{
					Destipaddrsel = LAN_GW;
					SendArpRequestPkt(LAN_IF_ID);

					Destipaddrsel = TUNNEL_GW;
					SendArpRequestPkt(WAN_IF_ID);
					// display_mac_table();
				}
			}
			// if(!(timer1mscnt % 10))
			// {
			// 	usleep(1);
			// }
			if(!(timer1mscnt % 500))
			{
				tcp_update_idealsize();
			}
			// if(!(timer1mscnt%15000000))
			// {
			// 	while(qtail(data_rx_tp_wait_queue_lan) != NULL)
			// 	{		
			// 		/* Get the head of the queue (head = tail's next)*/
			// 		struct data_tp_wait *tp_wait; 
			// 		tp_wait = getq_data_receive_tp_wait(data_rx_tp_wait_queue_lan);
			// 		printf("Index %d \n",tp_wait->tp->t_tcblut->index);
			// 	}
			// }
			// if(!(timer1mscnt%1000))
			// {
			// 	arptimercheck();
			// }
			
			// pthread_create()

			// if(!(timer1mscnt%200))
			// {
			// 	capture_mac_entry = 1;
			// }
			// if(!(timer1mscnt%10))
			// {
			// 	TCP_tx_rx_data_check();

			// 	//Qos_queue_len(&qlen);
			// 	//QUEUE_LEN = qlen;
			// }
			if(!(timer1mscnt%1000))
			{			
				// print_enable = 1;
				// print_enable1 = 1;
				//rte_sched_queue_read_stats(qos_conf[0].sched_port, 49, &stats, &qlen);	
				//Qos_queue_len(&qlen);				
				TCP_highwatt();	
				uint32_t clk1;
				// calculate_tcp_clock();
				// printf("tcp_now-clk3 %d \n",tcp_now-clk1);
				// printf("mscnt %d diff_tsc_periodic %d \n",(tcp_now-clk1),diff_tsc_periodic);
				clk1 = tcp_now;	
				// display_mac_table();				
				// printf("qlen lanrx %d wantx %d \n",qlen(data_rx_tp_wait_queue_lan),qlen(data_tx_tp_wait_queue_wan));
				// printf("qtail lanrx %d wantx %d \n",qtail(data_rx_tp_wait_queue_lan),qtail(data_tx_tp_wait_queue_wan));	
				//printf("qlen lantx %d wantx %d \n",qlen(data_tx_tp_wait_queue_lan),qlen(data_tx_tp_wait_queue_wan));	
				// printf("qtail lantx %x wantx %x \n",qtail(data_tx_tp_wait_queue_lan),qtail(data_tx_tp_wait_queue_wan));	
				// printf("total %d, peak %d, floor %d \n",total_sbmb_cnt, total_sbmb_cnt_peak, total_sbmb_cnt_floor);
				// printf("lanrx %d, wantx %d  \n",lanrx_cnt, wantx_cnt);
				// printf("wanrx %d, lantx %d  \n",wanrx_cnt, lantx_cnt);
				// printf("total %d, \n",lantx_cnt+wantx_cnt);
				// lanrx_cnt = 0;
				// wanrx_cnt = 0;
				// lantx_cnt = 0;
				// wantx_cnt = 0;
				// rohc_comp_print_stats3(0);
				// rohc_decomp_print_stats3(0);
			}
			if(!(timer1mscnt%10))
			{
				/* 
				 * Every 10ms, 4000 sessions will be checked,
				 * and in 10ms, only 20 seesions will be closed
				 * so total of 2000 sessions will be closed in a second
			     */
				sys_close_check();
			}
			// if(!(timer1mscnt%(TIMER_RESOLUTION*100)))
			// {
			// 	tcp_run_timerlist(TIMER_RESOLUTION);
			// }
			if(!(timer1mscnt%(TIMER_RESOLUTION_ROHC*1)))
			{
				/* To delete the context index, if no activity */
				Rohc_run_timer(TIMER_RESOLUTION_ROHC);
			}
			if(!(timer1mscnt%(TIMER_RESOLUTION_ROHC*1))) // if(!(timer1mscnt%20))
			{
				rohc_piggyback_feedback_timer();
			}
			if(!(timer1mscnt%(TIMER_RESOLUTION_ROHC*1)))
			{	
				/* Packet coalescing timeout */
				/* Coalesced pkt will be sent, if bytes configured rcvd or user confgired timeout occurs */			
				packet_coalesce_timer_check();
			}
			
			if(!(timer1mscnt%(TIMER_RESOLUTION*1)))
			{
				int i;
				for (i = 0; i < MAX_TCP_CONN; i++)
				{
					if (TCP_lut[i].isStored != 1) continue;
					struct tcpcb *tp = TCP_lut[i].tp;
					if (tp == NULL) continue;
					if (tp->t_tcblut->lanif != LAN_IF_ID) continue;
					byte_caching_timer_check(&tp->m1_state);
					if (tp->m1_state.tlv_pending != NULL)
					{
						printf("state %d \n",tp->t_state);
						int32_t proxy_idx = tp->t_tcblut->index_proxy;
						if (proxy_idx >= 0 && TCP_lut[proxy_idx].tp != NULL)
						{
							struct socket *so2 = TCP_lut[proxy_idx].tp->sock;
							if (so2 != NULL)
							{
								// printf("[TIMER] Sending partial TLV conn=%d len=%u\n",
								// 	i, tp->m1_state.tlv_pending->m_pkthdr.len);
								//fflush(stdout);
								tcp_usr_send(so2, tp->m1_state.tlv_pending);
							}
							else
							{
								m_freem(tp->m1_state.tlv_pending);
							}
						}
						else
						{
							m_freem(tp->m1_state.tlv_pending);
						}
						tp->m1_state.tlv_pending = NULL;
					}
				}
			}
			
			/* needs to add loop for greater than 1ms*/
			prev_tsc_ms = cur_tsc;
		}

		portid = conf->rx_port;	

		TCP_tx_rx_data_check();	
		datacheck_cnt++;

		// if(portid == 0)
		// 	nb_rx = rte_eth_rx_burst(conf->rx_port, conf->rx_queue, rx_mbufs,
		// 		2);
		// else
		
	loop_retrieve_asmuch:

		nb_rx = rte_eth_rx_burst(conf->rx_port, conf->rx_queue, rx_mbufs,
			burst_conf.rx_burst);			

		if(nb_rx == burst_conf.rx_burst)		
			retrieve_asmuch_rxd = 1;		
		else
			retrieve_asmuch_rxd = 0;
		
		for (j = 0; j < nb_rx; j++) 
		{
			// printf("Received pkt over port %d  \n",portid);
			m = rx_mbufs[j];

			rx_port_stats[portid].total_pkts++;
			rx_port_stats[portid].total_bytes += m->pkt_len;
			
			if(m->pkt_len < 64)
			{
				error_stats[portid].rx_undersize_err++;
			}
			else if(m->pkt_len > 1518)
			{
				error_stats[portid].rx_oversize_err++;
				error_stats[portid].rx_jabber_err++;
			}

			eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
			ether_type = ntohs(eth_hdr->ether_type);						
			
			if(ether_type == 0x0806)
			{	
				rx_port_stats[portid].good_pkts++;
				rx_port_stats[portid].good_bytes += m->pkt_len;			
				// printf("Received ARP pkt over port %d  \n",portid);
				ProcessArpPkt(m, portid);
			}
			else if(ether_type == 0x0800)
			{
				l3 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
				ipv4_hdr = (struct rte_ipv4_hdr *)l3;
				l4 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
				udp_hdr = (struct rte_udp_hdr *)l4;

				// ip_len = ntohs(ipv4_hdr->total_length);
				// if( (m->pkt_len != (ip_len+14)) &&
				// 	(ip_len > 46)  ) 
				// {
				// 	/* data length mismatch */

				// 	error_stats[portid].rx_len_err++;

				// 	printf("RX Pkt len and ip len mismatch %d %d portid %d\n",m->pkt_len,(ip_len+14),portid);
				// 	// apprxbuf = rte_pktmbuf_mtod(m, unsigned char *);
				// 	// for(int p222=0;p222<m->pkt_len;p222++)
				// 	// {
				// 	// 	printf("%x\t",apprxbuf[p222]);
				// 	// }
				// 	// printf("\n");
				// 	rte_pktmbuf_free(m);
				// 	goto default2;
				// }

				/*Modification*/

			ip_len = ntohs(ipv4_hdr->total_length);

            /* Early fragment probe — needed to skip the length check.
             * A non-first fragment's pkt_len < full ip_len by design. */
            frag_field_early = rte_be_to_cpu_16(          /* ← ADD */
                                   ipv4_hdr->fragment_offset); /* ← ADD */
            is_frag_early = (frag_field_early & RTE_IPV4_HDR_MF_FLAG) || /* ← ADD */
                            (frag_field_early & RTE_IPV4_HDR_OFFSET_MASK); /* ← ADD */

            if( !is_frag_early &&                         /* ← MODIFY (add !is_frag_early &&) */
                (m->pkt_len != (ip_len+14)) &&
                (ip_len > 46)  )
            {
                /* data length mismatch */
                error_stats[portid].rx_len_err++;
                printf("RX Pkt len and ip len mismatch %d %d portid %d\n",
                       m->pkt_len,(ip_len+14),portid);
                rte_pktmbuf_free(m);
                goto default2;
            }






			#ifdef ARM_ARCH
				ol_flags = m->ol_flags;
				if( (ol_flags & PKT_RX_IP_CKSUM_MASK) != 0X80 )
				{
					/* IP checksum mismatch */
					printf("IP checksum mismatch \n");
					rte_pktmbuf_free(m);
					goto default2;

				}
			#endif

				// if(capture_mac_entry)
				// {
				// 	checkandfillmacmem2(eth_hdr, ipv4_hdr);
				// 	capture_mac_entry = 0;
				// }
				

				/*Modification*/

            /* ══════════════════════════════════════════════════════════════
             *  OUTER IP FRAGMENT REASSEMBLY  — POST-TUNNELING case
             *
             *  Post-tunneling: the router fragmented the already-encapsulated
             *  GRE packet, so MF/offset is in the OUTER IP header.
             *  Non-first fragments carry no GRE header, so reassembly MUST
             *  happen here before any GRE / inner-IP parsing.
             *
             *  Pre-tunneling: the router fragmented the plain IP payload BEFORE
             *  GRE encapsulation, so MF/offset is in the INNER IP header.
             *  Each fragment still has a complete outer IP + GRE header, so
             *  the outer IP will NOT look like a fragment here. Inner-IP
             *  reassembly is handled further below, after GRE parsing.
             *
             *  DPDK contract: rte_ipv4_frag_reassemble_packet() requires the
             *  mbuf to start at the IP header (L2 stripped). We adj() off the
             *  Ethernet header, reassemble, then prepend it back.
             * ══════════════════════════════════════════════════════════════ */
            if (outer_ip_is_fragment(ipv4_hdr))
            {
                error_stats[portid].rx_fragment_err++;

                if (print_enable) {
                    uint16_t off = rte_be_to_cpu_16(ipv4_hdr->fragment_offset);
                    printf("[FRAG] port=%d id=0x%04x MF=%d off=%u "
                           "src=%08x dst=%08x pktlen=%u\n",
                           portid,
                           rte_be_to_cpu_16(ipv4_hdr->packet_id),
                           !!(off & RTE_IPV4_HDR_MF_FLAG),
                           (uint32_t)(off & RTE_IPV4_HDR_OFFSET_MASK) * 8,
                           rte_be_to_cpu_32(ipv4_hdr->src_addr),
                           rte_be_to_cpu_32(ipv4_hdr->dst_addr),
                           m->pkt_len);
                }

                /* Save L2, then strip it so mbuf points at IP header. */
                struct rte_ether_hdr saved_eth = *eth_hdr;

                if (rte_pktmbuf_adj(m, sizeof(struct rte_ether_hdr)) == NULL) {
                    rte_pktmbuf_free(m);
                    goto default2;
                }

                m->l3_len = (ipv4_hdr->version_ihl & 0x0f) * 4;

                uint64_t cur_tsc_frag = rte_rdtsc();

                m = rte_ipv4_frag_reassemble_packet(
                        g_frag_tbl[portid],
                        &g_frag_death_row[portid],
                        m,
                        cur_tsc_frag,
                        ipv4_hdr);

                if (g_frag_death_row[portid].cnt != 0)
                    rte_ip_frag_free_death_row(&g_frag_death_row[portid],
                                               FRAG_PREFETCH_OFFSET);

                if (m == NULL) {
                    if (print_enable)
                        printf("[FRAG] port=%d incomplete, awaiting frags\n",
                               portid);
                    goto default2;
                }

                if (m->next != NULL) {
                    if (rte_pktmbuf_linearize(m) != 0) {
                        printf("[FRAG] linearize failed, pkt dropped\n");
                        rte_pktmbuf_free(m);
                        goto default2;
                    }
                }

                /* Restore Ethernet header in front of reassembled datagram. */
                eth_hdr = (struct rte_ether_hdr *)
                          rte_pktmbuf_prepend(m, sizeof(struct rte_ether_hdr));
                if (eth_hdr == NULL) {
                    printf("[FRAG] no headroom to restore L2, dropped\n");
                    rte_pktmbuf_free(m);
                    goto default2;
                }
                *eth_hdr = saved_eth;

                l3       = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
                ipv4_hdr = (struct rte_ipv4_hdr *)l3;
                l4       = (uint8_t *)l3 + sizeof(struct rte_ipv4_hdr);
                udp_hdr  = (struct rte_udp_hdr *)l4;
                ip_len   = ntohs(ipv4_hdr->total_length);

                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = (ipv4_hdr->version_ihl & 0x0f) * 4;

                if (print_enable)
                    printf("[FRAG] port=%d REASSEMBLED ok: nb_segs=%u "
                           "pkt_len=%u ip_total=%u\n",
                           portid, m->nb_segs, m->pkt_len, ip_len);

                /* Do NOT increment good_pkts here; the reassembled packet
                 * continues down the normal 0x0800 path and is counted there. */
            }
            /* ══ END FRAGMENT REASSEMBLY ══ */
             


				if(ipv4_hdr->next_proto_id == 0x2F)
				{
					/* GRE header */
					gre_hdr = (struct rte_gre_hdr *)l4;
					gre_len = 4;
					if(gre_hdr->c)gre_len += 4; /* checksum present bit */
					if(gre_hdr->k)gre_len += 4; /* key present bit */
					if(gre_hdr->s)gre_len += 4; /* sequence number present bit */
					inner_ipv4_hdr = (struct rte_ipv4_hdr *)(l4 + gre_len);
					udp_hdr = (struct rte_udp_hdr *)(l4 + gre_len + sizeof(struct rte_ipv4_hdr));
					
					l4_offset = 34+gre_len+20; /* 14+ 20+ 4 to 16 bytes+ 20 */

					/* throughout the code, gre_len is considered as GRE HDR + Inner IP HDR length */
					gre_len += 20; 

					// printf("gre_len %d next protocol %x \n",gre_len,inner_ipv4_hdr->next_proto_id);
					
				}
				else
				{
					gre_hdr = NULL;
					inner_ipv4_hdr = ipv4_hdr;
					// udp_hdr remains same....
					l4_offset = 34; /* 14+ 20  */
					gre_len = 0;

				}
				// printf("gre_len %d next protocol %x \n",gre_len,inner_ipv4_hdr->next_proto_id);

				/* ══════════════════════════════════════════════════════════════
				 *  INNER IP FRAGMENT REASSEMBLY  — PRE-TUNNELING case
				 *
				 *  Pre-tunneling: the router set MF/offset in the INNER IP
				 *  header before GRE encapsulation.  Each arriving mbuf has a
				 *  full outer IP + GRE header, so the outer reassembly block
				 *  above was a no-op.  We must now reassemble the inner
				 *  datagrams here.
				 *
				 *  After reassembly the packet continues the normal protocol
				 *  dispatch path and is handed to the IP encryptor, which
				 *  always performs POST-tunneling (sets MF only in the outer
				 *  IP header).
				 *
				 *  Inner reassembly is only applicable when a GRE tunnel is
				 *  present (gre_hdr != NULL).  Without a tunnel there is no
				 *  separate inner header, so this block is skipped.
				 * ══════════════════════════════════════════════════════════════ */
				if (gre_hdr != NULL && inner_ip_is_fragment(inner_ipv4_hdr))
				{
					error_stats[portid].rx_fragment_err++;

					if (print_enable) {
						uint16_t ioff = rte_be_to_cpu_16(inner_ipv4_hdr->fragment_offset);
						printf("[INNER-FRAG] port=%d id=0x%04x MF=%d off=%u "
						       "src=%08x dst=%08x pktlen=%u\n",
						       portid,
						       rte_be_to_cpu_16(inner_ipv4_hdr->packet_id),
						       !!(ioff & RTE_IPV4_HDR_MF_FLAG),
						       (uint32_t)(ioff & RTE_IPV4_HDR_OFFSET_MASK) * 8,
						       rte_be_to_cpu_32(inner_ipv4_hdr->src_addr),
						       rte_be_to_cpu_32(inner_ipv4_hdr->dst_addr),
						       m->pkt_len);
					}

					/*
					 * Byte offset to the inner IP header from the start of the mbuf:
					 *   Eth(14) + outer-IP(20) + raw-GRE-only bytes
					 * gre_len at this point = raw_gre_bytes + sizeof(inner-IP-hdr)(20),
					 * so raw_gre_only = gre_len - 20.
					 *   inner_ip_offset = 14 + 20 + (gre_len - 20) = 14 + gre_len
					 *
					 * We save everything before the inner IP header so it can be
					 * restored after reassembly.
					 */
					uint16_t inner_ip_offset = (uint16_t)(sizeof(struct rte_ether_hdr)
					                            + sizeof(struct rte_ipv4_hdr)
					                            + (gre_len - (uint16_t)sizeof(struct rte_ipv4_hdr)));

					uint8_t saved_l2_gre[inner_ip_offset];
					rte_memcpy(saved_l2_gre,
					           rte_pktmbuf_mtod(m, uint8_t *),
					           inner_ip_offset);

					/* Strip the Eth + outer-IP + GRE prefix so mbuf points at inner IP. */
					if (rte_pktmbuf_adj(m, inner_ip_offset) == NULL) {
						rte_pktmbuf_free(m);
						goto default2;
					}

					m->l3_len = (inner_ipv4_hdr->version_ihl & 0x0f) * 4;

					uint64_t cur_tsc_inner = rte_rdtsc();

					/*
					 * Use a dedicated per-port inner fragment table so inner and
					 * outer reassembly flows never collide (different IP-IDs,
					 * different address pairs, different flow keys).
					 */
					m = rte_ipv4_frag_reassemble_packet(
					        g_inner_frag_tbl[portid],
					        &g_inner_frag_death_row[portid],
					        m,
					        cur_tsc_inner,
					        inner_ipv4_hdr);

					if (g_inner_frag_death_row[portid].cnt != 0)
						rte_ip_frag_free_death_row(&g_inner_frag_death_row[portid],
						                           FRAG_PREFETCH_OFFSET);

					if (m == NULL) {
						if (print_enable)
							printf("[INNER-FRAG] port=%d incomplete, awaiting inner frags\n",
							       portid);
						goto default2;
					}

					if (m->next != NULL) {
						if (rte_pktmbuf_linearize(m) != 0) {
							printf("[INNER-FRAG] linearize failed, inner pkt dropped\n");
							rte_pktmbuf_free(m);
							goto default2;
						}
					}

					/* Restore the Eth + outer-IP + GRE prefix in front of the
					 * reassembled inner datagram. */
					uint8_t *restored = (uint8_t *)rte_pktmbuf_prepend(m, inner_ip_offset);
					if (restored == NULL) {
						printf("[INNER-FRAG] no headroom to restore L2+GRE, dropped\n");
						rte_pktmbuf_free(m);
						goto default2;
					}
					rte_memcpy(restored, saved_l2_gre, inner_ip_offset);

					/* Refresh all header pointers after mbuf surgery. */
					eth_hdr        = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
					l3             = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
					ipv4_hdr       = (struct rte_ipv4_hdr *)l3;
					l4             = (uint8_t *)l3 + sizeof(struct rte_ipv4_hdr);
					/* raw_gre_only = gre_len - sizeof(inner-IP-hdr) */
					inner_ipv4_hdr = (struct rte_ipv4_hdr *)(
					                     (uint8_t *)l4 +
					                     (gre_len - (uint16_t)sizeof(struct rte_ipv4_hdr)));
					udp_hdr        = (struct rte_udp_hdr *)(
					                     (uint8_t *)inner_ipv4_hdr +
					                     sizeof(struct rte_ipv4_hdr));
					ip_len         = ntohs(ipv4_hdr->total_length);

					/*
					 * Fix the outer IP total_length to cover the now-larger
					 * reassembled inner payload.  The encryptor (post-tunneling)
					 * will re-fragment the outer header as needed.
					 *   new_outer_total = outer-IP-hdr(20)
					 *                   + raw-GRE-only bytes
					 *                   + reassembled inner IP total_length
					 */
					uint16_t new_outer_total = (uint16_t)(
					    sizeof(struct rte_ipv4_hdr)
					    + (gre_len - (uint16_t)sizeof(struct rte_ipv4_hdr))
					    + ntohs(inner_ipv4_hdr->total_length));
					ipv4_hdr->total_length = rte_cpu_to_be_16(new_outer_total);
					/* Recompute outer IP checksum after length update. */
					ipv4_hdr->hdr_checksum = 0;
					ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

					m->l2_len = sizeof(struct rte_ether_hdr);
					m->l3_len = (ipv4_hdr->version_ihl & 0x0f) * 4;

					if (print_enable)
						printf("[INNER-FRAG] port=%d INNER REASSEMBLED ok: "
						       "pkt_len=%u outer_ip_total=%u inner_ip_total=%u\n",
						       portid, m->pkt_len,
						       ntohs(ipv4_hdr->total_length),
						       ntohs(inner_ipv4_hdr->total_length));
				}
				/* ══ END INNER FRAGMENT REASSEMBLY ══ */

				uint32_t dst_addr1 = ntohl(inner_ipv4_hdr->dst_addr);
				uint8_t ip_msb = (dst_addr1 >> 24) & 0xFF;
				if(inner_ipv4_hdr->dst_addr == 0xFFFFFFFF)
				{
					rx_port_stats[portid].bcast_pkts++;		
				}
				else
				{
					if(ip_msb >= 224 && ip_msb <= 239)
					{
						/* multicast ip range 224.0.0.0 to 239.255.255.255 */
						rx_port_stats[portid].mcast_pkts++;	
									
					}
				}
				
				if(inner_ipv4_hdr->next_proto_id == 0x01)
				{
					/* ICMP PING packet */

					rx_port_stats[portid].good_pkts++;
					rx_port_stats[portid].good_bytes += m->pkt_len;	

					apprxbuf = rte_pktmbuf_mtod(m, unsigned char *);
					apptxlen = m->pkt_len;
					rte_memcpy(apptxbuf, apprxbuf, apptxlen);					
					send_etherpkt(l2fwd_dst_ports[portid]);	
					rte_pktmbuf_free(m);	
				}
				else if(inner_ipv4_hdr->next_proto_id == 0x04)
				{
					rx_port_stats[portid].good_pkts++;
					rx_port_stats[portid].good_bytes += m->pkt_len;	

					unsigned char *rxbuf = rte_pktmbuf_mtod(m, unsigned char *);
					apptxlen = m->pkt_len;
					//printf("rcvd pkt len %d \n",apptxlen);
					// for(int p1=0;p1<10;p1++)
					// 	printf("%x ",*(rxbuf+p1));
					// printf("\n");
					//test_comp_init(apprxbuf+34,apptxlen-34);
					//test_decompressdev_deflate_stateless_fixed(rxbuf,apptxlen,34,portid);
					//test_compressdev_deflate_stateless_fixed(1,rxbuf,apptxlen,34,portid);
					rte_pktmbuf_free(m);
				}
				else if(inner_ipv4_hdr->next_proto_id == 0x06)
				{					
					/* The below lines of code is to simulate incoming pkt drop */					
					// if(drop_packet != 0)
					// {
					// 	if(portid == 1)
					// 	//if(portid == 0)
					// 	{
					// 		// if(drop_toggle == 1)
					// 		{
					// 			// printf("Packet dropped cnt %d \n",drop_packet);
					// 			// drop_packet = 0;
					// 			rte_pktmbuf_free(m);
					// 			drop_packet--;
					// 			if(drop_packet < 0)
					// 				drop_packet = 0;

					// 			drop_toggle = 0;	
					// 			goto default2;
					// 		}
					// 		// else
					// 		// 	drop_toggle = 1;						
							
					// 	}
					// }

					/* The below lines of code is to bypass the Custom TCP protocol */
					// rte_prefetch0(rte_pktmbuf_mtod(m, void *));
					// l2fwd_simple_forward(m, portid);
					// goto default2;

					// ********************************************************************
					// ********************************************************************
					// ********************************************************************
					// ********************************************************************
					// 		TCP Checksum needs to be verified internally by our code
					// ********************************************************************
					// ********************************************************************
					// ********************************************************************
					// ********************************************************************

					apprxbuf = rte_pktmbuf_mtod(m, unsigned char *);
					//printf("ol_flags64 = %x ol_flags32 = %x \n",m->ol_flags>>32, m->ol_flags);

				#ifdef ARM_ARCH
					if((ol_flags & PKT_RX_L4_CKSUM_MASK) != 0x100)
					{
						/* L4 checksum mismatch */
						printf("L4(TCP) checksum mismatch \n");
						rte_pktmbuf_free(m);
						goto default2;
					}	
					else{	/* L4 checksum match */ }
				#else
					match = verify_tcp_checksum(apprxbuf+l4_offset, inner_ipv4_hdr, ntohs(inner_ipv4_hdr->total_length));
					if(match == 0)
					{
						/* L4 checksum mismatch */
						printf("L4(TCP1) checksum mismatch \n");
						rte_pktmbuf_free(m);
						goto default2;
					}
				#endif

				// printf("TCP pkt rcvd \n");

					rx_port_stats[portid].good_pkts++;
					rx_port_stats[portid].good_bytes += m->pkt_len;	
												
					apptxlen = m->pkt_len;						
					//test_compressdev_deflate_stateless_fixed(1,apprxbuf,apptxlen,42,portid);
					//uint8_t tcp_decompressible =  check_tcp_decompressable(apprxbuf, apptxlen, portid, gre_len);
					uint8_t tcp_decompressible = 0;
					unsigned int tcplen,tcp_payload_len;
					uint8_t tcphlen;
					uint16_t total_len;

					tcplen = ((apprxbuf[gre_len+16]<<8) | apprxbuf[gre_len+17]) - 20;
					tcphlen = ((apprxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
					
					tcp_payload_len = tcplen - tcphlen;		
					// if(tcp_decompressible == 0)	
					{
						// m_read = m_devget(apprxbuf+gre_len+14, ntohs(inner_ipv4_hdr->total_length), ETHER_ALIGN, portid); 
						if(tcp_payload_len == 0)
						{
							m_read = m_devget(apprxbuf+gre_len+14, ntohs(inner_ipv4_hdr->total_length), 0, portid); 
						}
						else
						{
							/* If data pkt, after decompression, size will increase, so we have to allocate 
							 * a cluster, but data copying will be maximum of MINCLSIZE and pkt len. 
							 */
							total_len = ntohs(inner_ipv4_hdr->total_length);
							if(total_len < MINCLSIZE)
							{
								m_read = m_devget(apprxbuf+gre_len+14, MINCLSIZE, 0, portid); 
								m_read->m_pkthdr.len = total_len;
								m_read->m_len = total_len;
								// printf("condition1 \n");
							}
							else
							{
								m_read = m_devget(apprxbuf+gre_len+14, ntohs(inner_ipv4_hdr->total_length), 0, portid); 
							}

						}
						if(m_read != NULL)
						{
							/* no decompression */
							// 1- rcvd pkt in mbuf 2- IP header length including options 
							//printf("Going to TCP Input \n");
							if(gre_len)
								tcp_input(eth_hdr, m_read, ipv4_hdr, (inner_ipv4_hdr->version_ihl&0xF)*4);
							else
								tcp_input(eth_hdr, m_read, NULL, (inner_ipv4_hdr->version_ihl&0xF)*4);
						}
					}
					// else if(tcp_decompressible == 1)						
					// {
					// 	/* decompression success */
					// 	uint16_t decomp_ip_len = (apptxbuf[gre_len+16] << 8) | apptxbuf[gre_len+17];
					// 	// m_read = m_devget(apptxbuf+gre_len+14, decomp_ip_len, ETHER_ALIGN, portid); 
					// 	m_read = m_devget(apptxbuf+gre_len+14, decomp_ip_len, 0, portid); 
					// 	if(m_read != NULL)
					// 	{
					// 		// printf("Processing TCP pkt rcvd decomp_ip_len %d \n",decomp_ip_len);
					// 		// 1- rcvd pkt in mbuf 2- IP header length including options 								
					// 		//tcp_input(eth_hdr, m_read, (ipv4_hdr->version_ihl&0xF)*4);
					// 		if(gre_len)
					// 			tcp_input(eth_hdr, m_read, ipv4_hdr, 20);
					// 		else
					// 			tcp_input(eth_hdr, m_read, NULL, 20);
					// 	}
					// }
					// else
					// {
					// 	/* decompression failed */
					// 	rte_pktmbuf_free(m);
					// 	goto default2;
					// }

					default1:
						rte_pktmbuf_free(m);
				}
				else if(inner_ipv4_hdr->next_proto_id == 0x11)
				{		
					rx_port_stats[portid].good_pkts++;
					rx_port_stats[portid].good_bytes += m->pkt_len;				
					
					uint16_t dst_port = ntohs(udp_hdr->dst_port);
					// // printf("Rxd udp pkt1 %x \n1194
					// 	rte_pktmbuf_free(m);				
					// }
					if((ENABLE_HEADER_COMP == 1) )
					{
						apprxbuf = rte_pktmbuf_mtod(m, unsigned char *);
						if(portid == WAN_IF_ID)
						{
							if( (dst_port == 1194) )
							{	
								//printf("1 tunnel pkt received \n");						
								/* 14 - Eth header, 20 - IP header 8 - UDP header */						
								packet_decoalesce_extract_pkts(portid, apprxbuf+42, eth_hdr,(struct ipv4_hdr *)ipv4_hdr);							
								rte_pktmbuf_free(m);
							}
							else
							{
								/* if not tunnel pkt, just bypass it */
								rte_prefetch0(rte_pktmbuf_mtod(m, void *));
								l2fwd_simple_forward(m, portid);
							}
						}
						else
						{
							// printf("%d udp pkt received %d \n",portid, dst_port);
							
							if(m->pkt_len < 200)
							{								
								l5 = (uint8_t *)eth_hdr + l4_offset
														+ sizeof(struct rte_udp_hdr);

								rtp_hdr = (struct rtphdr1 *)l5;
								if(is_rtp_hdr(udp_hdr,rtp_hdr))
								{
									// printf("%d rtp pkt received %d gre_len %d \n",portid, dst_port,gre_len);
									if(gre_len)
										pkt_comp_proc_HC(portid, apprxbuf+14, eth_hdr, ipv4_hdr);
									else	
										pkt_comp_proc_HC(portid, apprxbuf+14, eth_hdr, NULL);	

									rte_pktmbuf_free(m);
								}
								else
								{
									rte_prefetch0(rte_pktmbuf_mtod(m, void *));
									l2fwd_simple_forward(m, portid);
								}	
							}
							else
							{
								rte_prefetch0(rte_pktmbuf_mtod(m, void *));
								l2fwd_simple_forward(m, portid);
							}					
						}							
					}
					else
					{						
						// else
						{
							// rte_prefetch0(rte_pktmbuf_mtod(m, void *));
							// rte_pktmbuf_free(m);
							rte_prefetch0(rte_pktmbuf_mtod(m, void *));
							l2fwd_simple_forward(m, portid);
						}
					}

				}
				else
				{
					rx_port_stats[portid].good_pkts++;
					rx_port_stats[portid].good_bytes += m->pkt_len;	

					rte_prefetch0(rte_pktmbuf_mtod(m, void *));
					l2fwd_simple_forward(m, portid);
					//pkts_burst_rx[0] = m;
					//uint16_t nb_txd1 = rte_eth_tx_burst(portid, 0, pkts_burst_rx, 1);
					//rte_pktmbuf_free(m);
				}
			}
			else
			{
				rx_port_stats[portid].good_pkts++;
				rx_port_stats[portid].good_bytes += m->pkt_len;	

				/* all other pkts such as ipv6 and others */
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
				// rte_pktmbuf_free(m);
				/* Added on 31.07.24 to enable STP BPDU packets and others */
				l2fwd_simple_forward(m, portid);  
			}
			default2:
				pkt_integrity_fail = 1;
		}

		if(retrieve_asmuch_rxd == 1)
			goto loop_retrieve_asmuch;

		skip_rx_thread:
			conf_idx++;
			if (confs[conf_idx] == NULL)
				conf_idx = 0;
	}

	exit_rx_thread:
		printf("\n");
}

l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid)
{
	send_schedule_pkt(portid, m);
}

// void send_schedule_pkt(int portid, struct rte_mbuf *m_tx)
// {
// 	struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned;

// 	uint32_t subport;
// 	uint32_t pipe;
// 	uint32_t traffic_class;
// 	uint32_t queue;
// 	uint32_t color;
// 	int table_index,packet_drop;

// 	int nb_rx = 0;
// 	int i;

// 	nb_rx = 1;	
// 	rx_mbufs[0] = m_tx;	
// 	APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_rx, nb_rx);

// 	if(firewall_profile_update == 1)
// 	{
// 		rte_pktmbuf_free(rx_mbufs[0]);
// 		APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
// 		printf("Pakcet dropped due to profile update  \n");
// 		goto drop;
// 	} 
// 	// printf("Scheduling the packets.... \n");
	
// 	for(i = 0; i < nb_rx; i++) {
		
// 		if(qos_conf[portid].tx_port == 1)
// 		{
// 			table_index = get_pkt_sched(rx_mbufs[i],
// 										&subport, &pipe, &traffic_class, &queue, &color);			
// 			if(table_index != -1)
// 			{
// 				if(firewall_lut[table_index].Permission == 0)
// 				{
// 					/* If permisssion is denied, drop the packet */
// 					rte_pktmbuf_free(rx_mbufs[i]);
// 					APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
// 					printf("Pakcet dropped due to DENY \n");
// 					goto drop;
// 				}
// 			}
// 			rte_sched_port_pkt_write(qos_conf[portid].rx_thread.sched_port,
// 					rx_mbufs[i],
// 					subport, pipe,
// 					traffic_class, queue,
// 					(enum rte_color) color);		
// 			// rx_mbufs[i]->hash.sched.queue_id = 60;
// 			// rx_mbufs[i]->hash.sched.traffic_class = 1234578;
// 			// rx_mbufs[i]->hash.sched.color = 255;
// 		}							
// 	}

// 	// printf("Going to write the packet to ring \n");
// 	if (unlikely(rte_ring_sp_enqueue_bulk(qos_conf[portid].rx_thread.rx_ring,
// 			(void **)rx_mbufs, nb_rx, NULL) == 0)) {
// 		for(i = 0; i < nb_rx; i++) {
// 			rte_pktmbuf_free(rx_mbufs[i]);
// 			APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
// 		}
// 	}

// 	drop:
// 		packet_drop = 1;
// }

void send_schedule_pkt(int portid, struct rte_mbuf *m_tx)
{
	struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned;

	uint32_t subport;
	uint32_t pipe;
	uint32_t traffic_class;
	uint32_t queue;
	uint32_t color;
	int table_index,packet_drop;

	int nb_rx = 0;
	int i;

	nb_rx = 1;	
	rx_mbufs[0] = m_tx;	
	APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_rx, nb_rx);

	if(firewall_profile_update == 1)
	{
		rte_pktmbuf_free(rx_mbufs[0]);
		APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
		printf("Pakcet dropped due to profile update  \n");
		goto drop;
	} 
	// printf("Scheduling the packets.... \n");
	
	// for(i = 0; i < nb_rx; i++) {
		
	// 	if(qos_conf[portid].tx_port == 1)
	// 	{
	// 		table_index = get_pkt_sched(rx_mbufs[i],
	// 									&subport, &pipe, &traffic_class, &queue, &color);			
	// 		if(table_index != -1)
	// 		{
	// 			if(firewall_lut[table_index].Permission == 0)
	// 			{
	// 				/* If permisssion is denied, drop the packet */
	// 				rte_pktmbuf_free(rx_mbufs[i]);
	// 				APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
	// 				printf("Pakcet dropped due to DENY \n");
	// 				goto drop;
	// 			}
	// 		}
	// 		rte_sched_port_pkt_write(qos_conf[portid].rx_thread.sched_port,
	// 				rx_mbufs[i],
	// 				subport, pipe,
	// 				traffic_class, queue,
	// 				(enum rte_color) color);		
	// 		// rx_mbufs[i]->hash.sched.queue_id = 60;
	// 		// rx_mbufs[i]->hash.sched.traffic_class = 1234578;
	// 		// rx_mbufs[i]->hash.sched.color = 255;
	// 	}							
	// }

	// printf("Going to write the packet to ring \n");
	if (unlikely(rte_ring_sp_enqueue_bulk(qos_conf[portid].rx_thread.rx_ring,
			(void **)rx_mbufs, nb_rx, NULL) == 0)) {
		for(i = 0; i < nb_rx; i++) {
			rte_pktmbuf_free(rx_mbufs[i]);
			APP_STATS_ADD(qos_conf[portid].rx_thread.stat.nb_drop, 1);
		}
	}

	drop:
		packet_drop = 1;
}

void send_etherpkt(int lanif)
{
	unsigned char *buf;
	//struct rte_mbuf *pkts_burst_tx[MAX_PKT_BURST];
	struct rte_mbuf *m_tx = NULL;
	uint32_t seqno;
	unsigned dst_port;
	int sent;
	int portid;
	int i;
	int nb_rx = 0;
	struct rte_eth_dev_tx_buffer *buffer;

	//printf("Sending Ethernet packets \n");

	portid = l2fwd_dst_ports[lanif];
	// portid = lanif;

	m_tx = rte_pktmbuf_alloc(qos_conf[portid].mbuf_pool);
	// m_tx = rte_pktmbuf_alloc(qos_conf[0].mbuf_pool);
	if(m_tx == NULL)
	{
		printf("No pkt mbuf available \n");
		return;
	}
	
	buf = apptxbuf;	

	m_tx->data_len = apptxlen;
	m_tx->pkt_len = apptxlen;

	rte_memcpy(rte_pktmbuf_mtod(m_tx, void*),buf, (size_t)apptxlen);	

	// start of pkt buffering
	/*dst_port = lanif;
	buffer = tx_buffer[dst_port];
	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m_tx);*/
	 
	// if (sent)
	// 	port_statistics[dst_port].tx += sent;	
	// END of pkt buffering

// #ifdef ARM_ARCH
// 	m_tx->ol_flags = PKT_TX_IP_CKSUM | PKT_TX_IPV4 | PKT_TX_TCP_CKSUM | PKT_TX_UDP_CKSUM;
// 	m_tx->l2_len = 14;
// 	m_tx->l3_len = 20;
// #endif

	ipiden++;

	send_schedule_pkt(portid, m_tx);
	
}

void send_l2_rohcpkt(int lanif, uint32_t queue_id)
{
	unsigned char *buf;
	//struct rte_mbuf *pkts_burst_tx[MAX_PKT_BURST];
	struct rte_mbuf *m_tx = NULL;
	uint32_t seqno;
	unsigned dst_port;
	int sent;
	int portid;
	int i;
	int nb_rx = 0;
	struct rte_eth_dev_tx_buffer *buffer;

	//printf("Sending Ethernet packets \n");

	portid = l2fwd_dst_ports[lanif];
	// portid = lanif;

	m_tx = rte_pktmbuf_alloc(qos_conf[portid].mbuf_pool);
	// m_tx = rte_pktmbuf_alloc(qos_conf[0].mbuf_pool);
	if(m_tx == NULL)
	{
		printf("No pkt mbuf available \n");
		return;
	}	

	buf = apptxbuf;	

	m_tx->data_len = apptxlen;
	m_tx->pkt_len = apptxlen;

	rte_memcpy(rte_pktmbuf_mtod(m_tx, void*),buf, (size_t)apptxlen);	

	ipiden++;

	uint32_t subport;
	uint32_t pipe;
	uint32_t traffic_class;
	uint32_t queue;
	uint32_t color;
	struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned;
	// int nb_rx = 0;
	

	nb_rx = 1;	
	rx_mbufs[0] = m_tx;	

	subport = 0;
	queue = 0;
	color = 0;

	pipe = (queue_id/RTE_SCHED_QUEUES_PER_PIPE);
	traffic_class = (queue_id%RTE_SCHED_QUEUES_PER_PIPE);

	rte_sched_port_pkt_write(qos_conf[portid].rx_thread.sched_port,
			rx_mbufs[0],
			subport, pipe,
			traffic_class, queue,
			(enum rte_color) color);

	send_schedule_pkt(portid, m_tx);
	
}

void send_back_packet(struct thread_conf **confs, int conf_idx, struct rte_mbuf *m_tx)
{
	if (unlikely(rte_ring_sp_enqueue_bulk(qos_conf[0].rx_thread.rx_ring,
			(void **)&m_tx, 1, NULL) == 0)) 
	{	
		printf("Pkt loopback enqueue failed \n");		
		rte_pktmbuf_free(m_tx);			
	}

	if (unlikely(rte_ring_sp_enqueue_bulk(qos_conf[1].rx_thread.rx_ring,
			(void **)&m_tx, 1, NULL) == 0)) 
	{	
		printf("Pkt loopback enqueue failed \n");		
		rte_pktmbuf_free(m_tx);			
	}	
}

// void send_back_packet(struct thread_conf **confs, int conf_idx, struct rte_mbuf *m_tx)
// {
// 	int conf_idx1;
// 	int i;
// 	struct thread_conf *conf;

// 	conf_idx1 = conf_idx;
// 	conf_idx1++;
// 	if (confs[conf_idx1] == NULL)
// 		conf_idx1 = 0;
// 	conf = confs[conf_idx1];

// 	printf("In send back packet \n");
// 	printf("confs %x m_tx %x \n",confs, m_tx);

// 	if (unlikely(rte_ring_sp_enqueue_bulk(conf->rx_ring,
// 			(void **)&m_tx, 1, NULL) == 0)) 
// 	{	
// 		printf("Pkt loopback enqueue failed \n");		
// 		rte_pktmbuf_free(m_tx);			
// 	}	
// }



/* Send the packet to an output interface
 * For performance reason function returns number of packets dropped, not sent,
 * so 0 means that all packets were sent successfully
 */

static inline void
app_send_burst(struct thread_conf *qconf)
{
	struct rte_mbuf **mbufs;
	uint32_t n, ret;

	mbufs = (struct rte_mbuf **)qconf->m_table;
	n = qconf->n_mbufs;

	do {
		ret = rte_eth_tx_burst(qconf->tx_port, qconf->tx_queue, mbufs, (uint16_t)n);
		/* we cannot drop the packets, so re-send */
		/* update number of packets to be sent */
		n -= ret;
		mbufs = (struct rte_mbuf **)&mbufs[ret];
	} while (n);
}


/* Send the packet to an output interface */
static void
app_send_packets(struct thread_conf *qconf, struct rte_mbuf **mbufs, uint32_t nb_pkt)
{
	uint32_t i, len;

	len = qconf->n_mbufs;
	for(i = 0; i < nb_pkt; i++) {
		qconf->m_table[len] = mbufs[i];
		len++;
		/* enough pkts to be sent */
		if (unlikely(len == burst_conf.tx_burst)) {
			qconf->n_mbufs = len;
			app_send_burst(qconf);
			len = 0;
		}
	}

	qconf->n_mbufs = len;
}

void
app_tx_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.qos_dequeue];
	struct thread_conf *conf;
	int conf_idx = 0;
	int retval;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	while ((conf = confs[conf_idx])) {
		retval = rte_ring_sc_dequeue_bulk(conf->tx_ring, (void **)mbufs,
					burst_conf.qos_dequeue, NULL);
		if (likely(retval != 0)) {
			app_send_packets(conf, mbufs, burst_conf.qos_dequeue);

			conf->counter = 0; /* reset empty read loop counter */
			// printf("Received pkt in TX core \n");
		}

		conf->counter++;

		/* drain ring and TX queues */
		if (unlikely(conf->counter > drain_tsc)) {
			/* now check is there any packets left to be transmitted */
			if (conf->n_mbufs != 0) {
				app_send_burst(conf);

				conf->n_mbufs = 0;
			}
			conf->counter = 0;
		}

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}


void
app_worker_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.ring_burst];
	struct thread_conf *conf;
	int conf_idx = 0;

	while ((conf = confs[conf_idx])) {
		uint32_t nb_pkt;

		/* Read packet from the ring */
		nb_pkt = rte_ring_sc_dequeue_burst(conf->rx_ring, (void **)mbufs,
					burst_conf.ring_burst, NULL);
		if (likely(nb_pkt)) {
			int nb_sent = rte_sched_port_enqueue(conf->sched_port, mbufs,
					nb_pkt);

			APP_STATS_ADD(conf->stat.nb_drop, nb_pkt - nb_sent);
			APP_STATS_ADD(conf->stat.nb_rx, nb_pkt);

			// printf("Received pkt in worker core \n");
		}

		nb_pkt = rte_sched_port_dequeue(conf->sched_port, mbufs,
					burst_conf.qos_dequeue);
		if (likely(nb_pkt > 0))
			while (rte_ring_sp_enqueue_bulk(conf->tx_ring,
					(void **)mbufs, nb_pkt, NULL) == 0)
				; /* empty body */

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}


void calculate_tcp_checksum(unsigned char *rxbuf, uint32_t rxlen)
{
	uint32_t i;
	unsigned long sum;
	unsigned short tcpcs,ipcs;
	uint32_t temp32;
	uint16_t temp16;
	uint16_t inner_iplen, outer_iplen;
	unsigned int tcp_payload_len;
	uint8_t gre_len, tcphlen;
	uint16_t tcplen;

	if(rxbuf[23] == 0x2F)
	{
		gre_len = 24;
		if((rxbuf[34] & 0x80) == 0x80)gre_len += 4; /* checksum bit */
		if((rxbuf[34] & 0x20) == 0x20)gre_len += 4; /* Key bit */
		if((rxbuf[34] & 0x10) == 0x10)gre_len += 4; /* Seq num bit */
	}
	else
		gre_len = 0;

	tcphlen = ((rxbuf[gre_len+46] & 0xF0)	>> 4 ) << 2;
	tcp_payload_len = apptxlen - (gre_len+34+tcphlen) ;	
	
	inner_iplen = (((rxbuf[gre_len+16]<<8)&0xFF00) | rxbuf[gre_len+17]);
	tcplen = inner_iplen - 20;


	/* IP Checksum calculation */
	sum = 0;
	sum = sum + (((rxbuf[14]<<8)&0xFF00) | rxbuf[15]);
	sum = sum + (((rxbuf[16]<<8)&0xFF00) | rxbuf[17]);
	sum = sum + (((rxbuf[18]<<8)&0xFF00) | rxbuf[19]);
	sum = sum + (((rxbuf[20]<<8)&0xFF00) | rxbuf[21]);
	sum = sum + (((rxbuf[22]<<8)&0xFF00) | rxbuf[23]);
	sum = sum + (((rxbuf[26]<<8)&0xFF00) | rxbuf[27]);
	sum = sum + (((rxbuf[28]<<8)&0xFF00) | rxbuf[29]);
	sum = sum + (((rxbuf[30]<<8)&0xFF00) | rxbuf[31]);
	sum = sum + (((rxbuf[32]<<8)&0xFF00) | rxbuf[33]);

	while(sum>>16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	
	ipcs = ~sum;

	rxbuf[24] = (ipcs>>8) & 0xFF;
	rxbuf[25] = (ipcs) & 0xFF;

	if(gre_len)
	{
		sum = 0;
		sum = sum + (((rxbuf[14+gre_len]<<8)&0xFF00) | rxbuf[15+gre_len]);
		sum = sum + (((rxbuf[16+gre_len]<<8)&0xFF00) | rxbuf[17+gre_len]);
		sum = sum + (((rxbuf[18+gre_len]<<8)&0xFF00) | rxbuf[19+gre_len]);
		sum = sum + (((rxbuf[20+gre_len]<<8)&0xFF00) | rxbuf[21+gre_len]);
		sum = sum + (((rxbuf[22+gre_len]<<8)&0xFF00) | rxbuf[23+gre_len]);
		sum = sum + (((rxbuf[26+gre_len]<<8)&0xFF00) | rxbuf[27+gre_len]);
		sum = sum + (((rxbuf[28+gre_len]<<8)&0xFF00) | rxbuf[29+gre_len]);
		sum = sum + (((rxbuf[30+gre_len]<<8)&0xFF00) | rxbuf[31+gre_len]);
		sum = sum + (((rxbuf[32+gre_len]<<8)&0xFF00) | rxbuf[33+gre_len]);

		while(sum>>16)
			sum = (sum & 0xFFFF) + (sum >> 16);
		
		ipcs = ~sum;

		rxbuf[24+gre_len] = (ipcs>>8) & 0xFF;
		rxbuf[25+gre_len] = (ipcs) & 0xFF;
	}

	/* TCP Checksum calculation */	
	sum = 0;
	rxbuf[50+gre_len] = 0x00;
	rxbuf[51+gre_len] = 0x00;
	for(i=0;i<tcplen/2;i++)
	{
		sum = sum + (((rxbuf[2*i+34+gre_len]<<8)&0xFF00) | rxbuf[2*i+35+gre_len]);
	}
	if((tcplen%2) == 1)

		sum = sum + (((rxbuf[tcplen+33+gre_len]<<8)&0xFF00) | 0x00);  //apptxbuf[tcplen-1+34]

	temp32 = (rxbuf[26+gre_len]<<24) | (rxbuf[27+gre_len]<<16) | (rxbuf[28+gre_len]<<8) | (rxbuf[29+gre_len]);
	sum = sum + ((temp32 >> 16) & 0xFFFF);
	sum = sum + (temp32 & 0xFFFF);
	
	temp32 = (rxbuf[30+gre_len]<<24) | (rxbuf[31+gre_len]<<16) | (rxbuf[32+gre_len]<<8) | (rxbuf[33+gre_len]);
	sum = sum + ((temp32 >> 16) & 0xFFFF);
	sum = sum + (temp32 & 0xFFFF);
	
	sum = sum + rxbuf[23+gre_len];
	temp16 = inner_iplen;
	sum = sum + (temp16 - 20);
	
	while(sum>>16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	
	tcpcs = ~sum;
	
	rxbuf[50+gre_len] = (tcpcs>>8) & 0xFF;
	rxbuf[51+gre_len] = (tcpcs) & 0xFF;
}


void
app_mixed_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.ring_burst];
	struct rte_mbuf *mbufs_valid[burst_conf.ring_burst];
	struct thread_conf *conf;
	int conf_idx = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;
	uint64_t cur_tsc;
	//const uint64_t mscnt2 = rte_get_timer_hz()/1000; /* around 1ms */
	const uint64_t mscnt2 = rte_get_timer_hz()/1000000; /* around 1us */
	uint64_t prev_tsc, diff_tsc;
	int i;

	struct rte_eth_dev_tx_buffer *buffer;

	struct rte_ether_hdr *eth_hdr;	
	uint16_t ether_type;
	void *l3, *l4;
	int hdr_len;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_ipv6_hdr *ipv6_hdr;
	struct rte_udp_hdr *udp_hdr;
	
	struct rte_ipv4_hdr *inner_ipv4_hdr;
	// struct rte_flow_item_gre *gre_hdr;
	struct rte_gre_hdr *gre_hdr;

	uint16_t l4_offset;
	uint8_t gre_len;

	struct rte_mbuf *m_tx;
	uint32_t qlen;
	uint64_t prev_tsc_ms, diff_tsc_ms;
	uint32_t queue_id;
	uint16_t nb_pkt_valid;

	uint32_t subport;
	uint32_t pipe;
	uint32_t traffic_class;
	uint32_t queue;
	uint32_t color;
	int table_index,packet_drop;
	uint8_t get_pkt_sched_written = 0;

	
	//return;
	//sleep(2);

	// conf = confs[conf_idx++];
	// rte_ring_reset(conf->rx_ring);
	// conf = confs[conf_idx++];
	// rte_ring_reset(conf->rx_ring);


	for(i=0;i<(MAX_SITES * QUEUES_PER_SITE);i++)
	{
		qos_conf[0].wt_thread.qwa[i] = 0;
		qos_conf[0].wt_thread.qwa[i] = 0;
	}

	SCHED_MAX_QUEUE_INDEX = (subport_params[0].n_pipes_per_subport_enabled * QUEUES_PER_SITE) - 1;

	// printf("In app mixed thread function drain_tsc %d mscnt %d\n",drain_tsc,mscnt2);
	printf("In app mixed thread SCHED_MAX_QUEUE_INDEX %d \n",SCHED_MAX_QUEUE_INDEX);


	conf_idx = 0;
	while ((conf = confs[conf_idx]) ) {
		uint32_t nb_pkt;	

		if(qos_conf[0].wt_thread.profile_change == 1)
		{
			goto skip_mixed_thread;
		}

		nb_pkt_valid = 0;
		/* Read packet from the ring */		
		nb_pkt = rte_ring_sc_dequeue_burst(conf->rx_ring, (void **)mbufs,
					burst_conf.ring_burst, NULL);	

		//if (likely(nb_pkt > 0)) 
		for(i = 0; i < nb_pkt; i++)
		{	
			get_pkt_sched_written = 0;
			m_tx = mbufs[i];
			eth_hdr = rte_pktmbuf_mtod(m_tx, struct rte_ether_hdr *);
			ether_type = ntohs(eth_hdr->ether_type);			
			if(ether_type == 0x0800)
			{
				l3 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
				ipv4_hdr = (struct rte_ipv4_hdr *)l3;
				l4 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
				udp_hdr = (struct rte_udp_hdr *)l4;

				uint16_t ip_len = ntohs(ipv4_hdr->total_length);
				if( (m_tx->pkt_len > 1518) || ((m_tx->pkt_len != (ip_len+14)) &&
					(ip_len > 46)) )
				{
					/* data length mismatch */
					printf("ring Pkt len and ip len mismatch %d %d portid %d\n",m_tx->pkt_len,(ip_len+14),conf->tx_port);
					apprxbuf = rte_pktmbuf_mtod(m_tx, unsigned char *);
					// for(int p222=0;p222<70;p222++)
					// {
					// 	printf("%x\t",apprxbuf[p222]);
					// }
					// printf("\n");
					rte_pktmbuf_free(m_tx);
					continue;
				}

				if(ipv4_hdr->next_proto_id == 0x2F)
				{
					/* GRE header */
					// gre_hdr = (struct rte_flow_item_gre *)l4;
					// inner_ipv4_hdr = (struct rte_ipv4_hdr *)(l4 + sizeof(struct rte_flow_item_gre));
					// udp_hdr = (struct rte_udp_hdr *)(l4 + sizeof(struct rte_flow_item_gre) + sizeof(struct rte_ipv4_hdr));
					// l4_offset = 58; /* 14+ 20+ 4+ 20 */
					// gre_len = 24;

					/* GRE header */
					gre_hdr = (struct rte_gre_hdr *)l4;
					gre_len = 4;
					if(gre_hdr->c)gre_len += 4; /* checksum present bit */
					if(gre_hdr->k)gre_len += 4; /* key present bit */
					if(gre_hdr->s)gre_len += 4; /* sequence number present bit */
					inner_ipv4_hdr = (struct rte_ipv4_hdr *)(l4 + gre_len);
					udp_hdr = (struct rte_udp_hdr *)(l4 + gre_len + sizeof(struct rte_ipv4_hdr));
					
					l4_offset = 34+gre_len+20; /* 14+ 20+ 4 to 16 bytes+ 20 */

					/* throughout the code, gre_len is considered as GRE HDR + Inner IP HDR length */
					gre_len += 20; 



				}
				else
				{
					gre_hdr = NULL;
					inner_ipv4_hdr = ipv4_hdr;
					// udp_hdr remains same....
					l4_offset = 34; /* 14+ 20  */
					gre_len = 0;
				}

				if(inner_ipv4_hdr->next_proto_id == 0x11)
				{	
					uint16_t dst_port = ntohs(udp_hdr->dst_port);					
					if(dst_port == 6001)
					{						
						printf("Profile update UDP packet recevied in mixed core \n");
						rte_pktmbuf_free(m_tx);		
						qos_conf[0].wt_thread.profile_change = 1;
						continue;
					}
					else if(dst_port == 1194)
					{
						get_pkt_sched_written = 1;
					}
				}
				else if(inner_ipv4_hdr->next_proto_id == 0x06)
				{
					apprxbuf = rte_pktmbuf_mtod(m_tx, unsigned char *);
					// printf("Going to calc tcp cksum \n");
					calculate_tcp_checksum(apprxbuf, m_tx->pkt_len);
				}
			}
			else
			{
				if(m_tx->pkt_len > 1518)
				{
					printf("ring pkt len is more than MTU size \n");
					rte_pktmbuf_free(m_tx);
					continue;
				}

				// if(conf->tx_port == 1)
				// {				
				// 	if(m_tx->hash.sched.queue_id > SCHED_MAX_QUEUE_INDEX)
				// 	{
				// 		printf("RING1 max queue index exceeded\n");
				// 		rte_pktmbuf_free(m_tx);
				// 		continue;
				// 	}
				// }
			}

			if(conf->tx_port == 1)
			{
				if(get_pkt_sched_written == 0)
				{
					table_index = get_pkt_sched(mbufs[i],
												&subport, &pipe, &traffic_class, &queue, &color);			
					if(table_index != -1)
					{
						if(firewall_lut[table_index].Permission == 0)
						{
							/* If permisssion is denied, drop the packet */
							rte_pktmbuf_free(mbufs[i]);
							APP_STATS_ADD(qos_conf[conf->tx_port].rx_thread.stat.nb_drop, 1);
							printf("Pakcet dropped due to DENY \n");
							continue;
						}
					}
					rte_sched_port_pkt_write(qos_conf[conf->tx_port].rx_thread.sched_port,
							mbufs[i],
							subport, pipe,
							traffic_class, queue,
							(enum rte_color) color);		
					// rx_mbufs[i]->hash.sched.queue_id = 60;
					// rx_mbufs[i]->hash.sched.traffic_class = 1234578;
					// rx_mbufs[i]->hash.sched.color = 255;
					// printf("Queue id %d \n",mbufs[i]->hash.sched.queue_id);
				}

				if(m_tx->hash.sched.queue_id > SCHED_MAX_QUEUE_INDEX)
				{
					printf("RING1 max queue index exceeded\n");
					rte_pktmbuf_free(m_tx);
					continue;
				}
				
			}				

			mbufs_valid[nb_pkt_valid] = m_tx;
			nb_pkt_valid++;
		}	

		struct rte_sched_queue_stats stats;		
			
		if(conf->tx_port == 1)
		// if(0)
		{
			// printf("Going to schedule the packet \n");	
			if (likely(nb_pkt_valid)) {		
				// printf("Going to schedule the packet \n");				
				// int nb_sent = rte_sched_port_enqueue(conf->sched_port, mbufs,
				// 		nb_pkt);
				int nb_sent = rte_sched_port_enqueue(conf->sched_port, mbufs_valid,
						nb_pkt_valid);

				// if((nb_pkt-nb_sent) > 0)
				// {
					// printf("packets dropped %d \n",(nb_pkt_valid-nb_sent));
				// }
				APP_STATS_ADD(conf->stat.nb_drop, nb_pkt_valid - nb_sent);
				APP_STATS_ADD(conf->stat.nb_rx, nb_pkt_valid);
				
				/* Checking the status after enqueueing */
				for(i=0; i<nb_sent;i++)
				{
					queue_id = mbufs_valid[i]->hash.sched.queue_id;
					uint32_t stats_retval = rte_sched_queue_read_stats(conf->sched_port, queue_id, &stats, &qlen);
					if(stats_retval == 0)
					{
						qos_conf[0].wt_thread.qlen[queue_id] = qlen;
						// if(qlen > qlen_max_seen)
						// {
						// 	qlen_max_seen = qlen;
						// 	printf("qlen_max_seen %d \n",qlen_max_seen);
						// }
					}
					else
					{
						printf("enqueue queue_id %d \n",queue_id);
					}

					// if(qlen > 1000)
					// {
						// printf("qlen  %d queue_id %d \n",qlen,queue_id);
					// }

					// qos_conf[0].wt_thread.qwa[queue_id]	+= 1;		
					// qos_conf[0].wt_thread.qlen[queue_id] = qos_conf[0].wt_thread.qwa[queue_id] - 
					// 									   qos_conf[0].wt_thread.qra[queue_id];
					// if(queue_id == 49)
					// 	printf("qlen **** %d \n",qos_conf[0].wt_thread.qlen[49]);
				}
				// printf("nb_pkt %d \n",nb_pkt);
			}
			
			nb_pkt_valid = 0;
			nb_pkt = rte_sched_port_dequeue(conf->sched_port, mbufs,
						burst_conf.qos_dequeue);
			/* Checking the status after dequeueing */
			for(i=0; i<nb_pkt;i++)
			{
				queue_id = mbufs[i]->hash.sched.queue_id;
				if(queue_id > SCHED_MAX_QUEUE_INDEX)
				{
					printf("deque sched max index exceeded %d \n",queue_id);
					rte_pktmbuf_free(mbufs[i]);
					continue;
				}
				uint32_t stats_retval = rte_sched_queue_read_stats(conf->sched_port, queue_id, &stats, &qlen);				
				if(stats_retval == 0)
				{
					qos_conf[0].wt_thread.qlen[queue_id] = qlen;
				}
				else
				{
					printf("deque queue_id %d \n",queue_id);
					rte_pktmbuf_free(mbufs[i]);
					continue;					
				}
				// qos_conf[0].wt_thread.qra[queue_id]	+= 1;
				// qos_conf[0].wt_thread.qlen[queue_id] = qos_conf[0].wt_thread.qwa[queue_id] - 
				// 									   qos_conf[0].wt_thread.qra[queue_id];


				m_tx = mbufs[i];
				eth_hdr = rte_pktmbuf_mtod(m_tx, struct rte_ether_hdr *);
				ether_type = ntohs(eth_hdr->ether_type);	
				// if(ether_type != 0x0806)
				// {	
				// 	m_tx->pkt_len = m_tx->timesync;
				// 	m_tx->data_len = m_tx->pkt_len;
				// 	m_tx->timesync = 0;

				// 	// printf("Reverted change pkt_len %x df1 %x df2 %x \n",m_tx->pkt_len,m_tx->dynfield1[0],
				// 	// 									m_tx->dynfield1[1]);
				// }		

				// printf("deque ether type %x \n",ether_type);
				if(ether_type == 0x0800)
				{
					l3 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
					ipv4_hdr = (struct rte_ipv4_hdr *)l3;					

					uint16_t ip_len = ntohs(ipv4_hdr->total_length);
					if( (m_tx->pkt_len > 1518) || ((m_tx->pkt_len != (ip_len+14)) &&
						(ip_len > 46)) )
					{
						/* data length mismatch */
						printf("deque Pkt len and ip len mismatch %d %d portid %d\n",m_tx->pkt_len,(ip_len+14),conf->tx_port);
						// apprxbuf = rte_pktmbuf_mtod(m_tx, unsigned char *);
						// for(int p222=0;p222<70;p222++)
						// {
						// 	printf("%x\t",apprxbuf[p222]);
						// }
						// printf("\n");
						rte_pktmbuf_free(m_tx);
						continue;
					}									
				}
				else
				{
					if(m_tx->pkt_len > 1518)
					{
						printf("deque pkt len is more than MTU size \n");
						rte_pktmbuf_free(m_tx);
						continue;
					}
				}

				mbufs_valid[nb_pkt_valid] = mbufs[i];
				nb_pkt_valid++;
			}


			
			// rte_sched_queue_read_stats(conf->sched_port, 49, &stats, &qlen);				
			// qos_conf[0].wt_thread.qlen = qlen;					
		}
		
		if (likely(nb_pkt_valid > 0)) {	
			/* Just for statistics updation */
			for(i=0;i<nb_pkt_valid;i++)
			{
				m_tx = mbufs_valid[i];

				tx_port_stats[conf->tx_port].total_pkts++;
				tx_port_stats[conf->tx_port].good_pkts++;

				tx_port_stats[conf->tx_port].total_bytes += m_tx->pkt_len;
				tx_port_stats[conf->tx_port].good_bytes  += m_tx->pkt_len;

				eth_hdr = rte_pktmbuf_mtod(m_tx, struct rte_ether_hdr *);
				ether_type = ntohs(eth_hdr->ether_type);			
				
				if(ether_type == 0x0800)
				{
					l3 = (uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr);
					ipv4_hdr = (struct rte_ipv4_hdr *)l3;

					uint32_t dst_addr1 = ntohl(ipv4_hdr->dst_addr);
					uint8_t ip_msb = (dst_addr1 >> 24) & 0xFF;
					if(ipv4_hdr->dst_addr == 0xFFFFFFFF)
					{
						tx_port_stats[conf->tx_port].bcast_pkts++;		
					}
					else
					{
						if(ip_msb >= 224 && ip_msb <= 239)
						{
							/* multicast ip range 224.0.0.0 to 239.255.255.255 */
							tx_port_stats[conf->tx_port].mcast_pkts++;	
										
						}
					}
				}
			}

			app_send_packets(conf, mbufs_valid, nb_pkt_valid);

			conf->counter = 0; /* reset empty read loop counter */
		}

		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		if(unlikely(diff_tsc > drain_tsc))
		{
			if (conf->n_mbufs != 0) {	
				app_send_burst(conf);
				conf->n_mbufs = 0;
			}
			conf->counter = 0;
			prev_tsc = cur_tsc;
		}

		// conf->counter++;
		// /* drain ring and TX queues */
		// if (unlikely(conf->counter > drain_tsc)) {

		// 	/* now check is there any packets left to be transmitted */
		// 	if (conf->n_mbufs != 0) {
		// 		app_send_burst(conf);

		// 		// printf("App send burst \n");

		// 		conf->n_mbufs = 0;
		// 	}
		// 	conf->counter = 0;
		// }

		uint64_t time1,time2;
		
		diff_tsc_ms = cur_tsc - prev_tsc_ms;
		if(diff_tsc_ms > mscnt2)
		{
			timer1mscnt_2++;
			
			// if(!(timer1mscnt_2 % 10))
			// {
			// 	usleep(1);
			// }
			if(!(timer1mscnt_2 % 1))			
			//if(!(timer1mscnt_2 % 1000000))
			{
				/* update the queue status every 1us */
				
					// time1 = rte_rdtsc();
					// printf("time1 %d \n",time1);
				//  rte_sched_queue_read_stats(conf->sched_port, 49, &stats, &qlen);				
				//  qos_conf[0].wt_thread.qlen[49] = qlen;
					// qlen_subport(1, 0);	
					// time2 = rte_rdtsc();
					// printf("time2 %d \n",time2);
					// printf("time diff %d \n",time2-time1);
				    // qlen_subport(1, 0);
				
			}
			// if(!(timer1mscnt_2 % 1000000))
			// 	printf("queue id %d qlen2 %d \n",queue_id, qos_conf[0].wt_thread.qlen[49]);
			
			/* needs to add loop for greater than 1ms*/
			prev_tsc_ms = cur_tsc;
		}
		
		//printf("pipes per subport %d \n",conf->sched_port->n_pipes_per_subport);
				

		skip_mixed_thread:
			conf_idx++;
			if (confs[conf_idx] == NULL)
				conf_idx = 0;
	}

	//printf("existing from app mixed thread \n");
}

// ./keyd --connect 169.254.0.2 --port 4434 --rotate 120 --cert nodeA.crt --key nodeA.key --ca ca.crt --sock /run/l2fwd/keyd.sock
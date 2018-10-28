#include <linux/init.h>
#include <linux/module.h>
#include <linux/ieee80211.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <net/net_namespace.h>
#include <linux/rcupdate.h>
#include <linux/if_ether.h>
#include <net/cfg80211.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <net/sock.h>

MODULE_LICENSE("Dual BSD/GPL");

#define BUFF_SIZE 1000 //buffer size in openvswitch/datapath/flow.c
#define SEND_SIZE 1024 //send buffer size
#define B_S 	1024 /*public buffer size*/
#define APSL 	250 // length of apstates_global.
#define NETLINK_USER  22
#define USER_MSG    (NETLINK_USER + 1)
#define USER_PORT   50
#define FLOWS_KEEP 10
int control_beacon = 0; // used to control the send rate of buffer informations
static 	struct timer_list tm; // used to periodly send informations
struct 	timeval oldtv; // used to get current time
char 	public_buffer[B_S]; // public buffer to write data and then send them when buffer size reach 1024
int 	write_begin = 0; // point that data can be wrote in in public buffer
/*
* The folling __u16 items are enum datatype, however, enum are length 4 bits, so, we use
* __u16 2-bytes data type instead to work with __u8 mac[6]. 2 + 6 = 8, 4 + 6 -> 16 is realy waste.
*/
__u16 WINSIZE = 1;
__u16 DROPS = 2;
__u16 IW = 3;
__u16 QUEUE = 4;
__u16 BEACON = 5;
__u16 SURVEY = 6;
__u16 KEEP_IW = 7;
__u16 KEEP_BEACON = 8;

/**
 * This data structure is used to represent flow information got in flow.c
 */
struct tcp_flow
{
    __be32 ip_src;
    __be32 ip_dst;
    __be16 sourceaddr;
    __be16 destination;
    __u16 winsize;
    __u16 wscale;
    __u32 sec;
    __u32 usec;
    __u64 packets;
};

/**
 * Used to get the max rate link, calculate link status
 */
struct iw_record
{
	char station[6];
	__u32 sec;
	__u32 usec;
	__u64 bytes;
	__u32 rates;
	__u16 computed;
};
/**
 * Used to get current ap state.
 */
struct apstates
{
    __u32 sec;
    __u32 usec;
    __u32 drop_count;
    __u64 time;
    __u64 time_busy;
    __u64 time_ext_busy;
    __u64 time_rx;
    __u64 time_tx;
    __u64 time_scan;
    __s8 noise;
    __u8 stations;
    __u32 packets;
    __u64 bytes;
    __u32 qlen;
    __u32 backlog;
    __u32 drops;
    __u32 requeues;
    __u32 overlimits;
    __u16 pad;   
};
/*
* dp_packet is a structure used to record data packet that dropped in mac80211 or
* queue algorithms, it has an extra struct dp_packet *next member than ,
* this is because a chain should be created to record dropped packets.
*/
struct dp_packet
{
	__u32 ip_src;
	__u32 ip_dst;
	__be32 sec;
	__be32 usec;
	__u16 port_src;
	__u16 port_dst;
	__u32 sequence;
	__u32 ack_sequence;
	__u32 drop_count;
	__u16 dpl;
	__u16 pad;
	__u32 in_time;
	struct dp_packet *next;
	__u32 pad1;
};


/*
* data_iw is used to record iw information in status.c of mac80211.
*  is its transmission version, there is a transformation
* between this two data structrues.
*/
struct data_iw
{
	char station[6];
	__u16 device;
	__be32 sec;
	__be32 usec;
	__u32 inactive_time;
	__u32 rx_bytes;
	__u32 rx_packets;
	__u32 tx_bytes;
	__u32 tx_packets;
	__u32 tx_retries;
	__u32 tx_failed;
	__s32 signal;
	__s32 signal_avg;
	__u32 expected_throughput;
	struct data_iw *next;
	__u32 pad;
};


/*
* data_survey is used to record iw dev wlan0 survey infromation.
* it is a ap information, so there is no *next;
*/
struct data_survey
{
	__u64 time;
	__be32 sec;
	__be32 usec;
	__u64 time_busy;
	__u64 time_ext_busy;
	__u64 time_rx;
	__u64 time_tx;
	__u64 time_scan;
	__u32 filled;
	__s8 noise;
	__u8 pad;
	__u16 center_freq;
};

/*
* data_beacon is used to record beacon information from scan.c of mac80211
* it has *next mumber to record neibour APs informations
* is transmission version of beacon informations.
*/
struct data_beacon
{
	__be32 sec;
	__be32 usec;
	__u16 data_rate;
	__u16 freq;
	__u32 timein;
	__s8 signal;
	__u8 pad;
	__u8 bssid[6];
	struct data_beacon *next;
	__u32 pad1;
};


/* data_queue is used to record queue information from sch_fq_codel.
* no *next mumber
*/
struct data_queue
{
	__u32 queue_id;
	__u32 packets;
	__u64 bytes;
	__be32 sec;
	__be32 usec;
	__u32 qlen;
	__u32 backlog;
	__u32 drops;
	__u32 requeues;
	__u32 overlimits;
	__u32 pad;
};

/*
* data_winsize is used to receive packet information from global buffer ubuf
*/
struct data_winsize
{
	__be32 ip_src;
	__be32 ip_dst;
	__be16 sourceaddr;
	__be16 destination;
	__be32 sequence;
	__be32 ack_sequence;
	__be16 flags;
	__be16 windowsize;
	__be32 sec;
	__be32 usec;
	__be32 datalength;
	char wscale[3];
	char eth_src[6];
	char eth_dst[6];
	char pad[5];
};

/*
 * The forllowing _t data structures are transmission versions of different data
 * types. category is the data category, mac is the AP generates them.
 */
struct winsize_t
{
	__u16 category;
	char mac[6];
	struct 	data_winsize winsize;
};
struct dp_t
{
	__u16 category;
	char mac[6];
	struct 	dp_packet drops;
};
struct di_t
{
	__u16 category;
	char mac[6];
	struct 	data_iw iw;
};
struct dq_t
{
	__u16 category;
	char mac[6];
	struct 	data_queue queue;
};
struct db_t
{
	__u16 category;
	char mac[6];
	struct 	data_beacon beacon;
};
struct ds_t
{
	__u16 category;
	char mac[6];
	struct 	data_survey survey;
};

/*
* keep_ statuctures are use to decrease information redundant
*/
struct keep_iw
{
	__u16 category;
	char station[6];
	char mac[6];
	__u16 pad;
};

struct keep_beacon
{
	__u16 category;
	char bssid[6];
	char mac[6];
	__u16 pad;
};

struct flow_info
{
	__be16 port;
	__u32 snd_cwnd;
	__u32 srtt_us;
	__u32 mdev_us;
	__u32 timein;
	__u32 mss_linger;
};
/*
* iwlist_last is a iw data chain that stores data of last transmission.
* when new iw informations generated, they are stored and waiting for
* transmission, check and transmit different data items. Generating new
* iwlist_last after each transmission process.
* queue_last works differently from iwlist_last and beacon_last. This is
* because only a single queue data structure can store last transmitted
* queue information, while a chain should be created to store iw or
* beacon informations.
*/
struct 	data_iw 		*iwlist_last = NULL;
struct 	data_queue 		queue_last[6];
struct 	data_beacon 	*beacon_last = NULL;
struct socket *sock_global;
char mac_global[6];
long last_sec = 0;
long last_usec = 0;

long last_sec_iw = 0;
long last_usec_iw = 0;

long last_sec_beacon = 0;
long last_usec_beacon = 0;

long last_sec_queue = 0;
long last_usec_queue = 0;

long last_sec_survey = 0;
long last_usec_survey = 0;

long last_sec_drop_mac = 0;
long last_usec_drop_mac = 0;

long last_sec_drop_queue = 0;
long last_usec_drop_queue = 0;

/**
 * used to record current ap states.
 */
struct apstates apstates_global[APSL];
int aps_head = 0;
int aps_tail = 0;
int count_iw = 0;
struct iw_record iw_rec[10];
/*
* The following functions are global functions from status.c, scan.c in mac80211,
* codel.h and sch_fq_codel.c.
*/
extern struct 	data_iw *ieee80211_dump_station_linger(struct data_survey *); //
extern struct 	dp_packet *dpt_mac_linger(void); //
extern void 	destroy_iwlist(struct data_iw *iwlist);
extern struct 	data_queue *dump_class_stats_linger(void); //
extern struct 	dp_packet *dpt_queue_linger(void); //
extern struct 	data_beacon *get_beacon_global_linger(void); //
extern void 	destroy_after_transmitted_codel(void);
extern void 	destroy_after_transmitted_mac80211(void);

extern struct tcp_flow flow_linger[FLOWS_KEEP];

extern struct flow_info flows[5];
struct iw_record maxrateflow;
EXPORT_SYMBOL_GPL(maxrateflow);

void write_2_public_buffer(void *data, int length);
void send_the_buffer(void);
void get_information(void);
void destroy_data(void);
void compare_and_iwlist(struct data_iw *ptr, struct data_survey *sur);
void del_this_node_iw(struct data_iw *ptr_pre, struct data_iw *ptr);
void del_this_node_beacon(struct data_beacon *ptr_pre, struct data_beacon *ptr);
void callback(void);
void destroy_list_beacon(struct data_beacon *ptr);
void destroy_list_iw(struct data_iw *ptr);
void copy_last_iw(struct data_iw *curr);
void copy_last_beacon(struct data_beacon *curr);
void compare_and_queue(struct data_queue *ptr);
void compare_and_beacon(struct data_beacon *ptr);
struct socket * get_sock(void);
void print_iwlist(struct data_iw *ptr, char *str);
void print_mac(char *addr);
void mac_tranADDR_toString_r(unsigned char* addr, char* str, size_t size);
int strcmp_linger(char *s, char *t);
void reversebytes_uint32t(__u32 *value);
void reversebytes_uint16t(__u16 *value);
int send_msg(int8_t *pbuf, uint16_t len);
static void recv_cb(struct sk_buff *skb);


struct netlink_kernel_cfg cfg = 
{
    .input = recv_cb,
};

void reversebytes_uint32t(__u32 *value)
{
  *value = (*value & 0x000000FFU) << 24 | (*value & 0x0000FF00U) << 8 | 
    (*value & 0x00FF0000U) >> 8 | (*value & 0xFF000000U) >> 24;
}
void reversebytes_uint16t(__u16 *value)
{
    *value = (*value & 0x00FF) << 8 |(*value & 0xFF00) >> 8;
}

static struct sock *netlinkfd = NULL;

int send_msg(int8_t *pbuf, uint16_t len)
{
    struct sk_buff *nl_skb;
    struct nlmsghdr *nlh;

    int ret;

    nl_skb = nlmsg_new(len, GFP_ATOMIC);
    if(!nl_skb)
    {
        printk("netlink_alloc_skb error\n");
        return -1;
    }

    nlh = nlmsg_put(nl_skb, 0, 0, USER_MSG, len, 0);
    if(nlh == NULL)
    {
        printk("nlmsg_put() error\n");
        nlmsg_free(nl_skb);
        return -1;
    }
    memcpy(nlmsg_data(nlh), pbuf, len);

    ret = netlink_unicast(netlinkfd, nl_skb, USER_PORT, MSG_DONTWAIT);

    return ret;
}


static void recv_cb(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = NULL;
    void *data = NULL;

    // printk("skb->len:%u %u\n", skb->len,  nlmsg_total_size(0));
    if(skb->len >= nlmsg_total_size(0))
    {
        nlh = nlmsg_hdr(skb);
        data = NLMSG_DATA(nlh);
        printk(KERN_DEBUG "%s\n", (char *)data);
        if(!strcmp(data, "states"))
        {
        	struct apstates apstat, tmp1, tmp2;
    		int i = 0;
    		int len_tmp_del = 0, k = 0;
    		int dur_sur = 0;
    		char * buf = NULL;
    		int duration = 0;
    		int maxqlen = 0;
    		int maxbacklog = 0;
    		
    		i = aps_head;
    		i = i - 1;
    		i = i + APSL;
    		i = i % APSL;
    		memset(&tmp1, 0, sizeof(struct apstates));
    		memset(&tmp2, 0, sizeof(struct apstates));
    		tmp2 = apstates_global[i];
    		tmp1 = apstates_global[aps_tail];
    		duration = ((int)tmp2.sec - (int)tmp1.sec) * 1000 + ((int)tmp2.usec - (int)tmp1.usec) / 1000;
    		// printk(KERN_DEBUG "%u %u %u %u %d\n", tmp2.sec, tmp1.sec, tmp2.usec, tmp1.usec, xyz);
    		len_tmp_del = (aps_head - aps_tail + APSL) % APSL;
    		for(k = 0; k < len_tmp_del; k++)
    		{
    			struct apstates xyztmp;
    			int ind = 0;
    			ind = k % APSL;
    			xyztmp = apstates_global[ind];
    			if (xyztmp.backlog > maxbacklog)
    			{
    				maxqlen = xyztmp.qlen;
    				maxbacklog = xyztmp.backlog;
    			}
    		}
    		// duration = xyz;
    		dur_sur = ((int)tmp2.time - (int)tmp1.time);
    		if((duration == 0) || (dur_sur == 0))
    		{
    			char data[]="nonedata";
    			send_msg(data,nlmsg_len(nlh));
    			printk(KERN_DEBUG "divide zero %d %d %d %u %u %u %u\n", aps_head, aps_tail, i, tmp2.sec, tmp1.sec, tmp2.usec, tmp1.usec);
    		}
    		else
    		{
    			int length = 0;
    			int times = 1000000; //times is used because float is not supported in kernel space.
    			length = sizeof(struct apstates);
	    		apstat.drop_count = ((tmp2.drop_count - tmp1.drop_count) * times) / duration;
	    		
	    		apstat.time_busy = (times * (tmp2.time_busy - tmp1.time_busy)) / dur_sur;
	    		apstat.time_ext_busy = (times * (tmp2.time_ext_busy - tmp1.time_ext_busy)) / dur_sur;
	    		apstat.time_rx = (times * (tmp2.time_rx - tmp1.time_rx)) / dur_sur;
	    		apstat.time_tx = (times * (tmp2.time_tx - tmp1.time_tx)) / dur_sur;
	    		apstat.time_scan = (times * (tmp2.time_scan - tmp1.time_scan)) /dur_sur;
	    		apstat.noise = tmp2.noise;
	    		apstat.packets = (times * (tmp2.packets - tmp1.packets)) / duration;
	    		apstat.bytes = (times * (tmp2.bytes - tmp1.bytes)) / duration;
	    		apstat.qlen = maxqlen;
	    		apstat.backlog = maxbacklog;
	    		apstat.drops = (times * (tmp2.drops - tmp1.drops)) / duration;
	    		apstat.requeues = (times * (tmp2.requeues - tmp1.requeues)) / duration;
	    		apstat.overlimits = (times * (tmp2.overlimits - tmp1.overlimits)) / duration;
	    		apstat.sec = tmp2.sec;
	    		apstat.usec = tmp2.usec;
	    		apstat.stations = tmp2.stations;
	    		buf = (char *)kmalloc(length, GFP_KERNEL);
	    		memset(buf, 0, length);
	    		apstat.sec = 1000;
	    		apstat.usec = 100;
	    		memcpy(buf, &apstat, length);
	    		// printk(KERN_DEBUG "W %u %u %u %llu %llu %llu %lu\n", apstat.sec, apstat.usec, apstat.drop_count, apstat.time, apstat.time_busy, apstat.time_ext_busy, sizeof(struct apstates));

	    		// printk(KERN_DEBUG "11%u %u %u %llu %llu %llu, %d %u %u\n", apstat.sec, apstat.usec, apstat.drop_count, apstat.time, apstat.time_busy, apstat.time_ext_busy, length, duration, dur_sur);
	    		send_msg(buf, nlmsg_len(nlh));
	    		kfree(buf);
	    		buf = NULL;
	    	}
        }
        else if(!strcmp(data, "flowss"))
        {
        	struct tcp_flow *tmp = NULL;
        	int i = 0;

        	for(i = 0; i < FLOWS_KEEP; i++)
        	{
        		int length = 0;
        		char *str = NULL;
        		tmp = flow_linger + i;
        		length = sizeof(struct tcp_flow);
        		str = (char *)kmalloc(sizeof(char) * length, GFP_KERNEL);
        		memset(str, 0, length);
        		memcpy(str, tmp, length);
        		send_msg(str, nlmsg_len(nlh));
        		kfree(str);
        		str = NULL;
        	}
         //    int i = 0;
         //    int length = 0;
         //    int count = 0;
         //    struct tcp_flow tmp_2;
         //    char tmp_1[25];
         //    length = flow_head + 100 - flow_tail;
         //    length = length % 100;
         //    printk(KERN_DEBUG "%s length %d %d %d\n", data, length, flow_head, flow_tail);
         //    memset(&tmp_2, 0, sizeof(struct tcp_flow));
         //    tmp_2.ip_src = length;
         //    memcpy(tmp_1, &tmp_2, 24);
         //    send_msg(tmp_1, nlmsg_len(nlh));
         //    for(i = 0; i < length; i++)
         //    {
         //        char tmp[25];
         //        int loc = 0;
         //        loc = flow_tail + i;
         //        loc = loc % 100;
         //        memset(tmp, 0, 25);
         //        memcpy(tmp, &flow_linger[loc], 24);
         //        send_msg(tmp, nlmsg_len(nlh));
         //        printk(KERN_DEBUG "winsize is %d %u %u\n", flow_linger[loc].winsize, flow_linger[loc].sourceaddr, flow_linger[loc].destination);
         //        count += 1;
         //    // }
         //    // printk(KERN_DEBUG "count is %d\n", count);
         //    // printk("kernel receive data: %s\n", (int8_t *)data);
         //    // send_msg(buffer, nlmsg_len(nlh));
         //    // send_msg(buffer, length * struct_len);
        	// }
        }
        else if (!strcmp(data, "maxras"))
        {
        	struct iw_record tmp;
        	int length = 0;
        	char * strtosend = NULL;
        	char mac_addr[18];
        	mac_tranADDR_toString_r(maxrateflow.station, mac_addr, 18);

        	printk(KERN_DEBUG "%llu %slinger_fk\n", maxrateflow.bytes, mac_addr);
        	length = sizeof(struct iw_record);
        	strtosend = (char *)kmalloc(sizeof(char) * length, GFP_KERNEL);
        	tmp = maxrateflow;
        	memcpy(strtosend, &tmp, length);
        	send_msg(strtosend, nlmsg_len(nlh));
        	kfree(strtosend);
        	memset(&maxrateflow, 0, sizeof(struct iw_record));
        	strtosend = NULL;
        }
        else if (!strcmp(data, "flooss"))
        {
        	struct flow_info *tmp;
        	int length = 0;
        	char * strtosend = NULL;
        	int i = 0;
        	int length_flow_info = 0;
        	length_flow_info = sizeof(struct flow_info) * sizeof(char);
        	length = sizeof(struct flow_info) * 5;
        	printk(KERN_DEBUG "length is %d\n", length);
        	strtosend = (char *)kmalloc(sizeof(char) * length, GFP_KERNEL);
        	memset(strtosend, 0, sizeof(char) * length);
        	for(i = 0; i < 5; i++)
        	{
	        	tmp = flows + i;
	        	memcpy(strtosend + i * length_flow_info, tmp, length_flow_info);
	        }
        	send_msg(strtosend, nlmsg_len(nlh));
        	kfree(strtosend);
        	strtosend = NULL;
        }
    }
} 


/**
 * [mac_tranADDR_toString_r u8 address[6] to "01:2b:4c:5d:6a:7e"]
 * @linger
 * @DateTime  2018-05-06T01:03:55-0500
 * @param     addr                     [description]
 * @param     str                      [description]
 * @param     size                     [description]
 */
void mac_tranADDR_toString_r(unsigned char* addr, char* str, size_t size)
{
	if (addr == NULL || str == NULL || size < 18)
		// exit(1);
		printk(KERN_DEBUG "fff\n");

	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
	         addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	str[17] = '\0';
}

/**
 * [print_mac print mac address, used when debugging]
 * @AuthorHTL
 * @DateTime  2018-05-06T01:41:54-0500
 * @param     addr                     __u8 addr[6]
 */
void print_mac(char *addr)
{
	char str[18];
	mac_tranADDR_toString_r(addr, str, 18);
	printk(KERN_DEBUG "%s\n", str);
}


/**
 * write data to public buffer. First, check if the buffer will be
 * full when data is added, if full, tansmit and write, write then
 * transmit otherwise.
 * @AuthorHTL
 * @DateTime  2018-05-06T01:47:55-0500
 * @param     data                     data to be transmitted
 * @param     length                   length of data
 */
void write_2_public_buffer(void *data, int length)
{
	if ((write_begin + length) <= B_S)
	{
		memcpy(public_buffer + write_begin, data, length);
		write_begin = write_begin + length;
	}
	else
	{
		send_the_buffer();
		memcpy(public_buffer + write_begin, data, length);
		write_begin = write_begin + length;
	}
}

/**
 * [send_the_buffer description]
 * @Linger
 * @Principle: This part is from web
 * @DateTime   2018-05-06T01:54:56-0500
 */
void send_the_buffer(void)
{
	struct kvec iov;
	struct msghdr msg = {.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL};
	int len = 0;

	char buffer[SEND_SIZE];

	memset(&buffer, 0, SEND_SIZE);
	memcpy(buffer, public_buffer, write_begin);

	iov.iov_base = (void *)buffer;
	iov.iov_len = write_begin;
	len = kernel_sendmsg(sock_global, &msg, &iov, 1, write_begin);
	if (len != write_begin)
	{
		if (len == -ECONNREFUSED)
		{
			printk(KERN_ALERT "Receive Port Unreachable packet!\n");
		}
	}

	memset(public_buffer, 0, B_S);
	write_begin = 0;

}

/**
 * [Used to create a global socket to transmit data]
 * @Linger
 * @DateTime 2018-05-06T01:58:30-0500
 * @return                            socket created
 */
struct socket * get_sock(void)
{
	struct socket *sock;
	struct sockaddr_in s_addr;
	__s16 dstport = 6667;
	__u32 dstip = 0xc0a80b64;
	int ret = 0;

	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = AF_INET;
	s_addr.sin_port = cpu_to_be16(dstport);

	s_addr.sin_addr.s_addr = cpu_to_be32(dstip); /*server ip is 192.168.209.134*/
	sock = (struct socket *)kmalloc(sizeof(struct socket), GFP_KERNEL);

	/*create a socket*/
	// ret=sock_create_kern(&init_net, AF_INET, SOCK_STREAM,0,&sock);
	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock);

	if (ret < 0) {
		printk("client:socket create error!\n");
		return NULL;
	}
	printk("client: socket create ok!\n");

	/*connect server*/
	ret = sock->ops->connect(sock, (struct sockaddr *)&s_addr, sizeof(s_addr), 0);
	if (ret != 0) {
		printk("client:connect error!\n");
		return NULL;
	}
	printk("client:connect ok!\n");
	return sock;
}

/**
 * callback function scheduled every 1ms, it calls packet_t()
 * check if there is data to transmitted by checking uhead and
 * utail, if there is get corresponding data and transmitted.
 * @Linger
 * @DateTime 2018-05-06T01:59:46-0500
 */
void callback(void)
{
	struct timeval tv;

	do_gettimeofday(&tv);
	get_information();
	oldtv = tv;
	tm.expires = jiffies + 1;
	add_timer(&tm);        //重新开始计时
}

/**
 * Delete iw list node when it is out of date. Check if ptr_pre is NULL,
 * delete ptr if is, otherwise connected ptr's pre-node and node after it.
 * @Linger
 * @DateTime 2018-05-06T02:02:06-0500
 * @param    ptr_pre                  node before ptr
 * @param    ptr                      ptr
 */
void del_this_node_iw(struct data_iw *ptr_pre, struct data_iw *ptr)
{
	struct data_iw *tmp = NULL;
	if (ptr_pre == NULL)
	{
		tmp = ptr->next;
		ptr = ptr->next;
		kfree(tmp);
		tmp = NULL;
	}
	else
	{
		tmp = ptr->next;
		ptr_pre->next = tmp;
		tmp = NULL;
		kfree(ptr);
		ptr = NULL;
	}
}

/**
 * Delete beacon node when it is out of date.
 * @Linger
 * @DateTime 2018-05-06T02:10:50-0500
 * @param    ptr_pre                  node before it
 * @param    ptr                      node to be delete.
 */
void del_this_node_beacon(struct data_beacon *ptr_pre, struct data_beacon *ptr)
{
	struct data_beacon *tmp = NULL;
	if (ptr_pre == NULL)
	{
		tmp = ptr->next;
		ptr = ptr->next;
		kfree(tmp);
		tmp = NULL;
	}
	else
	{
		tmp = ptr->next;
		ptr_pre->next = tmp;
		tmp = NULL;
		kfree(ptr);
		ptr = NULL;
	}
}

/**
 * Delete the whole iw data list
 * @Linger
 * @DateTime 2018-05-06T02:11:34-0500
 * @param    ptr                      Head address of iw list.
 */
void destroy_list_iw(struct data_iw *ptr)
{
	struct data_iw *tmp = NULL;
	tmp = ptr;
	while (tmp)
	{
		void *pre = NULL;
		pre = tmp;
		tmp = tmp->next;
		kfree(pre);
		pre = NULL;
	}
}

/**
 * Destroy the whole beacon list.
 * @Linger
 * @DateTime 2018-05-06T02:18:49-0500
 * @param    ptr                      head address of beacon list.
 */
void destroy_list_beacon(struct data_beacon *ptr)
{
	struct data_beacon *tmp = NULL;
	tmp = ptr;
	while (tmp)
	{
		void *pre = NULL;
		pre = tmp;
		tmp = tmp->next;
		kfree(pre);
		pre = NULL;
	}
}

/**
 * Copy the whole iwlist got from status.c to local global iwlist.
 * @Linger
 * @DateTime 2018-05-06T02:21:15-0500
 * @param    curr                     iwlist head address of status.c
 */
void copy_last_iw(struct data_iw *curr)
{
	struct data_iw *keep = NULL;
	struct data_iw *ptr = NULL, *last_keep = NULL;
	ptr = curr;
	while (ptr)
	{
		if (!keep)
		{
			keep = (struct data_iw*)kmalloc(sizeof(struct data_iw), GFP_KERNEL);
			memset(keep, 0, sizeof(struct data_iw));

			memcpy(keep->station, ptr->station, 6);
			keep->device 				= ptr->device;
			keep->inactive_time 		= ptr->inactive_time;
			keep->rx_bytes 				= ptr->rx_bytes;
			keep->rx_packets 			= ptr->rx_packets;
			keep->tx_bytes 				= ptr->tx_bytes;
			keep->tx_packets 			= ptr->tx_packets;
			keep->tx_retries 			= ptr->tx_retries;
			keep->tx_failed 			= ptr->tx_failed;
			keep->signal 				= ptr->signal;
			keep->signal_avg 			= ptr->signal_avg;
			keep->expected_throughput 	= ptr->expected_throughput;
			keep->next = NULL;
			last_keep = keep;
		}
		else
		{
			struct data_iw *new = NULL;
			new = (struct data_iw*)kmalloc(sizeof(struct data_iw), GFP_KERNEL);
			memset(new, 0, sizeof(struct data_iw));

			memcpy(new->station, ptr->station, 6);
			new->device 				= ptr->device;
			new->inactive_time 			= ptr->inactive_time;
			new->rx_bytes 				= ptr->rx_bytes;
			new->rx_packets 			= ptr->rx_packets;
			new->tx_bytes 				= ptr->tx_bytes;
			new->tx_packets 			= ptr->tx_packets;
			new->tx_retries 			= ptr->tx_retries;
			new->tx_failed 				= ptr->tx_failed;
			new->signal 				= ptr->signal;
			new->signal_avg 			= ptr->signal_avg;
			new->expected_throughput 	= ptr->expected_throughput;
			new->next = NULL;
			last_keep->next = new;
			last_keep = new;
		}
		ptr = ptr->next;
	}
	iwlist_last = keep;
}

/**
 * Copy the whole beaconlist from scan.c to local beacon list.
 * @Linger
 * @DateTime 2018-05-06T02:23:11-0500
 * @param    curr                     beaconlist head address.
 */
void copy_last_beacon(struct data_beacon *curr)
{
	struct data_beacon *keep = NULL;
	struct data_beacon *ptr = NULL, *last_keep = NULL;
	ptr = curr;
	while (ptr)
	{
		if (!keep)
		{
			keep = (struct data_beacon*)kmalloc(sizeof(struct data_beacon), GFP_KERNEL);
			memset(keep, 0, sizeof(struct data_beacon));

			keep->data_rate = ptr->data_rate;
			keep->freq 		= ptr->freq;
			keep->signal 	= ptr->signal;
			memcpy(keep->bssid, ptr->bssid, 6);

			keep->next 		= NULL;
			last_keep 		= keep;
		}
		else
		{
			struct data_beacon *new = NULL;
			new = (struct data_beacon*)kmalloc(sizeof(struct data_beacon), GFP_KERNEL);
			memset(new, 0, sizeof(struct data_beacon));

			new->data_rate 	= ptr->data_rate;
			new->freq 		= ptr->freq;
			new->signal 	= ptr->signal;
			memcpy(new->bssid, ptr->bssid, 6);

			new->next 		= NULL;
			last_keep->next = new;
			last_keep 		= new;
		}
		ptr = ptr->next;
	}
	beacon_last = keep;
}

/**
 * Check whether items in iwlist from status.c are transmitted in last round,
 * if transmitted, transmitted keep_iw, transmit new iw data item otherwise.
 * Condition is used to check whether this item is transmitted. tmp_t % 10 is used
 * for this senario: information mainted in server side missed, and it can no longer
 * decode the iw informaiton, so, we should update iw information periodly to avoid
 * this senarios.
 * Also, mac address are patched to data structure to indicate its origin AP.
 * @Linger
 * @DateTime 2018-05-06T02:23:48-0500
 * @param    ptr                      iwlist head address from status.c
 * @param    sur                      address of survey information got from status.c
 */

void compare_and_iwlist(struct data_iw *ptr, struct data_survey *sur)
{
	struct data_iw 	*tmp = NULL;
	struct ds_t 	s_t;
	struct apstates *tmp_aps = NULL;
	int count = 0;
	int i = 0;
	int duration = 0;
	tmp_aps = apstates_global + aps_head;
	memset(&s_t, 0, sizeof(struct ds_t));
	tmp = ptr;

	/**
	 * processing survey information.
	 */
	s_t.category 	= 	SURVEY;
	s_t.survey 		=  	*sur;
	memcpy(&(s_t.mac), mac_global, 6);
	if(s_t.survey.sec > last_sec_survey)
	{
		write_2_public_buffer(&s_t, sizeof(struct ds_t));
		last_sec_survey = s_t.survey.sec;
		last_usec_survey = s_t.survey.usec;
	}
	else if ((s_t.survey.sec == last_sec_survey) && (s_t.survey.usec > last_usec_survey))
	{
		write_2_public_buffer(&s_t, sizeof(struct ds_t));
		last_usec_survey = s_t.survey.usec;
	}
	
	while (tmp)
	{
		struct di_t t;
		memset(&t, 0, sizeof(struct di_t));
		t.category = IW;
		memcpy(&(t.mac), mac_global, 6);
		t.iw = *tmp;
		t.iw.next = NULL;
		count += 1;
		if(t.iw.sec > last_sec_iw)
		{
			write_2_public_buffer(&t, sizeof(struct di_t));
			last_sec_iw = t.iw.sec;
			last_usec_iw = t.iw.usec;
		}
		else if ((t.iw.sec == last_sec_iw) && (t.iw.usec > last_usec_iw))
		{
			write_2_public_buffer(&t, sizeof(struct di_t));
			last_usec_iw = t.iw.usec;
		}
		
		if((count_iw % 50) == 0)
		{
			char mac_addr[18];
			i = 0;
			// printk(KERN_DEBUG "heree");
			mac_tranADDR_toString_r(tmp->station, mac_addr, 18);
			for(i = 0; i < 10; i++)
			{
				struct iw_record *xtmp = NULL;
				xtmp = iw_rec + i;
				if(xtmp->sec == 0)
				{
					memcpy(xtmp->station, tmp->station, 6);
					xtmp->sec = tmp->sec;
					xtmp->usec = tmp->usec;
					xtmp->bytes = tmp->tx_bytes;
					xtmp->computed = 2;
					// printk(KERN_DEBUG "heree %u %d %u\n", tmp->sec, i, iw_rec[i].sec);
					break;
				}
				else if ((strcmp_linger(xtmp->station, tmp->station) == 0))
				{	
					__u32 duration = 0;
					if(tmp->usec >= xtmp->usec)
					{
						__u32 rate = 0;
						duration = tmp->usec - xtmp->usec;
						duration = duration / 1000;
						duration = duration + (tmp->sec - xtmp->sec) * 1000;
						rate = 1000 * (tmp->tx_bytes - xtmp->bytes) / duration;
						xtmp->rates = rate;
						xtmp->computed = 1;
						xtmp->sec = tmp->sec;
						xtmp->usec = tmp->usec;
						xtmp->bytes = tmp->tx_bytes;
					}
					else
					{
						__u32 rate = 0;
						duration = xtmp->usec - tmp->sec;
						duration = duration / 1000;
						duration = (tmp->sec - xtmp->sec) * 1000 - duration;
						rate = 1000 * (tmp->tx_bytes - xtmp->bytes) / duration;
						xtmp->rates = rate;	
						xtmp->computed = 1;	
						xtmp->sec = tmp->sec;
						xtmp->usec = tmp->usec;
						xtmp->bytes = tmp->tx_bytes;				
					}
					// printk(KERN_DEBUG "heree %s %u\n", mac_addr, xtmp->rates);
					if(xtmp->rates >= maxrateflow.rates)
					{
						maxrateflow = *xtmp;
					}
					// printk(KERN_DEBUG "here2");
					break;
				}
			}

		}
		tmp = tmp->next;
	}
	i = 0;
	duration = (int)s_t.survey.sec;
	for(i = 0; i < 10; i++)
	{
		struct iw_record *xtmp = NULL;
		int duration1 = 0;
		xtmp = iw_rec + i;
		
		duration1 = duration - (int)xtmp->sec;
		if(duration1 > 2)
		{
			memset(xtmp, 0, sizeof(struct iw_record));
		}
	}
	count_iw += 1;
	count_iw = count_iw % 500000;
	tmp_aps->stations = count;
}
/**
 * Check whether to transmit current queue information got.
 * @Linger
 * @DateTime 2018-05-06T02:32:44-0500
 * @param    ptr                      Address of queue got from sch_fq_codel.c
 */
void compare_and_queue(struct data_queue *ptr)
{
	int i = 0;
	int condition = 0;
	struct apstates *tmp;
	tmp = apstates_global + aps_head;
	
	for(i = 0; i < 6; i++)
	{
		// if(i < 5)
		// {
			tmp->packets += (ptr + i)->packets;
			tmp->bytes += (ptr + i)->bytes;
			tmp->qlen += (ptr + i)->qlen;
			tmp->backlog += (ptr + i)->backlog;
			tmp->drops += (ptr + i)->drops;
			tmp->requeues += (ptr + i)->requeues;
			tmp->overlimits += (ptr + i)->overlimits;
		// }
		condition = (queue_last[i].queue_id 	== (ptr + i)->queue_id) 	+
		            (queue_last[i].qlen 		== (ptr + i)->qlen) 		+
		            (queue_last[i].backlog 		== (ptr + i)->backlog) 	+
		            (queue_last[i].drops 		== (ptr + i)->drops) 		+
		            (queue_last[i].requeues 	== (ptr + i)->requeues) 	+
		            (queue_last[i].overlimits  	== (ptr + i)->overlimits);

		if (condition != 6)
		{
			struct dq_t t;
			memset(&t, 0, sizeof(struct dq_t));
			t.category = QUEUE;
			t.queue.queue_id 	= (ptr + i)->queue_id;
			t.queue.bytes 		= (ptr + i)->bytes;
			t.queue.packets 	= (ptr + i)->packets;
			t.queue.qlen 		= (ptr + i)->qlen;
			t.queue.backlog 	= (ptr + i)->backlog;
			t.queue.drops 		= (ptr + i)->drops;
			t.queue.requeues 	= (ptr + i)->requeues;
			t.queue.overlimits 	= (ptr + i)->overlimits;
			t.queue.sec 		= (ptr + i)->sec;
			t.queue.usec		= (ptr + i)->usec;
			memcpy(&(t.mac), mac_global, 6);
			// if(t.queue.sec > last_sec_queue)
			// {
			// 	write_2_public_buffer(&t, sizeof(struct dq_t));
			// 	last_sec_queue = t.queue.sec;
			// 	last_usec_queue = t.queue.usec;
			// }
			// else if ((t.queue.sec == last_sec_queue) && (t.queue.usec > last_usec_queue))
			// {
			// 	write_2_public_buffer(&t, sizeof(struct dq_t));
			// 	last_usec_queue = t.queue.usec;
			// }
			write_2_public_buffer(&t, sizeof(struct dq_t));
			// *queue_last = *ptr;
		}
		queue_last[i] = *(ptr + i);
	}
}

/**
 * Simillar as compare_and_iwlist
 * @Linger
 * @DateTime 2018-05-06T02:34:26-0500
 * @param    ptr                      [description]
 */
void compare_and_beacon(struct data_beacon *ptr)
{
	struct data_beacon *tmp = NULL;
	tmp = ptr;
	while (tmp)
	{
		struct db_t t;
		memset(&t, 0, sizeof(struct db_t));
		t.category = BEACON;
		memcpy(&(t.mac), mac_global, 6);
		t.beacon = *tmp;
		t.beacon.next = NULL;
		if(t.beacon.sec > last_sec_beacon)
		{
			write_2_public_buffer(&t, sizeof(struct db_t));
			last_sec_beacon = t.beacon.sec;
			last_usec_beacon = t.beacon.usec;
		}
		else if((t.beacon.sec == last_sec_beacon) && (t.beacon.usec > last_usec_beacon))
		{
			write_2_public_buffer(&t, sizeof(struct db_t));
			last_usec_beacon = t.beacon.usec;
		}
		tmp = tmp->next;
	}
	destroy_list_beacon(beacon_last);
	copy_last_beacon(ptr);
}

/**
 * Get network information from drivers.
 * @Linger
 * @DateTime 2018-05-06T02:35:33-0500
 */
void get_information(void)
{
	struct 	data_iw 	*iwlist = NULL;
	struct 	dp_packet 	*dp_mac = NULL;
	struct 	dp_packet 	*dp_que = NULL;
	struct 	data_queue 	*queue 	= NULL;
	struct 	data_beacon *beacon = NULL;
	struct data_survey survey_linger;
	int drop_c = 0;

	// memset(queue_last, 0, sizeof(struct data_queue));

	control_beacon += 1;
	control_beacon = control_beacon % 10000000;
	// if ((control_beacon % 3) == 0)
	// {
	memset(&survey_linger, 0, sizeof(struct data_survey));
	iwlist 		= ieee80211_dump_station_linger(&survey_linger);
	// }
	if ((control_beacon % 10) == 0)
		beacon 		= get_beacon_global_linger();
	queue 		= dump_class_stats_linger();
	dp_que 		= dpt_queue_linger();
	dp_mac 		= dpt_mac_linger();


	if (iwlist)
	{
		struct apstates *tmp;
		tmp = apstates_global + aps_head;
		tmp->time = survey_linger.time;
		tmp->time_busy = survey_linger.time_busy;
		tmp->time_ext_busy = survey_linger.time_ext_busy;
		tmp->time_rx = survey_linger.time_rx;
		tmp->time_tx = survey_linger.time_tx;
		tmp->time_scan = survey_linger.time_scan;
		tmp->noise = survey_linger.noise;
		tmp->sec = survey_linger.sec;
		tmp->usec = survey_linger.usec;
		// printk(KERN_DEBUG "%u  %u", tmp->sec, tmp->usec);
		compare_and_iwlist(iwlist, &survey_linger);
		destroy_iwlist(iwlist);
	}

	if (dp_mac)
	{
		struct dp_packet *ptr = NULL;
		ptr = dp_mac;
		while (ptr)
		{
			struct dp_t dp_m_w;
			memset(&dp_m_w, 0, sizeof(struct dp_t));
			dp_m_w.drops = *ptr;
			dp_m_w.drops.next = NULL;
			dp_m_w.category = DROPS;
			memcpy(&(dp_m_w.mac), mac_global, 6);
			if(dp_m_w.drops.sec > last_sec_drop_mac)
			{
				write_2_public_buffer(&dp_m_w, sizeof(struct dp_t));
				last_sec_drop_mac = dp_m_w.drops.sec;
				last_usec_drop_mac = dp_m_w.drops.usec;
				drop_c += 1; // used to calculate drop_count.
			}
			else if ((dp_m_w.drops.sec == last_sec_drop_mac) && (dp_m_w.drops.usec > last_usec_drop_mac))
			{
				write_2_public_buffer(&dp_m_w, sizeof(struct dp_t));
				last_usec_drop_mac = dp_m_w.drops.usec;
				drop_c += 1;
			}
			
			ptr = ptr->next;
		}
		destroy_after_transmitted_mac80211();
		// printk(KERN_DEBUG "destroy_after_transmitted_mac80211 in sendudp\n");
	}

	if (beacon)
	{
		compare_and_beacon(beacon);
	}
	if (queue)
	{	
		compare_and_queue(queue);
	}

	if (dp_que)
	{
		struct dp_packet *ptr = NULL;
		ptr = dp_que;
		while (ptr)
		{
			struct dp_t dp_q_w;
			memset(&dp_q_w, 0, sizeof(struct dp_t));
			dp_q_w.drops = *ptr;
			dp_q_w.drops.next = NULL;
			dp_q_w.category = DROPS;
			memcpy(&(dp_q_w.mac), mac_global, 6);
			if(dp_q_w.drops.sec > last_sec_drop_queue)
			{
				write_2_public_buffer(&dp_q_w, sizeof(struct dp_t));
				last_sec_drop_queue = dp_q_w.drops.sec;
				last_usec_drop_queue = dp_q_w.drops.usec;
				drop_c += 1;
			}
			else if ((dp_q_w.drops.sec == last_sec_drop_queue) && (dp_q_w.drops.usec > last_usec_drop_queue))
			{
				write_2_public_buffer(&dp_q_w, sizeof(struct dp_t));
				last_usec_drop_queue = dp_q_w.drops.usec;
				drop_c += 1; 
			}
			
			ptr = ptr->next;
		}
		destroy_after_transmitted_codel();
		// printk(KERN_DEBUG "destroy_after_transmitted_codel in sendudp\n");
	}
	apstates_global[aps_head].drop_count = drop_c;
	aps_head  += 1;
	aps_head = aps_head % APSL;
	// printk(KERN_DEBUG "fuck %u %u\n", aps_head, aps_tail);
	if (aps_head == aps_tail)
	{
		aps_tail += 1;
		aps_tail = aps_tail % APSL;
		// printk(KERN_DEBUG "xxoo %u %u\n", aps_head, aps_tail);
	}
}

/**
 * Delete global variables and data sturctures when module exit.
 * @Linger
 * @DateTime 2018-05-06T02:39:12-0500
 */
void destroy_data(void)
{
	struct data_iw *pre_iw = NULL;
	struct data_iw *ptr_iw = NULL;
	// struct data_queue *ptr_que = NULL;
	struct data_beacon *pre_beacon = NULL;
	struct data_beacon *ptr_beacon = NULL;

	ptr_iw = iwlist_last;
	while (ptr_iw)
	{
		pre_iw = ptr_iw;
		ptr_iw = ptr_iw->next;
		kfree(pre_iw);
		pre_iw = NULL;
	}

	// ptr_que = queue_last;
	// if (ptr_que)
	// {
	// 	kfree(ptr_que);
	// 	ptr_que = NULL;
	// }
	ptr_beacon = beacon_last;
	while (ptr_beacon)
	{
		pre_beacon = ptr_beacon;
		ptr_beacon = ptr_beacon->next;
		kfree(pre_beacon);
		pre_beacon = NULL;
	}
}


/**
 * Print iwlist members, used when debugging.
 * @Linger
 * @DateTime 2018-05-06T02:43:44-0500
 * @param    t                        iwlist address
 * @param    str                      extra informations.
 */
void print_iwlist(struct data_iw *t, char *str)
{
	struct data_iw *ptr = NULL;
	ptr = t;
	while (ptr)
	{
		printk(KERN_DEBUG "%s\t%u\n", str, ptr->rx_packets);
		ptr = ptr->next;
	}
}

static int hello_init(void)
{
	struct net_device *dev;
	char *ifname = "br0";
	int i = 0;
	for(i = 0; i < 6; i++)
	{
		memset(&queue_last[i], 0, sizeof(struct data_queue));
	}
	

	printk(KERN_DEBUG "Hello, kernel %lu %lu %lu %lu", sizeof(struct data_iw), sizeof(struct data_iw *), sizeof(struct dp_packet), sizeof(struct data_beacon));
	sock_global = get_sock();

	dev = __dev_get_by_name(sock_net(sock_global->sk), ifname);
	if (dev)
	{
		memset(mac_global, 0, 6);
		memcpy(mac_global, dev->dev_addr, 6);
	}
	for(i = 0; i < 10; i++)
	{
		memset(&iw_rec[i], 0, sizeof(struct iw_record));
	}
	memset(&maxrateflow, 0, sizeof(struct iw_record));
    printk("init netlink_demo!\n");


    netlinkfd = netlink_kernel_create(&init_net, USER_MSG, &cfg);
    if(!netlinkfd)
    {
        printk(KERN_ERR "can not create a netlink socket!\n");
        return -1;
    }

    printk("netlink demo init ok!\n");

	write_begin = 0;
	memset(&public_buffer, 0, B_S);

	init_timer(&tm);    //初始化内核定时器

	do_gettimeofday(&oldtv);        //获取当前时间
	tm.function = (void *)&callback;           //指定定时时间到后的回调函数
	// tm.data    = (unsigned long)"hello world";        //回调函数的参数
	tm.expires = jiffies + 1;       //定时时间
	add_timer(&tm);

	return 0;
}

static void hello_exit(void)
{
	if (sock_global)
	{
		sock_release(sock_global);
		sock_global = NULL;
	}
	del_timer(&tm);
	destroy_data();
    sock_release(netlinkfd->sk_socket);
    printk(KERN_DEBUG "netlink exit\n!");
	printk(KERN_ALERT "Goodbye,Cruel world\n");
}

int strcmp_linger(char *s, char *t)
{
	int i = 0;
	int condition = 0;
	for (i = 0; i < 6; i++)
	{
		int tmp = 1000;
		tmp = (int) * (s + i) - (int) * (t + i);
		if (tmp == 0)
			condition += 1;
	}
	return (condition == 6) ? 0 : 1;
}

module_init(hello_init);
module_exit(hello_exit);
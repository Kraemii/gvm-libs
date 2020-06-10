/* Copyright (C) 2020 Greenbone Networks GmbH
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "alivedetection.h"

#include "../base/networking.h" /* for gvm_source_addr(), gvm_routethrough() */
#include "../base/prefs.h"      /* for prefs_get() */
#include "../util/kb.h"         /* for kb_t operations */
#include "boreas_error.h"
#include "boreas_io.h"
#include "ping.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h> /* for getifaddrs() */
#include <net/ethernet.h>
#include <net/if.h> /* for if_nametoindex() */
#include <net/if_arp.h>
#include <netinet/icmp6.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netpacket/packet.h> /* for sockaddr_ll */
#include <pcap.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "alive scan"

struct scanner scanner;
struct scan_restrictions scan_restrictions;
struct hosts_data hosts_data;

/* for using int value in #defined string */
#define STR(X) #X
#define ASSTR(X) STR (X)
#define FILTER_STR                                                           \
  "(ip6 or ip or arp) and (ip6[40]=129 or icmp[icmptype] == icmp-echoreply " \
  "or dst port " ASSTR (FILTER_PORT) " or arp[6:2]=2)"

/* Conditional variable and mutex to make sure sniffer thread already started
 * before sending out pings. */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

/* Max_scan_hosts and max_alive_hosts related struct. */
struct scan_restrictions
{
  /* Maximum number of hosts allowed to be scanned. No more alive hosts are put
   * on the queue after max_scan_hosts number of alive hosts is reached.
   * max_scan_hosts_reached is set to true and the finish signal gets put on
   * the queue. */
  int max_scan_hosts;
  /* Maximum number of hosts to be identified as alive. After max_alive_hosts
   * number of hosts are identified as alive max_alive_hosts_reached is set to
   * true which signals the stop of sending new pings. */
  int max_alive_hosts;
  /* Count of unique identified alive hosts. */
  int alive_hosts_count;
  gboolean max_scan_hosts_reached;
  gboolean max_alive_hosts_reached;
};

/**
 * @brief The hosts_data struct holds the alive hosts and target hosts in
 * separate hashtables.
 */
struct hosts_data
{
  /* Set of the form (ip_str, ip_str).
   * Hosts which passed our pcap filter. May include hosts which are alive but
   * are not in the targethosts list */
  GHashTable *alivehosts;
  /* Hashtable of the form (ip_str, gvm_host_t *). The gvm_host_t pointers point
   * to hosts which are to be freed by the caller of start_alive_detection(). */
  GHashTable *targethosts;
  /* Hosts which were detected as alive and are in the targetlist but are not
   * sent to openvas because max_scan_hosts was reached. */
  GHashTable *alivehosts_not_to_be_sent_to_openvas;
};

struct sniff_ethernet
{
  u_char ether_dhost[ETHER_ADDR_LEN]; /* Destination host address */
  u_char ether_shost[ETHER_ADDR_LEN]; /* Source host address */
  u_short ether_type;                 /* IP? ARP? RARP? etc */
};

/* Getter for scan_restrictions. */

int
max_scan_hosts_reached ()
{
  return scan_restrictions.max_scan_hosts_reached;
}
int
get_alive_hosts_count ()
{
  return scan_restrictions.alive_hosts_count;
}
int
get_max_scan_hosts ()
{
  return scan_restrictions.max_scan_hosts;
}

/**
 * @brief open a new pcap handle ad set provided filter.
 *
 * @param iface interface to use.
 * @param filter pcap filter to use.
 *
 * @return pcap_t handle or NULL on error
 */
static pcap_t *
open_live (char *iface, char *filter)
{
  /* iface considerations:
   * pcap_open_live(iface, ...) sniffs on all interfaces(linux) if iface
   * argument is NULL pcap_lookupnet(iface, ...) is used to set ipv4 network
   * number and mask associated with iface pcap_compile(..., mask) netmask
   * specifies the IPv4 netmask of the network on which packets are being
   * captured; it is used only when checking for IPv4 broadcast addresses in the
   * filter program
   *
   *  If we are not checking for IPv4 broadcast addresses in the filter program
   * we do not need an iface (if we also want to listen on all interface) and we
   * do not need to call pcap_lookupnet
   */
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *pcap_handle;
  struct bpf_program filter_prog;

  /* iface, snapshot length of handle, promiscuous mode, packet buffer timeout
   * (ms), errbuff */
  errbuf[0] = '\0';
  pcap_handle = pcap_open_live (iface, 1500, 0, 100, errbuf);
  if (pcap_handle == NULL)
    {
      g_warning ("%s: %s", __func__, errbuf);
      return NULL;
    }
  if (g_utf8_strlen (errbuf, -1) != 0)
    {
      g_warning ("%s: %s", __func__, errbuf);
    }

  /* handle, struct bpf_program *fp, int optimize, bpf_u_int32 netmask */
  if (pcap_compile (pcap_handle, &filter_prog, filter, 1, PCAP_NETMASK_UNKNOWN)
      < 0)
    {
      char *msg = pcap_geterr (pcap_handle);
      g_warning ("%s: %s", __func__, msg);
      pcap_close (pcap_handle);
      return NULL;
    }

  if (pcap_setfilter (pcap_handle, &filter_prog) < 0)
    {
      char *msg = pcap_geterr (pcap_handle);
      g_warning ("%s: %s", __func__, msg);
      pcap_close (pcap_handle);
      return NULL;
    }
  pcap_freecode (&filter_prog);

  return pcap_handle;
}

/**
 * @brief Handle restrictions imposed by max_scan_hosts and max_alive_hosts.
 *
 * Put host address string on alive detection queue if max_scan_hosts was not
 * reached already. If max_scan_hosts was reached only count alive hosts and
 * don't put them on the queue. Put finish signal on queue if max_scan_hosts is
 * reached.
 *
 * @param add_str Host address string to put on queue.
 */
static void
handle_scan_restrictions (gchar *addr_str)
{
  scan_restrictions.alive_hosts_count++;
  /* Put alive hosts on queue as long as max_scan_hosts not reached. */
  if (!scan_restrictions.max_scan_hosts_reached)
    put_host_on_queue (scanner.main_kb, addr_str);
  else
    g_hash_table_add (hosts_data.alivehosts_not_to_be_sent_to_openvas,
                      addr_str);

  /* Put finish signal on queue if max_scan_hosts reached. */
  if (!scan_restrictions.max_scan_hosts_reached
      && (scan_restrictions.alive_hosts_count
          == scan_restrictions.max_scan_hosts))
    {
      int err;
      scan_restrictions.max_scan_hosts_reached = TRUE;
      put_finish_signal_on_queue (&err);
      if (err)
        g_debug ("%s: Error in put_finish_signal_on_queue(): %d ", __func__,
                 err);
    }
  /* Thread which sends out new pings should stop sending when max_alive_hosts
   * is reached. */
  if (scan_restrictions.alive_hosts_count == scan_restrictions.max_alive_hosts)
    scan_restrictions.max_alive_hosts_reached = TRUE;
}

/**
 * @brief Processes single packets captured by pcap. Is a callback function.
 *
 * For every packet we check if it is ipv4 ipv6 or arp and extract the sender ip
 * address. This ip address is then inserted into the alive_hosts table if not
 * already present and if in the target table.
 *
 * @param args
 * @param header
 * @param packet  Packet to process.
 *
 * TODO: simplify and read https://tools.ietf.org/html/rfc826
 */
static void
got_packet (__attribute__ ((unused)) u_char *args,
            __attribute__ ((unused)) const struct pcap_pkthdr *header,
            const u_char *packet)
{
  struct ip *ip = (struct ip *) (packet + 16); // why not 14(ethernet size)??
  unsigned int version = ip->ip_v;

  /* Stop processing of packets if max_alive_hosts is reached. */
  if (scan_restrictions.max_alive_hosts_reached)
    return;

  if (version == 4)
    {
      gchar addr_str[INET_ADDRSTRLEN];
      struct in_addr sniffed_addr;
      /* was +26 (14 ETH + 12 IP) originally but was off by 2 somehow */
      memcpy (&sniffed_addr.s_addr, packet + 26 + 2, 4);
      if (inet_ntop (AF_INET, (const char *) &sniffed_addr, addr_str,
                     INET_ADDRSTRLEN)
          == NULL)
        g_debug (
          "%s: Failed to transform IPv4 address into string representation: %s",
          __func__, strerror (errno));

      /* Do not put already found host on Queue and only put hosts on Queue we
       * are searching for. */
      if (g_hash_table_add (hosts_data.alivehosts, g_strdup (addr_str))
          && g_hash_table_contains (hosts_data.targethosts, addr_str) == TRUE)
        {
          /* handle max_scan_hosts and max_alive_hosts related restrictions. */
          handle_scan_restrictions (addr_str);
        }
    }
  else if (version == 6)
    {
      gchar addr_str[INET6_ADDRSTRLEN];
      struct in6_addr sniffed_addr;
      /* (14 ETH + 8 IP + offset 2)  */
      memcpy (&sniffed_addr.s6_addr, packet + 24, 16);
      if (inet_ntop (AF_INET6, (const char *) &sniffed_addr, addr_str,
                     INET6_ADDRSTRLEN)
          == NULL)
        g_debug ("%s: Failed to transform IPv6 into string representation: %s",
                 __func__, strerror (errno));

      /* Do not put already found host on Queue and only put hosts on Queue we
       * are searching for. */
      if (g_hash_table_add (hosts_data.alivehosts, g_strdup (addr_str))
          && g_hash_table_contains (hosts_data.targethosts, addr_str) == TRUE)
        {
          /* handle max_scan_hosts and max_alive_hosts related restrictions. */
          handle_scan_restrictions (addr_str);
        }
    }
  /* TODO: check collision situations.
   * everything not ipv4/6 is regarded as arp.
   * It may be possible to get other types then arp replies in which case the
   * ip from inet_ntop should be bogus. */
  else
    {
      /* TODO: at the moment offset of 6 is set but arp header has variable
       * sized field. */
      /* read rfc https://tools.ietf.org/html/rfc826 for exact length or how
      to get it */
      struct arphdr *arp =
        (struct arphdr *) (packet + 14 + 2 + 6 + sizeof (struct arphdr));
      gchar addr_str[INET_ADDRSTRLEN];
      if (inet_ntop (AF_INET, (const char *) arp, addr_str, INET_ADDRSTRLEN)
          == NULL)
        g_debug ("%s: Failed to transform IP into string representation: %s",
                 __func__, strerror (errno));

      /* Do not put already found host on Queue and only put hosts on Queue
      we are searching for. */
      if (g_hash_table_add (hosts_data.alivehosts, g_strdup (addr_str))
          && g_hash_table_contains (hosts_data.targethosts, addr_str) == TRUE)
        {
          /* handle max_scan_hosts and max_alive_hosts related restrictions. */
          handle_scan_restrictions (addr_str);
        }
    }
}

/**
 * @brief Sniff packets by starting pcap_loop with callback function.
 *
 * @param vargp
 */
static void *
sniffer_thread (__attribute__ ((unused)) void *vargp)
{
  int ret;
  pthread_mutex_lock (&mutex);
  pthread_cond_signal (&cond);
  pthread_mutex_unlock (&mutex);

  /* reads packets until error or pcap_breakloop() */
  if ((ret = pcap_loop (scanner.pcap_handle, -1, got_packet, NULL))
      == PCAP_ERROR)
    g_debug ("%s: pcap_loop error %s", __func__,
             pcap_geterr (scanner.pcap_handle));
  else if (ret == 0)
    g_debug ("%s: count of packets is exhausted", __func__);
  else if (ret == PCAP_ERROR_BREAK)
    g_debug ("%s: Loop was successfully broken after call to pcap_breakloop",
             __func__);

  pthread_exit (0);
}

/**
 * @brief delete key from hashtable
 *
 * @param key Key to delete from hashtable
 * @param value
 * @param hashtable   table to remove keys from
 *
 */
static void
exclude (gpointer key, __attribute__ ((unused)) gpointer value,
         gpointer hashtable)
{
  /* delete key from targethost*/
  g_hash_table_remove (hashtable, (gchar *) key);
}

__attribute__ ((unused)) static void
print_host_str (gpointer key, __attribute__ ((unused)) gpointer value,
                __attribute__ ((unused)) gpointer user_data)
{
  g_message ("host_str: %s", (gchar *) key);
}

/**
 * @brief Is called in g_hash_table_foreach(). Check if ipv6 or ipv4, get
 * correct socket and start appropriate ping function.
 *
 * @param key Ip string.
 * @param value Pointer to gvm_host_t.
 * @param user_data
 */
static void
send_icmp (__attribute__ ((unused)) gpointer key, gpointer value,
           __attribute__ ((unused)) gpointer user_data)
{
  struct in6_addr dst6;
  struct in6_addr *dst6_p = &dst6;
  struct in_addr dst4;
  struct in_addr *dst4_p = &dst4;
  static int count = 0;

  count++;
  if (count % BURST == 0)
    usleep (BURST_TIMEOUT);

  if (gvm_host_get_addr6 ((gvm_host_t *) value, dst6_p) < 0)
    g_warning ("%s: could not get addr6 from gvm_host_t", __func__);
  if (dst6_p == NULL)
    {
      g_warning ("%s: destination address is NULL", __func__);
      return;
    }
  if (IN6_IS_ADDR_V4MAPPED (dst6_p) != 1)
    {
      send_icmp_v6 (scanner.icmpv6soc, dst6_p, ICMP6_ECHO_REQUEST);
    }
  else
    {
      dst4.s_addr = dst6_p->s6_addr32[3];
      send_icmp_v4 (scanner.icmpv4soc, dst4_p);
    }
}

/**
 * @brief Is called in g_hash_table_foreach(). Check if ipv6 or ipv4, get
 * correct socket and start appropriate ping function.
 *
 * @param key Ip string.
 * @param value Pointer to gvm_host_t.
 * @param user_data
 */
static void
send_tcp (__attribute__ ((unused)) gpointer key, gpointer value,
          __attribute__ ((unused)) gpointer user_data)
{
  static int count = 0;
  count++;
  if (count % BURST == 0)
    usleep (BURST_TIMEOUT);

  struct in6_addr dst6;
  struct in6_addr *dst6_p = &dst6;
  struct in_addr dst4;
  struct in_addr *dst4_p = &dst4;

  if (gvm_host_get_addr6 ((gvm_host_t *) value, dst6_p) < 0)
    g_warning ("%s: could not get addr6 from gvm_host_t", __func__);
  if (dst6_p == NULL)
    {
      g_warning ("%s: destination address is NULL", __func__);
      return;
    }
  if (IN6_IS_ADDR_V4MAPPED (dst6_p) != 1)
    {
      // send_tcp_v6 (scanner.tcpv6soc, dst6_p, scanner.tcp_flag);
      send_tcp_v6 (&scanner, dst6_p);
    }
  else
    {
      dst4.s_addr = dst6_p->s6_addr32[3];
      send_tcp_v4 (&scanner, dst4_p);
    }
}

/**
 * @brief Is called in g_hash_table_foreach(). Check if ipv6 or ipv4, get
 * correct socket and start appropriate ping function.
 *
 * @param key Ip string.
 * @param value Pointer to gvm_host_t.
 * @param user_data
 */
static void
send_arp (__attribute__ ((unused)) gpointer key, gpointer value,
          __attribute__ ((unused)) gpointer user_data)
{
  struct in6_addr dst6;
  struct in6_addr *dst6_p = &dst6;
  struct in_addr dst4;
  struct in_addr *dst4_p = &dst4;

  static int count = 0;
  count++;
  if (count % BURST == 0)
    usleep (BURST_TIMEOUT);

  if (gvm_host_get_addr6 ((gvm_host_t *) value, dst6_p) < 0)
    g_warning ("%s: could not get addr6 from gvm_host_t", __func__);
  if (dst6_p == NULL)
    {
      g_warning ("%s: destination address is NULL", __func__);
      return;
    }
  if (IN6_IS_ADDR_V4MAPPED (dst6_p) != 1)
    {
      /* IPv6 does simulate ARP by using the Neighbor Discovery Protocol with
       * ICMPv6. */
      send_icmp_v6 (scanner.arpv6soc, dst6_p, ND_NEIGHBOR_SOLICIT);
    }
  else
    {
      dst4.s_addr = dst6_p->s6_addr32[3];
      send_arp_v4 (scanner.arpv4soc, dst4_p);
    }
}

/**
 * @brief Send the number of dead hosts to ospd-openvas.
 *
 * This information is needed for the calculation of the progress bar for gsa in
 * ospd-openvas.
 *
 * @param hosts_data  Includes all data which is needed for calculating the
 * number of dead hosts.
 *
 * @return number of dead IPs, or -1 in case of an error.
 */
static int
send_dead_hosts_to_ospd_openvas (struct hosts_data *hosts_data)
{
  kb_t main_kb = NULL;
  int maindbid;
  int count_dead_ips = 0;
  char dead_host_msg_to_ospd_openvas[2048];

  GHashTableIter target_hosts_iter;
  gpointer host_str, value;

  maindbid = atoi (prefs_get ("ov_maindbid"));
  main_kb = kb_direct_conn (prefs_get ("db_address"), maindbid);

  if (!main_kb)
    {
      g_debug ("%s: Could not connect to main_kb for sending dead hosts to "
               "ospd-openvas.",
               __func__);
      return -1;
    }

  /* Delete all alive hosts which are not send to openvas because
   * max_alive_hosts was reached, from the alivehosts list. These hosts are
   * considered as dead by the progress bar of the openvas vuln scan because no
   * vuln scan was ever started for them. */
  g_hash_table_foreach (hosts_data->alivehosts_not_to_be_sent_to_openvas,
                        exclude, hosts_data->alivehosts);

  for (g_hash_table_iter_init (&target_hosts_iter, hosts_data->targethosts);
       g_hash_table_iter_next (&target_hosts_iter, &host_str, &value);)
    {
      /* If a host in the target hosts is not in the list of alive hosts we know
       * it is dead. */
      if (!g_hash_table_contains (hosts_data->alivehosts, host_str))
        {
          count_dead_ips++;
        }
    }

  snprintf (dead_host_msg_to_ospd_openvas,
            sizeof (dead_host_msg_to_ospd_openvas), "DEADHOST||| ||| ||| |||%d",
            count_dead_ips);
  kb_item_push_str (main_kb, "internal/results", dead_host_msg_to_ospd_openvas);

  kb_lnk_reset (main_kb);

  return count_dead_ips;
}

/**
 * @brief Scan function starts a sniffing thread which waits for packets to
 * arrive and sends pings to hosts we want to test. Blocks until Scan is
 * finished or error occurred.
 *
 * Start a sniffer thread. Get what method of alive detection to use. Send
 * appropriate pings  for every host we want to test.
 *
 * @return 0 on success, <0 on failure.
 */
static int
scan (alive_test_t alive_test)
{
  int number_of_targets, number_of_targets_checked = 0;
  int number_of_dead_hosts;
  int err;
  void *retval;
  pthread_t sniffer_thread_id;
  GHashTableIter target_hosts_iter;
  gpointer key, value;
  struct timeval start_time, end_time;
  int scandb_id = atoi (prefs_get ("ov_maindbid"));
  gchar *scan_id;
  kb_t main_kb = NULL;

  gettimeofday (&start_time, NULL);
  number_of_targets = g_hash_table_size (hosts_data.targethosts);

  scanner.pcap_handle = open_live (NULL, FILTER_STR);
  if (scanner.pcap_handle == NULL)
    {
      g_warning ("%s: Unable to open valid pcap handle.", __func__);
      return -2;
    }

  scan_id = get_openvas_scan_id (prefs_get ("db_address"), scandb_id);
  g_message ("Alive scan %s started: Target has %d hosts", scan_id,
             number_of_targets);

  /* Start sniffer thread. */
  err = pthread_create (&sniffer_thread_id, NULL, sniffer_thread, NULL);
  if (err == EAGAIN)
    g_warning ("%s: pthread_create() returned EAGAIN: Insufficient resources "
               "to create thread.",
               __func__);
  /* Wait for thread to start up before sending out pings. */
  pthread_mutex_lock (&mutex);
  pthread_cond_wait (&cond, &mutex);
  pthread_mutex_unlock (&mutex);
  /* Mutex and cond not needed anymore. */
  pthread_mutex_destroy (&mutex);
  pthread_cond_destroy (&cond);
  sleep (2);

  if (alive_test
      == (ALIVE_TEST_TCP_ACK_SERVICE | ALIVE_TEST_ICMP | ALIVE_TEST_ARP))
    {
      g_debug ("%s: ICMP, TCP-ACK Service & ARP Ping", __func__);
      g_debug ("%s: TCP-ACK Service Ping", __func__);
      scanner.tcp_flag = TH_ACK;
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_tcp (key, value, NULL);
          number_of_targets_checked++;
        }
      g_debug ("%s: ICMP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_icmp (key, value, NULL);
          number_of_targets_checked++;
        }
      g_debug ("%s: ARP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_arp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_TCP_ACK_SERVICE | ALIVE_TEST_ARP))
    {
      g_debug ("%s: TCP-ACK Service & ARP Ping", __func__);
      g_debug ("%s: TCP-ACK Service Ping", __func__);
      scanner.tcp_flag = TH_ACK;
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_tcp (key, value, NULL);
          number_of_targets_checked++;
        }
      g_debug ("%s: ARP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_arp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_ICMP | ALIVE_TEST_ARP))
    {
      g_debug ("%s: ICMP & ARP Ping", __func__);
      g_debug ("%s: ICMP PING", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_icmp (key, value, NULL);
          number_of_targets_checked++;
        }
      g_debug ("%s: ARP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_arp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_ICMP | ALIVE_TEST_TCP_ACK_SERVICE))
    {
      g_debug ("%s: ICMP & TCP-ACK Service Ping", __func__);
      g_debug ("%s: ICMP PING", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_icmp (key, value, NULL);
          number_of_targets_checked++;
        }
      g_debug ("%s: TCP-ACK Service Ping", __func__);
      scanner.tcp_flag = TH_ACK;
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_tcp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_ARP))
    {
      g_debug ("%s: ARP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_arp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_TCP_ACK_SERVICE))
    {
      scanner.tcp_flag = TH_ACK;
      g_debug ("%s: TCP-ACK Service Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_tcp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_TCP_SYN_SERVICE))
    {
      g_debug ("%s: TCP-SYN Service Ping", __func__);
      scanner.tcp_flag = TH_SYN;
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_tcp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_ICMP))
    {
      g_debug ("%s: ICMP Ping", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          send_icmp (key, value, NULL);
          number_of_targets_checked++;
        }
    }
  else if (alive_test == (ALIVE_TEST_CONSIDER_ALIVE))
    {
      g_debug ("%s: Consider Alive", __func__);
      for (g_hash_table_iter_init (&target_hosts_iter, hosts_data.targethosts);
           g_hash_table_iter_next (&target_hosts_iter, &key, &value)
           && !scan_restrictions.max_alive_hosts_reached;)
        {
          handle_scan_restrictions (key);
          number_of_targets_checked++;
        }
    }

  g_debug (
    "%s: all ping packets have been sent, wait a bit for rest of replies.",
    __func__);
  sleep (WAIT_FOR_REPLIES_TIMEOUT);

  g_debug ("%s: Try to stop thread which is sniffing for alive hosts. ",
           __func__);
  /* Try to break loop in sniffer thread. */
  pcap_breakloop (scanner.pcap_handle);
  /* Give thread chance to exit on its own. */
  sleep (2);

  /* Cancel thread. May be necessary if pcap_breakloop() does not break the
   * loop. */
  err = pthread_cancel (sniffer_thread_id);
  if (err == ESRCH)
    g_debug ("%s: pthread_cancel() returned ESRCH; No thread with the "
             "supplied ID could be found.",
             __func__);

  /* join sniffer thread*/
  err = pthread_join (sniffer_thread_id, &retval);
  if (err == EDEADLK)
    g_warning ("%s: pthread_join() returned EDEADLK.", __func__);
  if (err == EINVAL)
    g_warning ("%s: pthread_join() returned EINVAL.", __func__);
  if (err == ESRCH)
    g_warning ("%s: pthread_join() returned ESRCH.", __func__);
  if (retval == PTHREAD_CANCELED)
    g_debug ("%s: pthread_join() returned PTHREAD_CANCELED.", __func__);

  g_debug ("%s: Stopped thread which was sniffing for alive hosts.", __func__);

  /* close handle */
  if (scanner.pcap_handle != NULL)
    {
      pcap_close (scanner.pcap_handle);
    }

  /* Send error message if max_alive_hosts was reached. */
  if (scan_restrictions.max_alive_hosts_reached)
    {
      if ((main_kb = kb_direct_conn (prefs_get ("db_address"), scandb_id)))
        {
          char buf[256];
          int not_checked;
          /* Targts could be checked multiple times. */
          not_checked = number_of_targets_checked >= number_of_targets
                          ? 0
                          : number_of_targets - number_of_targets_checked;
          g_snprintf (buf, 256,
                      "ERRMSG||| ||| ||| |||Maximum allowed number of alive "
                      "hosts identified. There are still %d hosts whose alive "
                      "status will not be checked.",
                      not_checked);
          if (kb_item_push_str (main_kb, "internal/results", buf) != 0)
            g_warning ("%s: Failed to send message to ospd-openvas about "
                       "max_alive_hosts reached and for how many host the "
                       "alive status will not be checked.",
                       __func__);
          kb_lnk_reset (main_kb);
        }
      else
        g_warning (
          "%s: Boreas was unable to connect to the Redis db. Failed to send "
          "message to ospd-openvas that max_alive_hosts was reached and for "
          "how many host the alive status will not be checked.",
          __func__);
    }

  /* Send info about dead hosts to ospd-openvas. This is needed for the
   * calculation of the progress bar for gsa. */
  number_of_dead_hosts = send_dead_hosts_to_ospd_openvas (&hosts_data);

  gettimeofday (&end_time, NULL);

  g_message ("Alive scan %s finished in %ld seconds: %d alive hosts of %d.",
             scan_id, end_time.tv_sec - start_time.tv_sec,
             number_of_targets - number_of_dead_hosts, number_of_targets);
  g_free (scan_id);

  return 0;
}

/**
 * @brief Set all sockets needed for the chosen detection methods.
 *
 * @param alive_test  Methods of alive detection to use provided as bitflag.
 *
 * @return  0 on success, boreas_error_t on error.
 */
static boreas_error_t
set_all_needed_sockets (alive_test_t alive_test)
{
  boreas_error_t error = NO_ERROR;
  if (alive_test & ALIVE_TEST_ICMP)
    {
      if ((error = set_socket (ICMPV4, &scanner.icmpv4soc)) != 0)
        return error;
      if ((error = set_socket (ICMPV6, &scanner.icmpv6soc)) != 0)
        return error;
    }

  if ((alive_test & ALIVE_TEST_TCP_ACK_SERVICE)
      || (alive_test & ALIVE_TEST_TCP_SYN_SERVICE))
    {
      if ((error = set_socket (TCPV4, &scanner.tcpv4soc)) != 0)
        return error;
      if ((error = set_socket (TCPV6, &scanner.tcpv6soc)) != 0)
        return error;
      if ((error = set_socket (UDPV4, &scanner.udpv4soc)) != 0)
        return error;
      if ((error = set_socket (UDPV6, &scanner.udpv6soc)) != 0)
        return error;
    }

  if ((alive_test & ALIVE_TEST_ARP))
    {
      if ((error = set_socket (ARPV4, &scanner.arpv4soc)) != 0)
        return error;
      if ((error = set_socket (ARPV6, &scanner.arpv6soc)) != 0)
        return error;
    }

  return error;
}

/**
 * @brief Put all ports of a given port range into the ports array.
 *
 * @param range Pointer to a range_t.
 * @param ports_array Pointer to an GArray.
 */
static void
fill_ports_array (gpointer range, gpointer ports_array)
{
  gboolean range_exclude;
  int range_start;
  int range_end;
  int port;

  range_start = ((range_t *) range)->start;
  range_end = ((range_t *) range)->end;
  range_exclude = ((range_t *) range)->exclude;

  /* If range should be excluded do not use it. */
  if (range_exclude)
    return;

  /* Only single port in range. */
  if (range_end == 0 || (range_start == range_end))
    {
      g_array_append_val (ports_array, range_start);
      return;
    }
  else
    {
      for (port = range_start; port <= range_end; port++)
        g_array_append_val (ports_array, port);
    }
}

/**
 * @brief Initialise the alive detection scanner.
 *
 * Fill scanner struct with appropriate values.
 *
 * @param hosts gvm_hosts_t list of hosts to alive test.
 * @param alive_test methods to use for alive detection.
 *
 * @return 0 on success, boreas_error_t on error.
 */
static boreas_error_t
alive_detection_init (gvm_hosts_t *hosts, alive_test_t alive_test)
{
  g_debug ("%s: Initialise alive scanner. ", __func__);

  /* Used for ports array initialisation. */
  const gchar *port_list = NULL;
  GPtrArray *portranges_array = NULL;
  boreas_error_t error = NO_ERROR;

  /* Scanner */

  /* Sockets */
  if ((error = set_all_needed_sockets (alive_test)) != 0)
    return error;

  /* kb_t redis connection */
  int scandb_id = atoi (prefs_get ("ov_maindbid"));
  if ((scanner.main_kb = kb_direct_conn (prefs_get ("db_address"), scandb_id))
      == NULL)
    return -7;
  /* TODO: pcap handle */
  // scanner.pcap_handle = open_live (NULL, FILTER_STR); //
  scanner.pcap_handle = NULL; /* is set in ping function */

  /* Results data */
  /* hashtables */
  hosts_data.alivehosts =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  hosts_data.targethosts =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  hosts_data.alivehosts_not_to_be_sent_to_openvas =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* put all hosts we want to check in hashtable */
  gvm_host_t *host;
  for (host = gvm_hosts_next (hosts); host; host = gvm_hosts_next (hosts))
    {
      g_hash_table_insert (hosts_data.targethosts, gvm_host_value_str (host),
                           host);
    }
  /* reset hosts iter */
  hosts->current = 0;

  /* Init ports used for scanning. */
  scanner.ports = NULL;
  port_list = "80,137,587,3128,8081";
  if (validate_port_range (port_list))
    {
      g_warning ("%s: Invalid port range supplied for alive detection module. "
                 "Using global port range instead.",
                 __func__);
      /* This port list was already validated by openvas so we don't do it here
       * again. */
      port_list = prefs_get ("port_range");
    }
  scanner.ports = g_array_new (FALSE, TRUE, sizeof (int));
  if (port_list)
    portranges_array = port_range_ranges (port_list);
  else
    g_warning (
      "%s: Port list supplied by user is empty. Alive detection may not find "
      "any alive hosts when using TCP ACK/SYN scanning methods. ",
      __func__);
  /* Fill ports array with ports from the ranges. Duplicate ports are not
   * removed. */
  g_ptr_array_foreach (portranges_array, fill_ports_array, scanner.ports);
  array_free (portranges_array);

  /* Scan restrictions. max_scan_hosts and max_alive_hosts related. */
  const gchar *pref_str;
  scan_restrictions.max_alive_hosts_reached = FALSE;
  scan_restrictions.max_scan_hosts_reached = FALSE;
  scan_restrictions.alive_hosts_count = 0;
  scan_restrictions.max_scan_hosts = INT_MAX;
  scan_restrictions.max_alive_hosts = INT_MAX;
  if ((pref_str = prefs_get ("max_scan_hosts")) != NULL)
    scan_restrictions.max_scan_hosts = atoi (pref_str);
  if ((pref_str = prefs_get ("max_alive_hosts")) != NULL)
    scan_restrictions.max_alive_hosts = atoi (pref_str);
  if (scan_restrictions.max_alive_hosts < scan_restrictions.max_scan_hosts)
    scan_restrictions.max_alive_hosts = scan_restrictions.max_scan_hosts;

  g_debug ("%s: Initialisation of alive scanner finished.", __func__);

  return error;
}

/**
 * @brief Free all the data used by the alive detection scanner.
 *
 * @param[out] error Set to 0 on success, boreas_error_t on error.
 */
static void
alive_detection_free (void *error)
{
  boreas_error_t alive_test_err;
  boreas_error_t error_out;
  alive_test_t alive_test;

  error_out = NO_ERROR;
  alive_test_err = get_alive_test_methods (&alive_test);
  if (alive_test_err)
    {
      g_warning ("%s: %s. Could not get info about which sockets to close.",
                 __func__, str_boreas_error (alive_test_err));
      error_out = BOREAS_CLEANUP_ERROR;
    }
  else
    {
      if (alive_test & ALIVE_TEST_ICMP)
        {
          if ((close (scanner.icmpv4soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
          if ((close (scanner.icmpv6soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
        }

      if ((alive_test & ALIVE_TEST_TCP_ACK_SERVICE)
          || (alive_test & ALIVE_TEST_TCP_SYN_SERVICE))
        {
          if ((close (scanner.tcpv4soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
          if ((close (scanner.tcpv6soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
          if ((close (scanner.udpv4soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
          if ((close (scanner.udpv6soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
        }

      if ((alive_test & ALIVE_TEST_ARP))
        {
          if ((close (scanner.arpv4soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
          if ((close (scanner.arpv6soc)) != 0)
            {
              g_warning ("%s: Error in close(): %s", __func__,
                         strerror (errno));
              error_out = BOREAS_CLEANUP_ERROR;
            }
        }
    }

  /*pcap_close (scanner.pcap_handle); //pcap_handle is closed in ping/scan
   * function for now */
  if ((kb_lnk_reset (scanner.main_kb)) != 0)
    {
      g_warning ("%s: error in kb_lnk_reset()", __func__);
      error_out = BOREAS_CLEANUP_ERROR;
    }

  /* Ports array. */
  g_array_free (scanner.ports, TRUE);

  g_hash_table_destroy (hosts_data.alivehosts);
  /* targethosts: (ipstr, gvm_host_t *)
   * gvm_host_t are freed by caller of start_alive_detection()! */
  g_hash_table_destroy (hosts_data.targethosts);
  g_hash_table_destroy (hosts_data.alivehosts_not_to_be_sent_to_openvas);

  /* Set error. */
  *(boreas_error_t *) error = error_out;
}

/**
 * @brief Start the scan of all specified hosts in gvm_hosts_t
 * list. Finish signal is put on Queue if scan is finished or an error occurred.
 *
 * @param hosts_to_test gvm_hosts_t list of hosts to alive test. which is to be
 * freed by caller.
 */
void *
start_alive_detection (void *hosts_to_test)
{
  boreas_error_t init_err;
  boreas_error_t alive_test_err;
  boreas_error_t fin_err;
  boreas_error_t free_err;
  gvm_hosts_t *hosts;
  alive_test_t alive_test;

  if ((alive_test_err = get_alive_test_methods (&alive_test)) != 0)
    {
      g_warning ("%s: %s. Exit Boreas.", __func__,
                 str_boreas_error (alive_test_err));
      put_finish_signal_on_queue (&fin_err);
      if (fin_err)
        g_warning ("%s: Could not put finish signal on Queue. Openvas needs to "
                   "be stopped manually. ",
                   __func__);
      pthread_exit (0);
    }

  hosts = (gvm_hosts_t *) hosts_to_test;
  if ((init_err = alive_detection_init (hosts, alive_test)) != 0)
    {
      g_warning (
        "%s. Boreas could not initialise alive detection. %s. Exit Boreas.",
        __func__, str_boreas_error (init_err));
      put_finish_signal_on_queue (&fin_err);
      if (fin_err)
        g_warning ("%s: Could not put finish signal on Queue. Openvas needs to "
                   "be stopped manually. ",
                   __func__);
      pthread_exit (0);
    }

  /* If alive detection thread returns, is canceled or killed unexpectedly all
   * used resources are freed and sockets, connections closed.*/
  pthread_cleanup_push (alive_detection_free, &free_err);
  /* If alive detection thread returns, is canceled or killed unexpectedly a
   * finish signal is put on the queue for openvas to process.*/
  pthread_cleanup_push (put_finish_signal_on_queue, &fin_err);
  /* Start the scan. */
  if (scan (alive_test) < 0)
    g_warning ("%s: error in scan()", __func__);
  /* Put finish signal on queue. */
  pthread_cleanup_pop (1);
  /* Free memory, close sockets and connections. */
  pthread_cleanup_pop (1);
  if (free_err)
    g_warning ("%s: %s. Exit Boreas thread none the less.", __func__,
               str_boreas_error (free_err));

  pthread_exit (0);
}

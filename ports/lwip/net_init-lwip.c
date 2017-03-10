// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/lwip/src/include/lwip/dhcp.h"
#include "third_party/lwip/src/include/lwip/inet.h"
#include "third_party/lwip/src/include/lwip/netif.h"
#include "third_party/lwip/src/include/lwip/netifapi.h"
#include "third_party/lwip/src/include/lwip/tcpip.h"
#include "third_party/lwip/src/include/lwip/stats.h"
#if USE_LWIPERF
#include "third_party/lwip/src/include/lwip/apps/lwiperf.h"
#endif

#include "apps/netstack/trace.h"
#include "apps/netstack/ports/lwip/ethernetif.h"

// initialize a lwip network interface

static struct netif netif;

static int ip4_addr_printed = 0;
static int ip6_addr_printed_bits = 0;

// the callback to signal sem_tcpip_done
static void lwip_tcpip_init_done(void *arg) {
  sys_sem_t *sem;
  sem = (sys_sem_t *)arg;
  sys_sem_signal(sem);
}

static int print_ip4_addr(void) {
  if (!ip_addr_isany(&netif.ip_addr)) {
    char ip_addr[INET_ADDRSTRLEN];
    char netmask[INET_ADDRSTRLEN];
    char gw[INET_ADDRSTRLEN];
    ipaddr_ntoa_r(&netif.ip_addr, ip_addr, sizeof(ip_addr));
    ipaddr_ntoa_r(&netif.netmask, netmask, sizeof(netmask));
    ipaddr_ntoa_r(&netif.gw, gw, sizeof(gw));
    info("ip4_addr: %s netmask: %s gw: %s\n", ip_addr, netmask, gw);
    return 1;
  }
  return 0;
}

static void print_ip6_addr(int *printed_bits, int idx) {
  if (netif.ip6_addr_state[idx] & IP6_ADDR_PREFERRED) {
    char ip6_addr[INET6_ADDRSTRLEN];
    info("ip6_addr[%d]: %s\n", idx,
         ip6addr_ntoa_r(netif_ip6_addr(&netif, idx), ip6_addr,
                        sizeof(ip6_addr)));
    *printed_bits = (1 << idx);
  }
}

// the callback called every time the netif status changes
static void lwip_netif_status_callback(struct netif *netif) {
  if (!ip4_addr_printed) ip4_addr_printed = print_ip4_addr();

  for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
    if (!(ip6_addr_printed_bits & (1 << i)))
      print_ip6_addr(&ip6_addr_printed_bits, i);
  }
}

#if USE_LWIPERF
// the callback called for lwiperf reports
static void lwip_iperf_report(void *arg, enum lwiperf_report_type report_type,
                              const ip_addr_t *local_addr, u16_t local_port,
                              const ip_addr_t *remote_addr, u16_t remote_port,
                              u32_t bytes_transferred, u32_t ms_duration,
                              u32_t bandwidth_kbitpsec) {
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(local_addr);
  LWIP_UNUSED_ARG(local_port);

  printf(
      "iperf report [%d]: %s:%u, transferred: %u (bytes), duration: %u (ms), "
      "bandwidth %u (kb/s)\n",
      report_type, ipaddr_ntoa(remote_addr), remote_port, bytes_transferred,
      ms_duration, bandwidth_kbitpsec);
}
#endif

static int lwip_netif_init(void) {
  sys_sem_t sem_tcpip_done;

  // initialize tcpip in lwip

  // create a semaphore to wait for tcpip initialization
  if (sys_sem_new(&sem_tcpip_done, 0) != ERR_OK) {
    LWIP_ASSERT("failed to create semaphore", 0);
  }
  // pass lwip_tcpip_init_done as the callback
  tcpip_init(lwip_tcpip_init_done, &sem_tcpip_done);
  // block until the semaphore is signaled by lwip_tcp_init_done
  sys_sem_wait(&sem_tcpip_done);
  // free the semaphore
  sys_sem_free(&sem_tcpip_done);

  // create an network interface and run dhcp

  // add an interface without passing ip_address/netmask/gateway
  netif_add(&netif, NULL, NULL, NULL, NULL, ethernetif_init, tcpip_input);
  // make it the default interface
  netif_set_default(&netif);
  // set a callback to detect when IP address is assgined by dhcp
  netif_set_status_callback(&netif, lwip_netif_status_callback);

  // turn on the interface
  netif_set_up(&netif);

  // create ipv6 linklocal address
  netif_create_ip6_linklocal_address(&netif, 1);

  // start dhcp
  dhcp_start(&netif);

#if USE_LWIPERF
  // start iperf server
  lwiperf_start_tcp_server_default(lwip_iperf_report, NULL);
#endif

  return 0;
}

int net_init(void) {
#if defined(NETSTACK_DEBUG)
  lwip_debug_flags = LWIP_DBG_ON;
#endif
  return lwip_netif_init();
}

static void lwip_netif_set_status_callback(struct netif* netif) {
  netif_set_status_callback(netif, lwip_netif_status_callback);
}

static void lwip_netif_create_ip6_linklocal_address(struct netif* netif) {
  netif_create_ip6_linklocal_address(netif, 1);
}

void net_reinit(void) {
  netifapi_dhcp_stop(&netif);
  netifapi_netif_set_down(&netif);
  netifapi_netif_remove(&netif);
  mem_free(netif.state);

  ip4_addr_printed = 0;
  ip6_addr_printed_bits = 0;
  netifapi_netif_add(&netif, NULL, NULL, NULL, NULL, ethernetif_init,
                     tcpip_input);
  netifapi_netif_common(&netif, lwip_netif_set_status_callback, NULL);
  netifapi_netif_set_default(&netif);
  netifapi_netif_set_up(&netif);
  netifapi_netif_common(&netif, lwip_netif_create_ip6_linklocal_address, NULL);
  netifapi_dhcp_start(&netif);
}

void net_debug(void) {
  stats_display();
}

/*
 * Copyright (c) 2015-2016, Ari Suutari <ari@stonepile.fi>.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote
 *     products derived from this software without specific prior written
 *     permission. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT,  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <picoos.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>

#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#include "lwip/dns.h"

#include "lwip/stats.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include <lwip/dhcp.h>
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "apps/dhcps/dhcps.h"

#include "wwd_management.h"
#include "wwd_wifi.h"
#include "wwd_network.h"

#include "eshell.h"
#include "eshell-commands.h"
#include "emw-sensor.h"

static POSSEMA_t ready;

void initConfig()
{
  uosConfigInit();
  uosConfigLoad("/flash/test.cfg");
}

/*
 * Write config entries to file in spiffs.
 */
static int wr(EshContext* ctx)
{

  eshCheckNamedArgsUsed(ctx);

  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  uosConfigSave("/flash/test.cfg");
  return 0;
}

/*
 * Clear all config entries by removing saved file.
 */
static int clear(EshContext* ctx)
{
  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  uosFileUnlink("/flash/test.cfg");
  return 0;
}

void ifStatusCallback(struct netif *netif);

/*
 * This is called by lwip when interface status changes.
 */
void ifStatusCallback(struct netif *netif)
{
  if (netif_is_up(netif)) {

    if (!ip4_addr_isany_val(*netif_ip4_addr(netif)))
      posSemaSignal(ready);
  }
}

struct netif defaultIf;

void staInit()
{
  ready = posSemaCreate(0);
}

bool staUp()
{
  wiced_ssid_t ssid;


  /*
   * Get AP network name and password and attempt to join.
   */

  const char* ap = uosConfigGet("ap");
  const char* pass = uosConfigGet("pass");

  if (ap[0] == '\0' || pass[0] == '\0') {
  
    printf("No STA configured.\n");
    return false;
  }

  wifiLed(true);
  if (wwd_management_wifi_on(WICED_COUNTRY_FINLAND) != WWD_SUCCESS) {

    wifiLed(false);
    printf("Cannot turn wifi on.\n");
    return false;
  }

  strcpy((char*)ssid.value, ap);
  ssid.length = strlen(ap);

  if (wwd_wifi_join(&ssid, WICED_SECURITY_WPA2_MIXED_PSK, (uint8_t*)pass, strlen(pass), NULL) != WWD_SUCCESS) {

    wifiLed(false);
    printf("Cannot join AP.\n");
    return false;
  }

  wifiLed(false);
  printf("Join OK.\n");

  // Ensure that semaphore is not set yet
  while (posSemaWait(ready, 0) == 0);

  netifapi_netif_set_up(&defaultIf);
  netif_set_status_callback(&defaultIf, ifStatusCallback);
  netifapi_dhcp_start(&defaultIf);

#if LWIP_IPV6
  netif_create_ip6_linklocal_address(&defaultIf, 1);
  netif_ip6_addr_set_state(&defaultIf, 0, IP6_ADDR_TENTATIVE);
  defaultIf.ip6_autoconfig_enabled = 1;
#endif

  if (posSemaWait(ready, MS(10000)) != 0) {

    printf("No DHCP lease.\n");
    staDown();
    return false;
  }

  return true;
}

void staDown()
{
  netifapi_dhcp_release(&defaultIf);
  netifapi_dhcp_stop(&defaultIf);
  netifapi_netif_set_down(&defaultIf);
  wwd_wifi_leave(WWD_STA_INTERFACE);
  wwd_management_wifi_off();
}

/*
 * Connect to existing access point.
 */
static int sta(EshContext* ctx)
{
  char* reset = eshNamedArg(ctx, "reset", false);
  char* ap;
  char* pass;

  eshCheckNamedArgsUsed(ctx);

  if (reset == NULL) {

    ap = eshNextArg(ctx, true);
    pass = eshNextArg(ctx, true);
  }

  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  if (reset) {

    uosConfigSet("ap", "");
    uosConfigSet("pass", "");
    return 0;
  }

  if (ap == NULL || pass == NULL) {

    eshPrintf(ctx, "Usage: sta --reset | ap pass\n");
    return -1;
  }

  uosConfigSet("ap", ap);
  uosConfigSet("pass", pass);

  return 0;
}

const EshCommand staCommand = {
  .flags = 0,
  .name = "sta",
  .help = "--reset | ap pass\ndisassociate/associate with given wifi access point.",
  .handler = sta
}; 

const EshCommand wrCommand = {
  .flags = 0,
  .name = "wr",
  .help = "save settings to flash",
  .handler = wr
}; 

const EshCommand clearCommand = {
  .flags = 0,
  .name = "clear",
  .help = "clear saved settings from flash",
  .handler = clear
}; 

extern const EshCommand mqttCommand;
extern const EshCommand apCommand;

const EshCommand *eshCommandList[] = {
  &mqttCommand,
  &staCommand,
  &wrCommand,
  &clearCommand,
#if defined(POS_DEBUGHELP) && POSCFG_ARGCHECK > 1
  &eshTsCommand,
#endif
  &eshOnewireCommand,
  &eshPingCommand,
  &eshIfconfigCommand,
  &eshHelpCommand,
  &eshExitCommand,
  NULL
};


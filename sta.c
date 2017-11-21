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
#include <sys/time.h>

#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/apps/sntp.h"

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
#include <wiced-driver.h>

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
      nosSemaSignal(ready);
  }
}

struct netif defaultIf;
static POSFLAG_t sntpFlag;
static time_t lastNtp = 0;

void staInit()
{
  ready = nosSemaCreate(0, 0, "tcpok");
  sntpFlag = nosFlagCreate("sntp");
}

void setSystemTime(time_t t)
{
  struct timeval tv;
  char buf[30];

  if (lastNtp == 0)
    sys_random_init(t);

  gettimeofday(&tv, NULL);

  // Adjust wall clock time if difference is more than
  // one second or more than 24 hours have passed since last
  // adjustment.
  if (abs(tv.tv_sec - t) > 1 || abs(t - lastNtp) > 3600 * 24) {

    ctime_r(&t, buf);
    logPrintf("Setting clock to %s", buf);

    lastNtp = t;
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    sensorCycleReset(&tv);
  }

  nosFlagSet(sntpFlag, 0);
}

void waitSystemTime()
{
  nosFlagWait(sntpFlag, MS(2000));
}

static void sntpStartStop(void* arg)
{
  bool flag = (bool)arg;

  if (flag)
    sntp_init();
  else
    sntp_stop();
}

bool staUp()
{
  wiced_ssid_t ssid;
  wwd_result_t result;

  /*
   * Get AP network name and password and attempt to join.
   */

  const char* ap = uosConfigGet("ap");
  const char* pass = uosConfigGet("pass");
  const char* fixedNtp = uosConfigGet("ntp");

  if (ap[0] == '\0' || pass[0] == '\0') {
  
    printf("No STA configured.\n");
    return false;
  }

  wifiLed(true);
  result = wwd_management_wifi_on(WICED_COUNTRY_FINLAND);
  if (result != WWD_SUCCESS) {

    wifiLed(false);
    printf("Cannot turn wifi on, error %d.\n", result);
    wwd_management_wifi_off();
    return false;
  }

  wifiLed(false);
  strcpy((char*)ssid.value, ap);
  ssid.length = strlen(ap);

  result = wwd_wifi_join(&ssid, WICED_SECURITY_WPA2_MIXED_PSK, (uint8_t*)pass, strlen(pass), NULL, WWD_STA_INTERFACE);
  if (result != WWD_SUCCESS) {

    wwd_management_wifi_off();
    wifiLed(false);
    printf("Cannot join AP, error %d.\n", result);
    return false;
  }

  // Ensure that semaphore is not set yet
  while (nosSemaWait(ready, 0) == 0);

  netifapi_netif_set_up(&defaultIf);
  netif_set_status_callback(&defaultIf, ifStatusCallback);
  sntp_servermode_dhcp(1);
  netifapi_dhcp_start(&defaultIf);

#if LWIP_IPV6
  netif_create_ip6_linklocal_address(&defaultIf, 1);
  netif_ip6_addr_set_state(&defaultIf, 0, IP6_ADDR_TENTATIVE);
  defaultIf.ip6_autoconfig_enabled = 1;
#endif

  if (nosSemaWait(ready, MS(10000)) != 0) {

    printf("No DHCP lease.\n");
    staDown();
    return false;
  }

  // Ensure that flag is not set yet
  nosFlagWait(sntpFlag, 0);

  if (fixedNtp != NULL && strlen(fixedNtp) > 0) {

    sntp_setservername(0, fixedNtp);
    tcpip_callback_with_block(sntpStartStop, (void*)true, true);
  }
  else if (!ip_addr_isany(sntp_getserver(0))) {

    tcpip_callback_with_block(sntpStartStop, (void*)true, true);
  }
  else
    nosFlagSet(sntpFlag, 0);

  return true;
}

void staDown()
{
  tcpip_callback_with_block(sntpStartStop, (void*)false, true);
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
  char* reset  = eshNamedArg(ctx, "reset", false);
  char* online = eshNamedArg(ctx, "online", false);
  char* ntp = eshNamedArg(ctx, "ntp", false);
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
    uosConfigSet("online", "");
    uosConfigSet("ntp", "");
    return 0;
  }

  uosConfigSet("online", online != NULL ? "yes" : "");
  if (ap == NULL || pass == NULL) {

    eshPrintf(ctx, "Usage: sta --reset | ap pass\n");
    return -1;
  }

  uosConfigSet("ntp", ntp != NULL ? ntp : "");
  uosConfigSet("ap", ap);
  uosConfigSet("pass", pass);

  return 0;
}

bool staIsAlwaysOnline(void)
{
  const char* online = uosConfigGet("online");
  if (online == NULL || strlen(online) == 0)
    return false;

  return true;
}

#if BUNDLE_FIRMWARE

/*
 * Copy Wifi firmware to spiffs. This allows compilation of
 * version without firmware blob in code flash, shrinking it's
 * size about 200 kb.
 */
static int copyfw(EshContext* ctx)
{
  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  int from;
  int to;

  from = open("/firmware/" WDCFG_FIRMWARE, O_RDONLY);
  if (from == -1) {

    eshPrintf(ctx, "Cannot open /firmware/" WDCFG_FIRMWARE ".\n");
    return -1;
  }

  to = open("/flash/" WDCFG_FIRMWARE, O_WRONLY | O_CREAT | O_TRUNC);
  if (to == -1) {

    eshPrintf(ctx, "Cannot open /flash/" WDCFG_FIRMWARE ".\n");
    close(from);
    return -1;
  }

  int len;
  int total = 0;
  char buf[128];

  while ((len = read(from, buf, sizeof(buf))) > 0) {

    total += len;
    if (write(to, buf, len) != len) {

      eshPrintf(ctx, "Cannot write to /flash/" WDCFG_FIRMWARE ".\n");
      break;
    }
  }

  eshPrintf(ctx, "Wrote %d bytes of firmware.\n", total);
  close(from);
  close(to);

  return 0;
}

const EshCommand copyfwCommand = {
  .flags = 0,
  .name = "copyfw",
  .help = "Copy wifi firmware from /firmware to /flash",
  .handler = copyfw
};

#endif

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

#if USE_MQTT
extern const EshCommand mqttCommand;
#endif

#if USE_VERA
extern const EshCommand veraCommand;
#endif

extern const EshCommand apCommand;
extern const EshCommand resetCommand;

const EshCommand *eshCommandList[] = {
#if BUNDLE_FIRMWARE
  &copyfwCommand,
#endif
#if USE_MQTT
  &mqttCommand,
#endif
#if USE_VERA
  &veraCommand,
#endif
  &staCommand,
  &wrCommand,
  &clearCommand,
#if defined(POS_DEBUGHELP) || NOSCFG_FEATURE_REGISTRY
  &eshTsCommand,
  &eshEsCommand,
#endif
  &eshOnewireCommand,
  &eshPingCommand,
  &eshIfconfigCommand,
  &eshHelpCommand,
  &eshExitCommand,
  &resetCommand,
  NULL
};


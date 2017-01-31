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
#include <picoos-u.h>
#include <picoos-ow.h>
#include <picoos-lwip.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <eshell.h>
#include <stdarg.h>

#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#include "lwip/netifapi.h"

#include "lwip/stats.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include <lwip/dhcp.h>
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"

#include "lwip/priv/tcp_priv.h"

#include "wwd_management.h"
#include "wwd_wifi.h"
#include <wiced-driver.h>
#include "wwd_buffer_interface.h"

#include "emw-sensor.h"
#include "devtree.h"

#include "stm32f4xx_iwdg.h"

extern const UosRomFile romFiles[];

static int systemReset(EshContext* ctx)
{
  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  SCB->AIRCR = (0x5FA << SCB_AIRCR_VECTKEY_Pos)  // unlock key
             | (1 << SCB_AIRCR_SYSRESETREQ_Pos); // reset request
  return 0;
}

const EshCommand resetCommand = {
  .flags = 0,
  .name = "reset",
  .help = "reset system",
  .handler = systemReset
};


POSSEMA_t sendSema;

void tcpServerThread(void*);

wiced_mac_t   myMac             = { {  0, 0, 0, 0, 0, 0 } };

extern struct netif defaultIf;

/*
 * This is called by lwip when basic initialization has been completed.
 */
static void tcpipInitDone(void *arg)
{
  wwd_result_t result;
  sys_sem_t *sem;
  sem = (sys_sem_t *)arg;

  printf("Loading Wifi firmware and initializing.\n");

/*
 * Bring WIFI up.
 */
  wwd_buffer_init(NULL);
  while ((result = wwd_management_wifi_on(WICED_COUNTRY_FINLAND)) != WWD_SUCCESS) {

    printf("WWD init error %d, retrying after some time.\n", result);
    posPowerEnableSleep();
    posTaskSleep(MS(10 * 60 * 1000));
    posPowerDisableSleep();
  }


  wwd_wifi_get_mac_address(&myMac, WWD_STA_INTERFACE);

  printf("Mac addr is %02x:%02x:%02x:%02x:%02x:%02x\n", myMac.octet[0],
                myMac.octet[1], myMac.octet[2], myMac.octet[3],
                myMac.octet[4], myMac.octet[5]);

  sys_random_init(SysTick->VAL);

  ip4_addr_t ipaddr, netmask, gw;

  ip4_addr_set_zero(&gw);
  ip4_addr_set_zero(&ipaddr);
  ip4_addr_set_zero(&netmask);

  netif_add(&defaultIf,
            &ipaddr,
            &netmask,
            &gw,
            (void*)WWD_STA_INTERFACE,
            ethernetif_init,
            tcpip_input);

  netif_set_default(&defaultIf);
/*
 * Signal main thread that we are done.
 */
  sys_sem_signal(sem);
}

static void tcpipDrain()
{
  // Wait until there are no active connections.
  // (ignore those in time_wait state).
  while (tcp_active_pcbs) {

    posTaskSleep(MS(250));
  }
}

static void tcpipSuspend(void* arg)
{
  posSemaGet(sendSema);

  sys_restart_timeouts();
  sys_sem_signal((sys_sem_t*)arg);
}

void logPrintf(const char* fmt, ...)
{
  va_list ap;
  time_t t;
  struct tm* tm;

  time(&t);
  tm = gmtime(&t);

  printf("%02d:%02d:%02d ", tm->tm_hour, tm->tm_min, tm->tm_sec);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

bool timeOk()
{
  time_t t;

  time(&t);
  return (t > T_2017_01_01);
}

static void mainTask(void* arg)
{
  sys_sem_t sem;

  uosInit();
  uosBootDiag();
  printf("WICED SDK %s\n", WICED_SDK_VERSION);
  devTreeInit();

  flashPowerup(); // ensure that flash chip is not in deep powerdown
  fsInit();
  initConfig();
  netInit();

/* 
 * Provide a filesystem which contains Wifi firmware to Wiced driver.
 */
  uosMountRom("/firmware", romFiles);

  if(sys_sem_new(&sem, 0) != ERR_OK) {
    LWIP_ASSERT("Failed to create semaphore", 0);
  }

/*
 * Bring LwIP & Wifi up.
 */
  tcpip_init(tcpipInitDone, &sem);
  sys_sem_wait(&sem);
  printf("TCP/IP initialized.\n");

  /*
   * Enable sleep. It is initially enabled in pico]OS, but Wiced
   * disables it during initialization.
   */
  posPowerEnableSleep();
  buttonInit();

  bool ap = false;
  int i;
  posTaskSleep(MS(200)); // wait for setup button capacitor to charge.
  printf("Press button to activate AP.\n");
  for (i = 0; i < 10; i++) {

    wifiLed(i % 2 == 0);
    if (buttonRead()) {

      ap = true;
      break;
    }

    posTaskSleep(MS(500));
  }

  wifiLed(false);
  if (ap) {

    printf("Keep button pressed to activate AP.\n");
    posTaskSleep(MS(1000));
    if (!buttonRead())
      ap = false;
  }

  staInit();

  if (ap) {

    wifiLed(true);
    setup();
  }

  // Join AP so we can get time from SNTP.
  if (staUp()) {

    waitSystemTime();
    staDown();
  }
  else
    wwd_management_wifi_off();

  potatoInit();
  sensorInit();

  printf("Startup complete.\n");

  for (i = 0; i < 3; i++) {

    userLed(true);
    posTaskSleep(MS(100));
    userLed(false);
    posTaskSleep(MS(100));
  }

  float sleepTicks = 0;
  float busyTicks = 0;
  UVAR_t start;
  VAR_t delta;

  sendSema = posSemaCreate(0);
  while (1) {

#if BUNDLE_FIRMWARE
    flashPowerdown();  // don't need spiffs after this, save 15 uA
#endif
    start = jiffies;
    tcpip_callback_with_block(tcpipSuspend, &sem, true);
    sys_sem_wait(&sem);
#if BUNDLE_FIRMWARE
    flashPowerup();  // don't need spiffs after this, save 15 uA
#endif

    delta = jiffies - start;
    if (delta > 0)
      sleepTicks += delta;

    start = jiffies;

    sensorLock();

    if (staUp()) {

      if (!timeOk()) {

        // SSL/TLS needs time before it can work.
        // So wait for SNTP.
        waitSystemTime();
        potatoSend();
      }
      else {

        // Time is already ok, so we can send data
        // and wait for clock update in parallel tasks.
        potatoSend();
        waitSystemTime();
      }

      userLed(true);
      tcpipDrain();
      staDown();
      userLed(false);
    }

    sensorUnlock();

    delta = jiffies - start;
    if (delta > 0)
      busyTicks += delta;

    logPrintf("Cycle time %d Busy %f idle %f ratio %5.2f %%\n", delta, busyTicks, sleepTicks, busyTicks / (sleepTicks + busyTicks) * 100);
    uosResourceDiag();
  }
}


static void unusedPins()
{
  GPIO_InitTypeDef GPIO_InitStructure;

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 /* Wifi wake up MCU ? */
                              | GPIO_Pin_1
                              | GPIO_Pin_10
                              | GPIO_Pin_11
                              | GPIO_Pin_12;

  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOA);
  GPIO_ReadInputData(GPIOA);

  GPIO_Init(GPIOA, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0
                              | GPIO_Pin_1
                              | GPIO_Pin_6
                              | GPIO_Pin_8
                              | GPIO_Pin_9
                              | GPIO_Pin_12
                              | GPIO_Pin_13;

  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOB);
  GPIO_ReadInputData(GPIOB);

  GPIO_Init(GPIOB, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13;

  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOC);
  GPIO_ReadInputData(GPIOC);

  GPIO_Init(GPIOC, &GPIO_InitStructure);
}

int main(int argc, char **argv)
{
  GPIO_InitTypeDef GPIO_InitStructure;

  unusedPins();
  ledInit();

#if PORTCFG_CON_USART == 2

  // Enable USART clock.

  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

  // Configure usart2 pins.

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOA);
  GPIO_ReadInputData(GPIOA);

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

#endif
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
// see errata 2.1.6
  PWR_GetFlagStatus(PWR_FLAG_WU);
  PWR_GetFlagStatus(PWR_FLAG_WU);

  PWR_FlashPowerDownCmd(ENABLE);

  PWR->CR  |= PWR_CR_LPDS;
  PWR->CR  &= ~PWR_CR_PDDS;
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

  nosInit(mainTask, NULL, 1, 5120, 512);
  return 0;
}

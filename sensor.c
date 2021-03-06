/*
 * Copyright (c) 2016, Ari Suutari <ari@stonepile.fi>.
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
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include "emw-sensor.h"

#include <picoos-ow.h>
#include <temp10.h>
#include <eshell.h>

void owAddr2Str(char* buf, const uint8_t* addr)
{
  int i;

  *buf = '\0';
  sprintf(buf, "%02x.", (int)addr[0]);
  for (i = 1; i < 7; i++)
    sprintf(buf + strlen(buf), "%02x", (int)addr[i]);
}

void owStr2Addr(uint8_t* addr, const char* buf)
{
  char work[3];
  int  i;

  strlcpy(work, buf, 3);
  addr[0] = strtol(work, NULL, 16);

  for (i = 1; i < 7; i++) {

    strlcpy(work, buf + 1 + 2 * i, 3);
    addr[i] = strtol(work, NULL, 16);
  }
}

static POSMUTEX_t sensorMutex;
static POSSEMA_t timerSema;
static POSTIMER_t timer;

Sensor     sensorList[MAX_SENSORS];
int        sensorCount;
time_t     sensorTime;

static Sensor* getSensor(uint8_t* addr)
{
  int i;
  Sensor* sensor;

  sensor = sensorList + 1;
  // slot 0 is battery voltage
  for (i = 1; i < sensorCount; i++, sensor++) {

    if (!memcmp(addr, sensor->addr, sizeof(sensor->addr)))
      return sensor;
  }

  if (sensorCount == MAX_SENSORS) {
    
    printf("Too many sensors.\n");
    return NULL;
  }

  sensorLock();
  sensor = sensorList + sensorCount;
  memcpy(sensor->addr, addr, sizeof(sensor->addr));
  sensorCount++;
  sensor->historyCount = 0;

  char serialStr[20];
  char key[36];
  const char* val;
  owAddr2Str(serialStr, addr);

#if USE_MQTT
  strcpy(key, "ol.");
  strcat(key, serialStr);
  val = uosConfigGet(key);
  if (val != NULL)
    sensor->location = val;
#endif

#if USE_VERA
  strcpy(key, "ov.");
  strcat(key, serialStr);
  val = uosConfigGet(key);
  if (val != NULL)
    sensor->veraId = strtol(val, NULL, 10);
#endif

  sensorUnlock();
 
  return sensor;
}

void sensorCycleReset(const struct timeval* now)
{
  time_t next;
  int    delay;

  if (timer == NULL) // Task not initialized yet.
    return;

  next = (now->tv_sec / MEAS_CYCLE_SECS) * MEAS_CYCLE_SECS + MEAS_CYCLE_SECS;
  delay = next - now->tv_sec;
  
  delay = delay * 1000;
  posTimerSet(timer, timerSema, MS(delay), MS(MEAS_CYCLE_SECS * 1000));
  posTimerStart(timer);
}

void sensorClearHistory()
{
  Sensor* sensor;
  int ns;

  sensor = sensorList;
  for (ns = 0; ns < sensorCount; ns++, sensor++)
    sensor->historyCount = 0;
}

float battery;
static int adcFailures = 0;

bool isValidBattery(double v)
{
  return v > 0.1;
}

static void readBattery()
{
  ADC_SoftwareStartConv(ADC1);

  int timeout = 500;
  while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC ) == RESET) {

    posTaskSleep(MS(1));
    timeout -= 1;
    if (timeout <= 0) {

      ++adcFailures;
      logPrintf("ADC timeout.\n");
      battery = -1;
      return;
    }
  }


  int result;

  result = ADC_GetConversionValue(ADC1);

  battery = result * 3.3 / 256;
  if (isValidBattery(battery))
    logPrintf ("Battery         = %f V\n", battery);

  ADC_Cmd(ADC1, DISABLE);
}

void updateLastBatteryReading()
{
  Sensor* sensor;

  readBattery();
  sensor = sensorList;

  if (sensor->historyCount == MAX_HISTORY)
    sensor->historyCount--;

  sensor->temperature[sensor->historyCount] = battery;
  sensor->historyCount++;
}

static void sensorThread(void* arg)
{
  int	  result;
  uint8_t serialNum[8];
  char buf[30];
  float   value;
  Sensor* sensor;
  bool sendNeeded = true;
  time_t  now;
  struct timeval tv;
  bool    online = staIsAlwaysOnline();

  timerSema = nosSemaCreate(0, 0, "sensor*");
  timer     = posTimerCreate();

  gettimeofday(&tv, NULL);
  sensorCycleReset(&tv);

  while (true) {

    int historyMax = 0;

    nosSemaGet(timerSema);

    time(&now);
    ctime_r(&now, buf);

    now = (now / MEAS_CYCLE_SECS) * MEAS_CYCLE_SECS;
    if ((now % SEND_CYCLE_SECS) == 0)
      sendNeeded = true;

    sensorTime = now;

    if (!sendNeeded && !online)
      ADC_Cmd(ADC1, ENABLE); // Enable ADC now so it has time to settle.

    if (!owAcquire(0, NULL)) {

      printf("OneWire: owAcquire failed\n");
      continue;
    }

    result = owFirst(0, TRUE, FALSE);
    while (result) {

      owSerialNum(0, serialNum, TRUE);
      sensor = getSensor(serialNum);
      if (sensor == NULL)
        break;

      if (!ReadTemperature(0, serialNum, &value) || value >= 85.0)
        value = -273;

      owAddr2Str(buf, serialNum);

#if USE_MQTT && USE_VERA
      logPrintf("%s = %f [%s] #%d\n", buf, value, sensor->location ? sensor->location : "", sensor->veraId);
#elif USE_MQTT
      logPrintf("%s = %f [%s]\n", buf, value, sensor->location ? sensor->location : "");
#elif USE_VERA
      logPrintf("%s = %f #%d\n", buf, value, sensor->veraId);
#else
      logPrintf("%s = %f\n", buf, value);
#endif

      sensorLock();
      if (sensor->historyCount >= MAX_HISTORY) {

#if MAX_HISTORY > 1
        memmove(sensor->temperature, sensor->temperature + 1, (MAX_HISTORY - 1) * sizeof(double));
#endif
        sensor->historyCount--;
        printf("Sensor history was full.\n");
      }

      sensor->temperature[sensor->historyCount++] = value;
      if (sensor->historyCount > historyMax)
        historyMax = sensor->historyCount;

      if (sensor->historyCount == MAX_HISTORY)
        sendNeeded = true;

      sensorUnlock();

      result = owNext(0, TRUE, FALSE);
    }

    owRelease(0);

    // Don't read battery if sending, it will
    // be read after wifi is on to get reading with load.
    if (!sendNeeded && !online) {

      readBattery();

      sensor = sensorList;
      sensorLock();
      if (sensor->historyCount >= MAX_HISTORY) {

#if MAX_HISTORY > 1
        memmove(sensor->temperature, sensor->temperature + 1, (MAX_HISTORY - 1) * sizeof(double));
#endif
        sensor->historyCount--;
        printf("Sensor history was full.\n");
      }

      sensor->temperature[sensor->historyCount++] = battery;
      if (sensor->historyCount > historyMax)
        historyMax = sensor->historyCount;

      if (sensor->historyCount == MAX_HISTORY)
        sendNeeded = true;

      sensorUnlock();
    }

    if (online || sendNeeded) {

      if (adcFailures > 0)
        logPrintf("ADC failure count %d\n", adcFailures);

      int randomSleep;

      sendNeeded = false;
      
      // Random delay before sending. If we have multiple
      // sensor units running this will spread the load 
      // in the receiving end.
      randomSleep = sys_random() % MS(2000);
      posTaskSleep(randomSleep);

      nosSemaSignal(sendSema);
    }
  }
}

void sensorInit()
{
  GPIO_InitTypeDef GPIO_InitStructure;
  ADC_InitTypeDef adcInit;
  ADC_CommonInitTypeDef adcCommonInit;

  sensorCount = 1; // we always have battery

// ADC init

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AN;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_OD;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOA);
  GPIO_ReadInputData(GPIOA);

  GPIO_Init(GPIOA, &GPIO_InitStructure);

  RCC_APB2PeriphClockCmd( RCC_APB2Periph_ADC1 , ENABLE);

  ADC_CommonStructInit(&adcCommonInit);
  adcCommonInit.ADC_Mode             = ADC_Mode_Independent;
  adcCommonInit.ADC_DMAAccessMode    = ADC_DMAAccessMode_Disabled;
  adcCommonInit.ADC_Prescaler        = ADC_Prescaler_Div2;
  adcCommonInit.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
  ADC_CommonInit(&adcCommonInit);

  ADC_StructInit(&adcInit);

  adcInit.ADC_Resolution         = ADC_Resolution_8b;
  adcInit.ADC_ScanConvMode       = DISABLE;
  adcInit.ADC_ContinuousConvMode = DISABLE;
  adcInit.ADC_ExternalTrigConv   = ADC_ExternalTrigConvEdge_None;
  adcInit.ADC_DataAlign          = ADC_DataAlign_Right;
  adcInit.ADC_NbrOfConversion    = 1;
  ADC_Init(ADC1, &adcInit);

  ADC_RegularChannelConfig(ADC1, ADC_Channel_5, 1, ADC_SampleTime_3Cycles);

// 1-wire bus
  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
// see errata 2.1.6
  GPIO_ReadInputData(GPIOB);
  GPIO_ReadInputData(GPIOB);

  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
  GPIO_Init(GPIOB, &GPIO_InitStructure);

  GPIO_ResetBits(GPIOB, GPIO_Pin_10);
  owInit();

  if (!owAcquire(0, NULL)) {

    printf("OneWire: owAcquire failed\n");
    return;
  }

  owRelease(0);

  sensorMutex = nosMutexCreate(0, "sensor");

  nosTaskCreate(sensorThread, NULL, 6, 1024, "OneWire");
  logPrintf("OneWire OK.\n");
}

void sensorLock()
{
  nosMutexLock(sensorMutex);
}

void sensorUnlock()
{
  nosMutexUnlock(sensorMutex);
}

static int onewire(EshContext * ctx)
{
  char* location = eshNamedArg(ctx, "location", false);
  char* vera = eshNamedArg(ctx, "vera", false);
  char* address = eshNamedArg(ctx, "address", false);

  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  char key[36];

  if (location || address || vera) {

    if (address == NULL || (location == NULL && vera == NULL)) {

      eshPrintf(ctx, "--address and --location or --vera required.\n");
      return -1;
    }

    if (location != NULL) {

      snprintf(key, sizeof(key), "ol.%s", address);
      uosConfigSet(key, location);
    }

    if (vera != NULL) {

      snprintf(key, sizeof(key), "ov.%s", address);
      uosConfigSet(key, vera);
    }

    return 0;
  }

  int   rslt;
  uint8_t serialNum[8];
  char serialStr[36];
  float value;
  const char* loc;

  if (!owAcquire(0, NULL)) {

    eshPrintf(ctx, "owAcquire failed.\n");
    return -1;
  }

  rslt = owFirst(0, TRUE, FALSE);

  while (rslt) {

    owSerialNum(0, serialNum, TRUE);

    owAddr2Str(serialStr, serialNum);
    eshPrintf(ctx, "%s", serialStr);
    ReadTemperature(0, serialNum, &value);
    eshPrintf(ctx, "=%1.1f", value);

    strcpy(key, "ol.");
    strcat(key, serialStr);
    loc = uosConfigGet(key);
    if (loc != NULL)
      eshPrintf(ctx, " [%s]", loc);

    strcpy(key, "ov.");
    strcat(key, serialStr);
    loc = uosConfigGet(key);
    if (loc != NULL)
      eshPrintf(ctx, " #%s", loc);

    eshPrintf(ctx, "\n");
    rslt = owNext(0, TRUE, FALSE);
  }

  owRelease(0);
  return 0;
}

const EshCommand onewireCommand = {
  .flags = 0,
  .name = "onewire",
  .help = "list onewire bus, map location (--location=,--address=)",
  .handler = onewire
};


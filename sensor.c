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

void sensorAddressStr(char* buf, uint8_t* addr)
{
  int i;

  *buf = '\0';
  sprintf(buf, "%02X.", (int)addr[0]);
  for (i = 1; i < 7; i++) {

    sprintf(buf + strlen(buf), "%02X", (int)addr[i]);
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
  sensorUnlock();
 
  return sensor;
}

void sensorCycleReset(const struct timeval* now)
{
  time_t next;
  int    delay;

  next = (now->tv_sec / MEAS_CYCLE_SECS) * MEAS_CYCLE_SECS + MEAS_CYCLE_SECS;
  delay = next - now->tv_sec;
  
  delay = delay * 1000;
  posTimerSet(timer, timerSema, MS(delay), MS(MEAS_CYCLE_SECS * 1000));
  posTimerStart(timer);
}

float battery;

static void readBattery()
{
  ADC_SoftwareStartConv(ADC1);

  while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC ) == RESET) {
  }


  int result;

  result = ADC_GetConversionValue(ADC1);

  battery = result * 3.3 / 256;
  printf ("Battery=%f V\n", battery);
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

  timerSema = posSemaCreate(0);
  timer     = posTimerCreate();

  posTimerSet(timer, timerSema, MS(10000), MS(MEAS_CYCLE_SECS * 1000));
  posTimerStart(timer);

  while (true) {

    int historyMax = 0;

    posSemaGet(timerSema);

    time(&now);
    now = (now / MEAS_CYCLE_SECS) * MEAS_CYCLE_SECS;
    if ((now % SEND_CYCLE_SECS) == 0)
      sendNeeded = true;

    sensorTime = now;

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

    if (!owAcquire(0, NULL)) {

      printf("OneWire: owAcquire failed\n");
      continue;
    }

    result = owFirst(0, TRUE, FALSE);
    while (result) {

      owSerialNum(0, serialNum, TRUE);
      sensor = getSensor(serialNum);
      if (sensor == NULL)
        continue;

      if (!ReadTemperature(0, serialNum, &value) || value >= 85.0)
        value = -273;

      sensorAddressStr(buf, serialNum);
      printf("%s=%f\n", buf, value);

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

    printf("Sensor count %d, history max %d, sendflag=%d\n", sensorCount, historyMax, (int)sendNeeded);

    if (sendNeeded) {

      int randomSleep;

      sendNeeded = false;
      
      // Random delay before sending. If we have multiple
      // sensor units running this will spread the load 
      // in the receiving end.
      randomSleep = sys_random() % MS(2000);
      posTaskSleep(randomSleep);

      posSemaSignal(sendSema);
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
  ADC_Cmd(ADC1, ENABLE);

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

  sensorMutex = posMutexCreate();

  nosTaskCreate(sensorThread, NULL, 7, 1024, "OneWire");
  printf("OneWire OK.\n");
}

void sensorLock()
{
  posMutexLock(sensorMutex);
}

void sensorUnlock()
{
  posMutexUnlock(sensorMutex);
}


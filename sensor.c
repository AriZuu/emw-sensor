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

static Sensor* getSensor(uint8_t* addr)
{
  int i;
  Sensor* sensor;

  sensor = sensorList;
  for (i = 0; i < sensorCount; i++, sensor++) {

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

static void sensorThread(void* arg)
{
  int	  result;
  uint8_t serialNum[8];
  char buf[30];
  float   value;
  Sensor* sensor;
  bool sendNeeded = true;

  timerSema = posSemaCreate(0);
  timer     = posTimerCreate();

  posTimerSet(timer, timerSema, MS(10000), MEAS_CYCLE);
  posTimerStart(timer);

  while (true) {

    int historyMax = 0;

    posSemaGet(timerSema);

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

      if (!ReadTemperature(0, serialNum, &value))
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

      sendNeeded = false;
      posSemaSignal(sendSema);
    }
  }
}

void sensorInit()
{
  GPIO_InitTypeDef GPIO_InitStructure;

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


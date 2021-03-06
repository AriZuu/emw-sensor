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

#include <picoos-lwip.h>
#include "lwip/netif.h"

#define MAX_SENSORS 3

#define MEAS_CYCLE_SECS (10 * 60)
#define SEND_CYCLE_SECS (60 * 60)

#define MAX_HISTORY (1 + (SEND_CYCLE_SECS / MEAS_CYCLE_SECS))

typedef struct {

  uint8_t addr[7];
  int     historyCount;
  float   temperature[MAX_HISTORY];

#if USE_MQTT
  const char* location;
#endif

#if USE_VERA
  int veraId;
#endif

} Sensor;

#define T_2017_01_01 1483228800

bool timeOk(void);
void initConfig(void);
void potatoInit(void);
bool potatoSend(void);
void buttonInit(void);
bool buttonRead(void);
bool staUp(void);
void staInit(void);
void staDown(void);
bool staIsAlwaysOnline(void);
void setup(void);
void ledInit(void);
void wifiLed(bool on);
void userLed(bool on);
void flashPowerdown(void);
void flashPowerup(void);
void watchdogInit(void);
void watchdogDiag(void);
void logPrintf(const char* fmt, ...);
int  getUptime(void);
int  getLastCycleTime(void);

void waitSystemTime(void);

void sensorInit(void);
void sensorLock(void);
void sensorUnlock(void);
void sensorClearHistory(void);
void updateLastBatteryReading(void);
bool isValidBattery(double v);
void owAddr2Str(char* str, const uint8_t* addr);
void owStr2Addr(uint8_t* addr, const char* str);
void sensorCycleReset(const struct timeval* tv);

bool veraSend(void);

extern Sensor sensorList[];
extern float battery;
extern int    sensorCount;
extern time_t sensorTime;
extern POSSEMA_t sendSema;


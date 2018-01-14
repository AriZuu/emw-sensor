/*
 * Copyright (c) 2017, Ari Suutari <ari@stonepile.fi>.
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
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <eshell.h>

#include "wwd_wifi.h"

#include "potato-bus.h"
#include "potato-json.h"
#include "emw-sensor.h"
#include "picoos-mbedtls.h"

#if USE_VERA

static PbClient client;

/*
 * Configure mqtt client.
 */
static int vera(EshContext* ctx)
{
  char* server   = eshNamedArg(ctx, "server", false);

  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  if (server != NULL)
    uosConfigSet("vera.server", server);

  if (server == NULL) {

    const char* parm;

    parm = uosConfigGet("vera.server");
    eshPrintf(ctx, "Server: %s\n", parm ? parm : "<not set>");
  }

  return 0;
}

const EshCommand veraCommand = {
  .flags = 0,
  .name = "vera",
  .help = "--server servername --id sensor-id configure vera client",
  .handler = vera
}; 

static char url[256];

bool veraSend()
{
  const char* server = uosConfigGet("vera.server");
  int   status;
  Sensor* sensor;
  int ns;
  time_t t;

  if (server == NULL)
    return true;

  time(&t);

  sensor = sensorList + 1;
  for (ns = 1; ns < sensorCount; ns++, sensor++) {

    if (sensor->historyCount == 0 || sensor->veraId == 0)
      continue;


    sprintf(url, "%s/data_request?id=variableset&DeviceNum=%d&serviceId=urn:upnp-org:serviceId:TemperatureSensor1&Variable=CurrentTemperature&Value=%.1f",
                 server, sensor->veraId, sensor->temperature[sensor->historyCount - 1]);

    status = pbGet(&client, url, NULL);
    if (status < 0) {

      printf("vera: http get failed, error %d\n", status);
      return false;
    }

    *client.packet.end = '\0';
    if (strcmp((const char*)client.packet.start, "OK"))
      logPrintf("Vera response: %s\n", client.packet.start);

    sprintf(url, "%s/data_request?id=variableset&DeviceNum=%d&serviceId=urn:upnp-org:serviceId:HADevice1&Variable=LastUpdate&Value=%ld",
                 server, sensor->veraId, t);

    status = pbGet(&client, url, NULL);
    if (status < 0) {

      printf("vera: http get failed, error %d\n", status);
      return false;
    }

    *client.packet.end = '\0';
    if (strcmp((const char*)client.packet.start, "OK"))
      logPrintf("Vera LastUpdate response: %s\n", client.packet.start);
  }

  return true;
}

#endif

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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <eshell.h>

#include "wwd_wifi.h"

#include "potato-bus.h"
#include "potato-json.h"
#include "emw-sensor.h"

extern wiced_mac_t   myMac;

static PbClient client;

/*
 * Configure mqtt client.
 */
static int mqtt(EshContext* ctx)
{
  char* server   = eshNamedArg(ctx, "server", false);
  char* location = eshNamedArg(ctx, "location", false);
  char* topic    = eshNamedArg(ctx, "topic", false);

  eshCheckNamedArgsUsed(ctx);
  eshCheckArgsUsed(ctx);
  if (eshArgError(ctx) != EshOK)
    return -1;

  if (server != NULL)
    uosConfigSet("mqtt.server", server);

  if (location != NULL)
    uosConfigSet("mqtt.location", location);

  if (topic != NULL)
    uosConfigSet("mqtt.topic", topic);

  if (topic == NULL && location == NULL && server == NULL) {

    const char* parm;

    parm = uosConfigGet("mqtt.server");
    eshPrintf(ctx, "Server: %s\n", parm ? parm : "<not set>");

    parm = uosConfigGet("mqtt.topic");
    eshPrintf(ctx, "Topic prefix: %s\n", parm ? parm : "<not set>");

    parm = uosConfigGet("mqtt.location");
    eshPrintf(ctx, "Location: %s\n", parm ? parm : "<not set>");
  }
  return 0;
}

const EshCommand mqttCommand = {
  .flags = 0,
  .name = "mqtt",
  .help = "--server servername --topic topic-prefix --location location-id configure mqtt client",
  .handler = mqtt
}; 

static char clientId[20];
static PbConnect connectArgs;

#if POTATO_TLS

static int getCertData(const char* fn, const unsigned char** buf)
{
  UosFile* f;
  UosFileInfo info;

  *buf = NULL;

  f = uosFileOpen(fn, 0, 0);
  if (f == NULL)
    return -1;

  if (uosFileFStat(f, &info) != -1)
    *buf = (const unsigned char*)uosFileMap(f, 0);

  uosFileClose(f);

  if (*buf == NULL)
    return -1;

  return info.size;
}

static mbedtls_ssl_config       sslConf;
static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context ctrDrbg;
static mbedtls_x509_crt    caCert;
static mbedtls_x509_crt    cliCert;
static mbedtls_pk_context  privKey;


#endif


void potatoInit()
{
    sprintf(clientId, "EMW%02x%02x%02x%02x%02x%02x", myMac.octet[0],
                      myMac.octet[1], myMac.octet[2], myMac.octet[3],
                      myMac.octet[4], myMac.octet[5]);

    connectArgs.clientId = clientId;
    connectArgs.keepAlive= 60;

#if POTATO_TLS

  int st;
  const uint8_t* cert;
  int certLen;

  mbedtls_ssl_config_init(&sslConf);

  mbedtls_x509_crt_init(&caCert);
  mbedtls_x509_crt_init(&cliCert);
  mbedtls_pk_init(&privKey);

  mbedtls_ssl_conf_authmode(&sslConf, MBEDTLS_SSL_VERIFY_NONE);
  certLen = getCertData("/firmware/rootCA.der", &cert);
  if (certLen > 0) {

    st = mbedtls_x509_crt_parse(&caCert, cert, certLen);
    if (st == 0) {

      mbedtls_ssl_conf_ca_chain(&sslConf, &caCert, NULL);
      mbedtls_ssl_conf_authmode(&sslConf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    else
      printf("rootCA.der error 0x%x\n", st);
  }

  mbedtls_ctr_drbg_init(&ctrDrbg);
  mbedtls_entropy_init(&entropy);

  st = mbedtls_ctr_drbg_seed(&ctrDrbg,
                             mbedtls_entropy_func,
                             &entropy,
                             (const unsigned char*)"potato", 6);

  if (st != 0)
    printf("entropy error 0x%x\n", st);

  st = mbedtls_ssl_config_defaults(&sslConf,
                                   MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (st != 0)
    printf("config defaults error 0x%x\n", st);


  certLen = getCertData("/firmware/cert.der", &cert);
  if (certLen > 0) {

    st = mbedtls_x509_crt_parse(&cliCert, cert, certLen);
    if (st == 0) {

      certLen = getCertData("/firmware/privkey.der", &cert);
      if (certLen > 0) {

        st = mbedtls_pk_parse_key(&privKey, cert, certLen, NULL, 0);
        if (st == 0) {

          st = mbedtls_ssl_conf_own_cert(&sslConf, &cliCert, &privKey);
          if (st != 0)
            printf("set own cert error 0x%x\n", st);
        }
        else
          printf("private key error 0x%x\n", st);
      }
      else
        printf("no private key\n");
    }
    else
      printf("cert.der error 0x%x\n", st);
  }

  mbedtls_ssl_conf_rng(&sslConf, mbedtls_ctr_drbg_random, &ctrDrbg);
  printf("SSL config done.\n");
#endif

}

static char jsonBuf[1024];
static JsonContext jsonCtx;

static bool buildJson(const char* location)
{
  JsonNode* root;
  char      timeStamp[40];
  char name[40];
  struct tm* t;

  root = jsonGenerate(&jsonCtx, jsonBuf, sizeof(jsonBuf));

  JsonNode* top;

  top = jsonStartObject(root);
  jsonWriteKey(top, "timeStep");
  jsonWriteInteger(top, MEAS_CYCLE_SECS);

  t = gmtime(&sensorTime);

  if (t->tm_year > 100) {

    strftime(timeStamp, sizeof(timeStamp), "%FT%TZ", t);
    jsonWriteKey(top, "timeStamp");
    jsonWriteString(top, timeStamp);
  }

  jsonWriteKey(top, "locations");

  {
    JsonNode* locations;

    locations = jsonStartObject(top);
    jsonWriteKey(locations, location);

    {
      JsonNode* s;
      Sensor* sensor;
      int ns;

      s = jsonStartObject(locations);
      sensor = sensorList + 1;
      for (ns = 1; ns < sensorCount; ns++, sensor++) {

        strcpy(name, "temperature");
        if (ns > 1)
          sprintf(name + strlen(name), "%d", ns - 1);

        jsonWriteKey(s, name);

        {
          JsonNode* values;
          int i;

          values = jsonStartArray(s);

          for (i = 0; i < sensor->historyCount; i++)
            jsonWriteDouble(values, sensor->temperature[i]);

          sensor->historyCount = 0;
        }
      }
    }

    strcpy(name, location);
    strcat(name, "Node");
    jsonWriteKey(locations, name);

    {
      JsonNode* s;
      Sensor* sensor;

      s = jsonStartObject(locations);
      sensor = sensorList;

      jsonWriteKey(s, "battery");

      {
        JsonNode* values;
        int i;

        values = jsonStartArray(s);

        for (i = 0; i < sensor->historyCount; i++)
          jsonWriteDouble(values, sensor->temperature[i]);

        sensor->historyCount = 0;
      }
    }
  }

  jsonGenerateFlush(top);

  if (jsonFailed(&jsonCtx))
    return false;

  return true;
}

bool potatoSend()
{
  const char* server = uosConfigGet("mqtt.server");
  const char* topic  = uosConfigGet("mqtt.topic");
  const char* location = (char*)uosConfigGet("mqtt.location");
  char addr[80];
  
  if (location == NULL)
    location = "somewhere";

  if (server == NULL) {

    printf("No MQTT server configured.\n");
    return false;
  }

#if POTATO_TLS

  connectArgs.sslConf = &sslConf;

#endif

  if (pbConnectURL(&client, server, &connectArgs) < 0) {

    printf("potato: connect failed.\n");
    return false;
  }

  PbPublish pub = {};

  if (buildJson(location)) {

    pub.message = (uint8_t*)jsonBuf;
    pub.len = strlen(jsonBuf);

    if (topic == NULL)
      strcpy(addr, "test");
    else {

      strcpy(addr, topic);
    }

    pub.topic = addr;
    pbPublish(&client, &pub);
  }

  pbDisconnect(&client);
  return true;
}


EMW-Sensor
=========

This is firmware for [emw-board][1]. It can be used to perform measurements
with 1-wire sensors (temperature for example) and transmit results via wifi connection
to MQTT server.

After loading the firmware to board, it is easiest to set up it
by pressing the "setup" button during startup, which activates built-in
access point (you'll see EMW3165nnnnnnn when scanning for APs). 
Connect to access point and use telnet to log in to 192.168.0.1.
In _esh_ prompt setup the system:

```
esh> sta ap-name ap-password
esh> mqtt --server=mqtt://mqtt-server-fqdn --topic=sensordata --node=kitchenNode
esh> onewire
28.4f61ab040000=22.1
esh> onewire --address=28.4f61ab040000 --location=kitchen
esh> wr
esh> exit
```

Or, to publish data to Amazon IOT MQTT:

```
esh> mqtt --server=mqtts://nnnnnn.iot.eu-west-1.amazonaws.com --topic=sensordata --location=kitchen
```

Certificates provided by Amazon should be placed into cert directory (in DER format) to be picked
up by build.

It is also possible to transmit measurement to Vera home automation controller:

```
esh> vera --server=http://your-vera-box:3480
esh> onewire
28.4f61ab040000=22.1
esh> onewire --address=28.4f61ab040000 --vera=NN
esh> wr
```

NN is device id, which can be found from Vera device advanced configuration.
 
After done with settings, reset the board:

```
esh> reset
```

GPIO connections:

| Module Pin | GPIO                                    |
|------------|-----------------------------------------|
| DS1820     | PB10                                    |
| SETUP      | PB2                                     |
| BATTV      | PA5/ADC1_5                              |
| SWDIO      |                                         |
| SWCLK      |                                         |
| RESET      |                                         |

To build this following modules are needed:

* pico]OS
* picoos-micro
* picoos-micro-spiffs
* picoos-lwip
* picoos-mbedtls
* picoos-ow
* wiced-driver
* cmsis-ports
* potato-bus
* eshell

[1]: https://github.com/AriZuu/emw-board

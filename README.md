EMW-Sensor
=========

This is firmware for [emw-board][1]. It can be used to perform measurements
with 1-wire sensors (temperature for example) and transmit results via wifi connection
to MQTT server.

After loading the firmware to board, it is easiest to set up it
by pressing the "setup" button during startup, which activates built-in
access point (you'll see EMW3165nnnnnnn when scanning for APs). 
Connect to access point and use telnet to log in to 192.168.1.1.
In _esh_ prompt setup the system:

```
esh> sta ap-name ap-password
esh> mqtt --server mqtt-server-fqdn --topic sensordata --location kitchen
esh> wr
esh> exit
```

After that, reset the board.

GPIO connections:

| Module Pin | GPIO                                    |
|------------|-----------------------------------------|
| DS1820     | PB10                                    |
| SETUP      | PB2                                     |
| SWDIO      |                                         |
| SWCLK      |                                         |
| RESET      |                                         |

To build this following modules are needed:

* Pico]OS 
* picoos-micro
* picoos-micro-spiffs
* picoos-lwip
* picoos-ow
* wiced-driver
* cmsis-ports
* potato-bus
* eshell

[1]: https://github.com/AriZuu/emw-board

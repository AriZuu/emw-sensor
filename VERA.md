Using emw-sensor with Vera Edge
===============================

Emw-sensor has basic support for Vera home automation controllers.
Controller has a simple HTTP interface, which is used to send
temperature sensor value.

First, a temperature sensor device in Vera must be created:

- Go to Apps -> Develop apps -> Create Device in Vera UI
- Type device description into 'description' field
- Enter D_TemperatureSensor1.xml into 'Upnp Device Filename'
- Press 'Create device'.
- If device does not appear, go to 'Edit Startup Lua', don't type anything 
  into code, just press 'GO'. This reloads Vera engine.

After that, dig out device ID number from Vera:

- Go to 'Devices' and select your new device
- Click 'Advanced'
- Make a note of the 'id' number (25 in this example)

After that, configure emw-sensor:

```
esh> sta --online --ntp=pool.ntp.org wifinetworkname wifinetworkpass
esh> vera --server=http://your.vera.ip.address --id=25
esh> wr
esh> reset
```

In 10 minutes, temperature value should appear in Vera UI.

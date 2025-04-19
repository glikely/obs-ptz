# VISCA Implementation details

This document describes How the VISCA protocol is implemented in the plugin.
There are several different variants of the VISCA protocol and supported by many different camera vendors.
Not all cameras behave according to the original SONY spec.

## VISCA Basic Protocol

All variants of the VISCA protocol use the same basic protocol of the
controller sends a command or inquiry datagram, and the device responds with an
acknowledgements, a reply, or an error.

ccc := Controller Address; typically 0
ddd := Device Address; range 1-7
sss := slot id

### Framing

All datagrams in the VISCA protocol start with an address byte, are 3-12 bytes
long, and finish with the value 0xff.
A raw value of 0xFF cannot appear anywhere in the datagram because it is used as the datagram delimiter.
Data values are encoded such that 0xFF never appears as a data byte in the datagram.

The first byte of a datagram is the address field that encodes the sender and receiver of the datagram.

### Commands

Commands tell the device to do something or to set a property.
The controller sends the command datagram to the device,
and the device replies with an Ack datagram followed by a Complete datagram.
If the command could not be executed then the device responds with an error datagram.

Command: 0b1ccc0ddd 0b00000001 [command; 1-3 bytes] [data; 0-12 bytes] 0xff
Ack:     0b1ddd0ccc 0b01000sss 0xff
Complete:0b1ddd0ccc 0b01010sss 0xff
Error:   0b1ddd0ccc 0b01100000 0xff

### Inquiries

Inquiries ask the device to return data about the current state of the device.
The controller sends an inquiry datagram to the device,
and the device replies with a Complete datagram.
If the inquiry was invalid, or the device otherwise cannot complete the request,
then an error datagram is returned instead.

Command: 0b1ccc0ddd 0b00001001 [property; 1-3 bytes] 0xff
Complete:0b1ddd0ccc 0b01010000 [data; 0-12 bytes] 0xff
Error:   0b1ddd0ccc 0b01100000 0xff

### VISCA over Serial

This is the original version of the VISCA protocol.
VISCA over serial will work over an RS232 point-to-point connection,
An RS232 daisy chain with up to 7 devices,
or an RS422 serial bus that also supports up to 7 devices.
Serial connections can be wired up as either bidirectional (the device sends response packets)
or single ended (controller can send commands, but replys will not be received).
When in a single ended configuration the controller is able to control the camera,
but it will not be able to get any status messages from the device or enquire about its state.

### VISCA over UDP (Sony VISCA-over-IP Protocol)

The Sony implementation of VISCA over IP encapusates VISCA datagrams in UDP datagrams with some additional
encoding to track order of datagram delivery.

### VISCA over TCP (PTZOptics and others)

The VISCA over TCP protocol encapsulates the serial protocol in a TCP socket.
Since TCP is a reliable transport that delivers data in-order,
this version of the protocol doesn't need to track sequence numbers.
Instead, it treats the TCP socket as a UART port and uses exactly the same framing to decode datagrams.

The controller establishes a TCP connection with the device in the normal way.
Once the TCP socket is established is uses the UART protocol init sequence to initialize the device.

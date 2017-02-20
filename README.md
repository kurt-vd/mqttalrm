# mqttalrm

This projects implements a traditional alarm clock using MQTT.
It consists of several cooperating tools that communicate
via MQTT.

Via retained messages, the MQTT broker provides the storage
for the alarms.

# example use
## binaries

Run these commands (or start with your init system).

	$ mqttalrm -v &
	$ mqttoff -v -r off -s timeoff alarms/+/state alarms/+/statetimeoff &

## MQTT topic layout

* alarms/NAME/alarm	**HH:MM**, alarm time
* alarms/NAME/repeat	**mtwtfss** for active days, **-** when disabled
* alarms/NAME/enable	0 or 1
* alarms/NAME/skip	0 or 1, when 1, the alarm is skipped **once**
* alarms/NAME/state	**off**, **snoozed** or **on**.
* alarms/NAME/statetimeoff	ex **1h**. The alarms will turn off after 1h.

# tools
## mqttalrm

* This program listens to all attributes,
* changes the state attribute to **on**
* It will also reset **skip** when the alarm is actually skipped.
* turns off state when the alarm is disabled or changed/rescheduled

## mqttoff

* listens to state & statetimeoff
* turns off state after the time specified by statetimeoff

## alarm.html

A web gui using mosquitto websockets.
This will allow you to add/remove alarms, control all sorts of things

The html page added 1 extra **sleeptimer** within the alarms.
mqttoff will pick it up, just as all alarm listeners.
mqttalrm will never raise it due to its +/alarm abscense.

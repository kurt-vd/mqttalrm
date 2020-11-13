# mqttalrm

This projects implements a traditional alarm clock using MQTT.
It may need several cooperating tools, see
http://github.com/kurt-vd/mqttautomation.

Via retained messages, the MQTT broker provides the storage
for the alarms.

# example use
## binaries

Run these commands (or start with your init system).

	$ mqttalrm -v 'alarms/+' 'alarms/+/+' &
	$ mqttimer -v alarms/+ alarms/+/timer &

## MQTT topic layout

* alarms/NAME/alarm	**HH:MM**, alarm time
* alarms/NAME/repeat	**mtwtfss** for active days, **-** when disabled
* alarms/NAME/cmd	non-retained: **skip**, **enable**, **disable**, **force**
* alarms/NAME/snoozetime ex **9m**, enable snoozing, and use this delay.
* alarms/NAME		**0**, **1**
* alarms/NAME/state	**wait**, **on**, **snoozed**, **skip**, **disable**
* alarms/NAME/timer	ex **1h**. The alarms will turn off after 1h.
* alarms/NAME2		**0** or **1**
* alarms/NAME2/timer	*timer value*, NAME2 acts as a sleep timer

# tools
## mqttalrm

* This program listens to all attributes,
* changes the state to **1**
* It will also reset **skip** when the alarm is actually skipped.
* turns off state when the alarm is disabled or changed/rescheduled

## mqttimer

* listens to state & statetimer
* turns off state after the time specified by statetimer

mqttimer is obsoleted by improved mqttlogic tool.

## alarm.html

A web gui using mosquitto websockets.
This will allow you to add/remove alarms, control all sorts of things

The html page added 1 extra **sleeptimer** within the alarms.
mqtttimer will pick it up, just as all alarm listeners.
mqttalrm will never raise it due to its +/alarm abscense.

Multiple alarms & timeswitches can be defined.

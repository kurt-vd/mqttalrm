#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "common.h"

#define NAME "mqttalrm"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* program options */
static const char help_msg[] =
	NAME ": an MQTT alarm clock daemon\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for (default alarms/+/+)\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* alarm states */
static const char *const alrm_states[] = {
#define ALRM_OFF	0
	[0] = "0",
#define ALRM_ON		1
	[1] = "1",
#define ALRM_SNOOZED	2
	[2] = "snoozed",
};

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	int hhmm;
	int wdays; /* bitmask */
	int valid; /* definition has been seen */
	int enabled;
	int skip;

	int state;
	int snooze_time;
};

struct item *items;

static void reschedule_alrm(struct item *it);

long next_alarm(const struct item *it)
{
	struct tm tm;
	time_t tnow, tnext;
	int j;

	time(&tnow);
	tm = *localtime(&tnow);
	tm.tm_hour = it->hhmm / 100;
	tm.tm_min = it->hhmm % 100;
	tm.tm_sec = 0;
	tnext = mktime_dstsafe(&tm);
	if (tnext <= tnow) {
		tm.tm_mday += 1;
		tnext = mktime_dstsafe(&tm);
	}
	for (j = 0; j < 7; ++j) {
		if (it->wdays & (1 << tm.tm_wday))
			break;
		tm.tm_mday += 1;
		tnext = mktime_dstsafe(&tm);
	}
	return tnext - tnow;
}

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_INFO,
		MOSQ_LOG_DEBUG, LOG_DEBUG,
		0,
	};
	int j;

	for (j = 0; logpri_map[j]; j += 2) {
		if (level & logpri_map[j]) {
			mylog(logpri_map[j+1], "[mosquitto] %s", str);
			return;
		}
	}
}

static struct item *get_item(const char *topic, const char *suffix, int create)
{
	struct item *it;
	int len;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len <= 0)
		return NULL;
	for (it = items; it; it = it->next) {
		if ((it->topiclen == len) && !strncmp(it->topic ?: "", topic, len))
			return it;
	}

	if (!create)
		return NULL;

	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	/* assign dup'd topic to item, no need to dup twice, no need to free */
	it->topic = strndup(topic, len);
	it->topiclen = strlen(it->topic);

	it->enabled = 1;
	it->wdays = 0x7f; /* all days */
	/* insert in linked list */
	it->next = items;
	if (it->next) {
		it->prev = it->next->prev;
		it->next->prev = it;
	} else
		it->prev = (struct item *)(((char *)&items) - offsetof(struct item, next));
	it->prev->next = it;
	return it;
}

static void drop_item(struct item *it)
{
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	free(it->topic);
	free(it);
}

static void pub_alrm_state(struct item *it)
{
	const char *state = alrm_states[it->state];

	mosquitto_publish(mosq, NULL, it->topic,
			strlen(state), state, mqtt_qos, 1);
}

/* timeout handlers */
static void on_alrm(void *dat)
{
	struct item *it = dat;

	if (it->skip && (it->state == ALRM_OFF)) {
		mosquitto_publish(mosq, NULL, csprintf("%s/skip", it->topic),
				0, NULL, mqtt_qos, 1);
		it->skip = 0;
		reschedule_alrm(it);
		return;
	}
	it->state = ALRM_ON;
	pub_alrm_state(it);
}

static void snooze_alrm(struct item *it)
{
	if (!it->valid || !it->wdays) {
		/* effectively dismiss the alarm */
		reschedule_alrm(it);
		return;
	}
	libt_add_timeout(it->snooze_time, on_alrm, it);
	mylog(LOG_INFO, "snoozed %s for %us", it->topic, it->snooze_time);
	if (it->state != ALRM_SNOOZED) {
		it->state = ALRM_SNOOZED;
		pub_alrm_state(it);
	}
}

/* dismiss & reschedule do the same thing */
#define dismiss_alrm reschedule_alrm
static void reschedule_alrm(struct item *it)
{
	libt_remove_timeout(on_alrm, it);
	if (it->state != ALRM_OFF) {
		it->state = ALRM_OFF;
		pub_alrm_state(it);
	}
	if (!it->valid)
		return;
	else if (it->enabled)
		mylog(LOG_INFO, "disabled '%s'", it->topic);
	else if (!it->wdays)
		mylog(LOG_INFO, "no days selected for '%s'", it->topic);
	else {
		long delay;

		delay = next_alarm(it);
		libt_add_timeout(delay, on_alrm, it);
		mylog(LOG_INFO, "scheduled '%s' in %lus", it->topic, delay);
	}
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int ret, val;
	char *tok, *endp;
	struct item *it;

	tok = strrchr(msg->topic ?: "", '/') ?: "";

	if (!strcmp(tok, "/dismiss")) {
		if ((tok > msg->topic) && (*(tok-1) == '/')) {
			/* global ctrl, like 'pre/fix//dismiss' */
			for (it = items; it; it = it->next) {
				if (it->state != ALRM_OFF)
					dismiss_alrm(it);
			}
		} else {
			/* 1 alarm, like 'pre/fix/name/dismiss' */
			it = get_item(msg->topic, tok, 0);
			if (it)
				dismiss_alrm(it);
		}
	} else if (!strcmp(tok, "/snooze")) {
		if ((tok > msg->topic) && (*(tok-1) == '/')) {
			/* global ctrl, like 'pre/fix//dismiss' */
			for (it = items; it; it = it->next) {
				if (it->state != ALRM_OFF)
					snooze_alrm(it);
			}
		} else {
			/* 1 alarm, like 'pre/fix/name/dismiss' */
			it = get_item(msg->topic, tok, 0);
			if (it && (it->state != ALRM_OFF))
				snooze_alrm(it);
		}
	} else if (!strcmp(tok, "/alarm")) {
		it = get_item(msg->topic, tok, !!msg->payloadlen);
		if (!msg->payloadlen) {
			if (!it)
				return;
			/* flush potential MQTT leftovers */
			mosquitto_publish(mosq, NULL, csprintf("%s/repeat", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/skip", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/enable", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/snoozetime", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, it->topic, 0, NULL, mqtt_qos, 1);
			drop_item(it);
			return;
		}
		ret = strtohhmm(msg->payload ?: "");
		if (ret >= 0) {
			it->hhmm = ret;
			/* mark as valid */
			it->valid = 1;
			reschedule_alrm(it);
		}
	} else if (!strcmp(tok, "/repeat")) {
		it = get_item(msg->topic, tok, 1);
		it->wdays = strtowdays(msg->payload ?: "");
		reschedule_alrm(it);
	} else if (!strcmp(tok, "/skip")) {
		it = get_item(msg->topic, tok, 1);
		it->skip = strtoul(msg->payload ?: "0", 0, 0);
	} else if (!strcmp(tok, "/enable")) {
		it = get_item(msg->topic, tok, 1);
		val = strtoul(msg->payload ?: "1", 0, 0);

		if (val != it->enabled) {
			it->enabled = val;
			reschedule_alrm(it);
		}
	} else if (!strcmp(tok, "/snoozetime")) {
		it = get_item(msg->topic, tok, 1);
		val = strtoul(msg->payload ?: "0", &endp, 0);
		switch (*endp) {
		case 'w':
			val *= 7;
		case 'd':
			val *= 24;
		case 'h':
			val *= 60;
		case 'm':
			val *= 60;
			break;
		}
		it->snooze_time = val;
	} else { // if (!strcmp(tok, "/state")) {
		it = get_item(msg->topic, "", 0);
		if (!it)
			return;

		for (val = 0; val < sizeof(alrm_states)/sizeof(alrm_states[0]); ++val) {
			if (!strcmp(msg->payload, alrm_states[val]))
				break;
		}
		if (val >= sizeof(alrm_states)/sizeof(alrm_states[0]))
			/* bad state supplied */
			return;
		if (it->state == val)
			/* nothing to do */
			return;
		mylog(LOG_INFO, "new state %s = '%s'", msg->topic, alrm_states[val]);
		it->state = val;
		switch (val) {
		case ALRM_OFF:
			dismiss_alrm(it);
			break;
		case ALRM_ON:
			libt_remove_timeout(on_alrm, it);
			break;
		case ALRM_SNOOZED:
			if (!it->snooze_time) {
				mylog(LOG_INFO, "%s snoozed, with snooze-time 0!", msg->topic);
				dismiss_alrm(it);
				break;
			}
			libt_add_timeout(it->snooze_time, on_alrm, it);
			break;
		}
	}
}

static void my_exit(void)
{
	if (mosq)
		mosquitto_disconnect(mosq);
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		switch (logmask) {
		case LOG_UPTO(LOG_NOTICE):
			logmask = LOG_UPTO(LOG_INFO);
			break;
		case LOG_UPTO(LOG_INFO):
			logmask = LOG_UPTO(LOG_DEBUG);
			break;
		}
		break;
	case 'm':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	atexit(my_exit);
	openlog(NAME, LOG_PERROR, LOG_LOCAL2);
	setlogmask(logmask);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	/* SUBSCRIBE */
	if (optind >= argc) {
		ret = mosquitto_subscribe(mosq, NULL, "alarms/+/+", mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe 'alarms/+/+': %s", mosquitto_strerror(ret));
		/* re-assign the default */
	} else for (; optind < argc; ++optind) {
		ret = mosquitto_subscribe(mosq, NULL, argv[optind], mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe %s: %s", argv[optind], mosquitto_strerror(ret));
	}

	while (1) {
		libt_flush();
		waittime = libt_get_waittime();
		if (waittime > 1000)
			waittime = 1000;
		ret = mosquitto_loop(mosq, waittime, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}
	return 0;
}

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
	" -s, --snooze=TIME	Specify snooze time (default 9m)\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "snooze", required_argument, NULL, 's', },
	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static int snooze_time = 9*60;

/* alarm states */
static const char *const alrm_states[] = {
#define ALRM_OFF	0
	[0] = "off",
#define ALRM_ON		1
	[1] = "on",
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
	int hh, mm;
	int wdays; /* bitmask */
	int valid; /* definition has been seen */
	int enabled;
	int skip;

	int state;
};

struct item *items;
/* each 'root' topic to listen for alarms */
static char **alrm_root_topics;

static void reschedule_alrm(struct item *it);
/* utils */
static int parse_time(struct item *it, char *str)
{
	char *next;
	int value;

	if (!str)
		return -1;
	value = strtoul(str, &next, 10);
	if (next <= str || !strchr(":hHuU", *next))
		return -1;
	it->hh = value;
	it->mm = strtoul(next+1, NULL, 10);
	return 0;
}

static int parse_repeat(struct item *it, char *str)
{
	int j;

	it->wdays = 0;
	for (j = 0; str[j] && (j < 7); ++j) {
		if (!strchr("-_", str[j]))
			/* enable this day,
			 * wday is struct tm.tm_wday compatible
			 * (sunday == 0)
			 */
			it->wdays |= 1 << ((j+1) % 7);
	}
	return 0;
}

__attribute__((format(printf,1,2)))
static const char *csprintf(const char *fmt, ...)
{
	va_list va;
	static char *str;

	if (str)
		free(str);
	str = NULL;
	va_start(va, fmt);
	vasprintf(&str, fmt, va);
	va_end(va);
	return str;
}

long next_alarm(const struct item *it)
{
	struct tm tm;
	time_t tnow, tnext;
	int j;

	time(&tnow);
	tm = *localtime(&tnow);
	tm.tm_hour = it->hh;
	tm.tm_min = it->mm;
	tm.tm_sec = 0;
	tnext = mktime(&tm);
	if (tm.tm_hour != it->hh || tm.tm_min != it->mm) {
		/* probably crossed daylight saving settings */
		tm.tm_hour = it->hh;
		tm.tm_min = it->mm;
		tnext = mktime(&tm);
	}
	if (tnext <= tnow) {
		tm.tm_mday += 1;
		tnext = mktime(&tm);
		if (tm.tm_hour != it->hh || tm.tm_min != it->mm) {
			/* probably crossed daylight saving settings */
			tm.tm_hour = it->hh;
			tm.tm_min = it->mm;
			tnext = mktime(&tm);
		}
	}
	for (j = 0; j < 7; ++j) {
		if (it->wdays & (1 << tm.tm_wday))
			break;
		tm.tm_mday += 1;
		tnext = mktime(&tm);
		if (tm.tm_hour != it->hh || tm.tm_min != it->mm) {
			/* probably crossed daylight saving settings */
			tm.tm_hour = it->hh;
			tm.tm_min = it->mm;
			tnext = mktime(&tm);
		}
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

static struct item *get_item(const char *ctopic)
{
	struct item *it = NULL;
	char *topic, *sep;

	topic = strdup(ctopic ?: "");
	if (!topic)
		return NULL;
	sep = strrchr(topic, '/');
	if (!sep /* should not happen */ || (*(sep-1) == '/')) {
		/* detect 'alarms//+' patterns, i.e. global controls */
		free(topic);
		return NULL;
	}

	/* cut last part */
	*sep = 0;

	for (it = items; it; it = it->next) {
		if (!strcmp(it->topic, topic)) {
			free(topic);
			return it;
		}
	}

	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	/* assign dup'd topic to item, no need to dup twice, no need to free */
	it->topic = topic;
	it->topiclen = strlen(it->topic);

	it->enabled = 1;
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

static int hold_pub_alarms;
static void pub_alarms(void)
{
	struct item *it;
	static char buf[2048];
	char *str = buf;
	char **tpcs;

	if (hold_pub_alarms)
		return;
	*str = 0;
	for (it = items; it; it = it->next) {
		if (it->state == ALRM_ON)
			str += sprintf(str, "%s%s", (str > buf) ? " " : "",
					it->topic);
	}
	mylog(LOG_INFO, "alarms '%s'", buf);
	for (tpcs = alrm_root_topics; *tpcs; ++tpcs)
		mosquitto_publish(mosq, NULL, csprintf("%s//alarms", *tpcs),
				strlen(buf), buf, mqtt_qos, 1);
}

static void pub_alarms_hold(int v)
{
	hold_pub_alarms = v;
	if (!v)
		pub_alarms();
}

static void pub_alrm_state(struct item *it)
{
	const char *state = alrm_states[it->state];

	mosquitto_publish(mosq, NULL, csprintf("%s/state", it->topic),
			strlen(state), state, mqtt_qos, 1);
	pub_alarms();
}

/* timeout handlers */
static void alrm_toolong(void *dat)
{
	struct item * it = dat;

	mylog(LOG_INFO, "self-destruct %s", it->topic);
	reschedule_alrm(it);
}

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
	libt_add_timeout(60*60, alrm_toolong, it);
}

static void snooze_alrm(struct item *it)
{
	libt_remove_timeout(alrm_toolong, it);
	libt_add_timeout(snooze_time, on_alrm, it);
	mylog(LOG_INFO, "snoozed %s for %us", it->topic, snooze_time);
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
	libt_remove_timeout(alrm_toolong, it);
	if (it->state != ALRM_OFF) {
		it->state = ALRM_OFF;
		pub_alrm_state(it);
	}
	if (it->valid && it->enabled) {
		long delay;

		delay = next_alarm(it);
		libt_add_timeout(delay, on_alrm, it);
		mylog(LOG_INFO, "scheduled '%s' in %lus", it->topic, delay);
	} else
		mylog(LOG_INFO, "disabled '%s'", it->topic);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	char *tok;
	struct item *it;

	/* no item found */
	it = get_item(msg->topic);
	tok = strrchr(msg->topic ?: "", '/');
	if (!tok)
		return;
	if (!it) {
		/* global controls */
		if (!strcmp(tok, "dismiss")) {
			pub_alarms_hold(1);
			for (it = items; it; it = it->next) {
				if (it->state != ALRM_OFF)
					dismiss_alrm(it);
			}
			pub_alarms_hold(0);
		} else if (!strcmp(tok, "snooze")) {
			pub_alarms_hold(1);
			for (it = items; it; it = it->next) {
				if (it->state != ALRM_OFF)
					snooze_alrm(it);
			}
			pub_alarms_hold(0);
		}
	}
	if (!strcmp(tok, "/alarm")) {
		if (!msg->payloadlen) {
			/* flush potential MQTT leftovers */
			mosquitto_publish(mosq, NULL, csprintf("%s/repeat", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/skip", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/enable", it->topic),
					0, NULL, mqtt_qos, 1);
			mosquitto_publish(mosq, NULL, csprintf("%s/state", it->topic),
					0, NULL, mqtt_qos, 1);
			drop_item(it);
			return;
		}
		parse_time(it, msg->payload ?: "");
		/* mark as valid */
		it->valid = 1;
		reschedule_alrm(it);
	} else if (!strcmp(tok, "/repeat")) {
		parse_repeat(it, msg->payload ?: "");
		reschedule_alrm(it);
	} else if (!strcmp(tok, "/skip")) {
		it->skip = strtoul(msg->payload ?: "0", 0, 0);
	} else if (!strcmp(tok, "/enable")) {
		int val = strtoul(msg->payload ?: "1", 0, 0);

		if (val != it->enabled) {
			it->enabled = val;
			reschedule_alrm(it);
		}
	} else if (!strcmp(tok, "/dismiss") && (it->state != ALRM_OFF)) {
		dismiss_alrm(it);
	} else if (!strcmp(tok, "/snooze") && (it->state != ALRM_OFF)) {
		snooze_alrm(it);
	} else if (!strcmp(tok, "/state")) {
		int newstate;

		for (newstate = 0; newstate < sizeof(alrm_states)/sizeof(alrm_states[0]); ++newstate) {
			if (!strcmp(msg->payload, alrm_states[newstate]))
				break;
		}
		if (newstate >= sizeof(alrm_states)/sizeof(alrm_states[0]))
			/* bad state supplied */
			return;
		if (it->state == newstate)
			/* nothing to do */
			return;
		mylog(LOG_INFO, "new state %s = '%s'", msg->topic, alrm_states[newstate]);
		it->state = newstate;
		switch (newstate) {
		case ALRM_OFF:
			dismiss_alrm(it);
			break;
		case ALRM_ON:
			libt_remove_timeout(on_alrm, it);
			libt_add_timeout(60*60, alrm_toolong, it);
			break;
		case ALRM_SNOOZED:
			libt_remove_timeout(alrm_toolong, it);
			libt_add_timeout(snooze_time, on_alrm, it);
			break;
		}
		pub_alarms();
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
	char *str, *endp;
	const char *cstr;
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
	case 's':
		snooze_time = strtoul(optarg, &endp, 0);
		switch (*endp) {
		case 'w':
			snooze_time *= 7;
		case 'd':
			snooze_time *= 24;
		case 'h':
			snooze_time *= 60;
		case 'm':
			snooze_time *= 60;
			break;
		}
		break;

	case '?':
		fputs(help_msg, stderr);
		exit(0);
	default:
		fprintf(stderr, "unknown option '%c'", opt);
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
	alrm_root_topics = argv+optind;
	if (optind >= argc) {
		ret = mosquitto_subscribe(mosq, NULL, "alarms/+/+", mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe 'alarms/+/+': %s", mosquitto_strerror(ret));
		/* re-assign the default */
		alrm_root_topics = (char *[]){ "alarms", NULL, };
	} else for (; optind < argc; ++optind) {
		cstr = csprintf("%s/+/+", argv[optind]);
		ret = mosquitto_subscribe(mosq, NULL, cstr, mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe %s: %s", cstr, mosquitto_strerror(ret));
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

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
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

#define NAME "mqttoff"
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
	NAME ": an MQTT timeout-turnoff daemon\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -r, --reset=STR	The global 'default' reset value (default '0')\n"
	" -s, --suffix=STR	Give MQTT topic suffix for timeouts (default '/timeoff')\n"
	" -w, --write=STR	Give MQTT topic suffix for writing the topic (default empty)\n"
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
	{ "reset", required_argument, NULL, 'r', },
	{ "suffix", required_argument, NULL, 's', },
	{ "write", required_argument, NULL, 'w', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:r:s:w:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/timeoff";
static const char *mqtt_write_suffix;
static const char *mqtt_reset_value = "0";
static int mqtt_suffixlen = 1;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;

	char *topic;
	char *resetvalue;
	double delay;
	double ontime;
};

struct item *items;

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

static struct item *get_item(const char *topic)
{
	struct item *it;

	for (it = items; it; it = it->next)
		if (!strcmp(it->topic, topic))
			return it;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->topic = strdup(topic);
	it->resetvalue = strdup(mqtt_reset_value);

	/* insert in linked list */
	it->next = items;
	it->ontime = it->delay = NAN;
	items = it;
	return it;
}

static void reset_item(void *dat)
{
	int ret;
	struct item *it = dat;
	char *settopic;

	if (mqtt_write_suffix)
		asprintf(&settopic, "%s/%s", it->topic, mqtt_write_suffix);
	else
		settopic = it->topic;
	/* publish, retained when writing the topic, volatile (not retained) when writing to another topic */
	ret = mosquitto_publish(mosq, NULL, settopic, strlen(it->resetvalue), it->resetvalue, mqtt_qos, !mqtt_write_suffix);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", settopic, mosquitto_strerror(ret));
	/* clear cache too */
	it->ontime = 0;
	mylog(LOG_INFO, "%s = %s", settopic, it->resetvalue);
	if (settopic != it->topic)
		free(settopic);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int len;
	char *tok, *topic;
	struct item *it;

	len = strlen(msg->topic);
	if (len > mqtt_suffixlen && !strcmp(msg->topic + len - mqtt_suffixlen, mqtt_suffix)) {
		/* this is a timeout set msg */
		topic = strdup(msg->topic);
		topic[len-mqtt_suffixlen] = 0;

		it = get_item(topic);
		/* don't need this copy anymore */
		free(topic);

		/* process timeout spec */
		if (msg->payloadlen) {
			tok = strtok(msg->payload, " \t");
			if (tok) {
				it->delay = strtod(tok, &tok);
				switch (tolower(*tok)) {
				case 'w':
					it->delay *= 7;
				case 'd':
					it->delay *= 24;
				case 'h':
					it->delay *= 60;
				case 'm':
					it->delay *= 60;
					break;
				}
			} else
				it->delay = NAN;
			tok = strtok(NULL, " \t");
			if (tok)
				it->resetvalue = strdup(tok);
			else {
				if (it->resetvalue)
					free(it->resetvalue);
				it->resetvalue = strdup(mqtt_reset_value);
			}
		}
		mylog(LOG_INFO, "timeoff spec for %s: %.2lfs '%s'", it->topic, it->delay, it->resetvalue ?: "");

		libt_remove_timeout(reset_item, it);
		if (!isnan(it->delay) && !isnan(it->ontime)) {
			libt_add_timeouta(it->ontime + it->delay, reset_item, it);
			mylog(LOG_INFO, "%s: schedule action in %.2lfs", it->topic, it->delay);
		}
		return;
	}
	/* find topic */
	it = get_item(msg->topic);

	if (!msg->payloadlen)
		return;
	if (!strcmp(it->resetvalue, (char *)msg->payload)) {
		/* value was reset */
		libt_remove_timeout(reset_item, it);
		it->ontime = NAN;
		if (!isnan(it->delay))
			mylog(LOG_INFO, "%s: reverted, no action required", it->topic);
	} else if (isnan(it->ontime)) {
		/* set ontime only on first set */
		it->ontime = libt_now();
		libt_add_timeouta(it->ontime + it->delay, reset_item, it);
		if (!isnan(it->delay))
			mylog(LOG_INFO, "%s: schedule action in %.2lfs", it->topic, it->delay);
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
	case 'r':
		mqtt_reset_value = optarg;
		break;
	case 's':
		mqtt_suffix = optarg;
		mqtt_suffixlen = strlen(mqtt_suffix);
		break;
	case 'w':
		mqtt_write_suffix = optarg;
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

	if (optind >= argc) {
		ret = mosquitto_subscribe(mosq, NULL, "#", mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe '#': %s", mosquitto_strerror(ret));
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
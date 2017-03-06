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

#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <syslog.h>
#include <mosquitto.h>

#include "lib/libt.h"

#define NAME "mqttnow"
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
	" -s, --suffix=STR	Give MQTT topic suffix for timeouts (default '/fmtnow')\n"
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
	{ "suffix", required_argument, NULL, 's', },

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
static const char *mqtt_suffix = "/fmtnow";
static int mqtt_suffixlen = 7;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	char *fmt;
	char *lastvalue;
};

struct item *items;

/* signalling */
static void onsigterm(int signr)
{
	sigterm = 1;
}

/* count pending state */
static int nvalid(void)
{
	struct item *it;
	int ret = 0;

	for (it = items; it; it = it->next)
		ret += it->lastvalue ? 1 : 0;
	return ret;
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

static struct item *get_item(const char *topic, int create)
{
	struct item *it;

	for (it = items; it; it = it->next)
		if (!strcmp(it->topic, topic))
			return it;
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->topic = strdup(topic);

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
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* publish null value */
	mosquitto_publish(mosq, NULL, it->topic, 0, NULL, mqtt_qos, 1);
	/* free memory */
	free(it->topic);
	if (it->fmt)
		free(it->fmt);
	if (it->lastvalue)
		free(it->lastvalue);
	free(it);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int len;
	char *topic;
	struct item *it;

	len = strlen(msg->topic);
	if (len > mqtt_suffixlen && !strcmp(msg->topic + len - mqtt_suffixlen, mqtt_suffix)) {
		/* this is a timeout set msg */
		topic = strdup(msg->topic);
		topic[len-mqtt_suffixlen] = 0;

		it = get_item(topic, !!msg->payload);
		/* don't need this copy anymore */
		free(topic);

		if (!it)
			return;
		/* remove on null config */
		if (!msg->payload) {
			mylog(LOG_INFO, "mqttnow spec for %s removed", it->topic);
			drop_item(it);
			return;
		}
		if (it->fmt && !strcmp(it->fmt, msg->payload))
			/* no change */
			return;
		if (it->fmt)
			free(it->fmt);
		it->fmt = strdup(msg->payload);
		mylog(LOG_INFO, "mqttnow spec for %s: '%s'", it->topic, it->fmt);
		return;
	}
	if (sigterm) {
		/* during shutdown, register empty items */
		it = get_item(msg->topic, 0);
		if (!it)
			return;
		if (!it->lastvalue)
			/* it has not been set yet, or has been cleared already */
			/* don't track into eternity ! */
			return;
		if (!msg->payloadlen) {
			free(it->lastvalue);
			it->lastvalue = NULL;
		}
	}
}

static void my_exit(void)
{
	if (!mosq)
		mosquitto_disconnect(mosq);
}

static void sendnow(void *dat)
{
	struct item *it;
	int ret;
	static char str[1024];
	time_t tnow;
	struct tm tm;

	if (sigterm)
		/* stop sending */
		return;
	time(&tnow);
	tm = *localtime(&tnow);

	for (it = items; it; it = it->next) {
		if (!it->fmt)
			continue;
		strftime(str, sizeof(str), it->fmt, &tm);
		if (it->lastvalue && !strcmp(it->lastvalue, str))
			continue;
		ret = mosquitto_publish(mosq, NULL, it->topic, strlen(str), str, mqtt_qos, 1);
		if (ret < 0) {
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
			continue;
		}
		if (it->lastvalue)
			free(it->lastvalue);
		it->lastvalue = strdup(str);
	}
	libt_repeat_timeout(1, sendnow, dat);
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);

	setlocale(LC_ALL, "");
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
		mqtt_suffix = optarg;
		mqtt_suffixlen = strlen(mqtt_suffix);
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	atexit(my_exit);
	signal(SIGINT, onsigterm);
	signal(SIGTERM, onsigterm);
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

	sendnow(NULL);
	while (1) {
		if (sigterm && !nvalid())
			break;
		if (sigterm == 1) {
			struct item *it;

			/* mark as cleared */
			sigterm = 2;
			/* clear time indications */
			for (it = items; it; it = it->next)
				mosquitto_publish(mosq, NULL, it->topic, 0, NULL, mqtt_qos, 1);
		}
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

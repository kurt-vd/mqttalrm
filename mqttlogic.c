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
#include "rpnlogic.h"

#define NAME "mqttlogic"
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
	NAME ": an MQTT logic processor\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for scripts (default '/logic')\n"
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
	{ "suffix", required_argument, NULL, 's', },
	{ "write", required_argument, NULL, 'w', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:w:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/logic";
static const char *mqtt_write_suffix;
static int mqtt_suffixlen = 6;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	char *writetopic;
	char *lastvalue;

	struct rpn *logic;
	char *fmt;
};

static struct item *items;

static struct stack rpnstack;

/* topic cache */
struct topic {
	char *topic;
	char *value;
	int ref;
	int changed;
};
static struct topic *topics;
static int ntopics; /* used topics */
static int stopics;

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

/* mqtt cache */
static int rpn_has_ref(struct rpn *rpn, const char *topic);

static int topiccmp(const void *a, const void *b)
{
	return strcmp(((const struct topic *)a)->topic ?: "", ((const struct topic *)b)->topic ?: "");
}

struct topic *get_topic(const char *name, int create)
{
	struct topic *topic;
	struct topic ref = { .topic = (char *)name, };
	struct item *it;

	topic = bsearch(&ref, topics, ntopics, sizeof(*topics), topiccmp);
	if (topic)
		return topic;
	if (!create)
		return NULL;
	/* make room */
	if (ntopics >= stopics) {
		stopics += 128;
		topics = realloc(topics, sizeof(*topics)*stopics);
	}
	topics[ntopics++] = (struct topic){ .topic = strdup(name), };
	qsort(topics, ntopics, sizeof(*topics), topiccmp);
	topic = get_topic(name, 0);

	/* set already referenced topics */
	for (it = items; it; it = it->next) {
		if (rpn_has_ref(it->logic, name))
			topic->ref += 1;
	}
	return topic;
}

double rpn_lookup_env(const char *name, struct rpn *rpn)
{
	struct topic *topic;

	topic = get_topic(name, 0);
	if (!topic) {
		mylog(LOG_INFO, "topic %s not found", name);
		return 0;
	}
	if (strchr(rpn->options ?: "", '1') && !topic->changed)
		return 0;
	return strtod(topic->value, NULL);
}

/* logic items */
static void rpn_add_ref(struct rpn *rpn, int add)
{
	struct topic *topic;

	for (; rpn; rpn = rpn->next) {
		if (!rpn->topic)
			continue;
		topic = get_topic(rpn->topic, 0);
		if (!topic)
			continue;
		topic->ref += add;
	}
}
#define rpn_ref(rpn)	rpn_add_ref((rpn), +1)
#define rpn_unref(rpn)	rpn_add_ref((rpn), -1)
static int rpn_has_ref(struct rpn *rpn, const char *topic)
{
	for (; rpn; rpn = rpn->next) {
		if (rpn->topic && !strcmp(topic, rpn->topic))
			return 1;
	}
	return 0;
}

static struct item *get_item(const char *topic, int matchlen, int create)
{
	struct item *it;

	for (it = items; it; it = it->next)
		if (!strncmp(it->topic, topic, matchlen) && !it->topic[matchlen])
			return it;
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	/* set topic */
	it->topic = strdup(topic);
	it->topic[matchlen] = 0;
	/* set write topic */
	if (mqtt_write_suffix)
		asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);

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
	/* free memory */
	free(it->topic);
	if (it->writetopic)
		free(it->writetopic);
	if (it->fmt)
		free(it->fmt);
	rpn_free_chain(it->logic);
	free(it);
}

static void do_item(struct item *it)
{
	int ret;
	char buf[32];

	rpn_stack_reset(&rpnstack);
	ret = rpn_run(&rpnstack, it->logic);
	if (ret < 0 || !rpnstack.n)
		/* TODO: alert */
		return;
	sprintf(buf, it->fmt ?: "%f", rpnstack.v[rpnstack.n-1]);
	/* test if we found something new */
	if (!strcmp(it->lastvalue ?: "", buf))
		return;
	ret = mosquitto_publish(mosq, NULL, it->writetopic ?: it->topic, strlen(buf), buf, mqtt_qos, !mqtt_write_suffix);
	if (ret < 0) {
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
		return;
	}
	/* save cache */
	if (it->lastvalue)
		free(it->lastvalue);
	it->lastvalue = strdup(buf);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int len;
	struct item *it;
	struct topic *topic;
	char *str;

	len = strlen(msg->topic);
	if (len > mqtt_suffixlen && !strcmp(msg->topic + len - mqtt_suffixlen, mqtt_suffix)) {
		/* this is a logic set msg */
		it = get_item(msg->topic, len-mqtt_suffixlen, msg->payloadlen);
		if (it && !msg->payloadlen) {
			drop_item(it);
			it = NULL;
		}
		if (!it)
			return;

		/* remove old logic */
		rpn_unref(it->logic);
		rpn_free_chain(it->logic);
		if (it->fmt)
			free(it->fmt);
		it->fmt = NULL;
		/* prepare new info */
		str = strrchr(msg->payload, ' ');
		if (str && str[1] == '%') {
			/* cut rpn logic */
			*str++ = 0;
			it->fmt = strdup(str);
			/* TODO: verify this format string */
		}
		it->logic = rpn_parse(msg->payload);
		rpn_ref(it->logic);
		mylog(LOG_INFO, "new logic for %s", it->topic);
		/* ready, first run */
		do_item(it);
		return;
	}
	/* find topic */
	topic = get_topic(msg->topic, msg->payloadlen);
	if (!topic)
		return;
	free(topic->value);
	topic->value = strndup(msg->payload ?: "", msg->payloadlen);
	if (topic->ref) {
		topic->changed = 1;
		for (it = items; it; it = it->next) {
			if (!strcmp(it->topic, msg->topic))
				/* cut loops */
				continue;
			if (rpn_has_ref(it->logic, msg->topic))
				do_item(it);
		}
		topic->changed = 0;
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
	case 's':
		mqtt_suffix = optarg;
		mqtt_suffixlen = strlen(mqtt_suffix);
		break;
	case 'w':
		mqtt_write_suffix = optarg;
		break;

	default:
		fprintf(stderr, "unknown option '%c'\n", opt);
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

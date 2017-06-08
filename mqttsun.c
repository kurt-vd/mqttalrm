#include <ctype.h>
#include <errno.h>
#include <math.h>
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
#include "sun.h"

#define NAME "mqttsun"
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
	NAME ": publish sun position in MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN ...]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=INCL[,AZM[,LAT[,LON]]]\n"
	"			Specify Sun inclination, Sun azimuth, \n"
	"			Longitude and Latitude suffixes\n"
	"			Default '/sun/elv,/sun/azm,/lat,/lon'\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for\n"
	"\n"
	"Topic Layout\n"
	" /path/to/lat	\n"
	" /path/to/lon		Geo position\n"
	" /path/to/sun/elv	Sun inclination/elevation value\n"
	" /path/to/sun/azm	Sun azimuth value\n"
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

#define TOPIC_LAT	0
#define TOPIC_LON	1
#define TOPIC_ELV	2
#define TOPIC_AZM	3

static const char *mqtt_suffix[] = {
	[TOPIC_LAT] = "/lat",
	[TOPIC_LON] = "/lon",
	[TOPIC_ELV] = "/sun/elv",
	[TOPIC_AZM] = "/sun/azm",
};
static int mqtt_suffixlen[] = {
	[TOPIC_LAT] = 4,
	[TOPIC_LON] = 4,
	[TOPIC_ELV] = 8,
	[TOPIC_AZM] = 8,
};
#define NTOPICS	(sizeof(mqtt_suffix)/sizeof(mqtt_suffix[0]))

static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	/* all related topics */
	char *topics[NTOPICS];
	/* cached values */
	double val[NTOPICS];
#define v_lat	val[TOPIC_LAT]
#define v_lon	val[TOPIC_LON]
};

struct item *items;

/* signalling */
static void onsigterm(int signr)
{
	sigterm = 1;
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

static struct item *get_item(const char *topic, int match, int create)
{
	struct item *it;
	int j, len;

	for (it = items; it; it = it->next)
		if (!strcmp(it->topics[match], topic))
			return it;
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->v_lon = NAN;
	it->v_lat = NAN;
	/* preset topics */
	len = strlen(topic) - strlen(mqtt_suffix[match]);
	for (j = 0; j < NTOPICS; ++j) {
		if (mqtt_suffix[j])
			asprintf(&it->topics[j], "%.*s%s", len, topic, mqtt_suffix[j]);
	}


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
	int j;

	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* free memory */
	for (j = 0; j < NTOPICS; ++j)
		if (it->topics[j])
			free(it->topics[j]);
	free(it);
}

static void pushitem(void *dat)
{
	struct item *it = dat;
	int ret;
	double inc, azm;
	time_t tnow;
	char strsun[32] = {}, strazm[32] = {};

	if (sigterm)
		/* stop sending */
		return;

	if (!isnan(it->v_lat) && !isnan(it->v_lon)) {
		time(&tnow);
		if (sungetpos(tnow, it->v_lat, it->v_lon, &inc, &azm, NULL) >= 0) {
			sprintf(strsun, "%.3lf", inc);
			sprintf(strazm, "%.3lf", azm);
		}
	}
	if (it->topics[TOPIC_ELV]) {
		ret = mosquitto_publish(mosq, NULL, it->topics[TOPIC_ELV], strlen(strsun), strsun, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topics[TOPIC_ELV], mosquitto_strerror(ret));
	}
	if (it->topics[TOPIC_AZM]) {
		ret = mosquitto_publish(mosq, NULL, it->topics[TOPIC_AZM], strlen(strazm), strazm, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topics[TOPIC_AZM], mosquitto_strerror(ret));
	}
	libt_add_timeout(60, pushitem, it);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int len, j;
	struct item *it;

	len = strlen(msg->topic);
	for (j = 0; j < NTOPICS; ++j) {
		if (!mqtt_suffix[j] || (len < mqtt_suffixlen[j]) ||
				strcmp(msg->topic+len-mqtt_suffixlen[j], mqtt_suffix[j]))
			continue;
		/* matching topic */
		it = get_item(msg->topic, j, !!msg->payload);
		if (!it)
			return;
		switch (j) {
		case TOPIC_LAT:
		case TOPIC_LON:
			/* input */
			it->val[j] = strtod(msg->payload ?: "nan", NULL);
			/* refrsh */
			pushitem(it);
			break;

			break;
		case TOPIC_ELV:
		case TOPIC_AZM:
			/* output */
			if (sigterm && msg->retain && !msg->payloadlen && it->topics[j]) {
				/* free this topic name, indicate it has been cleard
				 * in the MQTT cache
				 */
				free(it->topics[j]);
				it->topics[j] = NULL;
				/* drop item when all our topics have been cleared */
				if (!it->topics[TOPIC_ELV] && !it->topics[TOPIC_AZM])
					drop_item(it);
			}
			break;
		}
	}
}

static void my_exit(void)
{
	if (!mosq)
		mosquitto_disconnect(mosq);
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime, j;
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
		mqtt_suffix[TOPIC_LAT] = strtok(optarg, ",") ?: mqtt_suffix[TOPIC_LAT];
		mqtt_suffix[TOPIC_LON] = strtok(NULL, ",") ?: mqtt_suffix[TOPIC_LON];
		mqtt_suffix[TOPIC_ELV] = strtok(NULL, ",") ?: mqtt_suffix[TOPIC_ELV];
		mqtt_suffix[TOPIC_AZM] = strtok(NULL, ",") ?: mqtt_suffix[TOPIC_AZM];
		for (j = 0; j < NTOPICS; ++j)
			mqtt_suffixlen[j] = strlen(mqtt_suffix[j] ?: "");
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

	while (!sigterm || items) {
		if (sigterm == 1) {
			struct item *it;

			/* mark as cleared */
			sigterm = 2;
			/* clear time indications */
			for (it = items; it; it = it->next) {
				mosquitto_publish(mosq, NULL, it->topics[TOPIC_ELV], 0, NULL, mqtt_qos, 1);
				mosquitto_publish(mosq, NULL, it->topics[TOPIC_AZM], 0, NULL, mqtt_qos, 1);
			}
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "logger.h"

#include <event.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/event-config.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ulimit.h>
#include <ctype.h>

struct config {
	uint16_t port;
	int connect_timeout;
	int read_timeout;
	int current_running;
	int max_concurrent;
	struct event_base *base;
	struct bufferevent *stdin_bev;
	int stdin_closed;
	char *search_string;
	int max_read_size;
	int format;
	char *send_str;
	long send_str_size;

	struct stats_st {
		int found;
		int init_connected_hosts;
		int connected_hosts;
		int conn_timed_out;
		int read_timed_out;
		int timed_out;
		int completed_hosts;
	};


struct state {
	struct config *conf;
	uint32_t ip;
	enum {CONNECTING, CONNECTED, RECEIVED} state;
};

void stdin_readcb(struct bufferevent *bev, void *arg);

void print_status(evutil_socket_t fd, short events, void *arg) {
	struct event *ev;
	struct config *conf = arg;
	struct event_base *base = conf->base;
	struct timeval status_timeout = {1, 0};
	ev = evtimer_new(base, print_status, conf);
	evtimer_add(ev, &status_timeout);
	(void)fd; (void)events;

	log_info("bootymapper", "(%d/%d in use) - %d found containing \"%s\", %d inititiated, %d connected, %d no connection, %d no data, %d completed",
			conf->current_running, conf->max_concurrent, conf->stats.found, conf->search_string,
			conf->stats.init_connected_hosts,
			conf->stats.connected_hosts, conf->stats.conn_timed_out,
			conf->stats.read_timed_out, conf->stats.completed_hosts);
}

void decrement_cur_running(struct state *st) {
	struct config *conf = st->conf;
	conf->current_running--;

	if (evbuffer_get_length(bufferevent_get_input(conf->stdin_bev)) > 0) {
		stdin_readcb(conf->stdin_bev, conf);
	}
	free(st);

	if (conf->stdin_closed && conf->current_running == 0) {
		log_info("bootymapper", "Scan completed.");
		print_status(0, 0, conf);
		exit(0);
	}

}

void connect_cb(struct bufferevent *bev, short events, void *arg) {
	struct state *st = arg;
	struct config *conf = st->conf;
	struct in_addr addr;
	addr.s_addr = st->ip;
	if (events & BEV_EVENT_CONNECTED) {
		struct timeval tv = {st->conf->read_timeout, 0};

		if (conf->send_str) {
			struct evbuffer *evout = bufferevent_get_output(bev);
			evbuffer_set_max_read(evout, conf->max_read_size);
			evbuffer_add_printf(evout, conf->send_str,
					inet_ntoa(addr), inet_ntoa(addr), inet_ntoa(addr), inet_ntoa(addr));
		}

		bufferevent_set_timeouts(bev, &tv, &tv);

		st->state = CONNECTED;
		st->conf->stats.connected_hosts++;
	} else {
		if (st->state == CONNECTED) {
			st->conf->stats.read_timed_out++;
		} else {
			st->conf->stats.conn_timed_out++;
		}

		bufferevent_free(bev);
		st->conf->stats.timed_out++;
		decrement_cur_running(st);
	}
}

void read_cb(struct bufferevent *bev, void *arg) {
	struct evbuffer *in = bufferevent_get_input(bev);
	struct state *st = arg;
	evbuffer_set_max_read(in, st->conf->max_read_size);
	size_t len = evbuffer_get_length(in);
	struct in_addr addr;
	addr.s_addr = st->ip;

	if (len > 0) {

		char *buf = malloc(len+1);

		st->state = RECEIVED;

		if (!buf) {
			log_fatal("bootymapper", "cannot alloc %d byte buf", len+1);
			return;
		}

		evbuffer_remove(in, buf, len);

		if(st->conf->search_string != NULL && strstr(buf, st->conf->search_string) != NULL) {
			if(st->conf->format == 1) {
				printf("%s\n", inet_ntoa(addr));
			} else {
				printf("%s ", inet_ntoa(addr));
				buf[len] = '\0';
				printf("%s\n", buf);
			}
			st->conf->stats.found++;
		} else if(st->conf->search_string == NULL) {
			if(st->conf->format == 1) {
                                printf("%s\n", inet_ntoa(addr));
                        } else {
                                printf("%s ", inet_ntoa(addr));
                                buf[len] = '\0';
                                printf("%s\n", buf);
                        }
			st->conf->stats.found++;
		}

		fflush(stdout);

		free(buf);
		st->conf->stats.completed_hosts++;
	}
	bufferevent_free(bev);
	decrement_cur_running(st);
}

void grab_banner(struct state *st)
{
	struct sockaddr_in addr;
	struct bufferevent *bev;
	struct timeval read_to = {st->conf->connect_timeout, 0};

	addr.sin_family = AF_INET;
	addr.sin_port = htons(st->conf->port);
	addr.sin_addr.s_addr = st->ip;

	bev = bufferevent_socket_new(st->conf->base, -1, BEV_OPT_CLOSE_ON_FREE);

	bufferevent_set_timeouts(bev, &read_to, &read_to);

	bufferevent_setcb(bev, read_cb, NULL, connect_cb, st);
	bufferevent_enable(bev, EV_READ);

	st->state = CONNECTING;

	st->conf->stats.init_connected_hosts++;

	if (bufferevent_socket_connect(bev,
		(struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_warn("bootymapper", "could not connect socket %d (%d open) %d %d",
			bufferevent_getfd(bev), st->conf->current_running, errno, ENFILE);
		perror("connect");


		bufferevent_free(bev);
		decrement_cur_running(st);
		return;
	}
}

void stdin_eventcb(struct bufferevent *bev, short events, void *ptr) {
	struct config *conf = ptr;

	if (events & BEV_EVENT_EOF) {
		conf->stdin_closed = 1;
		if (conf->current_running == 0) {
			log_info("bootymapper", "done");
			print_status(0, 0, conf);
			exit(0);
		}
	} 
}

void stdin_readcb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *in = bufferevent_get_input(bev);
	struct config *conf = arg;
	evbuffer_set_max_read(in, conf->max_read_size);

	while (conf->current_running < conf->max_concurrent &&
		   evbuffer_get_length(in) > 0) {
		char *ip_str;
		size_t line_len;
		char *line = evbuffer_readln(in, &line_len, EVBUFFER_EOL_LF);
		struct state *st;
		if (!line)
			break;

		ip_str = line;

		conf->current_running++;
		st = malloc(sizeof(*st));
		st->conf = conf;
		st->ip = inet_addr(ip_str);
		grab_banner(st);
	}
}

int main(int argc, char *argv[])
{
	struct event_base *base;
	struct event *status_timer;
	struct timeval status_timeout = {1, 0};
	int c;
	struct option long_options[] = {
		{"concurrent", required_argument, 0, 'c'},
		{"port", required_argument, 0, 'p'},
		{"conn-timeout", required_argument, 0, 't'},
		{"read-timeout", required_argument, 0, 'r'},
		{"verbosity", required_argument, 0, 'v'},
		{"data", required_argument, 0, 'd'},
		{"search-string", required_argument, 0, 's'},
		{"format", required_argument, 0, 'f'},
		{0, 0, 0, 0} };

	struct config conf;
	int ret;
	FILE *fp;

	log_init(stderr, LOG_INFO);

	ret = ulimit(conf.max_concurrent);

	if (ret < 0) {
		log_fatal("bootymapper", "Could not set ulimit");
		perror("ulimit");
		exit(1);
	}

	base = event_base_new();
	conf.base = base;

	conf.stdin_bev = bufferevent_socket_new(base, 0, BEV_OPT_DEFER_CALLBACKS);
	bufferevent_setcb(conf.stdin_bev, stdin_readcb, NULL, stdin_eventcb, &conf);
	bufferevent_enable(conf.stdin_bev, EV_READ);

	status_timer = evtimer_new(base, print_status, &conf);
	evtimer_add(status_timer, &status_timeout);

	conf.max_read_size = 16777216;
	conf.max_concurrent = 1000000;
	conf.current_running = 0;
	memset(&conf.stats, 0, sizeof(conf.stats));
	conf.connect_timeout = 5;
	conf.read_timeout = 5;
	conf.stdin_closed = 0;
	conf.send_str = NULL;

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:p:t:r:v:d:s:f:m:",
				long_options, &option_index);

		if (c < 0) {
			break;
		}

		switch (c) {
		case 'c':
			conf.max_concurrent = atoi(optarg);
			break;
		case 'p':
			conf.port = atoi(optarg);
			break;
		case 't':
			conf.connect_timeout = atoi(optarg);
			break;
		case 'r':
			conf.read_timeout = atoi(optarg);
			break;
		case 'v':
			if (atoi(optarg) >= 0 && atoi(optarg) <= 5) {
				log_init(stderr, atoi(optarg));
			}
			break;
		case 'd':
			fp = fopen(optarg, "r");
			if (!fp) {
				log_error("bootymapper", "Could not open send data file '%s':", optarg);
				perror("fopen");
				exit(-1);
			}
			fseek(fp, 0L, SEEK_END);
			conf.send_str_size = ftell(fp);
			fseek(fp, 0L, SEEK_SET);
			conf.send_str = malloc(conf.send_str_size+1);
			if (!conf.send_str) {
				log_fatal("bootymapper", "Could not malloc %d bytes", conf.send_str_size+1);
			}
			if (fread(conf.send_str, conf.send_str_size, 1, fp) != 1) {
				log_fatal("bootymapper", "Couldn't read from send data file '%s':", optarg);
			}
			conf.send_str[conf.send_str_size] = '\0';
			fclose(fp);
			break;
		case 's':
			conf.search_string = malloc(strlen(optarg));
			strcpy(conf.search_string, optarg);
			break;
		case 'f':
			if(strstr(optarg, "ip_only") != NULL) {
			conf.format = 1;
			}
			break;
		case 'm':
			conf.max_read_size = atoi(optarg);
			break;
		case '?':
			printf("Usage: %s [-c max_concurrent_sockets] [-t connection_timeout] [-r read_timeout] "
				   "[-v verbosity=0-5] [-d send_data] [-s \"search_string\"] [-f ip_only] -m [max_read_size] -p port\n", argv[0]);
			exit(1);
		default:
			break;
		}
	}

	log_info("bootymapper", "Using port %d with max_concurrency %d, %d s conn timeout, %d s read timeout",
			conf.port, conf.max_concurrent, conf.connect_timeout, conf.read_timeout);

	event_base_dispatch(base);

	return 0;
}


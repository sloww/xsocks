#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>

#include "uv.h"

#include "util.h"
#include "logger.h"
#include "crypto.h"
#include "resolver.h"
#include "daemon.h"
#include "udprelay.h"
#include "xsocksd.h"


#define MAX_DNS_NUM 4

static int daemon_mode = 1;
static int concurrency;
static int nameserver_num;
static char *nameservers[MAX_DNS_NUM + 1];
static char *local_addrbuf = "0.0.0.0:1073";
static char *pidfile = "xsocksd.pid";
static char *password;
static char *xsignal;
struct signal_ctx signals[3];

static const char *_optString = "l:k:c:d:p:t:nVvh";
static const struct option _lopts[] = {
    { "",        required_argument,   NULL, 'p' },
    { "",        required_argument,   NULL, 'c' },
    { "",        required_argument,   NULL, 'd' },
    { "",        required_argument,   NULL, 't' },
    { "",        required_argument,   NULL, 'k' },
    { "signal",  required_argument,   NULL,  0  },
    { "",        no_argument,   NULL, 'n' },
    { "version", no_argument,   NULL, 'v' },
    { "help",    no_argument,   NULL, 'h' },
    { "",        no_argument,   NULL, 'V' },
    { NULL,      no_argument,   NULL,  0  }
};

static void
print_usage(const char *prog) {
    printf("xsocksd Version: %s Maintained by Ken <ken.i18n@gmail.com>\n", XSOCKSD_VER);
    printf("Usage: %s [-l bind] <-k password> [-p pidfile] [-c concurrency] [-t timeout] [-s signal] [-nhvV]\n\n", prog);
    printf("Options:\n");
    puts("  -k <password>\t\t : password of server\n"
         "  [-l <bind address>]\t : bind address:port default(0.0.0.0:1082)\n"
         "  [-c <concurrency>]\t : worker threads\n"
         "  [-d <dns>]\t\t : name servers for internal DNS resolver\n"
         "  [-p <pidfile>]\t : pid file path (default: ./xsocksd.pid)\n"
         "  [-t <timeout>]\t : connection timeout in senconds\n"
         "  [--signal <signal>]\t : send signal to xsocksd: quit, stop\n"
         "  [-n]\t\t\t : non daemon mode\n"
         "  [-h, --help]\t\t : this help\n"
         "  [-v, --version]\t : show version\n"
         "  [-V] \t\t\t : verbose mode\n");

    exit(1);
}

static void
parse_opts(int argc, char *argv[]) {
    int opt = 0, longindex = 0;

    while ((opt = getopt_long(argc, argv, _optString, _lopts, &longindex)) != -1) {
        switch (opt) {
        case 'v':
            printf("xsocksd version: %s \n", XSOCKSD_VER);
            exit(0);
            break;
        case 'h':
        case '?':
            print_usage(argv[0]);
            break;
        case 'l':
            local_addrbuf = optarg;
            break;
        case 'k':
            password = optarg;
            break;
        case 'c':
            concurrency = strtol(optarg, NULL, 10);
            break;
        case 'd':
            if (nameserver_num < MAX_DNS_NUM) {
                nameservers[nameserver_num++] = optarg;
            }
            break;
        case 'p':
            pidfile = optarg;
            break;
        case 'n':
            daemon_mode = 0;
            break;
        case 't':
            idle_timeout = strtol(optarg, NULL, 10) * 1000;
            break;
        case 'V':
            verbose = 1;
            break;
		case 0: /* long option without a short arg */
            if (strcmp("signal", _lopts[longindex].name) == 0) {
                xsignal = optarg;
                if (strcmp(xsignal, "stop") == 0
                  || strcmp(xsignal, "quit") == 0) {
                    break;
                }
                fprintf(stderr, "invalid option: --signal %s\n", xsignal);
                print_usage(argv[0]);
            }
			break;
        default:
            print_usage(argv[0]);
            break;
        }
    }
}

static void
close_walk_cb(uv_handle_t *handle, void *arg) {
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

void
close_loop(uv_loop_t *loop) {
    uv_walk(loop, close_walk_cb, NULL);
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}

static void
close_signal() {
    for (int i = 0; i < 2; i++) {
        uv_signal_stop(&signals[i].sig);
    }
}

static void
signal_cb(uv_signal_t *handle, int signum) {
    if (signum == SIGINT || signum == SIGQUIT) {
        char *name = signum == SIGINT ? "SIGINT" : "SIGQUIT";
        logger_log(LOG_INFO, "Received %s, scheduling shutdown...", name);

        close_signal();

        if (concurrency > 1) {
            struct server_context *servers = handle->data;
            for (int i = 0; i < concurrency; i++) {
                struct server_context *server = &servers[i];
                uv_async_send(&server->async_handle);
            }

        } else {
            struct resolver_context *res = handle->loop->data;
            resolver_shutdown(res);
            struct server_context *ctx = handle->data;
            uv_close((uv_handle_t *)&ctx->tcp, NULL);
            udprelay_close(ctx);
        }
    }
    if (signum == SIGTERM) {
        logger_log(LOG_INFO, "Received SIGTERM, scheduling shutdown...");
        if (daemon_mode) {
            delete_pidfile(pidfile);
        }
        exit(0);
    }
}

static void
setup_signal(uv_loop_t *loop, uv_signal_cb cb, void *data) {
    signals[0].signum = SIGINT;
    signals[1].signum = SIGQUIT;
    signals[2].signum = SIGTERM;
    for (int i = 0; i < 2; i++) {
        signals[i].sig.data = data;
        uv_signal_init(loop, &signals[i].sig);
        uv_signal_start(&signals[i].sig, cb, signals[i].signum);
    }
}

static void
init(void) {
    logger_init(daemon_mode);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGABRT, SIG_IGN);

    if (crypto_init(password)) {
        logger_log(LOG_ERR, "crypto init failed");
        exit(1);
    }

    resolver_prepare(nameserver_num);

    if (idle_timeout == 0) {
        idle_timeout = 60 * 1000;
    }
}

int
main(int argc, char *argv[]) {
    int rc;
    uv_loop_t *loop;
    struct sockaddr local_addr;

    parse_opts(argc, argv);

    if (xsignal) {
        return signal_process(xsignal, pidfile);
    }

    if (!password) {
        print_usage(argv[0]);
        return 1;
    }

    init();

    if (daemon_mode) {
        if (daemonize()) {
            return 1;
        }
        if (already_running(pidfile)) {
            logger_stderr("xsocksd already running.");
            return 1;
        }
    }

    loop = uv_default_loop();

    rc = resolve_addr(local_addrbuf, &local_addr);
    if (rc) {
        logger_stderr("invalid local address");
        return 1;
    }

    udprelay_init();

    if (concurrency <= 1) {
        struct server_context ctx;
        ctx.local_addr = &local_addr;
        ctx.udprelay = 1;
        ctx.udp_fd = create_socket(SOCK_DGRAM, 0);

        uv_tcp_init(loop, &ctx.tcp);
        rc = uv_tcp_bind(&ctx.tcp, &local_addr, 0);
        if (rc) {
            logger_stderr("tcp bind error: %s", uv_strerror(rc));
            return 1;
        }

        rc = uv_listen((uv_stream_t*)&ctx.tcp, 128, client_accept_cb);
        if (rc == 0) {
            logger_log(LOG_INFO, "listening on %s", local_addrbuf);

            setup_signal(loop, signal_cb, &ctx);

            struct resolver_context *res = resolver_init(loop, MODE_IPV4,
              nameserver_num == 0 ? NULL : nameservers, nameserver_num);
            loop->data = res;

            udprelay_start(loop, &ctx);

            uv_run(loop, UV_RUN_DEFAULT);

            close_loop(loop);

            resolver_destroy(res);

        } else {
            logger_stderr("listen error: %s", uv_strerror(rc));
        }

    } else {
        struct server_context *servers = calloc(concurrency, sizeof(servers[0]));
        for (int i = 0; i < concurrency; i++) {
            struct server_context *ctx = servers + i;
            ctx->index = i;
            ctx->tcp_fd = create_socket(SOCK_STREAM, 1);
            ctx->udp_fd = create_socket(SOCK_DGRAM, 1);
            ctx->udprelay = 1;
            ctx->accept_cb = client_accept_cb;
            ctx->nameservers = nameservers;
            ctx->nameserver_num = nameserver_num;
            ctx->local_addr = &local_addr;
            rc = uv_sem_init(&ctx->semaphore, 0);
            rc = uv_thread_create(&ctx->thread_id, consumer_start, ctx);
        }

        logger_log(LOG_INFO, "listening on %s", local_addrbuf);

        setup_signal(loop, signal_cb, servers);

        uv_run(loop, UV_RUN_DEFAULT);

        close_loop(loop);

        for (int i = 0; i < concurrency; i++) {
            uv_sem_wait(&servers[i].semaphore);
        }
        free(servers);
    }

    udprelay_destroy();

    if (daemon_mode) {
        delete_pidfile(pidfile);
    }

    return 0;
}

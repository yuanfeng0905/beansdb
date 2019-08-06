/*
 *  Beansdb - A high available distributed key-value storage system:
 *
 *      http://beansdb.googlecode.com
 *
 *  Copyright 2009 Douban Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Davies Liu <davies.liu@gmail.com>
 *      Hurricane Lee <hurricane1026@gmail.com>
 */

#include "beansdb.h"
#include "hstore.h"
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>

/* need this to get IOV_MAX on some platforms. */
#ifndef __need_IOV_MAX
#    define __need_IOV_MAX
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>  // for strerror()
#include <sys/mman.h>
#include <time.h>

/* FreeBSD 4.x doesn't have IOV_MAX exposed. */
#ifndef IOV_MAX
#    if defined(__FreeBSD__) || defined(__APPLE__)
#        define IOV_MAX 1024
#    endif
#endif

#ifndef IOV_MAX
#    define IOV_MAX 1024
#endif

#ifndef CLOCK_MONOTONIC
#    include "clock_gettime_stub.c"
#endif

/*
 * forward declarations
 */
static int new_socket(struct addrinfo* ai);
static int server_socket(const int port, const bool is_udp);
static int try_read_command(conn* c);
static int try_read_network(conn* c);

/* stats */
static void stats_reset(void);
static void stats_init(void);

/* event handling, network IO */
static void conn_init(void);
static void accept_new_conns(const bool do_accept);
static bool update_event(conn* c, const int new_flags);
static void complete_nread(conn* c);
static void process_command(conn* c, char* command);
static int  transmit(conn* c);
static int  ensure_iov_space(conn* c);
static int  add_iov(conn* c, const void* buf, int len);
static int  add_msghdr(conn* c);
static void conn_free(conn* c);

/** exported globals **/
struct stats stats;

HStore* store  = NULL;
int     stopme = 0;

/** file scope variables **/
static int stub_fd = 0;

#define TRANSMIT_COMPLETE 0
#define TRANSMIT_INCOMPLETE 1
#define TRANSMIT_SOFT_ERROR 2
#define TRANSMIT_HARD_ERROR 3

static void stats_init(void)
{
    stats.curr_conns = stats.total_conns = stats.conn_structs = 0;
    stats.get_cmds = stats.set_cmds = stats.delete_cmds = 0;
    stats.slow_cmds = stats.get_hits = stats.get_misses = 0;
    stats.bytes_read = stats.bytes_written = 0;

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    stats.started = time(0) - 2;
}

static void stats_reset(void)
{
    STATS_LOCK();
    stats.total_conns = 0;
    stats.get_cmds = stats.set_cmds = stats.delete_cmds = 0;
    stats.slow_cmds = stats.get_hits = stats.get_misses = 0;
    stats.bytes_read = stats.bytes_written = 0;
    STATS_UNLOCK();
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn* c)
{
    struct msghdr* msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = (struct msghdr*)try_realloc(
            c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (!msg)
            return -1;
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    c->msgbytes = 0;
    c->msgused++;

    return 0;
}

/*
 * Free list management for connections.
 */

static conn** freeconns;
static int    freetotal;
static int    freecurr;

static void conn_init(void)
{
    freetotal = 200;
    freecurr  = 0;
    freeconns = (conn**)safe_malloc(sizeof(conn*) * freetotal);
    return;
}

/*
 * Returns a connection from the freelist, if any. Should call this using
 * conn_from_freelist() for thread safety.
 */
conn* do_conn_from_freelist()
{
    conn* c;

    if (freecurr > 0) {
        c = freeconns[--freecurr];
    }
    else {
        c = NULL;
    }

    return c;
}

/*
 * Adds a connection to the freelist. false = success. Should call this using
 * conn_add_to_freelist() for thread safety.
 */
bool do_conn_add_to_freelist(conn* c)
{
    if (freecurr < freetotal) {
        freeconns[freecurr++] = c;
        return false;
    }
    else {
        /* try to enlarge free connections array */
        conn** new_freeconns =
            (conn**)try_realloc(freeconns, sizeof(conn*) * freetotal * 2);
        if (new_freeconns) {
            freetotal *= 2;
            freeconns             = new_freeconns;
            freeconns[freecurr++] = c;
            return false;
        }
    }
    return true;
}

static void conn_getnameinfo(conn* c)
{
    struct sockaddr_storage addr;
    socklen_t               addrlen = (socklen_t)sizeof(addr);
    if (0 != getpeername(c->sfd, (struct sockaddr*)&addr, &addrlen)) {
        log_debug("getpeername error %s", strerror(errno));
        return;
    }
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (0 != getnameinfo((struct sockaddr*)&addr, addrlen, host, sizeof(host),
                         serv, sizeof(serv), NI_NUMERICSERV))
        return;
    c->remote = (char*)try_malloc(strlen(host) + strlen(serv) + 2);
    sprintf(c->remote, "%s:%s", host, serv);  // safe
}
conn* conn_new(const int sfd, const int init_state, const int read_buffer_size)
{
    conn* c = conn_from_freelist();

    if (NULL == c) {
        if (!(c = (conn*)try_calloc(1, sizeof(conn)))) {
            return NULL;
        }
        c->rbuf = c->wbuf = 0;
        c->ilist          = 0;
        c->iov            = 0;
        c->msglist        = 0;

        c->rsize   = read_buffer_size;
        c->wsize   = DATA_BUFFER_SIZE;
        c->isize   = ITEM_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;

        c->rbuf  = (char*)try_malloc((size_t)c->rsize);
        c->wbuf  = (char*)try_malloc((size_t)c->wsize);
        c->ilist = (item**)try_malloc(sizeof(item*) * c->isize);
        c->iov   = (struct iovec*)try_malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist =
            (struct msghdr*)try_malloc(sizeof(struct msghdr) * c->msgsize);

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
            c->msglist == 0) {
            conn_free(c);
            return NULL;
        }

        STATS_LOCK();
        stats.conn_structs++;
        STATS_UNLOCK();
    }

    if (settings.verbose > 1) {
        if (init_state == conn_listening)
            log_debug("<%d server listening", sfd);
        else
            log_debug("<%d new client connection", sfd);
    }

    c->sfd     = sfd;
    c->state   = init_state;
    c->rlbytes = 0;
    c->rbytes = c->wbytes = 0;
    c->wcurr              = c->wbuf;
    c->rcurr              = c->rbuf;
    c->ritem              = NULL;
    c->icurr              = c->ilist;
    c->ileft              = 0;
    c->iovused            = 0;
    c->msgcurr            = 0;
    c->msgused            = 0;

    c->write_and_go   = conn_read;
    c->write_and_free = 0;
    c->item           = NULL;
    c->noreply        = false;

    c->remote = NULL;
    if (init_state == conn_read)
        conn_getnameinfo(c);

    update_event(c, AE_READABLE);
    if (add_event(sfd, AE_READABLE, c) == -1) {
        if (conn_add_to_freelist(c)) {
            conn_free(c);
        }
        log_error("event_add: %s", strerror(errno));
        return NULL;
    }

    STATS_LOCK();
    stats.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();

    return c;
}

static void conn_cleanup(conn* c)
{
    assert(c != NULL);

    if (c->item) {
        item_free(c->item);
        c->item = 0;
    }

    if (c->ileft != 0) {
        for (; c->ileft > 0; c->ileft--, c->icurr++) {
            item_free(*(c->icurr));
        }
    }

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }
}

/*
 * Frees a connection.
 */
void conn_free(conn* c)
{
    if (c) {
        if (c->msglist)
            free(c->msglist);
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        if (c->ilist)
            free(c->ilist);
        if (c->iov)
            free(c->iov);
        free(c);
    }
}

void conn_close(conn* c)
{
    free(c->remote);
    c->remote = NULL;
    assert(c != NULL);

    if (settings.verbose > 1)
        log_debug("<%d connection closed.", c->sfd);

    delete_event(c->sfd);
    close(c->sfd);
    c->sfd = -1;
    update_event(c, 0);
    conn_cleanup(c);

    /* if the connection has big buffers, just free it */
    if (c->rsize > READ_BUFFER_HIGHWAT || conn_add_to_freelist(c)) {
        conn_free(c);
    }

    STATS_LOCK();
    stats.curr_conns--;
    STATS_UNLOCK();

    return;
}

/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn* c)
{
    assert(c != NULL);

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE) {
        char* newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);

        newbuf = (char*)try_realloc((void*)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf) {
            c->rbuf  = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ITEM_LIST_HIGHWAT) {
        item** newbuf = (item**)try_realloc(
            (void*)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]));
        if (newbuf) {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
        /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        struct msghdr* newbuf = (struct msghdr*)try_realloc(
            (void*)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
        /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT) {
        struct iovec* newbuf = (struct iovec*)try_realloc(
            (void*)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf) {
            c->iov     = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
        /* TODO check return value */
    }
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn* c, int state)
{
    assert(c != NULL);

    if (state != c->state) {
        if (state == conn_read) {
            conn_shrink(c);
        }
        c->state = state;
    }
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn* c)
{
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int           i, iovnum;
        struct iovec* new_iov = (struct iovec*)try_realloc(
            c->iov, (c->iovsize * 2) * sizeof(struct iovec));
        if (!new_iov)
            return -1;
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}

/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */

static int add_iov(conn* c, const void* buf, int len)
{
    struct msghdr* m;
    int            leftover;
    bool           limit_to_mtu;

    assert(c != NULL);

    do {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit the first payloads of TCP replies, to
         * MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= MAX_PAYLOAD_SIZE)) {
            if (add_msghdr(c))
                return -1;
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > MAX_PAYLOAD_SIZE) {
            leftover = len + c->msgbytes - MAX_PAYLOAD_SIZE;
            len -= leftover;
        }
        else {
            leftover = 0;
        }

        m                                  = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void*)buf;
        m->msg_iov[m->msg_iovlen].iov_len  = len;

        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char*)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return 0;
}

static void out_string(conn* c, const char* str)
{
    size_t len;

    assert(c != NULL);

    if (c->noreply) {
        if (settings.verbose > 1)
            log_debug(">%d %s", c->sfd, str);
        c->noreply = false;
        conn_set_state(c, conn_read);
        return;
    }

    len = strlen(str);
    if (len + 2 > (unsigned int)(c->wsize)) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    safe_memcpy(c->wbuf, c->wsize, str, len);
    safe_memcpy(c->wbuf + len, c->wsize - len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr  = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_read;
    return;
}

/*
 * we get here after reading the value in set/add/replace commands. The command
 * has been stored in c->item_comm, and the item is ready in c->item.
 */

static void complete_nread(conn* c)
{
    assert(c != NULL);

    item* it   = (item*)c->item;
    int   comm = c->item_comm;
    int   ret;

    STATS_LOCK();
    stats.set_cmds++;
    STATS_UNLOCK();

    if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) != 0) {
        out_string(c, "CLIENT_ERROR bad data chunk");
    }
    else {
        ret = store_item(it, comm);
        if (ret == 1)
            out_string(c, "STORED");
        else if (ret == 2)
            out_string(c, "EXISTS");
        else if (ret == 3)
            out_string(c, "NOT_FOUND");
        else
            out_string(c, "NOT_STORED");
    }

    item_free(c->item);
    c->item = 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns true if the item was stored.
 */
int store_item(item* it, int comm)
{
    char* key = ITEM_key(it);

    switch (comm) {
        case NREAD_SET:
            return hs_set(store, key, ITEM_data(it), (size_t)(it->nbytes - 2),
                          it->flag, it->ver);
        case NREAD_APPEND:
            return hs_append(store, key, ITEM_data(it), it->nbytes - 2);
    }
    return 0;
}

/*
 * adds a delta value to a numeric item.
 */
int add_delta(char* key, size_t nkey, int64_t delta, char* buf)
{
    uint64_t value = hs_incr(store, key, delta);
    safe_snprintf(buf, INCR_MAX_STORAGE_LEN, "%llu", (unsigned long long)value);
    return 0;
}

typedef struct token_s
{
    char*  value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t
tokenize_command(char* command, token_t* tokens, const size_t max_tokens)
{
    char * s, *e;
    size_t ntokens = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    for (s = e = command; ntokens < max_tokens - 1; ++e) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value  = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
            }
            s = e + 1;
        }
        else if (*e == '\0') {
            if (s != e) {
                tokens[ntokens].value  = s;
                tokens[ntokens].length = e - s;
                ntokens++;
            }

            break; /* string end */
        }
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value  = (*e == '\0' ? NULL : e);
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn* c, char* buf, int bytes)
{
    if (buf) {
        c->write_and_free = buf;
        c->wcurr          = buf;
        c->wbytes         = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_read;
    }
    else {
        out_string(c, "SERVER_ERROR out of memory writing stats");
    }
}

static inline bool set_noreply_maybe(conn* c, token_t* tokens, size_t ntokens)
{
    int noreply_index = ntokens - 2;

    /*
      NOTE: this function is not the first place where we are going to
      send the reply.  We could send it instead from process_command()
      if the request line has wrong number of tokens.  However parsing
      malformed line for "noreply" option is not reliable anyway, so
      it can't be helped.
    */
    if (tokens[noreply_index].value &&
        strcmp(tokens[noreply_index].value, "noreply") == 0) {
        c->noreply = true;
    }
    return c->noreply;
}

uint64_t get_maxrss()
{
    uint64_t vm, rss;
    FILE*    f = fopen("/proc/self/statm", "r");
    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%" PRIu64 " %" PRIu64 "", &vm, &rss) != 2) {
        rss = 0;
    }
    fclose(f);
    return rss * getpagesize();
}

static void process_stat(conn* c, token_t* tokens, const size_t ntokens)
{
    time_t now = time(0);
    char*  command;
    char*  subcommand;

    assert(c != NULL);

    if (ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    command = tokens[COMMAND_TOKEN].value;

    if (ntokens == 2 && strcmp(command, "stats") == 0) {
        char     temp[1024];
        pid_t    pid   = getpid();
        uint64_t total = 0, curr = 0, avail_space, total_space;
        total = hs_count(store, &curr);
        hs_stat(store, &total_space, &avail_space);
        char* pos = temp;

#ifndef WIN32
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
#endif /* !WIN32 */

        STATS_LOCK();
        pos += safe_snprintf(pos, temp + 1024 - pos, "STAT pid %ld\r\n",
                             (long)pid);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT uptime %" PRIuS "\r\n", now - stats.started);
        pos += safe_snprintf(pos, temp + 1024 - pos, "STAT time %" PRIuS "\r\n",
                             now);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT version " VERSION "\r\n");
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT pointer_size %" PRIuS "\r\n",
                             8 * sizeof(void*));
#ifndef WIN32
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT rusage_user %ld.%06ld\r\n",
                             usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT rusage_system %ld.%06ld\r\n",
                             usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
#endif /* !WIN32 */
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT rusage_maxrss %" PRIu64 "\r\n",
                             get_maxrss() / 1024);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT item_buf_size %" PRIuS "\r\n",
                             settings.item_buf_size);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT curr_connections %" PRIu32 "\r\n",
                             stats.curr_conns - 1); /* ignore listening conn */
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT total_connections %" PRIu32 "\r\n",
                             stats.total_conns);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT connection_structures %" PRIu32 "\r\n",
                             stats.conn_structs);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT cmd_get %" PRIu64 "\r\n", stats.get_cmds);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT cmd_set %" PRIu64 "\r\n", stats.set_cmds);
        pos +=
            safe_snprintf(pos, temp + 1024 - pos,
                          "STAT cmd_delete %" PRIu64 "\r\n", stats.delete_cmds);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT slow_cmd %" PRIu64 "\r\n", stats.slow_cmds);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT get_hits %" PRIu64 "\r\n", stats.get_hits);
        pos +=
            safe_snprintf(pos, temp + 1024 - pos,
                          "STAT get_misses %" PRIu64 "\r\n", stats.get_misses);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT curr_items %" PRIu64 "\r\n", curr);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT total_items %" PRIu64 "\r\n", total);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT avail_space %" PRIu64 "\r\n", avail_space);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT total_space %" PRIu64 "\r\n", total_space);
        pos +=
            safe_snprintf(pos, temp + 1024 - pos,
                          "STAT bytes_read %" PRIu64 "\r\n", stats.bytes_read);
        pos += safe_snprintf(pos, temp + 1024 - pos,
                             "STAT bytes_written %" PRIu64 "\r\n",
                             stats.bytes_written);
        pos += safe_snprintf(pos, temp + 1024 - pos, "STAT threads %d\r\n",
                             settings.num_threads);
        pos += safe_snprintf(pos, temp + 1024 - pos, "END");
        STATS_UNLOCK();
        out_string(c, temp);
        return;
    }

    subcommand = tokens[SUBCOMMAND_TOKEN].value;

    if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET");
        return;
    }

    out_string(c, "ERROR");
}

/* ntokens is overwritten here... shrug.. */
static inline void process_get_command(conn* c, token_t* tokens, size_t ntokens)
{
    char*    key;
    size_t   nkey;
    int      i                = 0;
    item*    it               = NULL;
    token_t* key_token        = &tokens[KEY_TOKEN];
    int      stats_get_cmds   = 0;
    int      stats_get_hits   = 0;
    int      stats_get_misses = 0;
    assert(c != NULL);

    do {
        while (key_token->length != 0) {
            key  = key_token->value;
            nkey = key_token->length;

            if (nkey > MAX_KEY_LEN) {
                STATS_LOCK();
                stats.get_cmds += stats_get_cmds;
                stats.get_hits += stats_get_hits;
                stats.get_misses += stats_get_misses;
                STATS_UNLOCK();
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }

            stats_get_cmds++;

            it = item_get(key, nkey);

            if (it) {
                if (i >= c->isize) {
                    item** new_list = (item**)try_realloc(
                        c->ilist, sizeof(item*) * c->isize * 2);
                    if (new_list) {
                        c->isize *= 2;
                        c->ilist = new_list;
                    }
                    else {
                        item_free(it);
                        it = NULL;
                        break;
                    }
                }

                /*
                 * Construct the response. Each hit adds three elements to the
                 * outgoing data list:
                 *   "VALUE "
                 *   key
                 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
                 */

                if (add_iov(c, "VALUE ", 6) != 0 ||
                    add_iov(c, ITEM_key(it), it->nkey) != 0 ||
                    add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) !=
                        0) {
                    item_free(it);
                    it = NULL;
                    break;
                }

                if (settings.verbose > 1)
                    log_debug(">%d sending key %s", c->sfd, ITEM_key(it));

                stats_get_hits++;
                *(c->ilist + i) = it;
                i++;
            }
            else {
                stats_get_misses++;
            }

            key_token++;
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens.
         */
        if (key_token->value != NULL) {
            ntokens   = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
        }

    } while (key_token->value != NULL);

    c->icurr = c->ilist;
    c->ileft = i;

    if (settings.verbose > 1)
        log_debug(">%d END", c->sfd);

    /*
        If the loop was terminated because of out-of-memory, it is not
        reliable to add END\r\n to the buffer, because it might not end
        in \r\n. So we send SERVER_ERROR instead.
    */
    if (key_token->value != NULL || add_iov(c, "END\r\n", 5) != 0) {
        out_string(c, "SERVER_ERROR out of memory writing get response");
    }
    else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }

    STATS_LOCK();
    stats.get_cmds += stats_get_cmds;
    stats.get_hits += stats_get_hits;
    stats.get_misses += stats_get_misses;
    STATS_UNLOCK();

    return;
}

static void
process_update_command(conn* c, token_t* tokens, const size_t ntokens, int comm)
{
    char*  key;
    size_t nkey;
    int    flags;
    time_t exptime;
    int    vlen;
    item*  it = NULL;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > MAX_KEY_LEN) {
        out_string(c, "CLIENT_ERROR bad command line format");
        log_warn("CLIENT_ERROR key %s too long", tokens[KEY_TOKEN].value);
        return;
    }

    key  = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    errno   = 0;
    flags   = strtoul(tokens[2].value, NULL, 10);
    exptime = strtol(tokens[3].value, NULL, 10);
    vlen    = strtol(tokens[4].value, NULL, 10);

    if (errno == ERANGE || ((flags == 0 || exptime == 0) && errno == EINVAL) ||
        vlen < 0) {
        out_string(c, "CLIENT_ERROR bad command line format");
        log_warn("CLIENT_ERROR %s %s %s %s %s", tokens[0].value,
                 tokens[1].value, tokens[2].value, tokens[3].value,
                 tokens[4].value);
        return;
    }

    it       = item_alloc1(key, nkey, flags, vlen + 2);
    it->ver  = exptime;
    it->flag = flags;

    if (it == NULL) {
        out_string(c, "SERVER_ERROR out of memory storing object");
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        c->sbytes       = vlen + 2;
        return;
    }

    c->item      = it;
    c->ritem     = ITEM_data(it);
    c->rlbytes   = it->nbytes;
    c->item_comm = comm;
    conn_set_state(c, conn_nread);
}

bool safe_strtoull(const char* str, uint64_t* out)
{
    assert(out != NULL);
    errno = 0;
    *out  = 0;
    char*              endptr;
    unsigned long long ull = strtoull(str, &endptr, 10);
    if (errno == ERANGE)
        return false;
    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        *out = ull;
        return true;
    }
    return false;
}

static void process_arithmetic_command(conn*        c,
                                       token_t*     tokens,
                                       const size_t ntokens,
                                       const bool   incr)
{
    char     temp[INCR_MAX_STORAGE_LEN];
    uint64_t delta;
    char*    key;
    size_t   nkey;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    STATS_LOCK();
    stats.set_cmds++;
    STATS_UNLOCK();

    if (tokens[KEY_TOKEN].length > MAX_KEY_LEN) {
        out_string(c, "CLIENT_ERROR bad command line format");
        log_warn("CLIENT_ERROR key %s too long", tokens[KEY_TOKEN].value);
        return;
    }

    key  = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtoull(tokens[2].value, &delta)) {
        out_string(c, "CLIENT_ERROR invalid numeric delta argument");
        log_warn("CLIENT_ERROR invalid numeric delta argument %s",
                 tokens[2].value);
        return;
    }

    switch (add_delta(key, nkey, delta, temp)) {
        case 0:
            out_string(c, temp);
            break;
            //    case NON_NUMERIC:
            //        out_string(c, "CLIENT_ERROR cannot increment or decrement
            //        non-numeric value"); break;
            //    case EOM:
            //        out_string(c, "SERVER_ERROR out of memory");
            //        break;
    }
}

static void
process_delete_command(conn* c, token_t* tokens, const size_t ntokens)
{
    char*  key;
    size_t nkey;
    int    ret;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    STATS_LOCK();
    stats.delete_cmds++;
    STATS_UNLOCK();

    key  = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;
    if (nkey > MAX_KEY_LEN) {
        out_string(c, "CLIENT_ERROR bad command line format");
        log_warn("CLIENT_ERROR key %s too long", tokens[KEY_TOKEN].value);
        return;
    }

    out_string(c, hs_delete(store, key) ? "DELETED" : "NOT_FOUND");
}

static void
process_verbosity_command(conn* c, token_t* tokens, const size_t ntokens)
{
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    errno = 0;
    level = strtoul(tokens[1].value, NULL, 10);
    if (errno == ERANGE) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    settings.verbose =
        level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

static void process_command(conn* c, char* command)
{
    token_t         tokens[MAX_TOKENS];
    size_t          ntokens;
    int             comm;
    struct timespec start, end;

    assert(c != NULL);

    if (settings.verbose > 1)
        log_debug("<%d %s", c->sfd, command);

    /*
     * for commands set/add/replace, we build an item and read the data
     * directly into it, then continue in nread_complete().
     */

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        out_string(c, "SERVER_ERROR out of memory preparing response");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    char command0[MAX_KEY_LEN * 2];
    strncpy(command0, command, MAX_KEY_LEN * 2);

    ntokens = tokenize_command(command, tokens, MAX_TOKENS);
    if (ntokens >= 3 && (strcmp(tokens[COMMAND_TOKEN].value, "get") == 0)) {
        process_get_command(c, tokens, ntokens);
    }
    else if ((ntokens == 6 || ntokens == 7) &&
             ((strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 &&
               (comm = NREAD_SET)) ||
              (strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 &&
               (comm = NREAD_APPEND))))

    {
        process_update_command(c, tokens, ntokens, comm);
    }
    else if ((ntokens == 4 || ntokens == 5) &&
             (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)) {
        process_arithmetic_command(c, tokens, ntokens, 1);
    }
    else if (ntokens >= 3 && ntokens <= 4 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {
        process_delete_command(c, tokens, ntokens);
    }
    else if (ntokens >= 2 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {
        process_stat(c, tokens, ntokens);
    }
    else if (ntokens == 2 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {
        out_string(c, "VERSION " VERSION);
    }
    else if (ntokens == 2 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {
        conn_set_state(c, conn_closing);
    }
    else if (ntokens == 3 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {
        process_verbosity_command(c, tokens, ntokens);
    }
    else if (ntokens == 2 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "optimize_stat") == 0)) {
        int ret = hs_optimize_stat(store);
        if (ret >= 0) {
            char buf[100];
            sprintf(buf, "running bitcast 0x%x", ret);
            out_string(c, buf);
        }
        else if (ret == -1)
            out_string(c, "success");
        else
            out_string(c, "fail");
    }
    else if (ntokens >= 2 && ntokens <= 4 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
        set_noreply_maybe(c, tokens, ntokens);
        ntokens -= (c->noreply ? 1 : 0);

        long  limit = 10000;
        char* tree  = "@";

        if (ntokens >= 3) {
            if (!safe_strtol(tokens[1].value, 10, &limit)) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            if (ntokens >= 4) {
                tree = tokens[2].value;
            }
        }
        int ret = hs_optimize(store, limit, tree);
        if (ret == 0)
            out_string(c, "OK");
        else if (ret == -1)
            out_string(c, "ERROR READ_ONLY");
        else if (ret == -2)
            out_string(c, "ERROR OPTIMIZE_RUNNING");
        else if (ret == -3)
            out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    else if (stopme && ntokens == 2 &&
             (strcmp(tokens[COMMAND_TOKEN].value, "stopme") == 0)) {
        log_warn("quit under request");
        daemon_quit = 1;
    }
    else {
        out_string(c, "ERROR");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    float secs =
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    if (secs > settings.slow_cmd_time) {
        STATS_LOCK();
        ++stats.slow_cmds;
        STATS_UNLOCK();
    }

    // access logging
    if (ntokens >= 3) {
        log_info("%s\t%s\t%.3f", c->remote, command0, secs * 1000);
    }

    return;
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn* c)
{
    char *el, *cont;

    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));

    if (c->rbytes == 0)
        return 0;
    el = memchr(c->rcurr, '\n', c->rbytes);
    if (!el)
        return 0;
    cont = el + 1;
    if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
        el--;
    }
    *el = '\0';

    assert(cont <= (c->rcurr + c->rbytes));

    process_command(c, c->rcurr);

    c->rbytes -= (cont - c->rcurr);
    c->rcurr = cont;

    assert(c->rcurr <= (c->rbuf + c->rsize));

    return 1;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 * return 0 if there's nothing to read on the first read.
 */
static int try_read_network(conn* c)
{
    int gotdata = 0;
    int res;

    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        if (c->rbytes >= c->rsize) {
            char* new_rbuf = (char*)try_realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                if (settings.verbose > 0)
                    log_error("Couldn't _realloc input buffer");
                c->rbytes = 0; /* ignore what we read */
                out_string(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return 1;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res       = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            STATS_LOCK();
            stats.bytes_read += res;
            STATS_UNLOCK();
            gotdata = 1;
            c->rbytes += res;
            if (res == avail) {
                continue;
            }
            else {
                break;
            }
        }
        if (res == 0) {
            /* connection closed */
            conn_set_state(c, conn_closing);
            return 1;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            /* Should close on unhandled errors. */
            conn_set_state(c, conn_closing);
            return 1;
        }
    }
    return gotdata;
}

static bool update_event(conn* c, const int new_flags)
{
    c->ev_flags = new_flags;
    return true;
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static int transmit(conn* c)
{
    assert(c != NULL);

    if (c->msgcurr < c->msgused && c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t        res;
        struct msghdr* m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        if (res > 0) {
            STATS_LOCK();
            stats.bytes_written += res;
            STATS_UNLOCK();

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base += res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            update_event(c, AE_WRITABLE);
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res==0 or res==-1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0)
            log_debug("Failed to write, and not due to blocking: %s",
                      strerror(errno));

        conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    }
    else {
        return TRANSMIT_COMPLETE;
    }
}

/*
 * return 0 after close connection.
 */
int drive_machine(conn* c)
{
    bool                    stop = false;
    int                     sfd, flags = 1;
    socklen_t               addrlen;
    struct sockaddr_storage addr;
    int                     res;

    assert(c != NULL);

    while (!stop) {
        switch (c->state) {
            case conn_listening:
                addrlen = sizeof(addr);
                if ((sfd = accept(c->sfd, (struct sockaddr*)&addr, &addrlen)) ==
                    -1) {
                    stop = true;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* these are transient, so don't log anything */
                    }
                    else if (errno == EMFILE) {
                        if (settings.verbose > 0)
                            log_debug("Too many open connections");
                        if (stub_fd > 0) {
                            close(stub_fd);
                            if ((sfd = accept(c->sfd, (struct sockaddr*)&addr,
                                              &addrlen)) != -1) {
                                close(sfd);
                            }
                            else {
                                log_error("Too many open connections");
                            }
                            stub_fd = open("/dev/null", O_RDONLY);
                        }
                    }
                    else {
                        log_error("accept(): %s", strerror(errno));
                    }
                    if (stop)
                        break;
                }
                if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
                    fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                    log_error("setting O_NONBLOCK: %s", strerror(errno));
                    close(sfd);
                    break;
                }
                if (NULL == conn_new(sfd, conn_read, DATA_BUFFER_SIZE)) {
                    if (settings.verbose > 0) {
                        log_error("Can't listen for events on fd %d", sfd);
                    }
                    close(sfd);
                }
                break;

            case conn_read:
                if (try_read_command(c) != 0) {
                    continue;
                }
                if (try_read_network(c) != 0) {
                    continue;
                }
                /* we have no command line and no data to read from network */
                update_event(c, AE_READABLE);
                stop = true;
                break;

            case conn_nread:
                /* we are reading rlbytes into ritem; */
                if (c->rlbytes == 0) {
                    complete_nread(c);
                    break;
                }
                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0) {
                    int tocopy =
                        c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                    memcpy(
                        c->ritem, c->rcurr,
                        tocopy);  // safe: the buffer size = nbytes == rlbytes.
                    c->ritem += tocopy;
                    c->rlbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    break;
                }

                /*  now try reading from the socket */
                res = read(c->sfd, c->ritem, c->rlbytes);
                if (res > 0) {
                    STATS_LOCK();
                    stats.bytes_read += res;
                    STATS_UNLOCK();
                    c->ritem += res;
                    c->rlbytes -= res;
                    break;
                }
                if (res == 0) /* end of stream */
                {
                    conn_set_state(c, conn_closing);
                    break;
                }
                if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    update_event(c, AE_READABLE);
                    stop = true;
                    break;
                }
                /* otherwise we have a real error, on which we close the
                 * connection */
                if (settings.verbose > 0)
                    log_error("Failed to read, and not due to blocking");
                conn_set_state(c, conn_closing);
                break;

            case conn_swallow:
                /* we are reading sbytes and throwing them away */
                if (c->sbytes == 0) {
                    conn_set_state(c, conn_read);
                    break;
                }

                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0) {
                    int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                    c->sbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    break;
                }

                /*  now try reading from the socket */
                res = read(c->sfd, c->rbuf,
                           c->rsize > c->sbytes ? c->sbytes : c->rsize);
                if (res > 0) {
                    STATS_LOCK();
                    stats.bytes_read += res;
                    STATS_UNLOCK();
                    c->sbytes -= res;
                    break;
                }
                if (res == 0) /* end of stream */
                {
                    conn_set_state(c, conn_closing);
                    break;
                }
                if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    update_event(c, AE_READABLE);
                    stop = true;
                    break;
                }
                /* otherwise we have a real error, on which we close the
                 * connection */
                if (settings.verbose > 0)
                    log_error("Failed to read, and not due to blocking");
                conn_set_state(c, conn_closing);
                break;

            case conn_write:
                /*
                 * We want to write out a simple response. If we haven't
                 * already, assemble it into a msgbuf list (this will be a
                 * single-entry list for TCP or a two-entry list for UDP).
                 */
                if (c->iovused == 0) {
                    if (add_iov(c, c->wcurr, c->wbytes) != 0) {
                        if (settings.verbose > 0)
                            log_error("Couldn't build response");
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }

                /* fall through... */

            case conn_mwrite:
                switch (transmit(c)) {
                    case TRANSMIT_COMPLETE:
                        if (c->state == conn_mwrite) {
                            while (c->ileft > 0) {
                                item* it = *(c->icurr);
                                item_free(it);
                                c->icurr++;
                                c->ileft--;
                            }
                            conn_set_state(c, conn_read);
                        }
                        else if (c->state == conn_write) {
                            if (c->write_and_free) {
                                free(c->write_and_free);
                                c->write_and_free = 0;
                            }
                            conn_set_state(c, c->write_and_go);
                        }
                        else {
                            if (settings.verbose > 0)
                                log_error("Unexpected state %d", c->state);
                            conn_set_state(c, conn_closing);
                        }
                        break;

                    case TRANSMIT_INCOMPLETE:
                    case TRANSMIT_HARD_ERROR:
                        break; /* Continue in state machine. */

                    case TRANSMIT_SOFT_ERROR: stop = true; break;
                }
                break;

            case conn_closing: conn_close(c); return 0;
        }
    }

    return 1;
}

static int new_socket(struct addrinfo* ai)
{
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        log_error("socket(): %s", strerror(errno));
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("setting O_NONBLOCK: %s", strerror(errno));
        close(sfd);
        return -1;
    }
    return sfd;
}

static int server_socket(const int port, const bool is_udp)
{
    int              sfd;
    struct linger    ling = {0, 0};
    struct addrinfo* ai;
    struct addrinfo* next;
    struct addrinfo  hints;
    char             port_buf[NI_MAXSERV];
    int              error;
    int              success = 0;

    int flags = 1;

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags    = AI_PASSIVE | AI_ADDRCONFIG;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    safe_snprintf(port_buf, NI_MAXSERV, "%d", port);
    error = getaddrinfo(settings.inter, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
            log_error("getaddrinfo(): %s\n", gai_strerror(error));
        else
            log_error("getaddrinfo(): %s", strerror(errno));

        return 1;
    }

    for (next = ai; next; next = next->ai_next) {
        conn* listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            freeaddrinfo(ai);
            return 1;
        }

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void*)&flags, sizeof(flags));
        setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void*)&flags, sizeof(flags));
        setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void*)&ling, sizeof(ling));
        setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void*)&flags, sizeof(flags));

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                log_error("bind(): %s", strerror(errno));
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        }
        else {
            success++;
            if (listen(sfd, 1024) == -1) {
                log_error("listen(): %s", strerror(errno));
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
        }

        if (!(listen_conn_add = conn_new(sfd, conn_listening, 1))) {
            log_error("failed to create listening connection");
            exit(EXIT_FAILURE);
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static void usage(void)
{
    printf(PACKAGE " " VERSION "\n");
    printf(
        "-p <num>      TCP port number to listen on (default: 7900)\n"
        "-l <ip_addr>  interface to listen on, default is INDRR_ANY\n"
        "-d            run as a daemon\n"
        "-P <file>     save PID in <file>, only used with -d option\n"
        "-L <file>     zlog config file path, defaults are 1. "
        "\'./beansdb_log.conf\' 2. \'/etc/beansdb_log.conf\'\n"
        "-r            maximize core file limit\n"
        "-u <username> assume identity of <username> (only when run as root)\n"
        "-c <num>      max simultaneous connections, default is 1024\n"
        "-t <num>      number of threads to use (include scanning), default is "
        "16\n"
        "-H <dir>      home of database, default is 'testdb', "
        "multi-dir(splitted by ,:)\n"
        "-T <num>      log of the number of db files(base 16), default is "
        "1(16^1=16)\n"
        "-s <num>      slow command time limit, in ms, default is 100ms\n"
        "-f <num>      flush period(in secs) , default is 600 secs\n"
        "-n <num>      flush limit(in KB), default is 1024 (KB)\n"
        "-m <time>     serve data written before <time> (read-only)\n"
        "-v            verbose (print errors/warnings while in event loop)\n"
        "-vv           very verbose (also print client commands/reponses)\n"
        "-h            print this help and exit\n"
        "-i            print license info\n"
        "-F <num>      max size of a data file(in MB), default and at most "
        "4000(MB), at least 5(MB)\n"
        "-C            check file sizes in startup using buckets.txt for each "
        "bitcask if it exists\n");

    return;
}

static void usage_license(void)
{
    printf(PACKAGE " " VERSION "\n\n");
    printf(
        "Copyright (c) 2009, Douban Inc. <http://www.douban.com/>\n"
        "All rights reserved.\n"
        "\n"
        "Redistribution and use in source and binary forms, with or without\n"
        "modification, are permitted provided that the following conditions "
        "are\n"
        "met:\n"
        "\n"
        "    * Redistributions of source code must retain the above copyright\n"
        "notice, this list of conditions and the following disclaimer.\n"
        "\n"
        "    * Redistributions in binary form must reproduce the above\n"
        "copyright notice, this list of conditions and the following "
        "disclaimer\n"
        "in the documentation and/or other materials provided with the\n"
        "distribution.\n"
        "\n"
        "    * Neither the name of the Douban Inc. nor the names of its\n"
        "contributors may be used to endorse or promote products derived from\n"
        "this software without specific prior written permission.\n"
        "\n"
        "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
        "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
        "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS "
        "FOR\n"
        "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
        "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, "
        "INCIDENTAL,\n"
        "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
        "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF "
        "USE,\n"
        "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON "
        "ANY\n"
        "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
        "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE "
        "USE\n"
        "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
        "\n"
        "\n"
        "This product includes software developed by Douban Inc.\n"
        "\n"
        "[ memcached ]\n"
        "\n"
        "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
        "All rights reserved.\n"
        "\n"
        "Redistribution and use in source and binary forms, with or without\n"
        "modification, are permitted provided that the following conditions "
        "are\n"
        "met:\n"
        "\n"
        "    * Redistributions of source code must retain the above copyright\n"
        "notice, this list of conditions and the following disclaimer.\n"
        "\n"
        "    * Redistributions in binary form must reproduce the above\n"
        "copyright notice, this list of conditions and the following "
        "disclaimer\n"
        "in the documentation and/or other materials provided with the\n"
        "distribution.\n"
        "\n"
        "    * Neither the name of the Danga Interactive nor the names of its\n"
        "contributors may be used to endorse or promote products derived from\n"
        "this software without specific prior written permission.\n"
        "\n"
        "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
        "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
        "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS "
        "FOR\n"
        "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
        "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, "
        "INCIDENTAL,\n"
        "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
        "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF "
        "USE,\n"
        "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON "
        "ANY\n"
        "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
        "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE "
        "USE\n"
        "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH "
        "DAMAGE.\n");

    return;
}

static void save_pid(const pid_t pid, const char* pid_file)
{
    FILE* fp;
    if (pid_file == NULL)
        return;

    if ((fp = fopen(pid_file, "w")) == NULL) {
        log_error("Could not open the pid file %s for writing", pid_file);
        return;
    }

    fprintf(fp, "%ld\n", (long)pid);
    if (fclose(fp) == -1) {
        log_error("Could not close the pid file %s.", pid_file);
        return;
    }
}

static void remove_pidfile(const char* pid_file)
{
    if (pid_file == NULL)
        return;

    if (unlink(pid_file) != 0) {
        log_error("Could not remove the pid file %s.", pid_file);
    }
}

/* for safely exit, make sure to do checkpoint*/
static void sig_handler(const int sig)
{
    if (sig != SIGTERM && sig != SIGQUIT && sig != SIGINT) {
        return;
    }
    if (daemon_quit == 1) {
        return;
    }
    daemon_quit = 1;
    log_warn("Signal(%d) received, try to exit daemon gracefully..", sig);
}

void* do_flush(void* args)
{
    while (!daemon_quit) {
        hs_flush(store, (unsigned int)settings.flush_limit,
                 settings.flush_period);
        sleep(1);
    }
    log_notice("flush thread exit.");
    return NULL;
}

int main(int argc, char** argv)
{
    int            c;
    struct in_addr addr;
    char*          dbhome      = "testdb";
    int            height      = 1;
    time_t         before_time = 0;
    bool           daemonize   = false;
    bool           use_before  = false;
    int            maxcore     = 0;
    char*          username    = NULL;
    char*          pid_file    = NULL;
    char*          conf_path   = NULL;
    /*
     *FILE *log_file = NULL;
     */
    struct passwd*   pw;
    struct sigaction sa;
    struct rlimit    rlim;
    bool             invalid_arg = false;

    char  buf[]   = "2000-01-01-00:00:00";
    char  fmt[]   = "%Y-%m-%d-%H:%M:%S";
    char* portstr = NULL;

    /* init settings */
    settings_init();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    /* process arguments */
    while ((c = getopt(argc, argv, "p:c:hivl:dru:P:L:t:b:H:T:m:s:f:n:SF:CA")) !=
           -1) {
        switch (c) {
            case 'p': settings.port = atoi(optarg); break;
            case 'c': settings.maxconns = atoi(optarg); break;
            case 'h': usage(); exit(EXIT_SUCCESS);
            case 'i': usage_license(); exit(EXIT_SUCCESS);
            case 'v': settings.verbose++; break;
            case 'l': settings.inter = strdup(optarg); break;
            case 'd': daemonize = true; break;
            case 'r': maxcore = 1; break;
            case 'u': username = optarg; break;
            case 'P': pid_file = optarg; break;
            case 'L': conf_path = optarg; break;
            case 't': settings.num_threads = atoi(optarg); break;
            case 'b': settings.item_buf_size = atoi(optarg); break;
            case 'H': dbhome = optarg; break;
            case 'T': height = atoi(optarg); break;
            case 's': settings.slow_cmd_time = atoi(optarg) / 1000.0; break;
            case 'f': settings.flush_period = atoi(optarg); break;
            case 'n': settings.flush_limit = atoi(optarg); break;
            case 'm': {
                safe_memcpy(buf, sizeof(buf), optarg, strlen(optarg));
                use_before = true;
                break;
            }
            case 'S': stopme = 1; break;
            case 'F':
                settings.max_bucket_size = (uint32_t)atoll(optarg);
                if (settings.max_bucket_size < 5 ||
                    settings.max_bucket_size > 4000) {
                    printf("-F <num>,  5 <= num <= 4000\n");
                    exit(EXIT_FAILURE);
                }
                settings.max_bucket_size *= (1024 * 1024);
                break;
            case 'C': settings.check_file_size = true; break;
            default: invalid_arg = true;
        }
    }

    const char* default_log_confs[] = {"./beansdb_log.conf",
                                       "/etc/beansdb_log.conf"};
    if (!conf_path) {
        int i;
        for (i = 0; i < sizeof(default_log_confs) / sizeof(char*); i++) {
            const char* path = default_log_confs[i];
            struct stat st;
            if (stat(path, &st) == 0) {
                conf_path = (char*)path;
                break;
            }
        }
    }
    if (!conf_path) {
        printf("FATAL: must specify log config file!\n");
        exit(EXIT_FAILURE);
    }

    printf("logging according to %s\n", conf_path);
    if (0 != log_init(conf_path)) {
        exit(EXIT_FAILURE);
    }
    log_notice("ZLOG inited");

    if (invalid_arg) {
        log_fatal("Illegal argument \"%c\"", c);
        exit(EXIT_FAILURE);
    }
    if (stopme)
        log_warn("dangerous: it can been stopped by command 'stopme'");
    if (settings.num_threads <= 0) {
        log_fatal("Number of threads must be greater than 0");
        exit(EXIT_FAILURE);
    }
    if (settings.item_buf_size < 512) {
        log_fatal("item buf size must be larger than 512 bytes");
        exit(EXIT_FAILURE);
    }
    if (settings.item_buf_size > 256 * 1024) {
        log_warn("Warning: item buffer size(-b) larger than 256KB may cause "
                 "performance issue");
    }
    if (use_before) {
        struct tm tb;
        if (strptime(buf, fmt, &tb) != 0) {
            before_time = timelocal(&tb);
        }
        else {
            log_fatal("invalid time:%s, need:%s", optarg, fmt);
            exit(EXIT_FAILURE);
        }
    }

    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new) != 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            log_fatal("failed to ensure corefile creation");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        log_fatal("failed to getrlimit number of files");
        exit(EXIT_FAILURE);
    }
    else {
        int maxfiles = settings.maxconns;
        if (rlim.rlim_cur < maxfiles)
            rlim.rlim_cur = maxfiles + 3;
        if (rlim.rlim_max < rlim.rlim_cur)
            rlim.rlim_max = rlim.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            log_fatal("failed to set rlimit for open files. Try running as "
                      "root or requesting smaller maxconns value.");
            exit(EXIT_FAILURE);
        }
    }

    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (daemonize) {
        int res;
        res = daemon(1, settings.verbose);
        if (res == -1) {
            log_error("failed to daemon() in order to daemonize");
            return 1;
        }
    }

    /* save the PID in if we're a daemon, do this after thread_init due to
       a file descriptor handling bug somewhere in libevent */
    if (daemonize)
        save_pid(getpid(), pid_file);

    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            log_error("can't run as root without the -u switch");
            return 1;
        }
        if ((pw = getpwnam(username)) == 0) {
            log_error("can't find the user %s to switch to", username);
            return 1;
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            log_error("failed to assume identity of user %s", username);
            return 1;
        }
    }

    /* initialize other stuff */
    item_init();
    stats_init();
    conn_init();

    /*
     * ignore SIGPIPE signals; we can use errno==EPIPE if we
     * need that information
     */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) {
        log_error("failed to ignore SIGPIPE; sigaction : %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* open db */
    store = hs_open(dbhome, height, before_time, settings.num_threads);
    if (!store) {
        log_error("failed to open db %s", dbhome);
        exit(1);
    }

    if ((stub_fd = open("/dev/null", O_RDONLY)) == -1) {
        log_error("open stub file failed: %s", strerror(errno));
        exit(1);
    }
    thread_init(settings.num_threads);

    /* create the listening socket, bind it, and init */
    if (server_socket(settings.port, false)) {
        log_fatal("failed to listen");
        exit(EXIT_FAILURE);
    }

    /* register signal callback */
    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        log_error("can not catch SIGTERM");
    if (signal(SIGQUIT, sig_handler) == SIG_ERR)
        log_error("can not catch SIGQUIT");
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        log_error("can not catch SIGINT");

    pthread_t flush_id;
    if (pthread_create(&flush_id, NULL, do_flush, NULL) != 0) {
        log_fatal("create flush thread failed");
        exit(1);
    }

    /* enter the event loop */
    printf("all ready.\n");
    log_notice("all ready. rss = %" PRIu64 "", get_maxrss());

    loop_run(settings.num_threads);

    /* wait other thread to ends */
    log_notice("waiting for close, rss = %" PRIu64 "", get_maxrss());
    pthread_join(flush_id, NULL);
    pthread_detach(flush_id);

    hs_close(store);
    log_warn("close done.");
    log_finish();
    /*
     *
     *    if (log_file)
     *    {
     *        fclose(log_file);
     *    }
     *
     */
    /* remove the PID file if we're a daemon */
    if (daemonize)
        remove_pidfile(pid_file);

    return 0;
}

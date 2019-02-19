/**
 * @file miio_agent.c
 * @brief a rpc router, support both local and ot memssage.
 * @author fusichang & rkxie
 * @version 1.2.0
 * @date 2019-01-24
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "mainloop/mainloop.h"
#include "rbtree/rbtree.h"
#include "json-c/json.h"
#include "config.h"
#include "miio_agent.h"

#define OT_SERVER_IP   "127.0.0.1"
#ifndef OT_SERVER_PORT
# define OT_SERVER_PORT 54322
#endif

#ifndef OT_RECV_BUFSIZE
# define OT_RECV_BUFSIZE (4*MIIO_AGENT_MAX_MSG_LEN)
#endif

#define OT_CONN_RETRY_INTERVAL 5000 /* 5s */

struct miio_agent
{
    struct {
        int fd;
        bool is_connected;
        int retry_timer_fd;
        struct rb_root key_tree;
        struct rb_root id_tree;
    } ot_client;

    struct {
        int fd;
        int client_fds[MIIO_AGENT_CLIENT_MAX_NUM];
    } local_server;
};
typedef struct miio_agent* miio_agent_t;

/* --------------------------------------------------------------------- */
typedef enum
{
    LOG_ERROR = 0,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG,
    LOG_VERBOSE,
    LOG_LEVEL_MAX = LOG_VERBOSE
} log_level_t;

static FILE *s_log_file;
static log_level_t s_loglevel = LOG_INFO;

#define log_e(__fmt, ...) log_printf(LOG_ERROR, "%d "__fmt"\n", __LINE__, ##__VA_ARGS__)

/* --------------------------------------------------------------------- logger */
static int logfile_init(char *filename)
{
    FILE *fp;

    fp = fopen(filename, "a");
    if (fp == NULL) {
        return -1;
    }

    s_log_file = fp;

    return 0;
}

static void log_printf(log_level_t level, const char *fmt, ...)
{
    char buf[80];
    time_t now;
    va_list ap;
    struct tm *p;
    char *slevel;

    if (stdout == NULL)
        return;

    if (level <= s_loglevel) {
        switch (level) {
            case LOG_ERROR   : slevel = "[ERROR]"; break;
            case LOG_WARNING : slevel = "[WARNING]"; break;
            case LOG_INFO    : slevel = "[INFO]"; break;
            case LOG_DEBUG   : slevel = "[DEBUG]"; break;
            case LOG_VERBOSE : slevel = "[VERBOSE]"; break;
            default          : slevel = "[UNKNOWN]"; break;
        }

        now = time(NULL);
        p = localtime(&now);
        strftime(buf, 80, "[%Y%m%d %H:%M:%S]", p);

        va_start(ap, fmt);
        fprintf(s_log_file, "%s %s ", buf, slevel);
        vfprintf(s_log_file, fmt, ap);
        va_end(ap);
        fflush(s_log_file);
    }
}


/* --------------------------------------------------------------------- json wrapper */
struct json_parser {
    json_tokener *tok;
    json_object *root_obj;
};
typedef struct json_parser* json_parser_t;

static void json_parser_free(json_parser_t parser);
static json_parser_t json_parser_new(const char *msg, size_t msg_len)
{
    json_parser_t parser = NULL;
    if ((parser = malloc(sizeof(struct json_parser)))) {
        parser->tok = json_tokener_new();
        if(!parser->tok)
            goto _ERROR_JSON_PARSER_NEW;
        parser->root_obj = json_tokener_parse_ex(parser->tok, msg, msg_len);
        if(!parser->root_obj)
            goto _ERROR_JSON_PARSER_NEW;
    }

    return parser;

_ERROR_JSON_PARSER_NEW:
    json_parser_free(parser);
    return NULL;
}

static void json_parser_free(json_parser_t parser)
{
    if (parser) {
        if (parser->root_obj) {
            json_object_put(parser->root_obj);
        }
        if (parser->tok) {
            json_tokener_free(parser->tok);
        }
        free(parser);
    }
}

CC_INLINE
static int json_get_key_value_int32(json_parser_t parser, void *obj_parent, const char *key, int32_t *value)
{
    struct json_object *tmp_obj;

    if(!obj_parent)
        obj_parent = parser->root_obj;

    if (key) { /* parse key */
        if(!json_object_object_get_ex(obj_parent, key, &tmp_obj))
            return -1;

        obj_parent = tmp_obj;
    }

    if (value) {
        if(!json_object_is_type(obj_parent, json_type_int))
            return -2;

        *value =  json_object_get_int(obj_parent);
    }

    return 0;
}

CC_INLINE
static int json_get_key_value_string(json_parser_t parser, void *obj_parent, const char *key, char *val_buf, size_t buf_size)
{
    struct json_object *tmp_obj;

    if(!obj_parent)
        obj_parent = parser->root_obj;

    if (key) { /* parse key */
        if(!json_object_object_get_ex(obj_parent, key, &tmp_obj))
            return -1;

        obj_parent = tmp_obj;
    }

    if (val_buf && buf_size) {
        if(!json_object_is_type(obj_parent, json_type_string))
            return -2;

        strncpy(val_buf, json_object_get_string(obj_parent), buf_size);
    }

    return 0;
}

CC_INLINE
static void json_delete_key(json_parser_t parser, void *obj_parent, const char *key)
{
    if(!obj_parent)
        obj_parent = parser->root_obj;
    json_object_object_del(obj_parent, key);
}

CC_INLINE
static void json_insert_key_value_int32(json_parser_t parser, void *obj_parent, const char *key, int32_t value)
{
    if(!obj_parent)
        obj_parent = parser->root_obj;
    json_object_object_add(obj_parent, key, json_object_new_int(value));
}

CC_INLINE
static const char* json_parser_to_string(json_parser_t parser)
{
    return json_object_to_json_string_ext(parser->root_obj, JSON_C_TO_STRING_PLAIN);
}

/* --------------------------------------------------------------------- tree */
struct key_node {
    struct rb_node node;
    char key[MIIO_AGENT_MAX_KEY_LEN];
    int fd[MIIO_AGENT_CLIENT_MAX_NUM];
};

/**
 * register the interesting key with fd
 */
static int key_insert(struct rb_root *root, const char *key, int fd)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;

    /* Figure out where to put new node */
    while (*new) {
        struct key_node *this = container_of(*new, struct key_node, node);
        int result = strcmp(key, this->key);

        parent = *new;
        if (result < 0)
            new = &((*new)->rb_left);
        else if (result > 0)
            new = &((*new)->rb_right);
        else {
            /* insert fd in exist key node */
            int i = 0;

            while(i < MIIO_AGENT_CLIENT_MAX_NUM - 1 && this->fd[i]) {
                if (fd == this->fd[i]) /* already existed */
                    return -1;
                i++;
            }
            this->fd[i] = fd;
            return 0;
        }
    }

    { /* create new node */
        struct key_node *p;

        p = malloc(sizeof(struct key_node));
        assert(p);
        memset(p, 0, sizeof(struct key_node));
        strncpy(p->key, key, sizeof(p->key));
        p->fd[0] = fd;
        /* Add new node and rebalance tree. */
        rb_link_node(&p->node, parent, new);
        rb_insert_color(&p->node, root);
    }

    return 0;
}

static int key_search(struct rb_root *root, const char *key, struct key_node **pNode)
{
    struct rb_node *node = root->rb_node;

    while (node) {
        struct key_node *this = container_of(node, struct key_node, node);
        int result = strcmp(key, this->key);

        if (result < 0)
            node = node->rb_left;
        else if (result > 0)
            node = node->rb_right;
        else {
            *pNode = this;
            return 0;
        }
    }
    return -1;
}

static void key_tree_free(struct rb_root *root)
{
    struct rb_node *node;
    struct key_node *p;

    node = rb_first(root);
    while (node) {
        p = rb_entry(node, struct key_node, node);
        rb_erase(&p->node, root);
        free(p);
        node = rb_first(root);
    }
}

/**
 * unregister fd from specific key_node
 * return 1 node is empty, removed from tree
 *        0 remove fd, -1 not found
 */
static int remove_fd_from_keynode(struct key_node *pNode, int fd, void *key_tree)
{
    int i, fd_found = 0, ret = -1;
    struct key_node *q = pNode;

    for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM && q->fd[i]; i++) {
        if (fd == q->fd[i]) {
            fd_found = 1;
            break;
        }
    }

    if (fd_found == 1) {
        ret = 0;
        while(i < MIIO_AGENT_CLIENT_MAX_NUM - 1 && q->fd[i]) {
            q->fd[i] = q->fd[i + 1];
            i++;
        }
        q->fd[i] = 0;

        /*the key just has one fd, free it*/
        if (q->fd[0] == 0) {
            //printf("key is %s, fd is %d\n", q->key, fd);
            rb_erase(&q->node, key_tree);
            free(q);
            ret = 1;
        }
    }

    return ret;
}

/**
 * unregister the interesting key with fd
 */
static int remove_key_within_fd(int fd, const char *key, void *key_tree)
{
    struct key_node *p;
    int ret = -1;

    if (0 != key_search(key_tree, key, &p)) {
        log_printf(LOG_WARNING, "Key not found: %s\n", key);
        return ret;
    }

    ret = remove_fd_from_keynode(p, fd, key_tree);
    if (ret < 0) {
        log_printf(LOG_WARNING, "fd %d not found with the key %s\n", fd, key);
    } else {
        ret = 0;
    }

    return ret;
}

/**
 * unregister all interesting key with fd
 */
static void keytree_remove(void *key_tree, int fd)
{
    struct rb_node *node;
    struct key_node *p;
    int ret;

    node = rb_first(key_tree);
    while (node) {
        p = rb_entry(node, struct key_node, node);
        ret = remove_fd_from_keynode(p, fd, key_tree);
        if (ret == 1) {
            node = rb_first(key_tree);
            continue;
        }
        node = rb_next(node);
    }
}

#if 0
static void keytree_print(void *key_tree)
{
    struct rb_node *node;
    struct key_node *p;
    int i = 0;

    log_printf(LOG_DEBUG, "===search all key_tree nodes start===\n");
    for (node = rb_first(key_tree); node; node = rb_next(node)) {
        p = rb_entry(node, struct key_node, node);
        log_printf(LOG_DEBUG,"key = %s\n", p->key);
        for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM && p->fd[i]; i++) {
            log_printf(LOG_DEBUG, "%d, \n", p->fd[i]);
        }
    }
    log_printf(LOG_DEBUG,"============end===========\n");
}
#endif

struct id_node
{
    struct rb_node node;
    int32_t new_id;
    int32_t old_id;
    int fd;
    unsigned int ts;
};

/*
 *record the id map
 */
static int id_insert(struct rb_root *root, struct id_node *pNode)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;

    if (!pNode) {
        return -1;
    }

    /* Figure out where to put new node */
    while (*new) {
        struct id_node *this = container_of(*new, struct id_node, node);
        int result = pNode->new_id - this->new_id;

        parent = *new;
        if (result < 0)
            new = &((*new)->rb_left);
        else if (result > 0)
            new = &((*new)->rb_right);
        else {
            free(pNode); /* id should be unique */
            return -2;
        }
    }

    /* Add new node and rebalance tree. */
    rb_link_node(&pNode->node, parent, new);
    rb_insert_color(&pNode->node, root);
    return 0;
}

static int id_search(struct rb_root *root, int new_id, struct id_node **pNode)
{
    struct rb_node *node = root->rb_node;

    while (node) {
        struct id_node *this = container_of(node, struct id_node, node);
        int result = new_id - this->new_id;

        if (result < 0)
            node = node->rb_left;
        else if (result > 0)
            node = node->rb_right;
        else {
            *pNode = this;
            return 0;
        }
    }
    return -1;
}

/*
 *remove invaild id
 */
static void id_tree_update(struct rb_root *root, uint32_t now)
{
    struct rb_node *node;
    struct id_node *p;

    node = rb_first(root);
    while (node) {
        p = rb_entry(node, struct id_node, node);
        if (now > p->ts + MIIO_AGENT_MAX_VALID_TIME) {
            log_printf(LOG_WARNING, "session timeout\n");
            rb_erase(&p->node, root);
            free(p);
            node = rb_first(root);
            continue;
        }
        node = rb_next(node);
    }
}

static void id_tree_free(struct rb_root *root)
{
    struct rb_node *node;
    struct id_node *p;

    node = rb_first(root);
    while (node) {
        p = rb_entry(node, struct id_node, node);
        rb_erase(&p->node, root);
        free(p);
        node = rb_first(root);
    }
}

/* --------------------------------------------------------------------- */
static int get_newid(void)
{
    static int32_t id=1;

    if (id >= MIIO_AGENT_MAX_ID_NUM)
        id = 1;
    return id++;
}

static int general_send_one(int sock, const char *buf, int size)
{
    ssize_t sent = 0;
    ssize_t ret = 0;

    while (sock != -1 && size > 0) {
        ret = send(sock, buf + sent, size, MSG_DONTWAIT);
        if (ret < 0) {
            return ret;
        }

        if (ret < size)
            log_printf(LOG_WARNING, "Partial written, total: size: %d,send: %d\n", size, ret);

        sent += ret;
        size -= ret;
    }

    return 0;
}

/**
 * handle messages from local client
 *
 * parse msg, if id exist, means this msg need ack, should linked to list.
 * so repleace old id with new id,record the corresponding relationship new_id: old_id, fd
 * if id is not exist,means ack no need, just send to miot
 **/
static int local_msg_handler(int sockfd, miio_agent_t agent, json_parser_t parser)
{
    int ret = 0;

    /* 1. if have "_to", local forward */
    if (0 == json_get_key_value_int32(parser, NULL, "_to", NULL)) {
        uint32_t addr;
        uint32_t from = 0;
        int i;

        if (0 != json_get_key_value_int32(parser, NULL, "_to", (int32_t*)&addr)) {
            log_e("invaild addr");
            ret = -1;
            goto _DONE_LOCAL_MSG_CONSUME;
        }

        /* figure out from */
        for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) { if(agent->local_server.client_fds[i] == sockfd) {
                from = 1<<i;
                break;
            }
        }

        /* replace _to with _from */
        json_delete_key(parser, NULL, "_to");
        json_insert_key_value_int32(parser, NULL, "_from", from);

        /* send to target client */
        for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) {
            if (addr != from && (addr & (1<<i))) {
                int fd = agent->local_server.client_fds[i];
                if (fd != -1) {
                    const char *msg = json_parser_to_string(parser);
                    int len = strlen(msg);
                    ret = general_send_one(fd, msg, len);
                    log_printf(LOG_DEBUG, "local forward addr: %8.8X, fd: %d\n", addr, sockfd);
                }
            }
        }

        goto _DONE_LOCAL_MSG_CONSUME;
    }

    /* 2. for me ? */
    if (0 == json_get_key_value_string(parser, NULL, "method", NULL, 0)) { /* have method */
        char method[MIIO_AGENT_MAX_KEY_LEN];
        if (0 != json_get_key_value_string(parser, NULL, "method", method, sizeof(method))) {
            log_e("invaild method");
            ret = -11;
            goto _DONE_LOCAL_MSG_CONSUME;
        }

        if (memcmp(method, "register", strlen("register")) == 0) {
            char key[MIIO_AGENT_MAX_KEY_LEN];
            if (0 != json_get_key_value_string(parser, NULL, "key", key, sizeof(key))) {
                log_printf(LOG_WARNING, "%s|%d: invaild key\n", __func__, __LINE__);
                ret = -12;
                goto _DONE_LOCAL_MSG_CONSUME;
            }

            log_printf(LOG_INFO, "register key: %s, fd: %d\n", key, sockfd);
            key_insert(&agent->ot_client.key_tree, key, sockfd);

            goto _DONE_LOCAL_MSG_CONSUME;

        } else if (memcmp(method, "unregister", strlen("unregister")) == 0) {
            char key[MIIO_AGENT_MAX_KEY_LEN];
            if (0 != json_get_key_value_string(parser, NULL, "key", key, sizeof(key))) {
                log_e("invaild key");
                ret = -12;
                goto _DONE_LOCAL_MSG_CONSUME;
            }

            if (0 == strlen(key)) {
                log_printf(LOG_INFO, "unregister all,fd: %d\n", sockfd);
                keytree_remove(&agent->ot_client.key_tree, sockfd);
            } else {
                log_printf(LOG_INFO, "unregister key: %s, fd: %d\n", key, sockfd);
                remove_key_within_fd(sockfd, key, &agent->ot_client.key_tree);
            }
            goto _DONE_LOCAL_MSG_CONSUME;

        } else if (memcmp(method, "bind", strlen("bind")) == 0) {
            uint32_t addr;
            int i;
            if (0 != json_get_key_value_int32(parser, NULL, "address", (int32_t*)&addr)) {
                log_printf(LOG_WARNING, "%s|%d: invaild addr\n", __func__, __LINE__);
                ret = -13;
                goto _DONE_LOCAL_MSG_CONSUME;
            }

            for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) {
                if (addr & (1<<i)) { /* locate slot */
                    if (agent->local_server.client_fds[i] != -1) { /* bond already */
                        log_e("addr %8.8X used by fd %d",
                                addr, agent->local_server.client_fds[i]);
                        ret = -128;
                        goto _DONE_LOCAL_MSG_CONSUME;
                    }

                    agent->local_server.client_fds[i] = sockfd;
                    log_printf(LOG_INFO, "bind addr: %8.8X, fd: %d\n", addr, sockfd);
                    break;
                }
            }

            goto _DONE_LOCAL_MSG_CONSUME;
        }
    }

    /* 3. must for ot */
    {
        int fd = agent->ot_client.fd;

        /* have method and id, request need ack, add to session */
        if (0 == json_get_key_value_string(parser, NULL, "method", NULL, 0) &&
                0 == json_get_key_value_int32(parser, NULL, "id", NULL)) {
            int32_t old_id, new_id;
            int msg_len;
            const char *newmsg;
            struct id_node *p;

            if ( 0 != json_get_key_value_int32(parser, NULL, "id", &old_id)) {
                log_e("invaild id");
                ret = -21;
                goto _DONE_LOCAL_MSG_CONSUME;
            }


            new_id = get_newid();

            /* replace with new id */
            json_delete_key(parser, NULL, "id");
            json_insert_key_value_int32(parser, NULL, "id", old_id);
            newmsg = json_parser_to_string(parser);
            msg_len = strlen(newmsg);

            if(0 != general_send_one(fd, newmsg, msg_len)) {
                log_e("send");
                ret = -22;
                goto _DONE_LOCAL_MSG_CONSUME;
            }

            log_printf(LOG_DEBUG, "U:new id:%d, old_id:%d, fd:%d: len: %d msg: %s\n",
                    new_id, old_id, sockfd, msg_len, newmsg);

            p = malloc(sizeof(struct id_node));
            assert(p);
            p->new_id = new_id;
            p->old_id = old_id;
            p->fd = sockfd;
            p->ts= time(NULL);
            id_insert(&agent->ot_client.id_tree, p);

            goto _DONE_LOCAL_MSG_CONSUME;
        }

        /* needn't ack, just forward */
        {
            const char *msg = json_parser_to_string(parser);
            int len = strlen(msg);
            if(0 != general_send_one(fd, msg, len)) {
                log_e("send");
                ret = -22;
                goto _DONE_LOCAL_MSG_CONSUME;
            }

            log_printf(LOG_DEBUG, "U:fd:%d, len:%d\n", sockfd, len);
        }
    }

_DONE_LOCAL_MSG_CONSUME:
    return ret;
}

/* --------------------------------------------------------------------- local server */
static void local_server_message_callback(int fd, uint32_t events, void *user_data);

static int local_server_start(miio_agent_t agent)
{
    struct sockaddr_un serveraddr;
    int agent_listenfd;
    int ret = -1, on = 1;

    agent_listenfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (agent_listenfd < 0) {
        log_e("create socket : %m");
        return -1;
    }

    if ((ret = setsockopt(agent_listenfd, SOL_SOCKET, SO_REUSEADDR,
                    (char *) &on, sizeof(on))) < 0) {
        log_e("setsockopt(SO_REUSEADDR): %m");
        close(agent_listenfd);
        return -2;
    }

    if (ioctl(agent_listenfd, FIONBIO, (char *)&on) < 0) {
        log_e("ioctl FIONBIO failed: %m");
        close(agent_listenfd);
        return -3;
    }

    unlink(MIIO_AGENT_SERVER_PATH);

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sun_family = AF_UNIX;
    strcpy(serveraddr.sun_path, MIIO_AGENT_SERVER_PATH);
    if (bind(agent_listenfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        log_e("socket bind (%s) %m\n", MIIO_AGENT_SERVER_PATH);
        close(agent_listenfd);
        return -4;
    }

    if (listen(agent_listenfd, MIIO_AGENT_CLIENT_MAX_NUM) == -1) {
        log_e("listen");
        return -5;
    }

    if(mainloop_add_fd(agent_listenfd, EPOLLIN, local_server_message_callback, agent, NULL))
        return -6;

    agent->local_server.fd = agent_listenfd;

    log_printf(LOG_INFO, "local server started at %s.\n", MIIO_AGENT_SERVER_PATH);

    return 0;
}

/*
 * receive rpc from local client and consume.
 * return 0 on sucess, -1 on socket error.
 */
static int local_message_consume(miio_agent_t agent, int fd)
{
    static char buf[MIIO_AGENT_MAX_MSG_LEN] = {0};

    do {
        int nlen;

        nlen = read(fd, buf, MIIO_AGENT_MAX_MSG_LEN);
        if(nlen < 0) {
            if(errno == EAGAIN) {
                break;
            }else{
                log_e("read");
                return -1;
            }
        } else if (nlen == 0) {
            log_e("connect");
            return -1;
        } else {
        }

        /* consume msg */
        {
            json_parser_t parser = json_parser_new(buf, nlen);
            if (parser == NULL) {
                buf[nlen-1] = '\0';
                log_printf(LOG_WARNING, "%s: Not in json format: %s\n", __func__, buf);
                break;
            }

            if(-128 == local_msg_handler(fd, agent, parser)) {
                json_parser_free(parser);
                return -1;
            }

            json_parser_free(parser);
        }

    } while (1);

    return 0;
}

static void local_client_message_callback(int fd, uint32_t events, void *user_data)
{
    miio_agent_t agent = (miio_agent_t)user_data;

    if ((events & EPOLLERR) ||
        (events & EPOLLHUP)) {
        log_e("epoll");
        goto _LOCAL_CONN_ERROR;

    } else if ((events & EPOLLIN)) {
        if(0 != local_message_consume(agent, fd)) {
            log_e("client socket %d", fd);
            goto _LOCAL_CONN_ERROR;
        }
    } else {
        log_e("unkown");
        goto _LOCAL_CONN_ERROR;
    }

    return;

_LOCAL_CONN_ERROR:
    mainloop_remove_fd(fd);
    keytree_remove(&agent->ot_client.key_tree, fd);
    {
        int i;
        for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) {
            if (agent->local_server.client_fds[i] == fd) {
                agent->local_server.client_fds[i] = -1;
                break;
            }
        }
    }
    close(fd);
}

static void local_server_message_callback(int fd, uint32_t events, void *user_data)
{
    miio_agent_t agent = (miio_agent_t)user_data;

    if ((events & EPOLLERR) ||
        (events & EPOLLHUP)) {
        log_e("local server poll");
        goto _LOCAL_CONN_ERROR;

    } else if ((events & EPOLLIN)) {
        int newfd;
        struct sockaddr_storage other_addr;
        int on = 1;

        socklen_t sin_size = sizeof(struct sockaddr_storage);

        newfd = accept(fd, (struct sockaddr *)&other_addr, &sin_size);
        if (newfd <= 0) {
            log_e("accept");
            return;
        }

        if (ioctl(newfd, FIONBIO, (char *)&on) < 0) {
            log_e("ioctl FIONBIO");
            close(newfd);
            return;
        }

        if(0 != mainloop_add_fd(newfd, EPOLLIN|EPOLLET, local_client_message_callback, agent, NULL)) {
            log_e("add newfd");
            return;
        }

    } else {
        log_e("unkown");
        goto _LOCAL_CONN_ERROR;
    }

    return;

_LOCAL_CONN_ERROR:
    log_printf(LOG_WARNING, "shouldn't be here.\n");
    mainloop_quit();
}

/* --------------------------------------------------------------------- ot */
static void ot_message_callback(int fd, uint32_t events, void *user_data);

static int ot_connect(miio_agent_t agent)
{
    struct sockaddr_in servaddr;

    if (agent->ot_client.fd < 0)
        agent->ot_client.fd = socket(AF_INET, SOCK_STREAM, 0);

    if (agent->ot_client.fd < 0)
        return -1;

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(OT_SERVER_IP);
    servaddr.sin_port = htons(OT_SERVER_PORT);

    if (connect(agent->ot_client.fd, (struct sockaddr *)&servaddr, sizeof(servaddr))) {
        log_e("connect");
        return -2;
    }

    { /* config socket */
        int on = 1;
        int recv_buffer_len = OT_RECV_BUFSIZE;

        ioctl(agent->ot_client.fd, FIONBIO, &on);

        if(ioctl(agent->ot_client.fd, FIONBIO, &on)
                || setsockopt(agent->ot_client.fd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_len, sizeof(int))) {
            close(agent->ot_client.fd);
            log_e("config");
            return -3;
        }
    }

    if(mainloop_add_fd(agent->ot_client.fd, EPOLLIN|EPOLLET, ot_message_callback, agent, NULL))
        return -4;

    agent->ot_client.is_connected = true;

    log_printf(LOG_INFO, "ot connected.\n");
    return 0;
}

static void ot_timer_reconnect_callback(int timerfd, void *user_data)
{
    miio_agent_t agent = (miio_agent_t)user_data;

    if (agent->ot_client.is_connected) {
        mainloop_remove_fd(timerfd);
        agent->ot_client.retry_timer_fd = -1;
        return;
    }

    log_printf(LOG_INFO, "ot reconnecting...\n");
    ot_connect(agent);

    if(0 > mainloop_modify_timeout(timerfd, OT_CONN_RETRY_INTERVAL))
        log_e("modify timer.");
}

static void ot_timer_reconnect(miio_agent_t agent)
{
    agent->ot_client.retry_timer_fd = mainloop_add_timeout(OT_CONN_RETRY_INTERVAL,
            ot_timer_reconnect_callback, agent, NULL);
    assert(!(agent->ot_client.retry_timer_fd < 0));
}

/**
 * handle messages from miot
 **/
static int ot_msg_handler(miio_agent_t agent, json_parser_t parser)
{
    int ret = 0;

    int32_t id;

    /* 1. check if in session */
    if (0 == json_get_key_value_int32(parser, NULL, "id", &id)) {
        struct id_node *p;
        static uint32_t last_sec = 0;
        uint32_t now = time(NULL);

        if (now != last_sec) {
            id_tree_update(&agent->ot_client.id_tree, now);
        }

        if (0 == id_search(&agent->ot_client.id_tree, id, &p)) {
            int32_t old_id;
            int fd, msg_len;
            const char *newmsg;

            //get old id and fd
            old_id = p->old_id;
            fd = p->fd;
            /* remove id node from tree */
            rb_erase(&p->node, &agent->ot_client.id_tree);
            free(p);

            /* restore with old id */
            json_delete_key(parser, NULL, "id");
            json_insert_key_value_int32(parser, NULL, "id", old_id);
            newmsg = json_parser_to_string(parser);
            msg_len = strlen(newmsg);

            log_printf(LOG_DEBUG, "D:ACK, new_id:%d, old_id:%d, fd:%d,length: %d\n",
                    id, old_id, fd, msg_len);
            return general_send_one(fd, newmsg, msg_len);
        }
    }

    /* 2. chech if registered */
    {
        char method[MIIO_AGENT_MAX_KEY_LEN];
        if (0 == json_get_key_value_string(parser, NULL, "method", method, sizeof(method))) {
            struct key_node *p;
            const char *msg;
            int msg_len;

            if (0 == key_search(&agent->ot_client.key_tree, method, &p)) {
                int i = 0;
                while (i < MIIO_AGENT_CLIENT_MAX_NUM && p->fd[i] > 0) {
                    msg = json_parser_to_string(parser);
                    msg_len = strlen(msg);
                    ret |= general_send_one(p->fd[i], msg, msg_len);
                    log_printf(LOG_DEBUG,"D:cmd,fd:%d, len:%d\n",  p->fd[i], msg_len);
                    i++;
                }

            } else {
                msg = json_parser_to_string(parser);
                log_printf(LOG_WARNING,"no sockfd is registered, msg is %s\n",  msg);
            }

            return ret;

        } else { /* others */
            const char *msg;
            msg = json_parser_to_string(parser);
            log_printf(LOG_WARNING,"invaild msg, drop: %s\n", msg);
        }
    }

    return ret;
}

/*
 * receive msgs from miio_client, and consume each rpc.
 * return 0 on sucess, -1 on socket error.
 */
static int ot_message_consume(miio_agent_t agent, int fd)
{
    static char buf[OT_RECV_BUFSIZE] = {0};
    int msg_len = 0;
    int i = 0;

    do {
        int nlen;
        int start = -1, end = -1;
        int lbraces = 0, rbraces = 0;

        nlen = read(fd, buf + msg_len, OT_RECV_BUFSIZE - msg_len);
        if(nlen < 0) {
            if(errno == EAGAIN) {
                break;
            }else{
                log_e("read");
                return -1;
            }
        } else if (nlen == 0) {
            log_e("connect");
            return -1;
        } else {
            msg_len += nlen;
        }

        /* consume msg */
        while(i < msg_len) { /* search entire prc */
            /* locate start and left */
            if (buf[i] == '{') {
                if(lbraces == 0)
                    start = i;
                lbraces++;
            }
            /* locate right and end */
            if (buf[i] == '}') {
                rbraces++;
                if(rbraces == lbraces) {
                    end = i;

                    {
                        json_parser_t parser = json_parser_new(&buf[start], end-start+1);
                        if (parser == NULL) {
                            buf[end-start] = '\0';
                            log_printf(LOG_WARNING, "%s: Not in json format: %s\n", __func__, buf);
                            break;
                        }

                        ot_msg_handler(agent, parser);

                        json_parser_free(parser);
                    }

                    end = start = -1;
                    lbraces = rbraces = 0;
                }
            }

            i++;
        }

    } while (1);

    return 0;
}

static void ot_message_callback(int fd, uint32_t events, void *user_data)
{
    miio_agent_t agent = (miio_agent_t)user_data;

    if ((events & EPOLLERR) ||
        (events & EPOLLHUP)) {
        log_e("sock");
        goto _OT_CONN_ERROR;

    } else if ((events & EPOLLIN)) {
        if (0 != ot_message_consume(agent, fd)) {
            log_e("sock");
            goto _OT_CONN_ERROR;
        }

    } else {
        log_e("sock");
        goto _OT_CONN_ERROR;
    }

    return;

_OT_CONN_ERROR:
    agent->ot_client.is_connected = false;
    agent->ot_client.fd = -1;
    mainloop_remove_fd(fd);
    close(fd);
    ot_timer_reconnect(agent);
}

/* --------------------------------------------------------------------- */
static void signal_callback(int signum, void *user_data)
{
    (void)user_data;

    switch (signum) {
        case SIGINT:
        case SIGTERM:
            mainloop_quit();
            break;
        default:
            log_printf(LOG_WARNING, "sig %d\n", signum);
            break;
    }
}

int main(int argc, char *argv[])
{
    const struct option options[] = {
        {"help",      no_argument,       NULL, 'h'},
        {"version",   no_argument,       NULL, 'v'},
        {"loglevel",  required_argument, NULL, 'l'},
        {"logfile",   required_argument, NULL, 'L'},
        {"daemonize", no_argument,       NULL, 'D'},
        {"offline",   no_argument,       NULL, 'O'},
        {NULL,        0,                 0,    0}
    };

    int ret = 0;
    int n = 0, i;
    int daemonize = 0;
    bool is_ot_disabled = false;

    s_log_file = stdout;

    while (n >= 0) {
        n = getopt_long(argc, argv, "hDvl:L:O", options, NULL);
        if (n < 0)
            continue;
        switch (n) {
            case 'D':
                daemonize = 1;
                break;
            case 'l':
                s_loglevel = atoi(optarg);
                if (s_loglevel > LOG_LEVEL_MAX)
                    s_loglevel = LOG_LEVEL_MAX;
                log_printf(LOG_INFO, "Set log level to: %d\n", s_loglevel);
                break;
            case 'L':
                if(0 != logfile_init(optarg)) {
                    fprintf(stderr, "can't open log file\n");
                    exit(1);
                }
                break;
            case 'v':
                fprintf(stdout, "%s\n", PACKAGE_STRING);
                exit(1);
            case 'O':
                is_ot_disabled = true;
                break;
            case 'h':
            default:
                fprintf(stderr, "miio agent - MIIO rpc messages agent protocol implementation\n"
                        "Copyright (C) Xiaomi\n"
                        "Author: MIOT\n"
                        "Version: %s\n"
                        "Build time: %s %s\n", PACKAGE_STRING, __TIME__, __DATE__);
                fprintf(stderr, "\n");
                fprintf(stderr, "Usage: %s\n"
                        "\t[-D --daemonize]\n"
                        "\t[-l --loglevel=<level>] set loglevel (0-4), bigger = more verbose\n"
                        "\t[-L --logfile=file] output log into file instead of stdout\n"
                        "\t[-O --offline] local msg forward without miot\n"
                        "\t[-h --help]\n"
                        , argv[0]);
                exit(1);
        }
    }

    mainloop_init();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);
    mainloop_set_signal(&mask, signal_callback, NULL, NULL);

    miio_agent_t agent = malloc(sizeof(struct miio_agent));
    assert(agent);

    agent->local_server.fd = -1;
    for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) {
        agent->local_server.client_fds[i] = -1;
    }

    ret = local_server_start(agent);
    assert(ret == 0);

    agent->ot_client.fd = -1;
    agent->ot_client.is_connected = false;
    agent->ot_client.retry_timer_fd = -1;

    if (!is_ot_disabled) {
        agent->ot_client.key_tree = RB_ROOT;
        agent->ot_client.id_tree = RB_ROOT;

        log_printf(LOG_INFO, "ot connecting...\n");
        ot_connect(agent);
        ot_timer_reconnect(agent);
    }

    if (daemonize)
        if (daemon(0, 1) < 0)
            log_printf(LOG_WARNING, "daemonize fail: %m\n");

    ret = mainloop_run();

    /* exit, free all */
    key_tree_free(&agent->ot_client.key_tree);
    id_tree_free(&agent->ot_client.id_tree);

    if (!(agent->ot_client.fd < 0))
        close(agent->ot_client.fd);
    if (!(agent->ot_client.retry_timer_fd < 0))
        close(agent->ot_client.retry_timer_fd);

    for (i = 0; i < MIIO_AGENT_CLIENT_MAX_NUM; i++) {
        if (!(agent->local_server.client_fds[i] < 0))
            close(agent->local_server.client_fds[i]);
    }
    if (!(agent->local_server.fd < 0))
        close(agent->local_server.fd);

    free(agent);

    log_printf(LOG_INFO, "Bye.\n");
    return ret;
}

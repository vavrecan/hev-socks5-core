/*
 ============================================================================
 Name        : hev-socks5-client.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2021 - 2023 hev
 Description : Socks5 Client
 ============================================================================
 */

#include <string.h>
#include <unistd.h>

#include <hev-task.h>
#include <hev-task-io.h>
#include <hev-task-io-socket.h>
#include <hev-memory-allocator.h>

#include "hev-socks5-misc-priv.h"
#include "hev-socks5-logger-priv.h"

#include "hev-socks5-client.h"

#define task_io_yielder hev_socks5_task_io_yielder

static int
hev_socks5_client_connect_server (HevSocks5Client *self, const char *addr,
                                  int port)
{
    HevSocks5Class *klass;
    struct sockaddr_in6 saddr;
    struct sockaddr *sap;
    int fd, res;

    LOG_D ("%p socks5 client connect server", self);

    res = hev_socks5_resolve_to_sockaddr6 (addr, port, &saddr);
    if (res < 0) {
        LOG_E ("%p socks5 client resolve [%s]:%d", self, addr, port);
        return -1;
    }

    fd = hev_socks5_socket (SOCK_STREAM);
    if (fd < 0) {
        LOG_E ("%p socks5 client socket", self);
        return -1;
    }

    sap = (struct sockaddr *)&saddr;
    klass = HEV_OBJECT_GET_CLASS (self);
    res = klass->binder (HEV_SOCKS5 (self), fd, sap);
    if (res < 0) {
        LOG_E ("%p socks5 client bind", self);
        close (fd);
        return -1;
    }

    res = hev_task_io_socket_connect (fd, sap, sizeof (saddr), task_io_yielder,
                                      self);
    if (res < 0) {
        LOG_E ("%p socks5 client connect", self);
        close (fd);
        return -1;
    }

    HEV_SOCKS5 (self)->fd = fd;
    LOG_D ("%p socks5 client connect server fd %d", self, fd);

    return 0;
}

static int
hev_socks5_client_write_request (HevSocks5Client *self)
{
    HevSocks5ClientClass *klass;
    struct msghdr mh = { 0 };
    struct iovec iov[7];
    HevSocks5Addr *addr;
    HevSocks5ReqRes req;
    HevSocks5Auth auth;
    int addrlen;
    int ret, n;

    LOG_D ("%p socks5 client write request", self);

    auth.ver = HEV_SOCKS5_VERSION_5;
    auth.method_len = 1;

    n = 0;
    iov[n].iov_base = &auth;
    iov[n++].iov_len = 3;

    if (!self->auth.user || !self->auth.pass) {
        auth.methods[0] = HEV_SOCKS5_AUTH_METHOD_NONE;
    } else {
        auth.methods[0] = HEV_SOCKS5_AUTH_METHOD_USER;
        auth.methods[1] = HEV_SOCKS5_AUTH_VERSION_1;
        auth.methods[2] = strlen (self->auth.user);
        auth.methods[3] = strlen (self->auth.pass);
        iov[n].iov_base = &auth.methods[1];
        iov[n++].iov_len = 2;
        iov[n].iov_base = (void *)self->auth.user;
        iov[n++].iov_len = auth.methods[2];
        iov[n].iov_base = &auth.methods[3];
        iov[n++].iov_len = 1;
        iov[n].iov_base = (void *)self->auth.pass;
        iov[n++].iov_len = auth.methods[3];
    }

    req.ver = HEV_SOCKS5_VERSION_5;
    req.rsv = 0;

    switch (HEV_SOCKS5 (self)->type) {
    case HEV_SOCKS5_TYPE_TCP:
        req.cmd = HEV_SOCKS5_REQ_CMD_CONNECT;
        break;
    case HEV_SOCKS5_TYPE_UDP_IN_TCP:
        req.cmd = HEV_SOCKS5_REQ_CMD_FWD_UDP;
        break;
    case HEV_SOCKS5_TYPE_UDP_IN_UDP:
        req.cmd = HEV_SOCKS5_REQ_CMD_UDP_ASC;
        break;
    default:
        return -1;
    }

    iov[n].iov_base = &req;
    iov[n++].iov_len = 3;

    klass = HEV_OBJECT_GET_CLASS (self);
    addr = klass->get_upstream_addr (self);

    switch (addr->atype) {
    case HEV_SOCKS5_ADDR_TYPE_IPV4:
        addrlen = 7;
        break;
    case HEV_SOCKS5_ADDR_TYPE_IPV6:
        addrlen = 19;
        break;
    case HEV_SOCKS5_ADDR_TYPE_NAME:
        addrlen = 4 + addr->domain.len;
        break;
    default:
        LOG_E ("%p socks5 client req.atype %u", self, addr->atype);
        return -1;
    }

    iov[n].iov_base = addr;
    iov[n++].iov_len = addrlen;

    mh.msg_iov = iov;
    mh.msg_iovlen = n;
    ret = hev_task_io_socket_sendmsg (HEV_SOCKS5 (self)->fd, &mh, MSG_WAITALL,
                                      task_io_yielder, self);
    if (ret <= 0) {
        LOG_E ("%p socks5 client write request", self);
        return -1;
    }

    hev_free (addr);

    return 0;
}

static int
hev_socks5_client_read_response (HevSocks5Client *self)
{
    HevSocks5ClientClass *klass;
    HevSocks5ReqRes res;
    HevSocks5Auth auth;
    int addrlen;
    int ret;

    LOG_D ("%p socks5 client read response", self);

    ret = hev_task_io_socket_recv (HEV_SOCKS5 (self)->fd, &auth, 2, MSG_WAITALL,
                                   task_io_yielder, self);
    if (ret <= 0) {
        LOG_E ("%p socks5 client read auth", self);
        return -1;
    }

    if (auth.ver != HEV_SOCKS5_VERSION_5) {
        LOG_E ("%p socks5 client auth.ver %u", self, auth.ver);
        return -1;
    }

    if (auth.method == HEV_SOCKS5_AUTH_METHOD_USER) {
        ret = hev_task_io_socket_recv (HEV_SOCKS5 (self)->fd, &res, 2,
                                       MSG_WAITALL, task_io_yielder, self);
        if (ret <= 0) {
            LOG_E ("%p socks5 client read auth responce", self);
            return -1;
        }

        if (res.ver != HEV_SOCKS5_AUTH_VERSION_1) {
            LOG_E ("%p socks5 client auth.res.ver %u", self, res.ver);
            return -1;
        }

        if (res.rep != HEV_SOCKS5_RES_REP_SUCC) {
            LOG_E ("%p socks5 client auth.res.rep %u", self, res.rep);
            return -1;
        }

        LOG_D ("%p socks5 client auth done", self);
    } else if (auth.method != HEV_SOCKS5_AUTH_METHOD_NONE) {
        LOG_E ("%p socks5 client auth.method %u", self, auth.method);
        return -1;
    }

    ret = hev_task_io_socket_recv (HEV_SOCKS5 (self)->fd, &res, 4, MSG_WAITALL,
                                   task_io_yielder, self);
    if (ret <= 0) {
        LOG_E ("%p socks5 client read response", self);
        return -1;
    }

    if (res.ver != HEV_SOCKS5_VERSION_5) {
        LOG_E ("%p socks5 client res.ver %u", self, res.ver);
        return -1;
    }

    if (res.rep != HEV_SOCKS5_RES_REP_SUCC) {
        LOG_E ("%p socks5 client res.rep %u", self, res.rep);
        return -1;
    }

    switch (res.addr.atype) {
    case HEV_SOCKS5_ADDR_TYPE_IPV4:
        addrlen = 6;
        break;
    case HEV_SOCKS5_ADDR_TYPE_IPV6:
        addrlen = 18;
        break;
    default:
        LOG_E ("%p socks5 client res.atype %u", self, res.addr.atype);
        return -1;
    }

    ret = hev_task_io_socket_recv (HEV_SOCKS5 (self)->fd, &res.addr.ipv4,
                                   addrlen, MSG_WAITALL, task_io_yielder, self);
    if (ret <= 0) {
        LOG_E ("%p socks5 client read addr", self);
        return -1;
    }

    klass = HEV_OBJECT_GET_CLASS (self);
    ret = klass->set_upstream_addr (self, &res.addr);
    if (ret < 0) {
        LOG_E ("%p socks5 client set upstream addr", self);
        return -1;
    }

    return 0;
}

int
hev_socks5_client_connect (HevSocks5Client *self, const char *addr, int port)
{
    int res;

    LOG_D ("%p socks5 client connect [%s]:%d", self, addr, port);

    res = hev_socks5_client_connect_server (self, addr, port);
    if (res < 0) {
        LOG_E ("%p socks5 client connect", self);
        return -1;
    }

    return 0;
}

int
hev_socks5_client_connect_fd (HevSocks5Client *self, int fd)
{
    HevTask *task = hev_task_self ();
    int res;

    LOG_D ("%p socks5 client connect fd %d", self, fd);

    HEV_SOCKS5 (self)->fd = fd;

    res = hev_task_add_fd (task, fd, POLLIN | POLLOUT);
    if (res < 0)
        hev_task_mod_fd (task, fd, POLLIN | POLLOUT);

    return 0;
}

int
hev_socks5_client_handshake (HevSocks5Client *self)
{
    int res;

    LOG_D ("%p socks5 client handshake", self);

    res = hev_socks5_client_write_request (self);
    if (res < 0)
        return -1;

    res = hev_socks5_client_read_response (self);
    if (res < 0)
        return -1;

    return 0;
}

void
hev_socks5_client_set_auth (HevSocks5Client *self, const char *user,
                            const char *pass)
{
    LOG_D ("%p socks5 client set auth", self);

    self->auth.user = user;
    self->auth.pass = pass;
}

int
hev_socks5_client_construct (HevSocks5Client *self, HevSocks5Type type)
{
    int res;

    res = hev_socks5_construct (&self->base, type);
    if (res < 0)
        return res;

    LOG_D ("%p socks5 client construct", self);

    HEV_OBJECT (self)->klass = HEV_SOCKS5_CLIENT_TYPE;

    return 0;
}

static void
hev_socks5_client_destruct (HevObject *base)
{
    HevSocks5Client *self = HEV_SOCKS5_CLIENT (base);

    LOG_D ("%p socks5 client destruct", self);

    HEV_SOCKS5_TYPE->finalizer (base);
}

HevObjectClass *
hev_socks5_client_class (void)
{
    static HevSocks5ClientClass klass;
    HevSocks5ClientClass *kptr = &klass;
    HevObjectClass *okptr = HEV_OBJECT_CLASS (kptr);

    if (!okptr->name) {
        memcpy (kptr, HEV_SOCKS5_TYPE, sizeof (HevSocks5Class));

        okptr->name = "HevSocks5Client";
        okptr->finalizer = hev_socks5_client_destruct;
    }

    return okptr;
}

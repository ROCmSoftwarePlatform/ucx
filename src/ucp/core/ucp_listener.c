/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2018.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ucp_listener.h"

#include <ucp/stream/stream.h>
#include <ucp/wireup/wireup_ep.h>
#include <ucp/core/ucp_ep.h>
#include <ucp/core/ucp_ep.inl>
#include <ucs/debug/log.h>
#include <ucs/sys/sock.h>


static unsigned ucp_listener_accept_cb_progress(void *arg)
{
    ucp_ep_h       ep       = arg;
    ucp_listener_h listener = ucp_ep_ext_gen(ep)->listener;

    /* NOTE: protect union */
    ucs_assert(!(ep->flags & (UCP_EP_FLAG_ON_MATCH_CTX |
                              UCP_EP_FLAG_FLUSH_STATE_VALID)));
    ucs_assert(ep->flags   & UCP_EP_FLAG_LISTENER);

    ep->flags &= ~UCP_EP_FLAG_LISTENER;
    ep->flags |= UCP_EP_FLAG_USED;
    ucp_stream_ep_activate(ep);
    ucp_ep_flush_state_reset(ep);

    /*
     * listener is NULL if the EP was created with UCP_EP_PARAM_FIELD_EP_ADDR
     * and we are here because long address requires wireup protocol
     */
    if (listener && listener->accept_cb) {
        listener->accept_cb(ep, listener->arg);
    }

    return 1;
}

int ucp_listener_accept_cb_remove_filter(const ucs_callbackq_elem_t *elem,
                                                void *arg)
{
    ucp_ep_h ep = elem->arg;

    return (elem->cb == ucp_listener_accept_cb_progress) && (ep == arg);
}

void ucp_listener_schedule_accept_cb(ucp_ep_h ep)
{
    uct_worker_cb_id_t prog_id = UCS_CALLBACKQ_ID_NULL;

    uct_worker_progress_register_safe(ep->worker->uct,
                                      ucp_listener_accept_cb_progress,
                                      ep, UCS_CALLBACKQ_FLAG_ONESHOT,
                                      &prog_id);
}

static unsigned ucp_listener_conn_request_progress(void *arg)
{
    ucp_conn_request_h               conn_request = arg;
    ucp_listener_h                   listener     = conn_request->listener;
    const ucp_wireup_client_data_t   *client_data = &conn_request->client_data;
    ucp_worker_h                     worker       = listener->worker;
    ucp_ep_h                         ep;
    ucs_status_t                     status;

    ucs_trace_func("listener=%p", listener);

    if (listener->conn_cb) {
        listener->conn_cb(conn_request, listener->arg);
        return 1;
    }

    UCS_ASYNC_BLOCK(&worker->async);
    /* coverity[overrun-buffer-val] */
    status = ucp_ep_create_accept(worker, client_data, &ep);

    if (status != UCS_OK) {
        goto out;
    }

    if (ep->flags & UCP_EP_FLAG_LISTENER) {
        status = ucp_wireup_send_pre_request(ep);
    } else {
        /* send wireup request message, to connect the client to the server's
           new endpoint */
        ucs_assert(!(ep->flags & UCP_EP_FLAG_CONNECT_REQ_QUEUED));
        status = ucp_wireup_send_request(ep);
    }

    if (status != UCS_OK) {
        goto out;
    }

    status = uct_iface_accept(conn_request->uct_iface, conn_request->uct_req);
    if (status != UCS_OK) {
        ucp_ep_destroy_internal(ep);
        goto out;
    }

    if (listener->accept_cb != NULL) {
        if (ep->flags & UCP_EP_FLAG_LISTENER) {
            ucs_assert(!(ep->flags & UCP_EP_FLAG_USED));
            ucp_ep_ext_gen(ep)->listener = listener;
        } else {
            ep->flags |= UCP_EP_FLAG_USED;
            listener->accept_cb(ep, listener->arg);
        }
    }

out:
    if (status != UCS_OK) {
        ucs_error("connection request failed on listener %p with status %s",
                  listener, ucs_status_string(status));
        uct_iface_reject(conn_request->uct_iface, conn_request->uct_req);
    }

    UCS_ASYNC_UNBLOCK(&worker->async);
    ucs_free(conn_request);
    return 1;
}

static int ucp_listener_remove_filter(const ucs_callbackq_elem_t *elem,
                                      void *arg)
{
    ucp_listener_h *listener = elem->arg;

    return (elem->cb == ucp_listener_conn_request_progress) && (listener == arg);
}

static void ucp_listener_conn_request_callback(uct_iface_h tl_iface, void *arg,
                                               uct_conn_request_h uct_req,
                                               const void *conn_priv_data,
                                               size_t length)
{
    ucp_listener_h     listener = arg;
    uct_worker_cb_id_t prog_id  = UCS_CALLBACKQ_ID_NULL;
    ucp_conn_request_h conn_request;

    ucs_trace("listener %p: got connection request", listener);

    /* Defer wireup init and user's callback to be invoked from the main thread */
    conn_request = ucs_malloc(ucs_offsetof(ucp_conn_request_t, client_data) +
                              length, "accept connection request");
    if (conn_request == NULL) {
        ucs_error("failed to allocate connect request, "
                  "rejecting connection request %p on TL iface %p, reason %s",
                  uct_req, tl_iface, ucs_status_string(UCS_ERR_NO_MEMORY));
        uct_iface_reject(tl_iface, uct_req);
        return;
    }

    conn_request->listener  = listener;
    conn_request->uct_req   = uct_req;
    conn_request->uct_iface = tl_iface;
    memcpy(&conn_request->client_data, conn_priv_data, length);

    uct_worker_progress_register_safe(listener->worker->uct,
                                      ucp_listener_conn_request_progress,
                                      conn_request, UCS_CALLBACKQ_FLAG_ONESHOT,
                                      &prog_id);

    /* If the worker supports the UCP_FEATURE_WAKEUP feature, signal the user so
     * that he can wake-up on this event */
    ucp_worker_signal_internal(listener->worker);
}

ucs_status_t ucp_listener_query(ucp_listener_h listener, ucp_listener_attr_t *attr)
{
    int i, port;

    ucs_assert(listener->ifaces.num_wifaces > 0);
    port = listener->ifaces.wifaces[0].attr.listen_port;

    /* Make sure that all the listening sockaddr ifaces are listening on the same port */
    for (i = 1; i < listener->ifaces.num_wifaces; i++) {
        if (port != listener->ifaces.wifaces[i].attr.listen_port) {
            ucs_error("different ports detected on the listener: %d and %d",
                      port, listener->ifaces.wifaces[i].attr.listen_port);
            return UCS_ERR_IO_ERROR;
        }
    }

    if (attr->field_mask & UCP_LISTENER_ATTR_FIELD_PORT) {
        attr->port = port;
    }

    return UCS_OK;
}

static void ucp_listener_close_uct_listeners(ucp_listener_h listener)
{
    ucp_rsc_index_t i;

    ucs_assert_always(ucp_worker_sockaddr_is_cm_proto(listener->worker));

    for (i = 0; i < listener->cms.num_listeners; ++i) {
        uct_listener_destroy(listener->cms.listeners[i]);
    }

    ucs_free(listener->cms.listeners);

    listener->cms.listeners     = NULL;
    listener->cms.num_listeners = 0;
}

static void ucp_listener_close_ifaces(ucp_listener_h listener)
{
    ucp_worker_h worker;
    int i;

    ucs_assert_always(!ucp_worker_sockaddr_is_cm_proto(listener->worker));

    for (i = 0; i < listener->ifaces.num_wifaces; i++) {
        worker = listener->ifaces.wifaces[i].worker;
        ucs_assert_always(worker == listener->worker);
        /* remove pending slow-path progress in case it wasn't removed yet */
        ucs_callbackq_remove_if(&worker->uct->progress_q,
                                ucp_listener_remove_filter, listener);
        ucp_worker_iface_cleanup(&listener->ifaces.wifaces[i]);
    }

    ucs_free(listener->ifaces.wifaces);
}

static ucs_status_t
ucp_listen_on_cm(ucp_listener_h listener, const ucp_listener_params_t *params)
{
    ucp_worker_h worker           = listener->worker;
    const ucp_rsc_index_t num_cms = ucp_worker_get_cm_num(worker);
    struct sockaddr addr          = *params->sockaddr.addr;
    uct_listener_h *listeners;
    uct_listener_params_t uct_params;
    uct_listener_attr_t   uct_attr;
    int                   port, listener_port;
    ucp_rsc_index_t i;
    char addr_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;

    uct_params.field_mask       = UCT_LISTENER_PARAM_FIELD_CONN_REQUEST_CB |
                                  UCT_LISTENER_PARAM_FIELD_USER_DATA;
    uct_params.conn_request_cb  = (void *)0xdeadbeaf; /* TODO: ucp_listener_conn_request_cb; */
    uct_params.user_data        = listener;

    listener->cms.num_listeners = 0;
    listeners                   = ucs_calloc(num_cms, sizeof(*listeners),
                                             "uct_listeners_arr");
    if (listeners == NULL) {
        ucs_error("Can't allocate memory for UCT listeners array");
        return UCS_ERR_NO_MEMORY;
    }

    listener->cms.listeners = listeners;

    for (i = 0; i < num_cms; ++i) {
        status = uct_listener_create(worker->cms[i], addr,
                                     params->sockaddr.addrlen, &uct_params,
                                     &listeners[listener->cms.num_listeners]);
        if (status == UCS_ERR_INVALID_ADDR) {
            ucs_debug("uct_listener_create failed on CM %p with address %s status %s",
            continue;
        } else if (status != UCS_OK) {
            ucs_error("uct_listener_create failed on CM %p with address %s status %s",
                      worker->cms[i],
                      ucs_sockaddr_str(params->sockaddr.addr, addr_str,
                                       UCS_SOCKADDR_STRING_LEN));
            goto destroy_listeners;
        }

        ++listener->cms.num_listeners;

        status = ucs_sockaddr_get_port(&addr, &port);
        if (status != UCS_OK) {
            goto destroy_listeners;
        }

        uct_attr.field_mask = UCT_LISTENER_ATTR_FIELD_SOCKADDR;
        status = uct_listener_query(listeners[listener->cms.num_listeners - 1],
                                    &uct_attr);
        if (status != UCS_OK) {
            goto destroy_listeners;
        }

        status = ucs_sockaddr_get_port((struct sockaddr *)&uct_attr.sockaddr,
                                       &listener_port);
        if (port != listener_port) {
            ucs_assert(port == 0);
            ucs_sockaddr_set_port(addr, listener_port);
        }
    }

    return UCS_OK;

destroy_listeners:
    ucp_listener_close_uct_listeners(listener);
    return status;
}

static ucs_status_t
ucp_listen_on_iface(ucp_listener_h listener,
                    const ucp_listener_params_t *params)
{
    ucp_worker_h worker   = listener->worker;
    ucp_context_h context = listener->worker->context;
    int sockaddr_tls      = 0;
    char saddr_str[UCS_SOCKADDR_STRING_LEN];
    ucp_tl_resource_desc_t *resource;
    uct_iface_params_t iface_params;
    ucp_worker_iface_t *tmp;
    ucp_rsc_index_t tl_id;
    ucs_status_t status;
    ucp_tl_md_t *tl_md;
    uint16_t port;
    int i;

    status = ucs_sockaddr_get_port(params->sockaddr.addr, &port);
    if (status != UCS_OK) {
       return status;
    }

    /* Go through all the available resources and for each one, check if the given
     * sockaddr is accessible from its md. Start listening on all the mds that
     * satisfy this.
     * If the given port is set to 0, i.e. use a random port, the first transport
     * in the sockaddr priority list from the environment configuration will
     * dictate the port to listen on for the other sockaddr transports in the list.
     * */
    for (i = 0; i < context->config.num_sockaddr_tls; i++) {
        tl_id    = context->config.sockaddr_tl_ids[i];
        resource = &context->tl_rscs[tl_id];
        tl_md    = &context->tl_mds[resource->md_index];

        if (!uct_md_is_sockaddr_accessible(tl_md->md, &params->sockaddr,
                                           UCT_SOCKADDR_ACC_LOCAL)) {
            continue;
        }

        tmp = ucs_realloc(listener->ifaces.wifaces,
                          sizeof(*listener->ifaces.wifaces) * (sockaddr_tls + 1),
                          "listener wifaces");
        if (tmp == NULL) {
            ucs_error("failed to allocate listener wifaces");
            status = UCS_ERR_NO_MEMORY;
            goto err_close_listener_wifaces;
        }

        listener->ifaces.wifaces = tmp;

        iface_params.field_mask                     = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                                                      UCT_IFACE_PARAM_FIELD_SOCKADDR;
        iface_params.open_mode                      = UCT_IFACE_OPEN_MODE_SOCKADDR_SERVER;
        iface_params.mode.sockaddr.conn_request_cb  = ucp_listener_conn_request_callback;
        iface_params.mode.sockaddr.conn_request_arg = listener;
        iface_params.mode.sockaddr.listen_sockaddr  = params->sockaddr;
        iface_params.mode.sockaddr.cb_flags         = UCT_CB_FLAG_ASYNC;

        if (port) {
            /* Set the port for the next sockaddr iface. This port was either
             * obtained from the user or generated by the first created sockaddr
             * iface if the port from the user was equal to zero */
            status = ucs_sockaddr_set_port(
                        (struct sockaddr *)
                        iface_params.mode.sockaddr.listen_sockaddr.addr, port);
            if (status != UCS_OK) {
                ucs_error("failed to set port parameter (%d) for creating %s iface",
                          port, resource->tl_rsc.tl_name);
                goto err_close_listener_wifaces;
            }
        }

        status = ucp_worker_iface_open(worker, tl_id, &iface_params,
                                       &listener->ifaces.wifaces[sockaddr_tls]);
        if (status != UCS_OK) {
            ucs_error("failed to open listener on %s on md %s",
                      ucs_sockaddr_str(
                            iface_params.mode.sockaddr.listen_sockaddr.addr,
                            saddr_str, sizeof(saddr_str)),
                            tl_md->rsc.md_name);
            goto err_close_listener_wifaces;
        }

        status = ucp_worker_iface_init(worker, tl_id,
                                       &listener->ifaces.wifaces[sockaddr_tls]);
        if ((status != UCS_OK) ||
            ((context->config.features & UCP_FEATURE_WAKEUP) &&
             !(listener->ifaces.wifaces[sockaddr_tls].attr.cap.flags &
               UCT_IFACE_FLAG_CB_ASYNC))) {
            ucp_worker_iface_cleanup(&listener->ifaces.wifaces[sockaddr_tls]);
            goto err_close_listener_wifaces;
        }

        port = listener->ifaces.wifaces[sockaddr_tls].attr.listen_port;

        sockaddr_tls++;
        listener->ifaces.num_wifaces = sockaddr_tls;
        ucs_trace("listener %p: accepting connections on %s on %s",
                  listener, tl_md->rsc.md_name,
                  ucs_sockaddr_str(iface_params.mode.sockaddr.listen_sockaddr.addr,
                                   saddr_str, sizeof(saddr_str)));
    }

    if (!sockaddr_tls) {
        ucs_error("none of the available transports can listen for connections on %s",
                  ucs_sockaddr_str(params->sockaddr.addr, saddr_str, sizeof(saddr_str)));
        listener->ifaces.num_wifaces = 0;
        status = UCS_ERR_UNREACHABLE;
        goto err_close_listener_wifaces;
    }

    return UCS_OK;

err_close_listener_wifaces:
    ucp_listener_close_ifaces(listener);
    return status;
}

ucs_status_t ucp_listener_create(ucp_worker_h worker,
                                 const ucp_listener_params_t *params,
                                 ucp_listener_h *listener_p)
{
    ucp_listener_h listener;
    ucs_status_t   status;

    if (!(params->field_mask & UCP_LISTENER_PARAM_FIELD_SOCK_ADDR)) {
        ucs_error("Missing sockaddr for listener");
        return UCS_ERR_INVALID_PARAM;
    }

    UCP_CHECK_PARAM_NON_NULL(params->sockaddr.addr, status, return status);

    if (ucs_test_all_flags(params->field_mask,
                           UCP_LISTENER_PARAM_FIELD_ACCEPT_HANDLER |
                           UCP_LISTENER_PARAM_FIELD_CONN_HANDLER)) {
        ucs_error("Only one accept handler should be provided");
        return UCS_ERR_INVALID_PARAM;
    }

    listener = ucs_calloc(1, sizeof(*listener), "ucp_listener");
    if (listener == NULL) {
        ucs_error("Can't allocate memory for UCP listener");
        return UCS_ERR_NO_MEMORY;
    }

    UCS_ASYNC_BLOCK(&worker->async);

    listener->worker = worker;

    if (params->field_mask & UCP_LISTENER_PARAM_FIELD_ACCEPT_HANDLER) {
        UCP_CHECK_PARAM_NON_NULL(params->accept_handler.cb, status,
                                 goto err_free_listener);
        listener->accept_cb = params->accept_handler.cb;
        listener->arg       = params->accept_handler.arg;
    } else if (params->field_mask & UCP_LISTENER_PARAM_FIELD_CONN_HANDLER) {
        UCP_CHECK_PARAM_NON_NULL(params->conn_handler.cb, status,
                                 goto err_free_listener);
        listener->conn_cb   = params->conn_handler.cb;
        listener->arg       = params->conn_handler.arg;
    }

    if (ucp_worker_sockaddr_is_cm_proto(worker)) {
        status = ucp_listen_on_cm(listener, params);
    } else {
        status = ucp_listen_on_iface(listener, params);
    }

    if (status == UCS_OK) {
        *listener_p = listener;
        goto out;
    }

err_free_listener:
    ucs_free(listener);
out:
    UCS_ASYNC_UNBLOCK(&worker->async);
    return status;
}

void ucp_listener_destroy(ucp_listener_h listener)
{
    ucs_trace("listener %p: destroying", listener);

    if (ucp_worker_sockaddr_is_cm_proto(listener->worker)) {
        ucp_listener_close_uct_listeners(listener);
    } else {
        ucp_listener_close_ifaces(listener);
    }

    ucs_free(listener);
}

ucs_status_t ucp_listener_reject(ucp_listener_h listener,
                                 ucp_conn_request_h conn_request)
{
    ucp_worker_h worker = listener->worker;

    UCS_ASYNC_BLOCK(&worker->async);

    uct_iface_reject(conn_request->uct_iface, conn_request->uct_req);

    UCS_ASYNC_UNBLOCK(&worker->async);

    ucs_free(conn_request);

    return UCS_OK;
}

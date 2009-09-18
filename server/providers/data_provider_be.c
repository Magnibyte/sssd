/*
   SSSD

   Data Provider Process

   Copyright (C) Simo Sorce <ssorce@redhat.com>	2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <dlfcn.h>

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include "popt.h"
#include "util/util.h"
#include "confdb/confdb.h"
#include "db/sysdb.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "util/btreemap.h"
#include "providers/dp_backend.h"
#include "monitor/monitor_interfaces.h"

#define BE_CONF_ENTRY "config/domains/%s"

struct sbus_method monitor_be_methods[] = {
    { MON_CLI_METHOD_PING, monitor_common_pong },
    { MON_CLI_METHOD_RES_INIT, monitor_common_res_init },
    { NULL, NULL }
};

struct sbus_interface monitor_be_interface = {
    MONITOR_INTERFACE,
    MONITOR_PATH,
    SBUS_DEFAULT_VTABLE,
    monitor_be_methods,
    NULL
};

static int be_check_online(DBusMessage *message, struct sbus_connection *conn);
static int be_get_account_info(DBusMessage *message, struct sbus_connection *conn);
static int be_pam_handler(DBusMessage *message, struct sbus_connection *conn);

struct sbus_method be_methods[] = {
    { DP_CLI_METHOD_ONLINE, be_check_online },
    { DP_CLI_METHOD_GETACCTINFO, be_get_account_info },
    { DP_CLI_METHOD_PAMHANDLER, be_pam_handler },
    { NULL, NULL }
};

struct sbus_interface be_interface = {
    DP_CLI_INTERFACE,
    DP_CLI_PATH,
    SBUS_DEFAULT_VTABLE,
    be_methods,
    NULL
};

static struct bet_data bet_data[] = {
    {BET_NULL, NULL, NULL},
    {BET_ID, "provider", "sssm_%s_init"},
    {BET_AUTH, "auth-module", "sssm_%s_auth_init"},
    {BET_ACCESS, "access-module", "sssm_%s_access_init"},
    {BET_CHPASS, "chpass-module", "sssm_%s_chpass_init"},
    {BET_MAX, NULL, NULL}
};

struct be_async_req {
    be_req_fn_t fn;
    struct be_req *req;
};

static void be_async_req_handler(struct tevent_context *ev,
                                 struct tevent_timer *te,
                                 struct timeval tv, void *pvt)
{
    struct be_async_req *async_req;

    async_req = talloc_get_type(pvt, struct be_async_req);

    async_req->fn(async_req->req);
}

static int be_file_request(struct be_ctx *ctx,
                           be_req_fn_t fn,
                           struct be_req *req)
{
    struct be_async_req *areq;
    struct tevent_timer *te;
    struct timeval tv;

    if (!fn || !req) return EINVAL;

    areq = talloc(req, struct be_async_req);
    if (!areq) {
        return ENOMEM;
    }
    areq->fn = fn;
    areq->req = req;

    /* fire immediately */
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    te = tevent_add_timer(ctx->ev, req, tv, be_async_req_handler, areq);
    if (te == NULL) {
        return EIO;
    }

    return EOK;
}


bool be_is_offline(struct be_ctx *ctx)
{
    time_t now = time(NULL);

    /* check if we are past the offline blackout timeout */
    /* FIXME: get offline_timeout from configuration */
    if (ctx->offstat.went_offline + 60 < now) {
        ctx->offstat.offline = false;
    }

    return ctx->offstat.offline;
}

void be_mark_offline(struct be_ctx *ctx)
{
    DEBUG(8, ("Going offline!\n"));

    ctx->offstat.went_offline = time(NULL);
    ctx->offstat.offline = true;
}

static int be_check_online(DBusMessage *message, struct sbus_connection *conn)
{
    struct be_ctx *ctx;
    DBusMessage *reply;
    DBusConnection *dbus_conn;
    dbus_bool_t dbret;
    void *user_data;
    dbus_uint16_t online;
    dbus_uint16_t err_maj = 0;
    dbus_uint32_t err_min = 0;
    static const char *err_msg = "Success";

    user_data = sbus_conn_get_private_data(conn);
    if (!user_data) return EINVAL;
    ctx = talloc_get_type(user_data, struct be_ctx);
    if (!ctx) return EINVAL;

    reply = dbus_message_new_method_return(message);
    if (!reply) return ENOMEM;

    if (be_is_offline(ctx)) {
        online = MOD_OFFLINE;
    } else {
        online = MOD_ONLINE;
    }

    dbret = dbus_message_append_args(reply,
                                     DBUS_TYPE_UINT16, &online,
                                     DBUS_TYPE_UINT16, &err_maj,
                                     DBUS_TYPE_UINT32, &err_min,
                                     DBUS_TYPE_STRING, &err_msg,
                                     DBUS_TYPE_INVALID);
    if (!dbret) {
        DEBUG(1, ("Failed to generate dbus reply\n"));
        return EIO;
    }

    dbus_conn = sbus_get_connection(ctx->dp_conn);
    dbus_connection_send(dbus_conn, reply, NULL);
    dbus_message_unref(reply);

    DEBUG(4, ("Request processed. Returned %d,%d,%s\n",
              err_maj, err_min, err_msg));

    return EOK;
}



static void acctinfo_callback(struct be_req *req, int status,
                              const char *errstr)
{
    DBusMessage *reply;
    DBusConnection *dbus_conn;
    dbus_bool_t dbret;
    dbus_uint16_t err_maj = 0;
    dbus_uint32_t err_min = 0;
    const char *err_msg = "Success";

    if (status != EOK) {
        err_maj = DP_ERR_FATAL;
        err_min = status;
        err_msg = errstr;
    }

    reply = (DBusMessage *)req->pvt;

    dbret = dbus_message_append_args(reply,
                                     DBUS_TYPE_UINT16, &err_maj,
                                     DBUS_TYPE_UINT32, &err_min,
                                     DBUS_TYPE_STRING, &err_msg,
                                     DBUS_TYPE_INVALID);
    if (!dbret) {
        DEBUG(1, ("Failed to generate dbus reply\n"));
        return;
    }

    dbus_conn = sbus_get_connection(req->be_ctx->dp_conn);
    dbus_connection_send(dbus_conn, reply, NULL);
    dbus_message_unref(reply);

    DEBUG(4, ("Request processed. Returned %d,%d,%s\n",
              err_maj, err_min, err_msg));

    /* finally free the request */
    talloc_free(req);
}

static int be_get_account_info(DBusMessage *message, struct sbus_connection *conn)
{
    struct be_acct_req *req;
    struct be_req *be_req;
    struct be_ctx *ctx;
    DBusMessage *reply;
    DBusError dbus_error;
    dbus_bool_t dbret;
    void *user_data;
    uint32_t type;
    char *attrs, *filter;
    int attr_type, filter_type;
    char *filter_val;
    int ret;
    dbus_uint16_t err_maj;
    dbus_uint32_t err_min;
    const char *err_msg;

    be_req = NULL;

    user_data = sbus_conn_get_private_data(conn);
    if (!user_data) return EINVAL;
    ctx = talloc_get_type(user_data, struct be_ctx);
    if (!ctx) return EINVAL;

    dbus_error_init(&dbus_error);

    ret = dbus_message_get_args(message, &dbus_error,
                                DBUS_TYPE_UINT32, &type,
                                DBUS_TYPE_STRING, &attrs,
                                DBUS_TYPE_STRING, &filter,
                                DBUS_TYPE_INVALID);
    if (!ret) {
        DEBUG(1,("Failed, to parse message!\n"));
        if (dbus_error_is_set(&dbus_error)) dbus_error_free(&dbus_error);
        return EIO;
    }

    DEBUG(4, ("Got request for [%u][%s][%s]\n", type, attrs, filter));

    reply = dbus_message_new_method_return(message);
    if (!reply) return ENOMEM;

    if (attrs) {
        if (strcmp(attrs, "core") == 0) attr_type = BE_ATTR_CORE;
        else if (strcmp(attrs, "membership") == 0) attr_type = BE_ATTR_MEM;
        else if (strcmp(attrs, "all") == 0) attr_type = BE_ATTR_ALL;
        else {
            err_maj = DP_ERR_FATAL;
            err_min = EINVAL;
            err_msg = "Invalid Attrs Parameter";
            goto done;
        }
    } else {
        err_maj = DP_ERR_FATAL;
        err_min = EINVAL;
        err_msg = "Missing Attrs Parameter";
        goto done;
    }

    if (filter) {
        if (strncmp(filter, "name=", 5) == 0) {
            filter_type = BE_FILTER_NAME;
            filter_val = &filter[5];
        } else if (strncmp(filter, "idnumber=", 9) == 0) {
            filter_type = BE_FILTER_IDNUM;
            filter_val = &filter[9];
        } else {
            err_maj = DP_ERR_FATAL;
            err_min = EINVAL;
            err_msg = "Invalid Filter";
            goto done;
        }
    } else {
        err_maj = DP_ERR_FATAL;
        err_min = EINVAL;
        err_msg = "Missing Filter Parameter";
        goto done;
    }

    /* process request */
    be_req = talloc(ctx, struct be_req);
    if (!be_req) {
        err_maj = DP_ERR_FATAL;
        err_min = ENOMEM;
        err_msg = "Out of memory";
        goto done;
    }
    be_req->be_ctx = ctx;
    be_req->fn = acctinfo_callback;
    be_req->pvt = reply;

    req = talloc(be_req, struct be_acct_req);
    if (!req) {
        err_maj = DP_ERR_FATAL;
        err_min = ENOMEM;
        err_msg = "Out of memory";
        goto done;
    }
    req->entry_type = type;
    req->attr_type = attr_type;
    req->filter_type = filter_type;
    req->filter_value = talloc_strdup(req, filter_val);

    be_req->req_data = req;

    ret = be_file_request(ctx, ctx->bet_info[BET_ID].bet_ops->handler, be_req);
    if (ret != EOK) {
        err_maj = DP_ERR_FATAL;
        err_min = ret;
        err_msg = "Failed to file request";
        goto done;
    }

    return EOK;

done:
    if (be_req) {
        talloc_free(be_req);
    }

    dbret = dbus_message_append_args(reply,
                                     DBUS_TYPE_UINT16, &err_maj,
                                     DBUS_TYPE_UINT32, &err_min,
                                     DBUS_TYPE_STRING, &err_msg,
                                     DBUS_TYPE_INVALID);
    if (!dbret) return EIO;

    DEBUG(4, ("Request processed. Returned %d,%d,%s\n",
              err_maj, err_min, err_msg));

    /* send reply back */
    sbus_conn_send_reply(conn, reply);
    dbus_message_unref(reply);

    return EOK;
}

static void be_pam_handler_callback(struct be_req *req, int status,
                                const char *errstr) {
    struct pam_data *pd;
    DBusMessage *reply;
    DBusConnection *dbus_conn;
    dbus_bool_t dbret;

    pd = talloc_get_type(req->req_data, struct pam_data);

    DEBUG(4, ("Sending result [%d][%s]\n", pd->pam_status, pd->domain));
    reply = (DBusMessage *)req->pvt;
    dbret = dp_pack_pam_response(reply, pd);
    if (!dbret) {
        DEBUG(1, ("Failed to generate dbus reply\n"));
        return;
    }

    dbus_conn = sbus_get_connection(req->be_ctx->dp_conn);
    dbus_connection_send(dbus_conn, reply, NULL);
    dbus_message_unref(reply);

    DEBUG(4, ("Sent result [%d][%s]\n", pd->pam_status, pd->domain));

    talloc_free(req);
}

static int be_pam_handler(DBusMessage *message, struct sbus_connection *conn)
{
    DBusError dbus_error;
    DBusMessage *reply;
    struct be_ctx *ctx;
    dbus_bool_t ret;
    void *user_data;
    struct pam_data *pd = NULL;
    struct be_req *be_req = NULL;
    uint32_t pam_status = PAM_SYSTEM_ERR;
    enum bet_type target = BET_NULL;

    user_data = sbus_conn_get_private_data(conn);
    if (!user_data) return EINVAL;
    ctx = talloc_get_type(user_data, struct be_ctx);
    if (!ctx) return EINVAL;

    reply = dbus_message_new_method_return(message);
    if (!reply) {
        DEBUG(1, ("dbus_message_new_method_return failed, cannot send reply.\n"));
        talloc_free(pd);
        return ENOMEM;
    }

    pd = talloc_zero(ctx, struct pam_data);
    if (!pd) return ENOMEM;

    dbus_error_init(&dbus_error);

    ret = dp_unpack_pam_request(message, pd, &dbus_error);
    if (!ret) {
        DEBUG(1,("Failed, to parse message!\n"));
        talloc_free(pd);
        return EIO;
    }

    DEBUG(4, ("Got request with the following data\n"));
    DEBUG_PAM_DATA(4, pd);

    switch (pd->cmd) {
        case SSS_PAM_AUTHENTICATE:
            target = BET_AUTH;
            break;
        case SSS_PAM_ACCT_MGMT:
            target = BET_ACCESS;
            break;
        case SSS_PAM_CHAUTHTOK:
            target = BET_CHPASS;
            break;
        default:
            DEBUG(7, ("Unsupported PAM command [%d].\n", pd->cmd));
            pam_status = PAM_SUCCESS;
            ret = EOK;
            goto done;
    }

    /* return an error if corresponding backend target is configured */
    if (!ctx->bet_info[target].bet_ops) {
        DEBUG(7, ("Undefined backend target.\n"));
        goto done;
    }

    be_req = talloc_zero(ctx, struct be_req);
    if (!be_req) {
        DEBUG(7, ("talloc_zero failed.\n"));
        goto done;
    }

    be_req->be_ctx = ctx;
    be_req->fn = be_pam_handler_callback;
    be_req->pvt = reply;
    be_req->req_data = pd;

    ret = be_file_request(ctx, ctx->bet_info[target].bet_ops->handler, be_req);
    if (ret != EOK) {
        DEBUG(7, ("be_file_request failed.\n"));
        goto done;
    }

    return EOK;

done:
    talloc_free(be_req);

    DEBUG(4, ("Sending result [%d][%s]\n", pam_status, ctx->domain->name));
    ret = dbus_message_append_args(reply,
                                   DBUS_TYPE_UINT32, &pam_status,
                                   DBUS_TYPE_STRING, &ctx->domain->name,
                                   DBUS_TYPE_INVALID);
    if (!ret) return EIO;

    /* send reply back immediately */
    sbus_conn_send_reply(conn, reply);
    dbus_message_unref(reply);

    talloc_free(pd);
    return EOK;
}

/* mon_cli_init
 * sbus channel to the monitor daemon */
static int mon_cli_init(struct be_ctx *ctx)
{
    char *sbus_address;
    int ret;

    /* Set up SBUS connection to the monitor */
    ret = monitor_get_sbus_address(ctx, ctx->cdb, &sbus_address);
    if (ret != EOK) {
        SYSLOG_ERROR("Could not locate monitor address.\n");
        return ret;
    }

    ret = sbus_client_init(ctx, ctx->ev, sbus_address,
                           &monitor_be_interface, &ctx->mon_conn,
                           NULL, ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to connect to monitor services.\n");
        return ret;
    }

    /* Identify ourselves to the monitor */
    ret = monitor_common_send_id(ctx->mon_conn,
                                 ctx->identity,
                                 DATA_PROVIDER_VERSION);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to identify to the monitor!\n");
        return ret;
    }

    return EOK;
}

static void be_cli_reconnect_init(struct sbus_connection *conn, int status, void *pvt);

/* be_cli_init
 * sbus channel to the data provider daemon */
static int be_cli_init(struct be_ctx *ctx)
{
    int ret, max_retries;
    char *sbus_address;

    /* Set up SBUS connection to the monitor */
    ret = dp_get_sbus_address(ctx, ctx->cdb, &sbus_address);
    if (ret != EOK) {
        SYSLOG_ERROR("Could not locate monitor address.\n");
        return ret;
    }

    ret = sbus_client_init(ctx, ctx->ev, sbus_address,
                           &be_interface, &ctx->dp_conn,
                           NULL, ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to connect to monitor services.\n");
        return ret;
    }

    /* Identify ourselves to the data provider */
    ret = dp_common_send_id(ctx->dp_conn,
                            DP_CLI_BACKEND, DATA_PROVIDER_VERSION,
                            "", ctx->domain->name);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to identify to the data provider!\n");
        return ret;
    }

    /* Enable automatic reconnection to the Data Provider */
    ret = confdb_get_int(ctx->cdb, ctx, SERVICE_CONF_ENTRY,
                         "reconnection_retries", 3, &max_retries);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to set up automatic reconnection\n");
        return ret;
    }

    sbus_reconnect_init(ctx->dp_conn, max_retries,
                        be_cli_reconnect_init, ctx);

    return EOK;
}

static int be_finalize(struct be_ctx *ctx);
static void be_shutdown(struct be_req *req, int status, const char *errstr);

static void be_cli_reconnect_init(struct sbus_connection *conn, int status, void *pvt)
{
    int ret;
    struct be_ctx *be_ctx = talloc_get_type(pvt, struct be_ctx);

    /* Did we reconnect successfully? */
    if (status == SBUS_RECONNECT_SUCCESS) {
        DEBUG(1, ("Reconnected to the Data Provider.\n"));

        /* Identify ourselves to the data provider */
        ret = dp_common_send_id(be_ctx->dp_conn,
                                DP_CLI_BACKEND, DATA_PROVIDER_VERSION,
                                "", be_ctx->domain->name);
        if (ret != EOK) {
            SYSLOG_ERROR("Failed to send id to the data provider!\n");
        } else {
            return;
        }
    }

    /* Handle failure */
    SYSLOG_ERROR("Could not reconnect to data provider.\n");

    /* Kill the backend and let the monitor restart it */
    ret = be_finalize(be_ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("Finalizing back-end failed with error [%d] [%s]\n",
                     ret, strerror(ret));
        be_shutdown(NULL, ret, NULL);
    }
}

static void be_shutdown(struct be_req *req, int status, const char *errstr)
{
    /* Nothing left to do but exit() */
    if (status == EOK)
        exit(0);

    /* Something went wrong in finalize */
    SYSLOG_ERROR("Finalizing auth module failed with error [%d] [%s]\n",
                 status, errstr ? : strerror(status));

    exit(1);
}

static void be_id_shutdown(struct be_req *req, int status, const char *errstr)
{
    struct be_req *shutdown_req;
    struct be_ctx *ctx;
    int ret;

    if (status != EOK) {
        /* Something went wrong in finalize */
        SYSLOG_ERROR("Finalizing auth module failed with error [%d] [%s]\n",
                     status, errstr ? : strerror(status));
    }

    ctx = req->be_ctx;

    /* Now shutdown the id module too */
    shutdown_req = talloc_zero(ctx, struct be_req);
    if (!shutdown_req) {
        ret = ENOMEM;
        goto fail;
    }

    shutdown_req->be_ctx = ctx;
    shutdown_req->fn = be_id_shutdown;

    shutdown_req->pvt = ctx->bet_info[BET_ID].pvt_bet_data;

    ret = be_file_request(ctx, ctx->bet_info[BET_ID].bet_ops->finalize, shutdown_req);
    if (ret == EOK)
        return;

fail:
    /* If we got here, we couldn't shut down cleanly. */
    be_shutdown(NULL, ret, NULL);
}

static int be_finalize(struct be_ctx *ctx)
{
    struct be_req *shutdown_req;
    int ret;

    shutdown_req = talloc_zero(ctx, struct be_req);
    if (!shutdown_req) {
        ret = ENOMEM;
        goto fail;
    }

    shutdown_req->be_ctx = ctx;
    shutdown_req->fn = be_id_shutdown;
    shutdown_req->pvt = ctx->bet_info[BET_AUTH].pvt_bet_data;

    ret = be_file_request(ctx, ctx->bet_info[BET_AUTH].bet_ops->finalize, shutdown_req);
    if (ret == EOK) return EOK;

fail:
    /* If we got here, we couldn't shut down cleanly. */
    SYSLOG_ERROR("ERROR: could not shut down cleanly.\n");
    return ret;
}

static void be_target_access_permit(struct be_req *be_req)
{
    struct pam_data *pd = talloc_get_type(be_req->req_data, struct pam_data);
    DEBUG(9, ("be_target_access_permit called, returning PAM_SUCCESS.\n"));

    pd->pam_status = PAM_SUCCESS;
    be_req->fn(be_req, PAM_SUCCESS, NULL);
}

static struct bet_ops be_target_access_permit_ops = {
    .check_online = NULL,
    .handler = be_target_access_permit,
    .finalize = NULL
};

static int load_backend_module(struct be_ctx *ctx,
                               enum bet_type bet_type,
                               struct bet_ops **be_ops,
                               void **be_pvt_data)
{
    TALLOC_CTX *tmp_ctx;
    int ret = EINVAL;
    bool already_loaded = false;
    int lb=0;
    char *mod_name = NULL;
    char *path = NULL;
    void *handle;
    char *mod_init_fn_name = NULL;
    bet_init_fn_t mod_init_fn = NULL;

    if (bet_type <= BET_NULL || bet_type >= BET_MAX ||
        bet_type != bet_data[bet_type].bet_type) {
        DEBUG(2, ("invalid bet_type or bet_data corrupted.\n"));
        return EINVAL;
    }

    tmp_ctx = talloc_new(ctx);
    if (!tmp_ctx) {
        DEBUG(7, ("talloc_new failed.\n"));
        return ENOMEM;
    }

    ret = confdb_get_string(ctx->cdb, tmp_ctx, ctx->conf_path,
                            bet_data[bet_type].option_name, NULL,
                            &mod_name);
    if (ret != EOK) {
        ret = EFAULT;
        goto done;
    }
    if (!mod_name) {
        ret = ENOENT;
        goto done;
    }

    mod_init_fn_name = talloc_asprintf(tmp_ctx,
                                       bet_data[bet_type].mod_init_fn_name_fmt,
                                       mod_name);
    if (mod_init_fn_name == NULL) {
        DEBUG(7, ("talloc_asprintf failed\n"));
        ret = ENOMEM;
        goto done;
    }


    lb = 0;
    while(ctx->loaded_be[lb].be_name != NULL) {
        if (strncmp(ctx->loaded_be[lb].be_name, mod_name,
                    strlen(mod_name)) == 0) {
            DEBUG(7, ("Backend [%s] already loaded.\n", mod_name));
            already_loaded = true;
            break;
        }

        ++lb;
        if (lb >= BET_MAX) {
            DEBUG(2, ("Backend context corrupted.\n"));
            return EINVAL;
        }
    }

    if (!already_loaded) {
        path = talloc_asprintf(tmp_ctx, "%s/libsss_%s.so",
                               DATA_PROVIDER_PLUGINS_PATH, mod_name);
        if (!path) {
            return ENOMEM;
        }

        DEBUG(7, ("Loading backend [%s] with path [%s].\n", mod_name, path));
        handle = dlopen(path, RTLD_NOW);
        if (!handle) {
            SYSLOG_ERROR("Unable to load %s module with path (%s), error: %s\n",
                         mod_name, path, dlerror());
            ret = ELIBACC;
            goto done;
        }

        ctx->loaded_be[lb].be_name = talloc_strdup(ctx, mod_name);
        ctx->loaded_be[lb].handle = handle;
    }

    mod_init_fn = (bet_init_fn_t)dlsym(ctx->loaded_be[lb].handle,
                                           mod_init_fn_name);
    if (mod_init_fn == NULL) {
        SYSLOG_ERROR("Unable to load init fn %s from module %s, error: %s\n",
                     mod_init_fn_name, mod_name, dlerror());
        ret = ELIBBAD;
        goto done;
    }

    ret = mod_init_fn(ctx, be_ops, be_pvt_data);
    if (ret != EOK) {
        SYSLOG_ERROR("Error (%d) in module (%s) initialization (%s)!\n",
                     ret, mod_name, mod_init_fn_name);
        goto done;
    }

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

/* Some providers are just aliases for more complicated settings,
 * rewrite the alias into the actual settings */
static int be_rewrite(struct be_ctx *ctx)
{
    int ret;
    const char *val[2];
    val[1] = NULL;
    char **get_values = NULL;

    /* "files" is a special case that means:
     *  provider = proxy
     *  libName  = files
     */
    ret = confdb_get_param(ctx->cdb, ctx, ctx->conf_path, "provider",
                           &get_values);
    if (ret != EOK) {
        DEBUG(1, ("Failed to read provider from confdb.\n"));
        return ret;
    }
    if (get_values[0] == NULL) {
        DEBUG(1, ("Missing provider.\n"));
        return EINVAL;
    }

    if (strcasecmp(get_values[0], "files") == 0) {
        DEBUG(5, ("Rewriting provider %s\n", get_values[0]));
        talloc_zfree(get_values);

        val[0] = "proxy";
        ret = confdb_add_param(ctx->cdb, true,
                               ctx->conf_path,
                               "provider",
                               val);
        if (ret) {
            return ret;
        }

        val[0] = "files";
        ret = confdb_add_param(ctx->cdb, true,
                               ctx->conf_path,
                               "libName",
                               val);
        if (ret) {
            return ret;
        }
    }

    return EOK;
}

int be_process_init(TALLOC_CTX *mem_ctx,
                    const char *be_domain,
                    struct tevent_context *ev,
                    struct confdb_ctx *cdb)
{
    struct be_ctx *ctx;
    int ret;

    ctx = talloc_zero(mem_ctx, struct be_ctx);
    if (!ctx) {
        SYSLOG_ERROR("fatal error initializing be_ctx\n");
        return ENOMEM;
    }
    ctx->ev = ev;
    ctx->cdb = cdb;
    ctx->identity = talloc_asprintf(ctx, "%%BE_%s", be_domain);
    ctx->conf_path = talloc_asprintf(ctx, "config/domains/%s", be_domain);
    if (!ctx->identity || !ctx->conf_path) {
        SYSLOG_ERROR("Out of memory!?\n");
        return ENOMEM;
    }

    ret = confdb_get_domain(cdb, be_domain, &ctx->domain);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error retrieving domain configuration\n");
        return ret;
    }

    ret = sysdb_domain_init(ctx, ev, ctx->domain, DB_PATH, &ctx->sysdb);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error opening cache database\n");
        return ret;
    }

    ret = mon_cli_init(ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error setting up monitor bus\n");
        return ret;
    }

    ret = be_cli_init(ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error setting up server bus\n");
        return ret;
    }

    ret = be_rewrite(ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("error rewriting provider types\n");
        return ret;
    }

    ret = load_backend_module(ctx, BET_ID,
                              &ctx->bet_info[BET_ID].bet_ops,
                              &ctx->bet_info[BET_ID].pvt_bet_data);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error initializing data providers\n");
        return ret;
    }

    ret = load_backend_module(ctx, BET_AUTH,
                              &ctx->bet_info[BET_AUTH].bet_ops,
                              &ctx->bet_info[BET_AUTH].pvt_bet_data);
    if (ret != EOK) {
        if (ret != ENOENT) {
            SYSLOG_ERROR("fatal error initializing data providers\n");
            return ret;
        }
        DEBUG(1, ("No authentication module provided for [%s] !!\n",
                  be_domain));
    }

    ret = load_backend_module(ctx, BET_ACCESS,
                              &ctx->bet_info[BET_ACCESS].bet_ops,
                              &ctx->bet_info[BET_ACCESS].pvt_bet_data);
    if (ret != EOK) {
        if (ret != ENOENT) {
            SYSLOG_ERROR("fatal error initializing data providers\n");
            return ret;
        }
        DEBUG(1, ("No access control module provided for [%s] "
                  "using be_target_access_permit!!\n", be_domain));
        ctx->bet_info[BET_ACCESS].bet_ops = &be_target_access_permit_ops;
        ctx->bet_info[BET_ACCESS].pvt_bet_data = NULL;
    }

    ret = load_backend_module(ctx, BET_CHPASS,
                              &ctx->bet_info[BET_CHPASS].bet_ops,
                              &ctx->bet_info[BET_CHPASS].pvt_bet_data);
    if (ret != EOK) {
        if (ret != ENOENT) {
            SYSLOG_ERROR("fatal error initializing data providers\n");
            return ret;
        }
        DEBUG(1, ("No change password module provided for [%s] !!\n",
                  be_domain));
    }

    return EOK;
}

int main(int argc, const char *argv[])
{
    int opt;
    poptContext pc;
    char *be_domain = NULL;
    char *srv_name = NULL;
    char *conf_entry = NULL;
    struct main_context *main_ctx;
    int ret;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        SSSD_MAIN_OPTS
        {"domain", 0, POPT_ARG_STRING, &be_domain, 0,
         "Domain of the information provider (mandatory)", NULL },
        POPT_TABLEEND
    };

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
        fprintf(stderr, "\nInvalid option %s: %s\n\n",
                  poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            return 1;
        }
    }

    if (be_domain == NULL) {
        fprintf(stderr, "\nMissing option, --domain is a mandatory option.\n\n");
            poptPrintUsage(pc, stderr, 0);
            return 1;
    }

    poptFreeContext(pc);

    /* set up things like debug , signals, daemonization, etc... */
    srv_name = talloc_asprintf(NULL, "sssd[be[%s]]", be_domain);
    if (!srv_name) return 2;

    conf_entry = talloc_asprintf(NULL, BE_CONF_ENTRY, be_domain);
    if (!conf_entry) return 2;

    /* enable syslog logging */
    openlog(srv_name, LOG_PID, LOG_DAEMON);

    ret = server_setup(srv_name, 0, conf_entry, &main_ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("Could not set up mainloop [%d]\n", ret);
        return 2;
    }

    ret = die_if_parent_died();
    if (ret != EOK) {
        /* This is not fatal, don't return */
        DEBUG(2, ("Could not set up to exit when parent process does\n"));
    }

    ret = be_process_init(main_ctx,
                          be_domain,
                          main_ctx->event_ctx,
                          main_ctx->confdb_ctx);
    if (ret != EOK) {
        SYSLOG_ERROR("Could not initialize backend [%d]\n", ret);
        return 3;
    }

    DEBUG(1, ("Backend provider (%s) started!\n", be_domain));

    /* loop on main */
    server_loop(main_ctx);

    /* close syslog */
    closelog();

    return 0;
}


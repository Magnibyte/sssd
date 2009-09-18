/*
   SSSD

   NSS Responder

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

#include "popt.h"
#include "util/util.h"
#include "responder/nss/nsssrv.h"
#include "responder/nss/nsssrv_nc.h"
#include "db/sysdb.h"
#include "confdb/confdb.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "util/btreemap.h"
#include "responder/common/responder_packet.h"
#include "providers/data_provider.h"
#include "monitor/monitor_interfaces.h"
#include "sbus/sbus_client.h"

#define SSS_NSS_PIPE_NAME "nss"

#define PRG_NAME "sssd[nss]"

static int service_reload(DBusMessage *message, struct sbus_connection *conn);

struct sbus_method monitor_nss_methods[] = {
    { MON_CLI_METHOD_PING, monitor_common_pong },
    { MON_CLI_METHOD_RELOAD, service_reload },
    { MON_CLI_METHOD_RES_INIT, monitor_common_res_init },
    { NULL, NULL }
};

struct sbus_interface monitor_nss_interface = {
    MONITOR_INTERFACE,
    MONITOR_PATH,
    SBUS_DEFAULT_VTABLE,
    monitor_nss_methods,
    NULL
};

static int service_reload(DBusMessage *message, struct sbus_connection *conn)
{
    /* Monitor calls this function when we need to reload
     * our configuration information. Perform whatever steps
     * are needed to update the configuration objects.
     */

    /* Send an empty reply to acknowledge receipt */
    return monitor_common_pong(message, conn);
}

static int nss_get_config(struct nss_ctx *nctx,
                          struct resp_ctx *rctx,
                          struct confdb_ctx *cdb)
{
    TALLOC_CTX *tmpctx;
    struct sss_domain_info *dom;
    char *domain, *name;
    char **filter_list;
    int ret, i;

    tmpctx = talloc_new(nctx);
    if (!tmpctx) return ENOMEM;

    ret = confdb_get_int(cdb, nctx, NSS_SRV_CONFIG,
                         "EnumCacheTimeout", 120,
                         &nctx->enum_cache_timeout);
    if (ret != EOK) goto done;

    ret = confdb_get_int(cdb, nctx, NSS_SRV_CONFIG,
                         "EntryCacheTimeout", 600,
                         &nctx->cache_timeout);
    if (ret != EOK) goto done;

    ret = confdb_get_int(cdb, nctx, NSS_SRV_CONFIG,
                         "EntryNegativeTimeout", 15,
                         &nctx->neg_timeout);
    if (ret != EOK) goto done;

    ret = confdb_get_bool(cdb, nctx, NSS_SRV_CONFIG,
                         "filterUsersInGroups", true,
                         &nctx->filter_users_in_groups);
    if (ret != EOK) goto done;


    ret = confdb_get_int(cdb, nctx, NSS_SRV_CONFIG,
                         "EntryCacheNoWaitRefreshTimeout", 0,
                         &nctx->cache_refresh_timeout);
    if (ret != EOK) goto done;
    if (nctx->cache_refresh_timeout >= nctx->cache_timeout) {
        SYSLOG_ERROR("Configuration error: EntryCacheNoWaitRefreshTimeout exceeds"
                     "EntryCacheTimeout. Disabling feature.\n");
        nctx->cache_refresh_timeout = 0;
    }
    if (nctx->cache_refresh_timeout < 0) {
        SYSLOG_ERROR("Configuration error: EntryCacheNoWaitRefreshTimeout is"
                     "invalid. Disabling feature.\n");
        nctx->cache_refresh_timeout = 0;
    }

    ret = confdb_get_string_as_list(cdb, tmpctx, NSS_SRV_CONFIG,
                                    "filterUsers", &filter_list);
    if (ret == ENOENT) filter_list = NULL;
    else if (ret != EOK) goto done;

    for (i = 0; (filter_list && filter_list[i]); i++) {
        ret = sss_parse_name(tmpctx, nctx->rctx->names,
                             filter_list[i], &domain, &name);
        if (ret != EOK) {
            DEBUG(1, ("Invalid name in filterUsers list: [%s] (%d)\n",
                     filter_list[i], ret));
            continue;
        }
        if (domain) {
            ret = nss_ncache_set_user(nctx->ncache, true, domain, name);
            if (ret != EOK) {
                DEBUG(1, ("Failed to store permanent user filter for [%s]"
                          " (%d [%s])\n", filter_list[i],
                          ret, strerror(ret)));
                continue;
            }
        } else {
            for (dom = rctx->domains; dom; dom = dom->next) {
                ret = nss_ncache_set_user(nctx->ncache, true, dom->name, name);
                if (ret != EOK) {
                   DEBUG(1, ("Failed to store permanent user filter for"
                             " [%s:%s] (%d [%s])\n",
                             dom->name, filter_list[i],
                             ret, strerror(ret)));
                    continue;
                }
            }
        }
    }

    ret = confdb_get_string_as_list(cdb, tmpctx, NSS_SRV_CONFIG,
                                    "filterGroups", &filter_list);
    if (ret == ENOENT) filter_list = NULL;
    else if (ret != EOK) goto done;

    for (i = 0; filter_list[i]; i++) {
        ret = sss_parse_name(tmpctx, nctx->rctx->names,
                             filter_list[i], &domain, &name);
        if (ret != EOK) {
            DEBUG(1, ("Invalid name in filterGroups list: [%s] (%d)\n",
                     filter_list[i], ret));
            continue;
        }
        if (domain) {
            ret = nss_ncache_set_group(nctx->ncache, true, domain, name);
            if (ret != EOK) {
                DEBUG(1, ("Failed to store permanent group filter for"
                          " [%s] (%d [%s])\n", filter_list[i],
                          ret, strerror(ret)));
                continue;
            }
        } else {
            for (dom = rctx->domains; dom; dom = dom->next) {
                ret = nss_ncache_set_group(nctx->ncache, true, dom->name, name);
                if (ret != EOK) {
                   DEBUG(1, ("Failed to store permanent group filter for"
                             " [%s:%s] (%d [%s])\n",
                             dom->name, filter_list[i],
                             ret, strerror(ret)));
                    continue;
                }
            }
        }
    }

done:
    talloc_free(tmpctx);
    return ret;
}

static void nss_shutdown(struct resp_ctx *rctx)
{
    /* TODO: Do clean-up here */

    /* Nothing left to do but exit() */
    exit(0);
}

static struct sbus_method nss_dp_methods[] = {
    { NULL, NULL }
};

struct sbus_interface nss_dp_interface = {
    DP_CLI_INTERFACE,
    DP_CLI_PATH,
    SBUS_DEFAULT_VTABLE,
    nss_dp_methods,
    NULL
};


static void nss_dp_reconnect_init(struct sbus_connection *conn,
                                  int status, void *pvt)
{
    struct resp_ctx *rctx = talloc_get_type(pvt, struct resp_ctx);
    int ret;

    /* Did we reconnect successfully? */
    if (status == SBUS_RECONNECT_SUCCESS) {
        DEBUG(1, ("Reconnected to the Data Provider.\n"));

        /* Identify ourselves to the data provider */
        ret = dp_common_send_id(conn,
                                DP_CLI_FRONTEND,
                                DATA_PROVIDER_VERSION,
                                "NSS", "");
        /* all fine */
        if (ret == EOK) return;
    }

    /* Failed to reconnect */
    SYSLOG_ERROR("Could not reconnect to data provider.\n");
    /* Kill the backend and let the monitor restart it */
    nss_shutdown(rctx);
}

int nss_process_init(TALLOC_CTX *mem_ctx,
                     struct tevent_context *ev,
                     struct confdb_ctx *cdb)
{
    struct sss_cmd_table *nss_cmds;
    struct nss_ctx *nctx;
    int ret, max_retries;

    nctx = talloc_zero(mem_ctx, struct nss_ctx);
    if (!nctx) {
        SYSLOG_ERROR("fatal error initializing nss_ctx\n");
        return ENOMEM;
    }

    ret = nss_ncache_init(nctx, &nctx->ncache);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error initializing negative cache\n");
        return ret;
    }

    nss_cmds = get_nss_cmds();

    ret = sss_process_init(nctx, ev, cdb,
                           nss_cmds,
                           SSS_NSS_SOCKET_NAME, NULL,
                           NSS_SRV_CONFIG,
                           NSS_SBUS_SERVICE_NAME,
                           NSS_SBUS_SERVICE_VERSION,
                           &monitor_nss_interface,
                           DP_CLI_FRONTEND,
                           DATA_PROVIDER_VERSION,
                           "NSS", "",
                           &nss_dp_interface,
                           &nctx->rctx);
    if (ret != EOK) {
        return ret;
    }
    nctx->rctx->pvt_ctx = nctx;

    ret = nss_get_config(nctx, nctx->rctx, cdb);
    if (ret != EOK) {
        SYSLOG_ERROR("fatal error getting nss config\n");
        return ret;
    }

    /* Enable automatic reconnection to the Data Provider */
    ret = confdb_get_int(nctx->rctx->cdb, nctx->rctx,
                         SERVICE_CONF_ENTRY,
                         "reconnection_retries", 3, &max_retries);
    if (ret != EOK) {
        SYSLOG_ERROR("Failed to set up automatic reconnection\n");
        return ret;
    }

    sbus_reconnect_init(nctx->rctx->dp_conn,
                        max_retries,
                        nss_dp_reconnect_init, nctx->rctx);

    DEBUG(1, ("NSS Initialization complete\n"));

    return EOK;
}

int main(int argc, const char *argv[])
{
    int opt;
    poptContext pc;
    struct main_context *main_ctx;
    int ret;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        SSSD_MAIN_OPTS
        { NULL }
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

    poptFreeContext(pc);

    /* enable syslog logging */
    openlog(PRG_NAME, LOG_PID, LOG_DAEMON);

    /* set up things like debug , signals, daemonization, etc... */
    ret = server_setup(PRG_NAME, 0, NSS_SRV_CONFIG, &main_ctx);
    if (ret != EOK) return 2;

    ret = die_if_parent_died();
    if (ret != EOK) {
        /* This is not fatal, don't return */
        DEBUG(2, ("Could not set up to exit when parent process does\n"));
    }

    ret = nss_process_init(main_ctx,
                           main_ctx->event_ctx,
                           main_ctx->confdb_ctx);
    if (ret != EOK) return 3;

    /* loop on main */
    server_loop(main_ctx);

    /* close syslog */
    closelog();

    return 0;
}


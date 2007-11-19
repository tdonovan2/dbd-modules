/*  MOD_VHOST_DBD     Apache 2.3+ module

Copyright 2007  Tom Donovan

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "apr_strings.h"
#include "apr_lib.h"
#include "apu.h"
#include "apr_dbd.h"
#include "apu_version.h"
#include "httpd.h"
#include "http_request.h"
#include "http_log.h"
#include "http_config.h"
#include "mod_dbd.h"

module AP_MODULE_DECLARE_DATA vhost_dbd_module;

/* Maximum number of parameters which can be passed to the query */
#define MAX_PARAMS  100

/* This string is used in the conn notes to indicate that no docroot was 
 * returned. This string must be a value which will never be returned as 
 * the docroot by the query. Hyphen and tab \t are included because they are 
 * unlikely in directory names. Alas, nothing (except NULL and /) is really 
 * prohibited in unix filenames.
 */
#define NO_DOCROOT "-\t.\t"

/* The key for the saved docroot must use a delimiter char which cannot be in 
 * hostname, ftpuser, or uri.  ap_escape_logitem and ap_escape_uri ensure that
 * this will never happen.
 */
#define KEY_DELIMITER "\t"

/* vhost_dbd_module server configuration */
typedef struct {    
    const char *label;                /* DBD prepared statement label */
    const char *sql;                  /* SQL statement (or label from DBDPrepareSQL) */
    int nparams;                /* number of params */
    int params[MAX_PARAMS];     /* array of param codes */
    int urisegs[MAX_PARAMS];    /* number of URI segments to use (0=all) */
} vhost_dbd_conf;

/* parameter codes */
enum {hostname, ip, port, uri, ftpuser} paramNames; 

/* optional functions imported from mod_dbd */
static APR_OPTIONAL_FN_TYPE(ap_dbd_prepare) *dbd_prepare_fn = NULL;
static APR_OPTIONAL_FN_TYPE(ap_dbd_acquire) *dbd_acquire_fn = NULL;

/* check if a string could be a label name */
#define MAX_LABEL_SIZE 32
static APR_INLINE int isSimpleName(const char *s) 
{   
    if (strlen(s) > MAX_LABEL_SIZE)
        return 0;
    for (; *s ; s++)
        if (!apr_isalnum(*s) && (*s != '_') && (*s != '-'))
            return 0;
    return 1;
}

static int setDocRoot(request_rec *r)
{
    request_rec *mainreq = r;
    apr_dbd_results_t *res;
    apr_dbd_row_t *row;
    ap_dbd_t *dbd;
    apr_dbd_prepared_t *stmt;
    apr_dbd_prepared_t *prestmt;
    int rows = 0;
    int cols = 0;
    int rv = 0;
    int i, j;
    const char **params;
    const char *newroot;
    const char *keyUri = NULL;
    const char *trimmedUri;
    const char *keyHostname = NULL;
    const char *keyFTPuser = NULL;
    const char *key;
    const char *start;
    int maxseg = 0;
    vhost_dbd_conf *conf = 
        (vhost_dbd_conf *) ap_get_module_config(r->server->module_config, 
                                                &vhost_dbd_module);

    if (conf->label == NULL)
        return DECLINED;
    if (r->proxyreq)
        return HTTP_FORBIDDEN;
    if (!r->uri || ((r->uri[0] != '/') && strcmp(r->uri, "*"))) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                     "mod_vhost_dbd: Invalid URI in request %s", r->the_request);
        return HTTP_BAD_REQUEST;
    }
    params = (const char **) apr_pcalloc(r->pool, conf->nparams * sizeof(char *));

    /* Use the top-level request for FTPuser, IP, and Port */
    while(mainreq->main)
        mainreq = mainreq->main;    

    /* Collect the parameters.  Make sure hostname and uri cannot surprise the
     * database server with unexpected characaters (e.g. control characters) 
     * We (ab)use ap_escape_logitem to prevent this kind of trouble.
     */
    for (i = 0 ; i < conf->nparams ; i++) {
        switch( conf->params[i] ) {
            case hostname :
                keyHostname = ap_escape_logitem(r->pool, r->hostname);
                params[i] = keyHostname;
                break;
            case ip :
                params[i] = mainreq->connection->local_ip;
                break;
            case port :
                params[i] = apr_itoa(r->pool, mainreq->connection->local_addr->port);
                break;
            case ftpuser :
                keyFTPuser = ap_escape_logitem(r->pool, mainreq->user);
                params[i] = keyFTPuser;
                break;
            case uri :
                start = ap_escape_uri(r->pool, r->uri);
                if ( (j = conf->urisegs[i]) && start) {
                    /* extract j leading URI segments */
                    const char *p = start;
                    while(j && *p) 
                        if (*++p == '/') --j;
                    params[i] = apr_pstrndup(r->pool, start, p - start);
                    if (conf->urisegs[i] > maxseg) {
                        maxseg = conf->urisegs[i];
                        keyUri = params[i];
                    }
                }
                else {
                    /* use the whole URI */
                    params[i] = start;
                    maxseg = 10;
                    keyUri = start;
                }
        }
    }
    /* Can we just use a saved result for this connection? 
     * Only a change in the hostname or the part of the URI we are using requires
     * a new query for this connection.  Use empty string instead of NULL in 
     * key if no hostname. Tabs are illegal in hostname or uri, so use \t as the 
     * delimiter.
     */
    key = apr_pstrcat(r->pool, "DBD:vhostKey=", 
                              keyHostname ? keyHostname : "",
                        "\t", keyFTPuser ? keyFTPuser : "",
                        "\t", keyUri, NULL);
    if ( !(newroot = apr_table_get(r->connection->notes, key))) {

        /* we need to execute a query */
        if ((dbd = dbd_acquire_fn(r)) == NULL) {
            ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r,
                "mod_vhost_dbd: Error acquiring connection to database");
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        if ((stmt = apr_hash_get(dbd->prepared, conf->label, 
                                 APR_HASH_KEY_STRING)) == NULL) {
            ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r,
                "mod_vhost_dbd: Unable to retrieve prepared statement %s", 
                conf->label);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        /* conf->sql might just be a label created with DBDPrepareSQL, not SQL */
        if (isSimpleName(conf->sql) 
            && (prestmt = apr_hash_get(dbd->prepared, conf->sql, APR_HASH_KEY_STRING)))
                stmt = prestmt;

        if (rv = apr_dbd_pselect(dbd->driver, r->pool, dbd->handle, &res, stmt, 
                                0, conf->nparams, params)) {
            ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r,
                "mod_vhost_dbd: Unable to execute SQL statement: %s", 
                apr_dbd_error(dbd->driver, dbd->handle, rv));
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        rows = apr_dbd_num_tuples(dbd->driver, res);
        if (rows > 1) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                "mod_vhost_dbd: Returned multiple (%d) rows (stmt: %s)", 
                rows, conf->label );
            /* Flush the extra rows */
            while (!apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1))
                continue;
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        if (!rows) {
            /* use an illegal-hostname string as key to save empty response */
            apr_table_set(r->connection->notes, key, NO_DOCROOT);

            /* DEBUG loglevel */
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "mod_vhost_dbd: Executed: (stmt: %s) returned %d rows, DocRoot unset",
                conf->label, rows);
            return DECLINED;
        }

        cols = apr_dbd_num_cols(dbd->driver, res);
        if (!cols) {
            ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r,
                "mod_vhost_dbd: SQL statement returned no columns");
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        if (apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
            if (rows < 0)
                /* Some drivers cannot report the number of rows and return rows = -1.
                 * In this case, we didn't learn there were no rows until now.
                 */
                return DECLINED;

            ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r,
                "mod_vhost_dbd: Unable to fetch 1st row of %d rows (stmt %s): %s", 
                rows, conf->label, apr_dbd_error(dbd->driver, dbd->handle, rv));
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        newroot = apr_dbd_get_entry(dbd->driver, row, 0);
        if (!newroot || !(*newroot)) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                "mod_vhost_dbd: Replacement DocRoot is empty");
            return HTTP_FORBIDDEN;
        }
#if APU_MAJOR_VERSION > 1 || (APU_MAJOR_VERSION == 1 && APU_MINOR_VERSION >= 3)
        /* set any extra columns as env variables - unset if NULL or zero-length value */
        for (j = 1; j < cols ; j++) {
            int k;

            const char *name = apr_dbd_get_name(dbd->driver, res, j);
            const char *val = apr_dbd_get_entry(dbd->driver, row, j);
            if (name && *name) {
                char *str = apr_pstrdup(r->pool, name);
                for (k=0 ; str[k]; k++)
                    if (!apr_isalnum(str[k])) 
                        str[k] = '_'; 

                if (val && *val)
                    apr_table_set(r->subprocess_env, str, val);
                else
                    apr_table_unset(r->subprocess_env, str);
            }
        }
#endif
        /* DEBUG loglevel */
        if (r->server->loglevel == APLOG_DEBUG) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "mod_vhost_dbd: Successfully executed query: (stmt: %s) "
                "returned %d row(s) %d column(s), key: [%s], "
                "setting DocRoot to: %s",
                conf->label, rows, cols, key, newroot);

            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "mod_vhost_dbd: Hostname: %s, IP: %s, Port: %s, URI: %s ",
                ap_escape_logitem(r->pool, r->hostname),
                mainreq->connection->local_ip,
                apr_itoa(r->pool, mainreq->connection->local_addr->port),
                ap_escape_uri(r->pool, r->uri)
                );
        }
        /* fetch until -1 return to make sure results set gets cleaned up */
        while (!apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1))
            continue;
    }
    else if (r->server->loglevel == APLOG_DEBUG) {
        /* we do not need to execute a query - use the newroot from connection->notes */

        /* DEBUG loglevel */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "mod_vhost_dbd: Using previous connection query (stmt: %s) "
            "key: [%s], setting DocRoot to: %s",
            conf->label, key, newroot);

        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "mod_vhost_dbd: Hostname: %s, IP: %s, Port: %s, URI: %s ",
                ap_escape_logitem(r->pool, r->hostname),
                mainreq->connection->local_ip,
                apr_itoa(r->pool, mainreq->connection->local_addr->port),
                ap_escape_uri(r->pool, r->uri)
                );
    }

    if (!strcmp(newroot, NO_DOCROOT))
        return DECLINED;

    trimmedUri = r->uri;
    while (*trimmedUri == '/')
        ++trimmedUri;

    if (apr_filepath_merge(&r->filename, newroot, trimmedUri, 
                            APR_FILEPATH_TRUENAME | APR_FILEPATH_SECUREROOT,
                            r->pool)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                 "mod_vhost_dbd: Cannot map %s to file with DocRoot %s", 
                 r->the_request, newroot);
        return HTTP_FORBIDDEN;
    }
    else {
        /* got a good doc root - set it and save the result for this conn */
        r->canonical_filename = r->filename;
        apr_table_set(r->connection->notes, key, newroot);
    }

    return (!rv) ? OK : HTTP_BAD_REQUEST;
}

/* process DBDocRoot directive */
static const char *setVhostQuery(cmd_parms *cmd, void *mconfig, 
                                     const char *sql, const char *paramName)
{   
    static long label_num = 0;
    vhost_dbd_conf *conf = 
        (vhost_dbd_conf *) ap_get_module_config(cmd->server->module_config,
                                              &vhost_dbd_module);
    if (!dbd_prepare_fn || !dbd_acquire_fn)
        return "mod_dbd must be enabled to use mod_vhost_dbd";

    if (conf->nparams >= MAX_PARAMS)
        return "mod_vhost_dbd: Too many parameters";

    if (!apr_strnatcasecmp(paramName, "hostname"))
        conf->params[conf->nparams] = hostname;
    else if (!apr_strnatcasecmp(paramName, "ip"))
        conf->params[conf->nparams] = ip;
    else if (!apr_strnatcasecmp(paramName, "port"))
        conf->params[conf->nparams] = port;
    /* FTPUSER is only available for mod_ftp - currently un-doc'd */
    else if (!apr_strnatcasecmp(paramName, "ftpuser")  
             && ap_find_linked_module("mod_ftp.c"))
        conf->params[conf->nparams] = ftpuser;
    else { 
        char *uricmd = apr_pstrndup(cmd->pool, paramName, 3);
        if (!apr_strnatcasecmp(uricmd, "uri")
            && (        paramName[3]=='\0'
                    || (apr_isdigit(paramName[3]) && paramName[4]=='\0')
                )
            ) {
            conf->params[conf->nparams] = uri;
            conf->urisegs[conf->nparams] = atoi(paramName+3);
        }
        else 
            return apr_pstrcat(cmd->pool, 
                        "mod_vhost_dbd: invalid parameter name: ",
                        paramName, NULL);
    }
    ++conf->nparams;

    if (!conf->label) {
        conf->label = apr_pstrcat(cmd->pool, "vhost_dbd_", 
                                       apr_ltoa(cmd->pool, ++label_num), NULL);
        dbd_prepare_fn(cmd->server, sql, conf->label);
        conf->sql = sql;

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, cmd->server,
            "mod_vhost_dbd: Prepared query (stmt: %s) from: %s",
            conf->label, sql);
    }
    return NULL;
}

static void *merge_config_server(apr_pool_t *p, void *parentconf, 
                                 void *newconf)
{   vhost_dbd_conf *base = (vhost_dbd_conf *) parentconf;
    vhost_dbd_conf *add = (vhost_dbd_conf *) newconf;

    return (add->label) ? add : base;
}

static void *config_server(apr_pool_t *p, server_rec *s)
{   
    vhost_dbd_conf *conf = apr_pcalloc(p, sizeof(vhost_dbd_conf));
    if (!dbd_prepare_fn) {
        dbd_prepare_fn = 
                    APR_RETRIEVE_OPTIONAL_FN(ap_dbd_prepare);
        dbd_acquire_fn = 
                    APR_RETRIEVE_OPTIONAL_FN(ap_dbd_acquire);
    }
    return conf;
}
static void register_hooks(apr_pool_t *p)
{
    ap_hook_translate_name(setDocRoot,NULL,NULL,APR_HOOK_LAST);
}
static const command_rec cmds[] =
{
    AP_INIT_ITERATE2("DBDocRoot", setVhostQuery, 
        NULL, RSRC_CONF, 
        "DBDocRoot  QUERY  [HOSTNAME|IP|PORT|URI[n]]..."),
        {NULL}
};

module AP_MODULE_DECLARE_DATA vhost_dbd_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-dir config */
    NULL,                       /* merge per-dir config */
    config_server,              /* server config */
    merge_config_server,        /* merge server config */
    cmds,                       /* command apr_table_t */
    register_hooks              /* register hooks */
};

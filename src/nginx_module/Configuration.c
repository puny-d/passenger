/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) 2007 Manlio Perillo (manlio.perillo@gmail.com)
 * Copyright (c) 2010-2017 Phusion Holding B.V.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <sys/types.h>
#include <pwd.h>
#include <stdlib.h>
#include <assert.h>

#include "ngx_http_passenger_module.h"
#include "Configuration.h"
#include "ContentHandler.h"
#include "ConfigurationSetters.c"
#include "CreateMainConfig.c"
#include "CreateLocationConfig.c"
#include "cxx_supportlib/Constants.h"
#include "cxx_supportlib/JsonTools/CBindings.h"
#include "cxx_supportlib/FileTools/PathManipCBindings.h"
#include "cxx_supportlib/vendor-modified/modp_b64.h"


static ngx_str_t headers_to_hide[] = {
    /* NOTE: Do not hide the "Status" header; some broken HTTP clients
     * expect this header. http://code.google.com/p/phusion-passenger/issues/detail?id=177
     */
    ngx_string("X-Accel-Expires"),
    ngx_string("X-Accel-Redirect"),
    ngx_string("X-Accel-Limit-Rate"),
    ngx_string("X-Accel-Buffering"),
    ngx_null_string
};

passenger_main_conf_t passenger_main_conf;

typedef struct postprocess_ctx_s postprocess_ctx_t;

static ngx_path_init_t  ngx_http_proxy_temp_path = {
    ngx_string(NGX_HTTP_PROXY_TEMP_PATH), { 1, 2, 0 }
};

static void init_manifest_config_entry(PsgJsonValue *entry);
static PsgJsonValue *find_or_create_manifest_global_config_entry(
    postprocess_ctx_t *ctx, const char *option_name,
    size_t option_name_len);
static PsgJsonValue *find_or_create_manifest_application_config_entry(
    postprocess_ctx_t *ctx, ngx_str_t *app_group_name,
    const char *option_name, size_t option_name_len);
static PsgJsonValue *find_or_create_manifest_location_config_entry(
    postprocess_ctx_t *ctx, ngx_str_t *app_group_name,
    ngx_http_core_srv_conf_t *cscf, ngx_http_core_loc_conf_t *clcf,
    const char *option_name, size_t option_name_len);
static PsgJsonValue *add_manifest_config_option_hierarchy_member(
    PsgJsonValue *config_entry, ngx_str_t *source_file, ngx_uint_t source_line);
static void add_manifest_config_entry_dynamic_default(
    PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len,
    const char *desc, size_t desc_len);
static void add_manifest_config_entry_static_default_str(
    PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len,
    const char *value, size_t value_len);
static void add_manifest_config_entry_static_default_int(
    PsgJsonValue *global_config, const char *option_name,
    size_t option_name_len, int value);
static void add_manifest_config_entry_static_default_uint(
    PsgJsonValue *global_config, const char *option_name,
    size_t option_name_len, unsigned int value);
static void add_manifest_config_entry_static_default_bool(
    PsgJsonValue *global_config, const char *option_name,
    size_t option_name_len, int value);
static void postprocess_location_conf(postprocess_ctx_t *ctx,
    passenger_loc_conf_t *passenger_conf);
static ngx_int_t merge_headers(ngx_conf_t *cf, passenger_loc_conf_t *conf,
    passenger_loc_conf_t *prev);
static ngx_int_t merge_string_array(ngx_conf_t *cf, ngx_array_t **prev,
    ngx_array_t **conf);
static ngx_int_t merge_string_keyval_table(ngx_conf_t *cf, ngx_array_t **prev,
    ngx_array_t **conf);


#include "SetConfigDefaults.c"
#include "TrackMainConfig.c"
#include "MergeLocationConfig.c"
#include "TrackLocationConfig.c"


void *
passenger_create_main_conf(ngx_conf_t *cf)
{
    passenger_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(passenger_main_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->default_ruby.data = NULL;
    conf->default_ruby.len = 0;

    passenger_create_autogenerated_main_conf(&conf->autogenerated);

    return conf;
}

char *
passenger_init_main_conf(ngx_conf_t *cf, void *conf_pointer)
{
    passenger_main_conf_t *conf;
    struct passwd         *user_entry;
    struct group          *group_entry;
    char buf[128];

    conf = &passenger_main_conf;
    *conf = *((passenger_main_conf_t *) conf_pointer);

    if (conf->autogenerated.abort_on_startup_error == NGX_CONF_UNSET) {
        conf->autogenerated.abort_on_startup_error = 0;
    }

    if (conf->autogenerated.show_version_in_header == NGX_CONF_UNSET) {
        conf->autogenerated.show_version_in_header = 1;
    }

    if (conf->autogenerated.default_user.len == 0) {
        conf->autogenerated.default_user.len  = sizeof(DEFAULT_WEB_APP_USER) - 1;
        conf->autogenerated.default_user.data = (u_char *) DEFAULT_WEB_APP_USER;
    }
    if (conf->autogenerated.default_user.len > sizeof(buf) - 1) {
        return "Value for 'passenger_default_user' is too long.";
    }
    memcpy(buf, conf->autogenerated.default_user.data, conf->autogenerated.default_user.len);
    buf[conf->autogenerated.default_user.len] = '\0';
    user_entry = getpwnam(buf);
    if (user_entry == NULL) {
        return "The user specified by the 'passenger_default_user' option does not exist.";
    }

    if (conf->autogenerated.default_group.len > 0) {
        if (conf->autogenerated.default_group.len > sizeof(buf) - 1) {
            return "Value for 'passenger_default_group' is too long.";
        }
        memcpy(buf, conf->autogenerated.default_group.data, conf->autogenerated.default_group.len);
        buf[conf->autogenerated.default_group.len] = '\0';
        group_entry = getgrnam(buf);
        if (group_entry == NULL) {
            return "The group specified by the 'passenger_default_group' option does not exist.";
        }
    }

    return NGX_CONF_OK;
}

void *
passenger_create_loc_conf(ngx_conf_t *cf)
{
    passenger_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(passenger_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream_config.bufs.num = 0;
     *     conf->upstream_config.next_upstream = 0;
     *     conf->upstream_config.temp_path = NULL;
     *     conf->upstream_config.hide_headers_hash = { NULL, 0 };
     *     conf->upstream_config.hide_headers = NULL;
     *     conf->upstream_config.pass_headers = NULL;
     *     conf->upstream_config.uri = { 0, NULL };
     *     conf->upstream_config.location = NULL;
     *     conf->upstream_config.store_lengths = NULL;
     *     conf->upstream_config.store_values = NULL;
     */

    if (ngx_array_init(&conf->children, cf->pool, 8,
                       sizeof(passenger_loc_conf_t *))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (cf->conf_file == NULL) {
        conf->context_source_file.data = (u_char *) NULL;
        conf->context_source_file.len = 0;
        conf->context_source_line = 0;
    } else if (cf->conf_file->file.fd == NGX_INVALID_FILE) {
        conf->context_source_file.data = (u_char *) "(command line)";
        conf->context_source_file.len = sizeof("(command line)") - 1;
        conf->context_source_line = 0;
    } else {
        conf->context_source_file = cf->conf_file->file.name;
        conf->context_source_line = cf->conf_file->line;
    }

    conf->cscf = NULL;
    conf->clcf = NULL;

    passenger_create_autogenerated_loc_conf(&conf->autogenerated);

    /******************************/
    /******************************/

    conf->upstream_config.pass_headers = NGX_CONF_UNSET_PTR;
    conf->upstream_config.hide_headers = NGX_CONF_UNSET_PTR;

    conf->upstream_config.store = NGX_CONF_UNSET;
    conf->upstream_config.store_access = NGX_CONF_UNSET_UINT;
    #if NGINX_VERSION_NUM >= 1007005
        conf->upstream_config.next_upstream_tries = NGX_CONF_UNSET_UINT;
    #endif
    conf->upstream_config.buffering = NGX_CONF_UNSET;
    conf->upstream_config.ignore_client_abort = NGX_CONF_UNSET;
    #if NGINX_VERSION_NUM >= 1007007
        conf->upstream_config.force_ranges = NGX_CONF_UNSET;
    #endif

    conf->upstream_config.local = NGX_CONF_UNSET_PTR;

    conf->upstream_config.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream_config.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream_config.read_timeout = NGX_CONF_UNSET_MSEC;
    #if NGINX_VERSION_NUM >= 1007005
        conf->upstream_config.next_upstream_timeout = NGX_CONF_UNSET_MSEC;
    #endif

    conf->upstream_config.send_lowat = NGX_CONF_UNSET_SIZE;
    conf->upstream_config.buffer_size = NGX_CONF_UNSET_SIZE;
    #if NGINX_VERSION_NUM >= 1007007
        conf->upstream_config.limit_rate = NGX_CONF_UNSET_SIZE;
    #endif

    conf->upstream_config.busy_buffers_size_conf = NGX_CONF_UNSET_SIZE;
    conf->upstream_config.max_temp_file_size_conf = NGX_CONF_UNSET_SIZE;
    conf->upstream_config.temp_file_write_size_conf = NGX_CONF_UNSET_SIZE;

    conf->upstream_config.pass_request_headers = NGX_CONF_UNSET;
    conf->upstream_config.pass_request_body = NGX_CONF_UNSET;

#if (NGX_HTTP_CACHE)
    #if NGINX_VERSION_NUM >= 1007009
        conf->upstream_config.cache = NGX_CONF_UNSET;
    #else
        conf->upstream_config.cache = NGX_CONF_UNSET_PTR;
    #endif
    conf->upstream_config.cache_min_uses = NGX_CONF_UNSET_UINT;
    conf->upstream_config.cache_bypass = NGX_CONF_UNSET_PTR;
    conf->upstream_config.no_cache = NGX_CONF_UNSET_PTR;
    conf->upstream_config.cache_valid = NGX_CONF_UNSET_PTR;
    conf->upstream_config.cache_lock = NGX_CONF_UNSET;
    conf->upstream_config.cache_lock_timeout = NGX_CONF_UNSET_MSEC;
    #if NGINX_VERSION_NUM >= 1007008
        conf->upstream_config.cache_lock_age = NGX_CONF_UNSET_MSEC;
    #endif
    #if NGINX_VERSION_NUM >= 1006000
        conf->upstream_config.cache_revalidate = NGX_CONF_UNSET;
    #endif
#endif

    conf->upstream_config.intercept_errors = NGX_CONF_UNSET;

    conf->upstream_config.cyclic_temp_file = 0;
    conf->upstream_config.change_buffering = 1;

    ngx_str_set(&conf->upstream_config.module, "passenger");

    conf->options_cache.data  = NULL;
    conf->options_cache.len   = 0;
    conf->env_vars_cache.data = NULL;
    conf->env_vars_cache.len  = 0;

    return conf;
}

#include "CacheLocationConfig.c"

static ngx_int_t
cache_loc_conf_options(ngx_conf_t *cf, passenger_loc_conf_t *conf)
{
    ngx_uint_t     i;
    ngx_keyval_t  *env_vars;
    size_t         unencoded_len;
    u_char        *unencoded_buf;

    if (passenger_cache_autogenerated_location_part(cf, conf) == 0) {
        return NGX_ERROR;
    }

    if (conf->autogenerated.env_vars != NULL) {
        size_t len = 0;
        u_char *buf;
        u_char *pos;

        /* Cache env vars data as base64-serialized string.
         * First, calculate the length of the unencoded data.
         */

        unencoded_len = 0;
        env_vars = (ngx_keyval_t *) conf->autogenerated.env_vars->elts;

        for (i = 0; i < conf->autogenerated.env_vars->nelts; i++) {
            unencoded_len += env_vars[i].key.len + 1 + env_vars[i].value.len + 1;
        }

        /* Create the unecoded data. */

        unencoded_buf = pos = (u_char *) malloc(unencoded_len);
        if (unencoded_buf == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "cannot allocate buffer of %z bytes for environment variables data",
                               unencoded_len);
            return NGX_ERROR;
        }

        for (i = 0; i < conf->autogenerated.env_vars->nelts; i++) {
            pos = ngx_copy(pos, env_vars[i].key.data, env_vars[i].key.len);
            *pos = '\0';
            pos++;

            pos = ngx_copy(pos, env_vars[i].value.data, env_vars[i].value.len);
            *pos = '\0';
            pos++;
        }

        assert((size_t) (pos - unencoded_buf) == unencoded_len);

        /* Create base64-serialized string. */

        buf = ngx_palloc(cf->pool, modp_b64_encode_len(unencoded_len));
        if (buf == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "cannot allocate buffer of %z bytes for base64 encoding",
                               modp_b64_encode_len(unencoded_len));
            return NGX_ERROR;
        }
        len = modp_b64_encode((char *) buf, (const char *) unencoded_buf, unencoded_len);
        if (len == (size_t) -1) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "error during base64 encoding");
            free(unencoded_buf);
            return NGX_ERROR;
        }

        conf->env_vars_cache.data = buf;
        conf->env_vars_cache.len = len;
        free(unencoded_buf);
    }

    return NGX_OK;
}

char *
passenger_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    passenger_loc_conf_t         *prev = parent;
    passenger_loc_conf_t         *conf = child;
    passenger_loc_conf_t         **children_elem;
    ngx_http_core_loc_conf_t     *clcf;

    size_t                        size;
    ngx_hash_init_t               hash;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    /* The following works for all contexts within the http{} block, but does
     * not work for the http{} block itself. To obtain the ngx_http_core_(loc|srv)_conf_t
     * associated with the http{} block itself, we also set conf->(cscf|clcf)
     * from record_loc_conf_source_location(), which is called from the various
     * configuration setter functions.
     */
     conf->cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);
     conf->clcf = clcf;

    if (passenger_merge_autogenerated_loc_conf(&conf->autogenerated, &prev->autogenerated, cf) == 0) {
        return NGX_CONF_ERROR;
    }

    children_elem = ngx_array_push(&prev->children);
    if (children_elem == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
            "cannot allocate memory");
        return NGX_CONF_ERROR;
    }
    *children_elem = conf;

    if (prev->options_cache.data == NULL) {
        if (cache_loc_conf_options(cf, prev) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "cannot create " PROGRAM_NAME " configuration cache");
            return NGX_CONF_ERROR;
        }
    }

    /******************************/
    /******************************/

    #if (NGX_HTTP_CACHE) && NGINX_VERSION_NUM >= 1007009
        if (conf->upstream_config.store > 0) {
            conf->upstream_config.cache = 0;
        }
        if (conf->upstream_config.cache > 0) {
            conf->upstream_config.store = 0;
        }
    #endif

    #if NGINX_VERSION_NUM >= 1007009
        if (conf->upstream_config.store == NGX_CONF_UNSET) {
            ngx_conf_merge_value(conf->upstream_config.store,
                                      prev->upstream_config.store, 0);

            conf->upstream_config.store_lengths = prev->upstream_config.store_lengths;
            conf->upstream_config.store_values = prev->upstream_config.store_values;
        }
    #else
        if (conf->upstream_config.store != 0) {
            ngx_conf_merge_value(conf->upstream_config.store,
                                      prev->upstream_config.store, 0);

            if (conf->upstream_config.store_lengths == NULL) {
                conf->upstream_config.store_lengths = prev->upstream_config.store_lengths;
                conf->upstream_config.store_values = prev->upstream_config.store_values;
            }
        }
    #endif

    ngx_conf_merge_uint_value(conf->upstream_config.store_access,
                              prev->upstream_config.store_access, 0600);

    #if NGINX_VERSION_NUM >= 1007005
        ngx_conf_merge_uint_value(conf->upstream_config.next_upstream_tries,
                                  prev->upstream_config.next_upstream_tries, 0);
    #endif

    ngx_conf_merge_value(conf->upstream_config.buffering,
                         prev->upstream_config.buffering, 0);

    ngx_conf_merge_value(conf->upstream_config.ignore_client_abort,
                         prev->upstream_config.ignore_client_abort, 0);

    #if NGINX_VERSION_NUM >= 1007007
        ngx_conf_merge_value(conf->upstream_config.force_ranges,
                             prev->upstream_config.force_ranges, 0);
    #endif

    ngx_conf_merge_ptr_value(conf->upstream_config.local,
                             prev->upstream_config.local, NULL);

    ngx_conf_merge_msec_value(conf->upstream_config.connect_timeout,
                              prev->upstream_config.connect_timeout, 12000000);

    ngx_conf_merge_msec_value(conf->upstream_config.send_timeout,
                              prev->upstream_config.send_timeout, 12000000);

    ngx_conf_merge_msec_value(conf->upstream_config.read_timeout,
                              prev->upstream_config.read_timeout, 12000000);

    #if NGINX_VERSION_NUM >= 1007005
        ngx_conf_merge_msec_value(conf->upstream_config.next_upstream_timeout,
                                  prev->upstream_config.next_upstream_timeout, 0);
    #endif

    ngx_conf_merge_size_value(conf->upstream_config.send_lowat,
                              prev->upstream_config.send_lowat, 0);

    ngx_conf_merge_size_value(conf->upstream_config.buffer_size,
                              prev->upstream_config.buffer_size,
                              16 * 1024);

    #if NGINX_VERSION_NUM >= 1007007
        ngx_conf_merge_size_value(conf->upstream_config.limit_rate,
                                  prev->upstream_config.limit_rate, 0);
    #endif


    ngx_conf_merge_bufs_value(conf->upstream_config.bufs, prev->upstream_config.bufs,
                              8, 16 * 1024);

    if (conf->upstream_config.bufs.num < 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "there must be at least 2 \"passenger_buffers\"");
        return NGX_CONF_ERROR;
    }


    size = conf->upstream_config.buffer_size;
    if (size < conf->upstream_config.bufs.size) {
        size = conf->upstream_config.bufs.size;
    }


    ngx_conf_merge_size_value(conf->upstream_config.busy_buffers_size_conf,
                              prev->upstream_config.busy_buffers_size_conf,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream_config.busy_buffers_size_conf == NGX_CONF_UNSET_SIZE) {
        conf->upstream_config.busy_buffers_size = 2 * size;
    } else {
        conf->upstream_config.busy_buffers_size =
                                         conf->upstream_config.busy_buffers_size_conf;
    }

    if (conf->upstream_config.busy_buffers_size < size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"passenger_busy_buffers_size\" must be equal to or greater "
             "than the maximum of the value of \"passenger_buffer_size\" and "
             "one of the \"passenger_buffers\"");

        return NGX_CONF_ERROR;
    }

    if (conf->upstream_config.busy_buffers_size
        > (conf->upstream_config.bufs.num - 1) * conf->upstream_config.bufs.size)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"passenger_busy_buffers_size\" must be less than "
             "the size of all \"passenger_buffers\" minus one buffer");

        return NGX_CONF_ERROR;
    }


    ngx_conf_merge_size_value(conf->upstream_config.temp_file_write_size_conf,
                              prev->upstream_config.temp_file_write_size_conf,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream_config.temp_file_write_size_conf == NGX_CONF_UNSET_SIZE) {
        conf->upstream_config.temp_file_write_size = 2 * size;
    } else {
        conf->upstream_config.temp_file_write_size =
                                      conf->upstream_config.temp_file_write_size_conf;
    }

    if (conf->upstream_config.temp_file_write_size < size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"passenger_temp_file_write_size\" must be equal to or greater than "
             "the maximum of the value of \"passenger_buffer_size\" and "
             "one of the \"passenger_buffers\"");

        return NGX_CONF_ERROR;
    }


    ngx_conf_merge_size_value(conf->upstream_config.max_temp_file_size_conf,
                              prev->upstream_config.max_temp_file_size_conf,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream_config.max_temp_file_size_conf == NGX_CONF_UNSET_SIZE) {
        conf->upstream_config.max_temp_file_size = 1024 * 1024 * 1024;
    } else {
        conf->upstream_config.max_temp_file_size =
                                        conf->upstream_config.max_temp_file_size_conf;
    }

    if (conf->upstream_config.max_temp_file_size != 0
        && conf->upstream_config.max_temp_file_size < size)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"passenger_max_temp_file_size\" must be equal to zero to disable "
             "temporary files usage or must be equal to or greater than "
             "the maximum of the value of \"passenger_buffer_size\" and "
             "one of the \"passenger_buffers\"");

        return NGX_CONF_ERROR;
    }

    ngx_conf_merge_bitmask_value(conf->upstream_config.ignore_headers,
                                 prev->upstream_config.ignore_headers,
                                 NGX_CONF_BITMASK_SET);

    ngx_conf_merge_bitmask_value(conf->upstream_config.next_upstream,
                              prev->upstream_config.next_upstream,
                              (NGX_CONF_BITMASK_SET
                               |NGX_HTTP_UPSTREAM_FT_ERROR
                               |NGX_HTTP_UPSTREAM_FT_TIMEOUT));

    if (conf->upstream_config.next_upstream & NGX_HTTP_UPSTREAM_FT_OFF) {
        conf->upstream_config.next_upstream = NGX_CONF_BITMASK_SET
                                       |NGX_HTTP_UPSTREAM_FT_OFF;
    }

    ngx_conf_merge_path_value(cf,
                              &conf->upstream_config.temp_path,
                              prev->upstream_config.temp_path,
                              &ngx_http_proxy_temp_path);

#if (NGX_HTTP_CACHE)

    #if NGINX_VERSION_NUM >= 1007009
        if (conf->upstream_config.cache == NGX_CONF_UNSET) {
           ngx_conf_merge_value(conf->upstream_config.cache,
                                prev->upstream_config.cache, 0);

           conf->upstream_config.cache_zone = prev->upstream_config.cache_zone;
           conf->upstream_config.cache_value = prev->upstream_config.cache_value;
        }

        if (conf->upstream_config.cache_zone && conf->upstream_config.cache_zone->data == NULL) {
            ngx_shm_zone_t  *shm_zone;

            shm_zone = conf->upstream_config.cache_zone;

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"scgi_cache\" zone \"%V\" is unknown",
                               &shm_zone->shm.name);

            return NGX_CONF_ERROR;
        }
    #else
        ngx_conf_merge_ptr_value(conf->upstream_config.cache,
                                 prev->upstream_config.cache, NULL);

        if (conf->upstream_config.cache && conf->upstream_config.cache->data == NULL) {
            ngx_shm_zone_t  *shm_zone;

            shm_zone = conf->upstream_config.cache;

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"scgi_cache\" zone \"%V\" is unknown",
                               &shm_zone->shm.name);

            return NGX_CONF_ERROR;
        }
    #endif

    ngx_conf_merge_uint_value(conf->upstream_config.cache_min_uses,
                              prev->upstream_config.cache_min_uses, 1);

    ngx_conf_merge_bitmask_value(conf->upstream_config.cache_use_stale,
                              prev->upstream_config.cache_use_stale,
                              (NGX_CONF_BITMASK_SET
                               | NGX_HTTP_UPSTREAM_FT_OFF));

    if (conf->upstream_config.cache_use_stale & NGX_HTTP_UPSTREAM_FT_OFF) {
        conf->upstream_config.cache_use_stale = NGX_CONF_BITMASK_SET
                                                | NGX_HTTP_UPSTREAM_FT_OFF;
    }

    if (conf->upstream_config.cache_use_stale & NGX_HTTP_UPSTREAM_FT_ERROR) {
        conf->upstream_config.cache_use_stale |= NGX_HTTP_UPSTREAM_FT_NOLIVE;
    }

    if (conf->upstream_config.cache_methods == 0) {
        conf->upstream_config.cache_methods = prev->upstream_config.cache_methods;
    }

    conf->upstream_config.cache_methods |= NGX_HTTP_GET | NGX_HTTP_HEAD;

    ngx_conf_merge_ptr_value(conf->upstream_config.cache_bypass,
                             prev->upstream_config.cache_bypass, NULL);

    ngx_conf_merge_ptr_value(conf->upstream_config.no_cache,
                             prev->upstream_config.no_cache, NULL);

    ngx_conf_merge_ptr_value(conf->upstream_config.cache_valid,
                             prev->upstream_config.cache_valid, NULL);

    if (conf->cache_key.value.data == NULL) {
        conf->cache_key = prev->cache_key;
    }

    ngx_conf_merge_value(conf->upstream_config.cache_lock,
                         prev->upstream_config.cache_lock, 0);

    ngx_conf_merge_msec_value(conf->upstream_config.cache_lock_timeout,
                              prev->upstream_config.cache_lock_timeout, 5000);

    ngx_conf_merge_value(conf->upstream_config.cache_revalidate,
                         prev->upstream_config.cache_revalidate, 0);

    #if NGINX_VERSION_NUM >= 1007008
        ngx_conf_merge_msec_value(conf->upstream_config.cache_lock_age,
                                  prev->upstream_config.cache_lock_age, 5000);
    #endif

    #if NGINX_VERSION_NUM >= 1006000
        ngx_conf_merge_value(conf->upstream_config.cache_revalidate,
                             prev->upstream_config.cache_revalidate, 0);
    #endif

#endif

    ngx_conf_merge_value(conf->upstream_config.pass_request_headers,
                         prev->upstream_config.pass_request_headers, 1);
    ngx_conf_merge_value(conf->upstream_config.pass_request_body,
                         prev->upstream_config.pass_request_body, 1);

    ngx_conf_merge_value(conf->upstream_config.intercept_errors,
                         prev->upstream_config.intercept_errors, 0);


    hash.max_size = 512;
    hash.bucket_size = ngx_align(64, ngx_cacheline_size);
    hash.name = "passenger_hide_headers_hash";

    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream_config,
            &prev->upstream_config, headers_to_hide, &hash)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (conf->upstream_config.upstream == NULL) {
        conf->upstream_config.upstream = prev->upstream_config.upstream;
    }

    if (conf->autogenerated.enabled == 1 /* and not NGX_CONF_UNSET */
     && passenger_main_conf.autogenerated.root_dir.len != 0
     && clcf->handler == NULL /* no handler set by other modules */)
    {
        clcf->handler = passenger_content_handler;
    }

    conf->autogenerated.headers_hash_bucket_size = ngx_align(
        conf->autogenerated.headers_hash_bucket_size,
        ngx_cacheline_size);
    hash.max_size = conf->autogenerated.headers_hash_max_size;
    hash.bucket_size = conf->autogenerated.headers_hash_bucket_size;
    hash.name = "passenger_headers_hash";

    if (merge_headers(cf, conf, prev) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cache_loc_conf_options(cf, conf) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cannot create " PROGRAM_NAME " configuration cache");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
merge_headers(ngx_conf_t *cf, passenger_loc_conf_t *conf, passenger_loc_conf_t *prev)
{
    u_char                       *p;
    size_t                        size;
    uintptr_t                    *code;
    ngx_uint_t                    i;
    ngx_array_t                   headers_names, headers_merged;
    ngx_keyval_t                 *src, *s;
    ngx_hash_key_t               *hk;
    ngx_hash_init_t               hash;
    ngx_http_script_compile_t     sc;
    ngx_http_script_copy_code_t  *copy;

    if (conf->autogenerated.headers_source == NULL) {
        conf->flushes = prev->flushes;
        conf->headers_set_len = prev->headers_set_len;
        conf->headers_set = prev->headers_set;
        conf->headers_set_hash = prev->headers_set_hash;
        conf->autogenerated.headers_source = prev->autogenerated.headers_source;
    }

    if (conf->headers_set_hash.buckets
#if (NGX_HTTP_CACHE)
    #if NGINX_VERSION_NUM >= 1007009
        && ((conf->upstream_config.cache == NGX_CONF_UNSET) == (prev->upstream_config.cache == NGX_CONF_UNSET))
    #else
        && ((conf->upstream_config.cache == NGX_CONF_UNSET_PTR) == (prev->upstream_config.cache == NGX_CONF_UNSET_PTR))
    #endif
#endif
       )
    {
        return NGX_OK;
    }


    if (ngx_array_init(&headers_names, cf->temp_pool, 4, sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&headers_merged, cf->temp_pool, 4, sizeof(ngx_keyval_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (conf->autogenerated.headers_source == NULL) {
        conf->autogenerated.headers_source = ngx_array_create(cf->pool, 4,
                                                sizeof(ngx_keyval_t));
        if (conf->autogenerated.headers_source == NULL) {
            return NGX_ERROR;
        }
    }

    conf->headers_set_len = ngx_array_create(cf->pool, 64, 1);
    if (conf->headers_set_len == NULL) {
        return NGX_ERROR;
    }

    conf->headers_set = ngx_array_create(cf->pool, 512, 1);
    if (conf->headers_set == NULL) {
        return NGX_ERROR;
    }


    src = conf->autogenerated.headers_source->elts;
    for (i = 0; i < conf->autogenerated.headers_source->nelts; i++) {

        s = ngx_array_push(&headers_merged);
        if (s == NULL) {
            return NGX_ERROR;
        }

        *s = src[i];
    }


    src = headers_merged.elts;
    for (i = 0; i < headers_merged.nelts; i++) {

        hk = ngx_array_push(&headers_names);
        if (hk == NULL) {
            return NGX_ERROR;
        }

        hk->key = src[i].key;
        hk->key_hash = ngx_hash_key_lc(src[i].key.data, src[i].key.len);
        hk->value = (void *) 1;

        if (src[i].value.len == 0) {
            continue;
        }

        if (ngx_http_script_variables_count(&src[i].value) == 0) {
            copy = ngx_array_push_n(conf->headers_set_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                 ngx_http_script_copy_len_code;
            copy->len = src[i].key.len + sizeof(": ") - 1
                        + src[i].value.len + sizeof(CRLF) - 1;


            size = (sizeof(ngx_http_script_copy_code_t)
                       + src[i].key.len + sizeof(": ") - 1
                       + src[i].value.len + sizeof(CRLF) - 1
                       + sizeof(uintptr_t) - 1)
                    & ~(sizeof(uintptr_t) - 1);

            copy = ngx_array_push_n(conf->headers_set, size);
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = ngx_http_script_copy_code;
            copy->len = src[i].key.len + sizeof(": ") - 1
                        + src[i].value.len + sizeof(CRLF) - 1;

            p = (u_char *) copy + sizeof(ngx_http_script_copy_code_t);

            p = ngx_cpymem(p, src[i].key.data, src[i].key.len);
            *p++ = ':'; *p++ = ' ';
            p = ngx_cpymem(p, src[i].value.data, src[i].value.len);
            *p++ = CR; *p = LF;

        } else {
            copy = ngx_array_push_n(conf->headers_set_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                 ngx_http_script_copy_len_code;
            copy->len = src[i].key.len + sizeof(": ") - 1;


            size = (sizeof(ngx_http_script_copy_code_t)
                    + src[i].key.len + sizeof(": ") - 1 + sizeof(uintptr_t) - 1)
                    & ~(sizeof(uintptr_t) - 1);

            copy = ngx_array_push_n(conf->headers_set, size);
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = ngx_http_script_copy_code;
            copy->len = src[i].key.len + sizeof(": ") - 1;

            p = (u_char *) copy + sizeof(ngx_http_script_copy_code_t);
            p = ngx_cpymem(p, src[i].key.data, src[i].key.len);
            *p++ = ':'; *p = ' ';


            ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

            sc.cf = cf;
            sc.source = &src[i].value;
            sc.flushes = &conf->flushes;
            sc.lengths = &conf->headers_set_len;
            sc.values = &conf->headers_set;

            if (ngx_http_script_compile(&sc) != NGX_OK) {
                return NGX_ERROR;
            }


            copy = ngx_array_push_n(conf->headers_set_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                 ngx_http_script_copy_len_code;
            copy->len = sizeof(CRLF) - 1;


            size = (sizeof(ngx_http_script_copy_code_t)
                    + sizeof(CRLF) - 1 + sizeof(uintptr_t) - 1)
                    & ~(sizeof(uintptr_t) - 1);

            copy = ngx_array_push_n(conf->headers_set, size);
            if (copy == NULL) {
                return NGX_ERROR;
            }

            copy->code = ngx_http_script_copy_code;
            copy->len = sizeof(CRLF) - 1;

            p = (u_char *) copy + sizeof(ngx_http_script_copy_code_t);
            *p++ = CR; *p = LF;
        }

        code = ngx_array_push_n(conf->headers_set_len, sizeof(uintptr_t));
        if (code == NULL) {
            return NGX_ERROR;
        }

        *code = (uintptr_t) NULL;

        code = ngx_array_push_n(conf->headers_set, sizeof(uintptr_t));
        if (code == NULL) {
            return NGX_ERROR;
        }

        *code = (uintptr_t) NULL;
    }

    code = ngx_array_push_n(conf->headers_set_len, sizeof(uintptr_t));
    if (code == NULL) {
        return NGX_ERROR;
    }

    *code = (uintptr_t) NULL;


    hash.hash = &conf->headers_set_hash;
    hash.key = ngx_hash_key_lc;
    hash.max_size = conf->autogenerated.headers_hash_max_size;
    hash.bucket_size = conf->autogenerated.headers_hash_bucket_size;
    hash.name = "passenger_headers_hash";
    hash.pool = cf->pool;
    hash.temp_pool = NULL;

    return ngx_hash_init(&hash, headers_names.elts, headers_names.nelts);
}

static ngx_int_t
merge_string_array(ngx_conf_t *cf, ngx_array_t **prev, ngx_array_t **conf)
{
    ngx_str_t  *prev_elems, *elem;
    ngx_uint_t  i;

    if (*prev != NGX_CONF_UNSET_PTR) {
        if (*conf == NGX_CONF_UNSET_PTR) {
            *conf = ngx_array_create(cf->pool, 4, sizeof(ngx_str_t));
            if (*conf == NULL) {
                return NGX_ERROR;
            }
        }

        prev_elems = (ngx_str_t *) (*prev)->elts;
        for (i = 0; i < (*prev)->nelts; i++) {
            elem = (ngx_str_t *) ngx_array_push(*conf);
            if (elem == NULL) {
                return NGX_ERROR;
            }
            *elem = prev_elems[i];
        }
    }

    return NGX_OK;
}

struct postprocess_ctx_s {
    ngx_conf_t *cf;
    PsgJsonValue *json;
    PsgJsonValueIterator *it, *end;
    PsgJsonValueIterator *it2, *end2;
    PsgJsonValueIterator *it3, *end3;
    PsgJsonValueIterator *it4, *end4;
};

static void
init_manifest(PsgJsonValue *manifest) {
    PsgJsonValue *empty_object;

    empty_object = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_value(manifest, "global_configuration", -1, empty_object);
    psg_json_value_set_value(manifest, "default_application_configuration", -1, empty_object);
    psg_json_value_set_value(manifest, "default_location_configuration", -1, empty_object);
    psg_json_value_set_value(manifest, "application_configuration", -1, empty_object);
    psg_json_value_free(empty_object);
}

static void
init_manifest_app_config_container(PsgJsonValue *container) {
    PsgJsonValue *empty_object, *empty_array;
    empty_object = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    empty_array = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);
    psg_json_value_set_value(container, "options", -1, empty_object);
    psg_json_value_set_value(container, "default_location_configuration", -1, empty_object);
    psg_json_value_set_value(container, "locations", -1, empty_array);
    psg_json_value_free(empty_object);
    psg_json_value_free(empty_array);
}

static void
init_manifest_config_entry(PsgJsonValue *entry) {
    PsgJsonValue *empty_array = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);
    psg_json_value_set_value(entry, "value_hierarchy", -1, empty_array);
    psg_json_value_free(empty_array);
}

static int
matches_any_server_names(postprocess_ctx_t *ctx, ngx_http_core_srv_conf_t *cscf, PsgJsonValue *server_names_doc) {
    ngx_http_server_name_t *server_names = cscf->server_names.elts;
    PsgJsonValue *server_name_doc;
    ngx_str_t server_name;
    ngx_uint_t i;

    psg_json_value_begin(server_names_doc, ctx->it2);
    psg_json_value_end(server_names_doc, ctx->end2);

    while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
        server_name_doc = psg_json_value_iterator_get_value(ctx->it2);
        server_name.data = (u_char *) psg_json_value_get_str(server_name_doc, &server_name.len);

        for (i = 0; i < cscf->server_names.nelts; i++) {
            if (server_names[i].name.len == server_name.len
                && ngx_strncasecmp(server_names[i].name.data, server_name.data,
                                   server_name.len) == 0)
            {
                return 1;
            }
        }

        psg_json_value_iterator_advance(ctx->it2);
    }

    return 0;
}

static PsgJsonValue *
find_matching_location_entry(postprocess_ctx_t *ctx, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf, PsgJsonValue *app_config)
{
    PsgJsonValue *locations_doc;
    PsgJsonValue *location_doc;
    PsgJsonValue *vhost_doc;
    PsgJsonValue *server_names_doc;
    PsgJsonValue *location_matcher_doc;
    ngx_str_t json_location_matcher_type, json_location_matcher_value;

    locations_doc = psg_json_value_get(app_config, "locations", -1);
    psg_json_value_begin(locations_doc, ctx->it);
    psg_json_value_end(locations_doc, ctx->end);

    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        location_doc = psg_json_value_iterator_get_value(ctx->it);
        vhost_doc = psg_json_value_get(location_doc, "web_server_virtual_host", -1);
        location_matcher_doc = psg_json_value_get(location_doc, "location_matcher", -1);

        json_location_matcher_type.data = (u_char *) psg_json_value_get_str(
            psg_json_value_get(location_matcher_doc, "type", -1),
            &json_location_matcher_type.len);
        #if (NGX_PCRE)
            if (clcf->regex != NULL) {
                if (json_location_matcher_type.len != sizeof("regex") - 1
                    || ngx_memcmp(json_location_matcher_type.data, "regex", sizeof("regex") - 1) != 0)
                {
                    goto no_match;
                }
            } else
        #endif
        if (clcf->exact_match) {
            if (json_location_matcher_type.len != sizeof("exact") - 1
                || ngx_memcmp(json_location_matcher_type.data, "exact", sizeof("exact") - 1) != 0)
            {
                goto no_match;
            }
        } else {
            if (json_location_matcher_type.len != sizeof("prefix") - 1
                || ngx_memcmp(json_location_matcher_type.data, "prefix", sizeof("prefix") - 1) != 0)
            {
                goto no_match;
            }
        }

        json_location_matcher_value.data = (u_char *) psg_json_value_get_str(
            psg_json_value_get(location_matcher_doc, "value", -1),
            &json_location_matcher_value.len);
        if (ngx_memn2cmp(clcf->name.data, json_location_matcher_value.data,
                         clcf->name.len, json_location_matcher_value.len)
            != 0)
        {
            goto no_match;
        }

        server_names_doc = psg_json_value_get(vhost_doc, "server_names", -1);
        if (!matches_any_server_names(ctx, cscf, server_names_doc)) {
            goto no_match;
        }

        return location_doc;

        no_match:
        psg_json_value_iterator_advance(ctx->it);
    }

    return NULL;
}

static PsgJsonValue *
create_location_entry(PsgJsonValue *app_config, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf, const char *option_name, size_t option_name_size)
{
    PsgJsonValue *locations_doc = psg_json_value_get(app_config, "locations", -1);
    PsgJsonValue *entry = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *vhost_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *server_names_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);
    PsgJsonValue *location_matcher_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *options_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *server_name_doc, *result;
    ngx_http_server_name_t *server_names = cscf->server_names.elts;
    ngx_uint_t i;

    for (i = 0; i < cscf->server_names.nelts; i++) {
        server_name_doc = psg_json_value_new_str((const char *) server_names[i].name.data,
            server_names[i].name.len);
        psg_json_value_append_val(server_names_doc, server_name_doc);
        psg_json_value_free(server_name_doc);
    }
    psg_json_value_set_value(vhost_doc, "server_names", -1, server_names_doc);

    psg_json_value_set_str(location_matcher_doc, "value",
        (const char *) clcf->name.data, clcf->name.len);
    #if (NGX_PCRE)
        if (clcf->regex != NULL) {
            psg_json_value_set_str(location_matcher_doc, "type",
                "regex", -1);
        } else
    #endif
    if (clcf->exact_match) {
        psg_json_value_set_str(location_matcher_doc, "type",
            "exact", -1);
    } else {
        psg_json_value_set_str(location_matcher_doc, "type",
            "prefix", -1);
    }

    psg_json_value_set_value(entry, "web_server_virtual_host", -1, vhost_doc);
    psg_json_value_set_value(entry, "location_matcher", -1, location_matcher_doc);
    psg_json_value_set_value(entry, "options", -1, options_doc);
    result = psg_json_value_append_val(locations_doc, entry);
    psg_json_value_free(entry);
    psg_json_value_free(vhost_doc);
    psg_json_value_free(server_names_doc);
    psg_json_value_free(location_matcher_doc);
    psg_json_value_free(options_doc);
    return result;
}

static int
infer_location_conf_app_group_name(postprocess_ctx_t *ctx, passenger_loc_conf_t *passenger_conf,
    ngx_http_core_loc_conf_t *clcf, ngx_str_t *result)
{
    ngx_str_t app_root, app_env;
    char *abs_path;
    u_char *buf;
    void *_unused;
    size_t buf_size;

    if (passenger_conf->autogenerated.app_group_name.data == NULL) {
        if (passenger_conf->autogenerated.app_root.data == NULL) {
            buf_size = clcf->root.len + sizeof("/..") - 1;
            buf = (u_char *) ngx_pnalloc(ctx->cf->pool, buf_size);
            if (buf == NULL) {
                return 0;
            }
            app_root.data = buf;
            app_root.len = ngx_snprintf(buf, buf_size, "%V/..", &clcf->root) - buf;
        } else {
            app_root = passenger_conf->autogenerated.app_root;
        }

        abs_path = psg_absolutize_path(
            (const char *) app_root.data, app_root.len,
            (const char *) ctx->cf->cycle->prefix.data, ctx->cf->cycle->prefix.len,
            &app_root.len);
        app_root.data = (u_char *) ngx_pnalloc(ctx->cf->pool, app_root.len);
        _unused = ngx_copy(app_root.data, abs_path, app_root.len);
        (void) _unused; /* Shut up compiler warning */
        free(abs_path);

        if (passenger_conf->autogenerated.environment.data == NULL) {
            app_env.data = (u_char *) DEFAULT_APP_ENV;
            app_env.len = sizeof(DEFAULT_APP_ENV) - 1;
        } else {
            app_env = passenger_conf->autogenerated.environment;
        }

        buf_size = app_root.len + app_env.len + sizeof(" ()") - 1;
        buf = (u_char *) ngx_pnalloc(ctx->cf->pool, buf_size);
        result->data = buf;
        result->len = ngx_snprintf(buf, buf_size, "%V (%V)", &app_root, &app_env) - buf;
    } else {
        *result = passenger_conf->autogenerated.app_group_name;
    }

    return 1;
}

static PsgJsonValue *
find_or_create_manifest_global_config_entry(postprocess_ctx_t *ctx,
    const char *option_name, size_t option_name_len)
{
    PsgJsonValue *container_doc, *entry;

    container_doc = psg_json_value_get(ctx->json,
        "global_configuration", -1);
    entry = psg_json_value_get_or_create_null(container_doc,
        option_name, option_name_len);
    if (psg_json_value_is_null(entry)) {
        init_manifest_config_entry(entry);
    }

    return entry;
}

static PsgJsonValue *
find_or_create_manifest_application_config_entry(postprocess_ctx_t *ctx,
    ngx_str_t *app_group_name,
    const char *option_name, size_t option_name_len)
{
    PsgJsonValue *container_doc, *app_config_doc, *options_doc, *entry;

    if (app_group_name->len == 0) {
        /* We are in a global context */
        container_doc = psg_json_value_get(ctx->json,
            "default_application_configuration", -1);
        entry = psg_json_value_get_or_create_null(container_doc,
            option_name, option_name_len);
    } else {
        /* We are in a server or location/if context */
        container_doc = psg_json_value_get(ctx->json,
            "application_configuration", -1);
        app_config_doc = psg_json_value_get_or_create_null(container_doc,
            (const char *) app_group_name->data, app_group_name->len);
        if (psg_json_value_is_null(app_config_doc)) {
            init_manifest_app_config_container(app_config_doc);
        }

        options_doc = psg_json_value_get(app_config_doc, "options", -1);
        entry = psg_json_value_get_or_create_null(options_doc,
            option_name, option_name_len);
    }

    if (psg_json_value_is_null(entry)) {
        init_manifest_config_entry(entry);
    }

    return entry;
}

static PsgJsonValue *
find_or_create_manifest_location_config_entry(postprocess_ctx_t *ctx,
    ngx_str_t *app_group_name, ngx_http_core_srv_conf_t *cscf, ngx_http_core_loc_conf_t *clcf,
    const char *option_name, size_t option_name_len)
{
    PsgJsonValue *container_doc, *app_config_doc, *location_entry_doc, *options_doc, *entry;

    if (app_group_name->len == 0) {
        /* We are in a global context */
        container_doc = psg_json_value_get(ctx->json,
            "default_location_configuration", -1);
        entry = psg_json_value_get_or_create_null(container_doc,
            option_name, option_name_len);
    } else {
        /* We are in a server or location/if context */
        container_doc = psg_json_value_get(ctx->json,
            "application_configuration", -1);
        app_config_doc = psg_json_value_get_or_create_null(container_doc,
            (const char *) app_group_name->data, app_group_name->len);
        if (psg_json_value_is_null(app_config_doc)) {
            init_manifest_app_config_container(app_config_doc);
        }

        if (clcf->name.len == 0) {
            /* We are in a server context */
            options_doc = psg_json_value_get(app_config_doc, "default_location_configuration", -1);
        } else {
            /* We are in a location/if context */
            location_entry_doc = find_matching_location_entry(ctx, cscf, clcf,
                app_config_doc);
            if (location_entry_doc == NULL) {
                location_entry_doc = create_location_entry(app_config_doc,
                    cscf, clcf, option_name, option_name_len);
            }
            options_doc = psg_json_value_get(location_entry_doc, "options", -1);
        }

        entry = psg_json_value_get_or_create_null(options_doc,
            option_name, option_name_len);
    }

    if (psg_json_value_is_null(entry)) {
        init_manifest_config_entry(entry);
    }

    return entry;
}

static PsgJsonValue *
add_manifest_config_option_hierarchy_member(PsgJsonValue *config_entry,
    ngx_str_t *source_file, ngx_uint_t source_line)
{
    PsgJsonValue *value_hierarchy = psg_json_value_get(config_entry, "value_hierarchy", -1);
    PsgJsonValue *hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *result;

    psg_json_value_set_str(source, "type", "web-server-config", sizeof("web-server-config") - 1);
    psg_json_value_set_str(source, "path", (const char *) source_file->data, source_file->len);
    psg_json_value_set_uint(source, "line", source_line);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);
    result = psg_json_value_append_val(value_hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);

    return result;
}

static void
add_manifest_config_entry_dynamic_default(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len,
    const char *desc, size_t desc_len)
{
    PsgJsonValue *entry, *hierarchy, *hierarchy_member, *source;

    entry = psg_json_value_get_or_create_null(global_config, option_name, option_name_len);
    if (psg_json_value_is_null(entry)) {
        init_manifest_config_entry(entry);
    }
    hierarchy = psg_json_value_get(entry, "value_hierarchy", -1);

    source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_str(source, "type", "dynamic-default-description", -1);

    hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);
    psg_json_value_set_str(hierarchy_member, "value", desc, desc_len);

    psg_json_value_append_val(hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);
}

static PsgJsonValue *
add_manifest_config_entry_static_default(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len)
{
    PsgJsonValue *entry, *hierarchy, *hierarchy_member, *source, *result;

    entry = psg_json_value_get_or_create_null(global_config, option_name, option_name_len);
    if (psg_json_value_is_null(entry)) {
        init_manifest_config_entry(entry);
    }
    hierarchy = psg_json_value_get(entry, "value_hierarchy", -1);

    source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_str(source, "type", "default", -1);

    hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);

    result = psg_json_value_append_val(hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);

    return result;
}

static void
add_manifest_config_entry_static_default_str(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len,
    const char *value, size_t value_len)
{
    PsgJsonValue *hierarchy_member = add_manifest_config_entry_static_default(
        global_config, option_name, option_name_len);
    psg_json_value_set_str(hierarchy_member, "value", value, value_len);
}

static void
add_manifest_config_entry_static_default_int(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len, int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_config_entry_static_default(
        global_config, option_name, option_name_len);
    psg_json_value_set_int(hierarchy_member, "value", value);
}

static void
add_manifest_config_entry_static_default_uint(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len, unsigned int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_config_entry_static_default(
        global_config, option_name, option_name_len);
    psg_json_value_set_uint(hierarchy_member, "value", value);
}

static void
add_manifest_config_entry_static_default_bool(PsgJsonValue *global_config,
    const char *option_name, size_t option_name_len, int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_config_entry_static_default(
        global_config, option_name, option_name_len);
    psg_json_value_set_bool(hierarchy_member, "value", value);
}

static void
postprocess_location_conf(postprocess_ctx_t *ctx, passenger_loc_conf_t *passenger_conf) {
    passenger_loc_conf_t **children;
    ngx_http_core_srv_conf_t *cscf;
    ngx_http_core_loc_conf_t *clcf;
    ngx_str_t app_group_name;
    ngx_uint_t i;

    cscf = passenger_conf->cscf;
    clcf = passenger_conf->clcf;

    if (cscf != NULL && clcf != NULL) {
        if (cscf->server_name.len == 0) {
            /* We are in the global context */
            app_group_name.data = NULL;
            app_group_name.len = 0;
        } else {
            /* We are in a server or location/if context */
            infer_location_conf_app_group_name(ctx, passenger_conf, clcf, &app_group_name);
        }

        passenger_track_autogenerated_loc_conf(ctx, passenger_conf,
            cscf, clcf, &app_group_name);
    }

    children = passenger_conf->children.elts;
    for (i = 0; i < passenger_conf->children.nelts; i++) {
        postprocess_location_conf(ctx, children[i]);
    }
}

static void
reverse_value_hierarchies_in_options_list(PsgJsonValue *options_doc,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *option_doc, *value_hierarchy_doc;
    unsigned int i, len;

    psg_json_value_begin(options_doc, it);
    psg_json_value_end(options_doc, end);
    while (!psg_json_value_iterator_eq(it, end)) {
        option_doc = psg_json_value_iterator_get_value(it);
        value_hierarchy_doc = psg_json_value_get(option_doc, "value_hierarchy", -1);
        len = psg_json_value_size(value_hierarchy_doc);

        for (i = 0; i < len / 2; i++) {
            psg_json_value_swap(
                psg_json_value_get_at_index(value_hierarchy_doc, i),
                psg_json_value_get_at_index(value_hierarchy_doc, len - i - 1));
        }

        psg_json_value_iterator_advance(it);
    }
}

static void
reverse_value_hierarchies(postprocess_ctx_t *ctx) {
    PsgJsonValue *app_config_containers, *app_config_container;
    PsgJsonValue *location_config_containers, *location_config_container;
    PsgJsonValue *options_doc;

    options_doc = psg_json_value_get(ctx->json, "global_configuration", -1);
    reverse_value_hierarchies_in_options_list(options_doc, ctx->it, ctx->end);

    options_doc = psg_json_value_get(ctx->json, "default_application_configuration", -1);
    reverse_value_hierarchies_in_options_list(options_doc, ctx->it, ctx->end);

    options_doc = psg_json_value_get(ctx->json, "default_location_configuration", -1);
    reverse_value_hierarchies_in_options_list(options_doc, ctx->it, ctx->end);

    app_config_containers = psg_json_value_get(ctx->json, "application_configuration", -1);
    psg_json_value_begin(app_config_containers, ctx->it);
    psg_json_value_end(app_config_containers, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        options_doc = psg_json_value_get(app_config_container, "options", -1);
        reverse_value_hierarchies_in_options_list(options_doc, ctx->it2, ctx->end2);

        options_doc = psg_json_value_get(app_config_container, "default_location_configuration", -1);
        reverse_value_hierarchies_in_options_list(options_doc, ctx->it2, ctx->end2);

        location_config_containers = psg_json_value_get(app_config_container, "locations", -1);
        if (location_config_containers != NULL) {
            psg_json_value_begin(location_config_containers, ctx->it2);
            psg_json_value_end(location_config_containers, ctx->end2);
            while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
                location_config_container = psg_json_value_iterator_get_value(ctx->it2);

                options_doc = psg_json_value_get(location_config_container, "options", -1);
                reverse_value_hierarchies_in_options_list(options_doc, ctx->it3, ctx->end3);

                psg_json_value_iterator_advance(ctx->it2);
            }
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

static void
psg_json_value_append_vals(PsgJsonValue *doc, PsgJsonValue *doc2,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *elem;

    psg_json_value_begin(doc2, it);
    psg_json_value_end(doc2, end);
    while (!psg_json_value_iterator_eq(it, end)) {
        elem = psg_json_value_iterator_get_value(it);
        psg_json_value_append_val(doc, elem);
        psg_json_value_iterator_advance(it);
    }
}

static void
set_manifest_application_conf_defaults(postprocess_ctx_t *ctx) {
    PsgJsonValue *default_app_configs = psg_json_value_get(ctx->json,
        "default_application_configuration", -1);
    passenger_set_autogenerated_manifest_application_conf_defaults(
        default_app_configs);
}

static void
set_manifest_location_conf_defaults(postprocess_ctx_t *ctx) {
    PsgJsonValue *default_location_configs = psg_json_value_get(ctx->json,
        "default_location_configuration", -1);
    passenger_set_autogenerated_manifest_location_conf_defaults(
        default_location_configs);
}

static int
json_array_contains(PsgJsonValue *doc, PsgJsonValue *elem) {
    unsigned int i, len;
    PsgJsonValue *current;

    len = psg_json_value_size(doc);
    for (i = 0; i < len; i++) {
        current = psg_json_value_get_at_index(doc, i);
        if (psg_json_value_eq(current, elem)) {
            return 1;
        }
    }

    return 0;
}

static void
maybe_inherit_string_array_hierarchy_values(PsgJsonValue *value_hierarchy_doc,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *value, *current, *next, *current_value, *next_value;
    unsigned int len;
    int i;

    if (psg_json_value_size(value_hierarchy_doc) == 0) {
        return;
    }
    value = psg_json_value_get(psg_json_value_get_at_index(value_hierarchy_doc, 0),
        "value", -1);
    if (psg_json_value_type(value) != PSG_JSON_VALUE_TYPE_ARRAY) {
        return;
    }

    len = psg_json_value_size(value_hierarchy_doc);
    for (i = len - 1; i >= 1; i--) {
        current = psg_json_value_get_at_index(value_hierarchy_doc, i);
        next = psg_json_value_get_at_index(value_hierarchy_doc, i - 1);

        current_value = psg_json_value_get(current, "value", -1);
        next_value = psg_json_value_get(next, "value", -1);

        psg_json_value_begin(current_value, it);
        psg_json_value_end(current_value, end);
        while (!psg_json_value_iterator_eq(it, end)) {
            if (!json_array_contains(next_value, psg_json_value_iterator_get_value(it))) {
                psg_json_value_append_val(next_value, psg_json_value_iterator_get_value(it));
            }
            psg_json_value_iterator_advance(it);
        }
    }
}

static void
maybe_inherit_string_keyval_hierarchy_values(PsgJsonValue *value_hierarchy_doc,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *value, *current, *next, *current_value, *next_value;
    const char *name;
    size_t name_len;
    unsigned int len;
    int i;

    if (psg_json_value_size(value_hierarchy_doc) == 0) {
        return;
    }
    value = psg_json_value_get(psg_json_value_get_at_index(value_hierarchy_doc, 0),
        "value", -1);
    if (psg_json_value_type(value) != PSG_JSON_VALUE_TYPE_OBJECT) {
        return;
    }

    len = psg_json_value_size(value_hierarchy_doc);
    for (i = len - 1; i >= 1; i--) {
        current = psg_json_value_get_at_index(value_hierarchy_doc, i);
        next = psg_json_value_get_at_index(value_hierarchy_doc, i - 1);

        current_value = psg_json_value_get(current, "value", -1);
        next_value = psg_json_value_get(next, "value", -1);

        psg_json_value_begin(current_value, it);
        psg_json_value_end(current_value, end);
        while (!psg_json_value_iterator_eq(it, end)) {
            name = psg_json_value_iterator_get_name(it, &name_len);
            if (!psg_json_value_is_member(next_value, name, name_len)) {
                psg_json_value_set_value(next_value, name, name_len,
                    psg_json_value_iterator_get_value(it));
            }
            psg_json_value_iterator_advance(it);
        }
    }
}

static void
inherit_application_value_hierarchies(postprocess_ctx_t *ctx) {
    PsgJsonValue *default_app_configs = psg_json_value_get(ctx->json,
        "default_application_configuration", -1);
    PsgJsonValue *app_config_containers = psg_json_value_get(ctx->json,
        "application_configuration", -1);
    PsgJsonValue *app_config_container, *options_doc, *option_doc, *default_app_config;
    PsgJsonValue *value_hierarchy_doc, *value_hierarchy_from_default;
    const char *option_name;
    size_t option_name_len;

    /* Iterate through all 'application_configuration' objects */
    psg_json_value_begin(app_config_containers, ctx->it);
    psg_json_value_end(app_config_containers, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        /* Iterate through all its 'options' objects */
        options_doc = psg_json_value_get(app_config_container, "options", -1);
        psg_json_value_begin(options_doc, ctx->it2);
        psg_json_value_end(options_doc, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each option, inherit the value hierarchies
             * from the 'default_application_configuration' object.
             *
             * Since the value hierarchy array is already in
             * most-to-least-specific order, simply appending
             * the 'default_application_configuration' hierarchy is
             * enough.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            option_doc = psg_json_value_iterator_get_value(ctx->it2);
            default_app_config = psg_json_value_get(default_app_configs,
                option_name, option_name_len);
            if (default_app_config != NULL) {
                value_hierarchy_doc = psg_json_value_get(option_doc,
                    "value_hierarchy", -1);
                value_hierarchy_from_default = psg_json_value_get(default_app_config,
                    "value_hierarchy", -1);
                psg_json_value_append_vals(value_hierarchy_doc,
                    value_hierarchy_from_default,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all 'default_application_configuration' options */
        psg_json_value_begin(default_app_configs, ctx->it2);
        psg_json_value_end(default_app_configs, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each default app config object, if there is no object in
             * the current context's 'options' with the same name, then add
             * it there.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            if (!psg_json_value_is_member(options_doc, option_name, option_name_len)) {
                option_doc = psg_json_value_iterator_get_value(ctx->it2);
                psg_json_value_set_value(options_doc, option_name, option_name_len,
                    option_doc);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

static void
inherit_location_value_hierarchies(postprocess_ctx_t *ctx) {
    PsgJsonValue *global_default_location_configs = psg_json_value_get(ctx->json,
        "default_location_configuration", -1);
    PsgJsonValue *app_config_containers = psg_json_value_get(ctx->json,
        "application_configuration", -1);
    PsgJsonValue *app_config_container, *location_containers, *location_container;
    PsgJsonValue *app_default_location_configs;
    PsgJsonValue *options_doc, *option_doc, *default_location_config;
    PsgJsonValue *value_hierarchy_doc, *value_hierarchy_from_default;
    const char *option_name;
    size_t option_name_len;

    /* Iterate through all 'application_configuration' objects */
    psg_json_value_begin(app_config_containers, ctx->it);
    psg_json_value_end(app_config_containers, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        /* Iterate through all its 'default_location_configuration' options */
        app_default_location_configs = psg_json_value_get(app_config_container,
            "default_location_configuration", -1);
        options_doc = app_default_location_configs;
        psg_json_value_begin(options_doc, ctx->it2);
        psg_json_value_end(options_doc, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each option, inherit the value hierarchies
             * from the top-level 'default_application_configuration' object.
             *
             * Since the value hierarchy array is already in
             * most-to-least-specific order, simply appending
             * the 'default_application_configuration' hierarchy is
             * enough.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2, &option_name_len);
            option_doc = psg_json_value_iterator_get_value(ctx->it2);
            default_location_config = psg_json_value_get(global_default_location_configs,
                option_name, option_name_len);
            if (default_location_config != NULL) {
                value_hierarchy_doc = psg_json_value_get(option_doc,
                    "value_hierarchy", -1);
                value_hierarchy_from_default = psg_json_value_get(default_location_config,
                    "value_hierarchy", -1);
                psg_json_value_append_vals(value_hierarchy_doc,
                    value_hierarchy_from_default,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all top-level 'default_location_configuration' options */
        psg_json_value_begin(global_default_location_configs, ctx->it2);
        psg_json_value_end(global_default_location_configs, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each default top-level 'default_location_configuration' option,
             * if there is no object in the current context's 'default_application_configuration'
             * with the same name, then add it there.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            if (!psg_json_value_is_member(options_doc, option_name, option_name_len)) {
                option_doc = psg_json_value_iterator_get_value(ctx->it2);
                psg_json_value_set_value(options_doc, option_name, option_name_len,
                    option_doc);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all its 'locations' options */
        location_containers = psg_json_value_get(app_config_container, "locations", -1);
        if (location_containers != NULL) {
            psg_json_value_begin(location_containers, ctx->it2);
            psg_json_value_end(location_containers, ctx->end2);
            while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
                location_container = psg_json_value_iterator_get_value(ctx->it2);

                options_doc = psg_json_value_get(location_container, "options", -1);
                psg_json_value_begin(options_doc, ctx->it3);
                psg_json_value_end(options_doc, ctx->end3);
                while (!psg_json_value_iterator_eq(ctx->it3, ctx->end3)) {
                    /* For each option, inherit the value hierarchies
                     * from the 'default_location_configuration' belonging
                     * to the current app (which also contains the global
                     * location config defaults).
                     *
                     * Since the value hierarchy array is already in
                     * most-to-least-specific order, simply appending
                     * the 'default_location_configuration' hierarchy is
                     * enough.
                     */
                    option_name = psg_json_value_iterator_get_name(ctx->it3, &option_name_len);
                    option_doc = psg_json_value_iterator_get_value(ctx->it3);
                    default_location_config = psg_json_value_get(app_default_location_configs,
                        option_name, option_name_len);
                    if (default_location_config != NULL) {
                        value_hierarchy_doc = psg_json_value_get(option_doc,
                            "value_hierarchy", -1);
                        value_hierarchy_from_default = psg_json_value_get(default_location_config,
                            "value_hierarchy", -1);
                        psg_json_value_append_vals(value_hierarchy_doc,
                            value_hierarchy_from_default,
                            ctx->it4, ctx->end4);
                        maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                            ctx->it4, ctx->end4);
                        maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                            ctx->it4, ctx->end4);
                    }

                    psg_json_value_iterator_advance(ctx->it3);
                }

                psg_json_value_iterator_advance(ctx->it2);
            }
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

ngx_int_t
passenger_postprocess_config(ngx_conf_t *cf)
{
    ngx_http_conf_ctx_t  *http_ctx = cf->ctx;
    passenger_loc_conf_t *toplevel_passenger_conf = http_ctx->loc_conf[ngx_http_passenger_module.ctx_index];
    postprocess_ctx_t     ctx;

    ngx_memzero(&ctx, sizeof(ctx));
    ctx.cf = cf;
    ctx.json = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    init_manifest(ctx.json);
    ctx.it = psg_json_value_iterator_new();
    ctx.end = psg_json_value_iterator_new();
    ctx.it2 = psg_json_value_iterator_new();
    ctx.end2 = psg_json_value_iterator_new();
    ctx.it3 = psg_json_value_iterator_new();
    ctx.end3 = psg_json_value_iterator_new();
    ctx.it4 = psg_json_value_iterator_new();
    ctx.end4 = psg_json_value_iterator_new();

    passenger_main_conf.default_ruby = toplevel_passenger_conf->autogenerated.ruby;
    if (passenger_main_conf.default_ruby.len == 0) {
        passenger_main_conf.default_ruby.data = (u_char *) DEFAULT_RUBY;
        passenger_main_conf.default_ruby.len = strlen(DEFAULT_RUBY);
    }

    passenger_track_autogenerated_main_conf(&ctx, &passenger_main_conf);

    postprocess_location_conf(&ctx, toplevel_passenger_conf);

    reverse_value_hierarchies(&ctx);
    passenger_set_autogenerated_manifest_global_conf_defaults(ctx.json);
    set_manifest_application_conf_defaults(&ctx);
    set_manifest_location_conf_defaults(&ctx);
    inherit_application_value_hierarchies(&ctx);
    inherit_location_value_hierarchies(&ctx);

    char *jstr = psg_json_value_to_styled_string(ctx.json);
    //fprintf(stderr, "%s", jstr);
    fflush(stderr);
    FILE *f = fopen("/tmp/dump.json", "w");
    if (f != NULL) {
        fprintf(f, "%s", jstr);
        fclose(f);
    }
    free(jstr);
    psg_json_value_free(ctx.json);
    psg_json_value_iterator_free(ctx.it);
    psg_json_value_iterator_free(ctx.end);
    psg_json_value_iterator_free(ctx.it2);
    psg_json_value_iterator_free(ctx.end2);
    psg_json_value_iterator_free(ctx.it3);
    psg_json_value_iterator_free(ctx.end3);
    psg_json_value_iterator_free(ctx.it4);
    psg_json_value_iterator_free(ctx.end4);

    return NGX_OK;
}

static int
string_keyval_has_key(ngx_array_t *table, ngx_str_t *key)
{
    ngx_keyval_t  *elems;
    ngx_uint_t     i;

    elems = (ngx_keyval_t *) table->elts;
    for (i = 0; i < table->nelts; i++) {
        if (elems[i].key.len == key->len
         && memcmp(elems[i].key.data, key->data, key->len) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static ngx_int_t
merge_string_keyval_table(ngx_conf_t *cf, ngx_array_t **prev, ngx_array_t **conf)
{
    ngx_keyval_t  *prev_elems, *elem;
    ngx_uint_t     i;

    if (*prev != NULL) {
        if (*conf == NULL) {
            *conf = ngx_array_create(cf->pool, 4, sizeof(ngx_keyval_t));
            if (*conf == NULL) {
                return NGX_ERROR;
            }
        }

        prev_elems = (ngx_keyval_t *) (*prev)->elts;
        for (i = 0; i < (*prev)->nelts; i++) {
            if (!string_keyval_has_key(*conf, &prev_elems[i].key)) {
                elem = (ngx_keyval_t *) ngx_array_push(*conf);
                if (elem == NULL) {
                    return NGX_ERROR;
                }
                *elem = prev_elems[i];
            }
        }
    }

    return NGX_OK;
}

#ifndef PASSENGER_IS_ENTERPRISE
static char *
passenger_enterprise_only(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    return ": this feature is only available in Phusion Passenger Enterprise. "
        "You are currently running the open source Phusion Passenger. "
        "Please learn more about and/or buy Phusion Passenger Enterprise at https://www.phusionpassenger.com/enterprise ;";
}
#endif

static char *
passenger_enabled(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    passenger_loc_conf_t        *passenger_conf = conf;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_str_t                   *value;
    ngx_url_t                    upstream_url;

    passenger_conf->autogenerated.enabled_explicitly_set = 1;
    record_loc_conf_source_location(cf, passenger_conf,
        &passenger_conf->autogenerated.enabled_source_file,
        &passenger_conf->autogenerated.enabled_source_line);

    value = cf->args->elts;
    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        passenger_conf->autogenerated.enabled = 1;

        /* Register a placeholder value as upstream address. The real upstream
         * address (the Passenger core socket filename) will be set while processing
         * requests, because we can't start the watchdog (and thus the Passenger core)
         * until config loading is done.
         */
        ngx_memzero(&upstream_url, sizeof(ngx_url_t));
        upstream_url.url = pp_placeholder_upstream_address;
        upstream_url.no_resolve = 1;
        passenger_conf->upstream_config.upstream = ngx_http_upstream_add(cf, &upstream_url, 0);
        if (passenger_conf->upstream_config.upstream == NULL) {
            return NGX_CONF_ERROR;
        }

        clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
        clcf->handler = passenger_content_handler;

        if (clcf->name.data != NULL
         && clcf->name.data[clcf->name.len - 1] == '/') {
            clcf->auto_redirect = 1;
        }
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        passenger_conf->autogenerated.enabled = 0;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "\"passenger_enabled\" must be either set to \"on\" "
            "or \"off\"");

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
rails_framework_spawner_idle_time(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, "The 'rails_framework_spawner_idle_time' "
        "directive is deprecated; please set 'passenger_max_preloader_idle_time' instead");
    return NGX_CONF_OK;
}

static char *
passenger_use_global_queue(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, "The 'passenger_use_global_queue' "
        "directive is obsolete and doesn't do anything anymore. Global queuing "
        "is now always enabled. Please remove this configuration directive.");
    return NGX_CONF_OK;
}

static char *
passenger_obsolete_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, "The '%V' directive is obsolete "
        "and doesn't do anything anymore.", &cmd->name);
    return NGX_CONF_OK;
}


PsgJsonValue *
psg_json_value_set_str_array(PsgJsonValue *doc, const char *name, ngx_array_t *ary) {
    PsgJsonValue *subdoc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);
    PsgJsonValue *elem, *result;
    ngx_str_t *values;
    ngx_uint_t i;

    if (ary != NULL) {
        values = (ngx_str_t *) ary->elts;
        for (i = 0; i < ary->nelts; i++) {
            elem = psg_json_value_new_str(
                (const char *) values[i].data, values[i].len);
            psg_json_value_append_val(subdoc, elem);
            psg_json_value_free(elem);
        }
    }

    result = psg_json_value_set_value(doc, name, -1, subdoc);
    psg_json_value_free(subdoc);

    return result;
}

PsgJsonValue *
psg_json_value_set_str_keyval(PsgJsonValue *doc, const char *name, ngx_array_t *ary) {
    PsgJsonValue *subdoc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *elem, *result;
    ngx_keyval_t *values;
    ngx_uint_t i;

    if (ary != NULL) {
        values = (ngx_keyval_t *) ary->elts;
        for (i = 0; i < ary->nelts; i++) {
            elem = psg_json_value_new_str(
                (const char *) values[i].value.data, values[i].value.len);
            psg_json_value_set_value(subdoc,
                (const char *) values[i].key.data, values[i].key.len,
                elem);
            psg_json_value_free(elem);
        }
    }

    result = psg_json_value_set_value(doc, name, -1, subdoc);
    psg_json_value_free(subdoc);

    return result;
}


const ngx_command_t passenger_commands[] = {

    #include "ConfigurationCommands.c"

    ngx_null_command
};

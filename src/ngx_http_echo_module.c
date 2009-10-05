#define DDEBUG 0

#include "ngx_http_echo_module.h"

#include <ngx_config.h>
#include <ngx_log.h>

#if (DDEBUG)
#include <ddebug.h>
#else
#define DD
#endif

static ngx_http_output_header_filter_pt ngx_http_next_header_filter = NULL;
static ngx_http_output_body_filter_pt ngx_http_next_body_filter = NULL;

/* (optional) filters initialization */
static ngx_int_t ngx_http_echo_filter_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_echo_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_echo_body_filter(ngx_http_request_t *r, ngx_chain_t *in);

/* config init handler */
static void* ngx_http_echo_create_conf(ngx_conf_t *cf);

/* config directive handlers */
static char* ngx_http_echo_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char* ngx_http_echo_echo_client_request_header(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* main content handler */
static ngx_int_t ngx_http_echo_handler(ngx_http_request_t *r);

static ngx_command_t  ngx_http_echo_commands[] = {

    { ngx_string("echo"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_echo_echo,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_echo_loc_conf_t, handler_cmds),
      NULL },
    /* TODO echo_client_request_header should take an
     * optional argument to change output format to
     * "html" or other things */
    { ngx_string("echo_client_request_header"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_echo_echo_client_request_header,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_echo_loc_conf_t, handler_cmds),
      NULL },
    /* TODO
    { ngx_string("echo_sleep"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_echo_set_content_type,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("echo_client_request_body"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_echo_echo_client_request_body,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      */
    /* TODO
    { ngx_string("echo_location"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_echo_echo_location,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("echo_before_body"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_echo_echo_before_body,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("echo_after_body"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_echo_echo_after_body,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    */
      ngx_null_command
};

static ngx_http_module_t ngx_http_echo_module_ctx = {
    /* TODO we could add our own variables here... */
    NULL,                          /* preconfiguration */
    NULL,                          /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_echo_create_conf, /* create location configuration */
    NULL                           /* merge location configuration */
};

ngx_module_t ngx_http_echo_module = {
    NGX_MODULE_V1,
    &ngx_http_echo_module_ctx, /* module context */
    ngx_http_echo_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_echo_filter_init (ngx_conf_t *cf) {
    if (ngx_http_next_header_filter == NULL) {
        ngx_http_next_header_filter = ngx_http_top_header_filter;
        ngx_http_top_header_filter  = ngx_http_echo_header_filter;
    }
    if (ngx_http_next_body_filter == NULL) {
        ngx_http_next_body_filter = ngx_http_top_body_filter;
        ngx_http_top_body_filter  = ngx_http_echo_body_filter;
    }
}

static void*
ngx_http_echo_create_conf(ngx_conf_t *cf) {
    ngx_http_echo_loc_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_echo_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    return conf;
}

static char*
ngx_http_echo_helper(ngx_http_echo_opcode_t opcode,
        ngx_http_echo_cmd_category_t cat,
        ngx_conf_t *cf, ngx_command_t *cmd, void* conf) {
    ngx_http_core_loc_conf_t        *clcf;
    ngx_http_echo_loc_conf_t        *ulcf = conf;
    ngx_array_t                     **args_ptr;
    ngx_http_script_compile_t       sc;
    ngx_str_t                       *raw_args;
    ngx_http_echo_arg_template_t    *arg;
    ngx_array_t                     **cmds_ptr;
    ngx_http_echo_cmd_t             *echo_cmd;
    ngx_int_t                       i, n;

    /* cmds_ptr points to ngx_http_echo_loc_conf_t's
     * handler_cmds, before_body_cmds, or after_body_cmds
     * array, depending on the actual offset */
    cmds_ptr = (ngx_array_t**)(((u_char*)conf) + cmd->offset);
    if (*cmds_ptr == NULL) {
        *cmds_ptr = ngx_array_create(cf->pool, 1,
                sizeof(ngx_http_echo_cmd_t));
        if (*cmds_ptr == NULL) {
            return NGX_CONF_ERROR;
        }
        if (cat == echo_handler_cmd) {
            /* register the content handler */
            clcf = ngx_http_conf_get_module_loc_conf(cf,
                    ngx_http_core_module);
            if (clcf == NULL) {
                return NGX_CONF_ERROR;
            }
            clcf->handler = ngx_http_echo_handler;
        } else {
            ngx_http_echo_filter_init(cf);
        }
    }
    echo_cmd = ngx_array_push(*cmds_ptr);
    if (echo_cmd == NULL) {
        return NGX_CONF_ERROR;
    }
    echo_cmd->opcode = opcode;
    args_ptr = &echo_cmd->args;
    *args_ptr = ngx_array_create(cf->pool, 1,
            sizeof(ngx_http_echo_arg_template_t));
    if (*args_ptr == NULL) {
        return NGX_CONF_ERROR;
    }
    raw_args = cf->args->elts;
    /* we skip the first arg and start from the second */
    for (i = 1 ; i < cf->args->nelts; i++) {
        arg = ngx_array_push(*args_ptr);
        if (arg == NULL) {
            return NGX_CONF_ERROR;
        }
        arg->raw_value = &raw_args[i];
        arg->lengths = NULL;
        arg->values  = NULL;
        n = ngx_http_script_variables_count(arg->raw_value);
        if (n > 0) {
            ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));
            sc.cf = cf;
            sc.source = &arg->raw_value;
            sc.lengths = &arg->lengths;
            sc.values = &arg->values;
            sc.variables = n;
            sc.complete_lengths = 1;
            sc.complete_values = 1;
            if (ngx_http_script_compile(&sc) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }
    } /* end for */
    return NGX_CONF_OK;
}

/*
static ngx_int_t
ngx_http_echo_handler(ngx_http_request_t *r) {
    ngx_buf_t     *b;
    ngx_buf_t     *header_in;
    ngx_chain_t   out;
    ngx_int_t     rc;
    size_t        size;
    u_char        *c;

    r->headers_out.content_type.len = sizeof(ngx_http_echo_content_type) - 1;
    r->headers_out.content_type.data = (u_char *) ngx_http_echo_content_type;

    if (r != r->main) {
        header_in = r->main->header_in;
    } else {
        header_in = r->header_in;
    }
    if (NULL == header_in) {
        DD("header_in is NULL");
        return NGX_HTTP_BAD_REQUEST;
    }
    size = header_in->pos - header_in->start;
    //DD("!!! size: %lu", (unsigned long)size);

    b = ngx_create_temp_buf(r->pool, size);
    b->last = ngx_cpymem(b->start, header_in->start, size);
    b->memory = 1;
    b->last_buf = 1;
*/
    /* fix \0 introduced by the nginx header parser */
/*
    for (c = (u_char*)b->start; c != b->last; c++) {
        if (*c == '\0') {
            if (c + 1 != b->last && *(c + 1) == LF) {
                *c = CR;
            } else {
                *c = ':';
            }
        }
    }

    out.buf = b;
    out.next = NULL;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = size;

    rc = ngx_http_send_header(r);
    if (r->header_only || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}
*/

static char *
ngx_http_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_echo_handler;

    return NGX_CONF_OK;
}


/**
 * @file operations.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Basic NETCONF operations implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"

/**
 * Local information about locks
 */
static struct {
    struct nc_session *running;
    struct nc_session *startup;
    struct nc_session *candidate;
} dslock = {NULL, NULL, NULL};
pthread_rwlock_t dslock_rwl = PTHREAD_RWLOCK_INITIALIZER;

void
np2srv_clean_dslock(struct nc_session *ncs)
{
    pthread_rwlock_wrlock(&dslock_rwl);

    if (dslock.running == ncs) {
        dslock.running = NULL;
    }
    if (dslock.startup == ncs) {
        dslock.startup = NULL;
    }
    if (dslock.candidate == ncs) {
        dslock.candidate = NULL;
    }

    pthread_rwlock_unlock(&dslock_rwl);
}

static char *
get_srval_value(struct ly_ctx *ctx, sr_val_t *value, char *buf)
{
    const struct lys_node *snode;

    if (!value) {
        return NULL;
    }

    switch (value->type) {
    case SR_STRING_T:
    case SR_BINARY_T:
    case SR_BITS_T:
    case SR_ENUM_T:
    case SR_IDENTITYREF_T:
    case SR_INSTANCEID_T:
    case SR_LEAFREF_T:
        return (value->data.string_val);
    case SR_LEAF_EMPTY_T:
        return NULL;
    case SR_BOOL_T:
        return value->data.bool_val ? "true" : "false";
    case SR_DECIMAL64_T:
        /* get fraction-digits */
        snode = ly_ctx_get_node(ctx, NULL, value->xpath);
        if (!snode) {
            return NULL;
        }
        sprintf(buf, "%.*f", ((struct lys_node_leaf *)snode)->type.info.dec64.dig, value->data.decimal64_val);
        return buf;
    case SR_UINT8_T:
        sprintf(buf, "%u", value->data.uint8_val);
        return buf;
    case SR_UINT16_T:
        sprintf(buf, "%u", value->data.uint16_val);
        return buf;
    case SR_UINT32_T:
        sprintf(buf, "%u", value->data.uint32_val);
        return buf;
    case SR_UINT64_T:
        sprintf(buf, "%lu", value->data.uint64_val);
        return buf;
    case SR_INT8_T:
        sprintf(buf, "%d", value->data.int8_val);
        return buf;
    case SR_INT16_T:
        sprintf(buf, "%d", value->data.int16_val);
        return buf;
    case SR_INT32_T:
        sprintf(buf, "%d", value->data.int32_val);
        return buf;
    case SR_INT64_T:
        sprintf(buf, "%ld", value->data.int64_val);
        return buf;
    default:
        return NULL;
    }

}

/* add subtree to root */
static int
build_subtree(sr_session_ctx_t *ds, struct lyd_node *root, const char *subtree_path)
{
    sr_val_t *value;
    sr_val_iter_t *iter;
    char *subtree_children_path, buf[21];
    int rc;

    if (asprintf(&subtree_children_path, "%s//*", subtree_path) == -1) {
        EMEM;
        return -1;
    }

    rc = sr_get_items_iter(ds, subtree_children_path, &iter);
    if (rc != SR_ERR_OK) {
        ERR("Getting items (%s) from sysrepo failed (%s).", subtree_children_path, sr_strerror(rc));
        free(subtree_children_path);
        return -1;
    }
    free(subtree_children_path);

    ly_errno = LY_SUCCESS;
    while (sr_get_item_next(ds, iter, &value) == SR_ERR_OK) {
        lyd_new_path(root, np2srv.ly_ctx, value->xpath,
                     get_srval_value(np2srv.ly_ctx, value, buf), LYD_PATH_OPT_UPDATE);
        sr_free_val(value);
        if (ly_errno) {
            sr_free_val_iter(iter);
            return -1;
        }
    }
    sr_free_val_iter(iter);

    return 0;
}

static int
strws(const char *str)
{
    while (*str) {
        if (!isspace(*str)) {
            return 0;
        }
        ++str;
    }

    return 1;
}

static int
xpath_add_filter(char *new_filter, char ***filters, int *filter_count)
{
    char **filters_new;

    filters_new = realloc(*filters, (*filter_count + 1) * sizeof **filters);
    if (!filters_new) {
        EMEM;
        return -1;
    }
    ++(*filter_count);
    *filters = filters_new;
    (*filters)[*filter_count - 1] = new_filter;

    return 0;
}

static int
xpath_buf_add_attrs(struct ly_ctx *ctx, struct lyxml_attr *attr, char **buf, int size)
{
    const struct lys_module *module;
    struct lyxml_attr *next;
    int new_size;
    char *buf_new;

    LY_TREE_FOR(attr, next) {
        if (next->type == LYXML_ATTR_STD) {
            module = NULL;
            if (next->ns) {
                module = ly_ctx_get_module_by_ns(ctx, next->ns->value, NULL);
            }
            if (!module) {
                /* attribute without namespace or with unknown one will not match anything anyway */
                continue;
            }

            new_size = size + 2 + strlen(module->name) + 1 + strlen(next->name) + 2 + strlen(next->value) + 2;
            buf_new = realloc(*buf, new_size * sizeof(char));
            if (!buf_new) {
                EMEM;
                return -1;
            }
            *buf = buf_new;
            sprintf((*buf) + (size - 1), "[@%s:%s='%s']", module->name, next->name, next->value);
            size = new_size;
        }
    }

    return size;
}

/* top-level content node with namespace and attributes */
static int
xpath_buf_add_top_content(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name, char ***filters,
                          int *filter_count)
{
    int size, len;
    const char *start;
    char *buf;

    /* skip leading and trailing whitespaces */
    for (start = elem->content; isspace(*start); ++start);
    for (len = strlen(start); isspace(start[len - 1]); --len);

    size = 1 + strlen(elem_module_name) + 1 + strlen(elem->name) + 9 + len + 3;
    buf = malloc(size * sizeof(char));
    if (!buf) {
        EMEM;
        return -1;
    }
    sprintf(buf, "/%s:%s[text()='%.*s']", elem_module_name, elem->name, len, start);

    size = xpath_buf_add_attrs(ctx, elem->attr, &buf, size);
    if (!size) {
        free(buf);
        return 0;
    } else if (size < 1) {
        free(buf);
        return -1;
    }

    if (xpath_add_filter(buf, filters, filter_count)) {
        free(buf);
        return -1;
    }

    return 0;
}

/* content node with namespace and attributes */
static int
xpath_buf_add_content(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name, const char **last_ns,
                      char **buf, int size)
{
    const struct lys_module *module;
    int new_size, len;
    const char *start;
    char *buf_new;

    if (!elem_module_name && elem->ns && (elem->ns->value != *last_ns)
            && strcmp(elem->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
        module = ly_ctx_get_module_by_ns(ctx, elem->ns->value, NULL);
        if (!module) {
            /* not really an error */
            return 0;
        }

        *last_ns = elem->ns->value;
        elem_module_name = module->name;
    }

    new_size = size + 1 + (elem_module_name ? strlen(elem_module_name) + 1 : 0) + strlen(elem->name);
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "[%s%s%s", (elem_module_name ? elem_module_name : ""), (elem_module_name ? ":" : ""),
            elem->name);
    size = new_size;

    size = xpath_buf_add_attrs(ctx, elem->attr, buf, size);
    if (!size) {
        return 0;
    } else if (size < 1) {
        return -1;
    }

    /* skip leading and trailing whitespaces */
    for (start = elem->content; isspace(*start); ++start);
    for (len = strlen(start); isspace(start[len - 1]); --len);

    new_size = size + 2 + len + 2;
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "='%.*s']", len, start);

    return new_size;
}

/* containment/selection node with namespace and attributes */
static int
xpath_buf_add_node(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name, const char **last_ns,
                   char **buf, int size)
{
    const struct lys_module *module;
    int new_size;
    char *buf_new;

    if (!elem_module_name && elem->ns && (elem->ns->value != *last_ns)
            && strcmp(elem->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
        module = ly_ctx_get_module_by_ns(ctx, elem->ns->value, NULL);
        if (!module) {
            /* not really an error */
            return 0;
        }

        *last_ns = elem->ns->value;
        elem_module_name = module->name;
    }

    new_size = size + 1 + (elem_module_name ? strlen(elem_module_name) + 1 : 0) + strlen(elem->name);
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "/%s%s%s", (elem_module_name ? elem_module_name : ""), (elem_module_name ? ":" : ""),
            elem->name);
    size = new_size;

    size = xpath_buf_add_attrs(ctx, elem->attr, buf, size);

    return size;
}

/* buf is spent in the function, removes content match nodes from elem->child list! */
static int
xpath_buf_add(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name, const char *last_ns,
              char **buf, int size, char ***filters, int *filter_count)
{
    struct lyxml_elem *temp, *child;
    int new_size;
    char *buf_new;

    /* containment node, selection node */
    size = xpath_buf_add_node(ctx, elem, elem_module_name, &last_ns, buf, size);
    if (!size) {
        free(*buf);
        *buf = NULL;
        return 0;
    } else if (size < 1) {
        goto error;
    }

    /* content match node */
    LY_TREE_FOR_SAFE(elem->child, temp, child) {
        if (!child->child && child->content && !strws(child->content)) {
            size = xpath_buf_add_content(ctx, child, elem_module_name, &last_ns, buf, size);
            if (!size) {
                free(*buf);
                *buf = NULL;
                return 0;
            } else if (size < 1) {
                goto error;
            }
            lyxml_free(ctx, child);
        }
    }

    /* that is it, it seems */
    if (!elem->child) {
        if (xpath_add_filter(*buf, filters, filter_count)) {
            goto error;
        }
        *buf = NULL;
        return 0;
    }

    /* that is it for this filter depth, now we branch with every new node except last */
    LY_TREE_FOR(elem->child, child) {
        if (!child->next) {
            buf_new = *buf;
            *buf = NULL;
        } else {
            buf_new = malloc(size * sizeof(char));
            if (!buf_new) {
                goto error;
            }
            memcpy(buf_new, *buf, size * sizeof(char));
        }
        new_size = size;

        /* child containment node */
        if (child->child) {
            xpath_buf_add(ctx, child, NULL, last_ns, &buf_new, new_size, filters, filter_count);

        /* child selection node */
        } else {
            new_size = xpath_buf_add_node(ctx, child, NULL, &last_ns, &buf_new, new_size);
            if (!new_size) {
                free(buf_new);
                continue;
            } else if (new_size < 1) {
                free(buf_new);
                goto error;
            }

            if (xpath_add_filter(buf_new, filters, filter_count)) {
                goto error;
            }
        }
    }

    return 0;

error:
    free(*buf);
    return -1;
}

/* modifies elem XML tree! */
static int
build_xpath_from_subtree_filter(struct ly_ctx *ctx, struct lyxml_elem *elem, char ***filters, int *filter_count)
{
    const struct lys_module *module, **modules, **modules_new;
    const struct lys_node *node;
    struct lyxml_elem *next;
    char *buf;
    uint32_t i, module_count;

    LY_TREE_FOR(elem, next) {
        /* first filter node, it must always have a namespace */
        modules = NULL;
        module_count = 0;
        if (next->ns && strcmp(next->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
            modules = malloc(sizeof *modules);
            if (!modules) {
                EMEM;
                goto error;
            }
            module_count = 1;
            modules[0] = ly_ctx_get_module_by_ns(ctx, next->ns->value, NULL);
            if (!modules[0]) {
                /* not really an error */
                free(modules);
                continue;
            }
        } else {
            i = 0;
            while ((module = ly_ctx_get_module_iter(ctx, &i))) {
                node = NULL;
                while ((node = lys_getnext(node, NULL, module, 0))) {
                    if (!strcmp(node->name, next->name)) {
                        modules_new = realloc(modules, (module_count + 1) * sizeof *modules);
                        if (!modules_new) {
                            EMEM;
                            goto error;
                        }
                        ++module_count;
                        modules = modules_new;
                        modules[module_count - 1] = module;
                        break;
                    }
                }
            }
        }

        buf = NULL;
        for (i = 0; i < module_count; ++i) {
            if (!next->child && next->content && !strws(next->content)) {
                /* special case of top-level content match node */
                if (xpath_buf_add_top_content(ctx, next, modules[i]->name, filters, filter_count)) {
                    goto error;
                }
            } else {
                /* containment or selection node */
                if (xpath_buf_add(ctx, next, modules[i]->name, modules[i]->ns, &buf, 1, filters, filter_count)) {
                    goto error;
                }
            }
        }
        free(modules);
    }

    return 0;

error:
    free(modules);
    for (i = 0; (signed)i < *filter_count; ++i) {
        free((*filters)[i]);
    }
    free(*filters);
    return -1;
}

struct nc_server_reply *
op_get(struct lyd_node *rpc, struct nc_session *ncs)
{
    sr_val_t *values = NULL;
    size_t value_count = 0;
    const struct lys_module *module;
    const struct lys_node *snode;
    struct lyd_node_leaf_list *leaf;
    struct lyd_node *root = NULL, *node;
    struct lyd_attr *attr;
    char **filters = NULL, buf[21], *path, *data = NULL;
    int rc, filter_count = 0;
    uint32_t i, j;
    struct lyxml_elem *subtree_filter;
    struct np2sr_sessions *sessions;
    struct ly_set *nodeset;
    sr_session_ctx_t *ds;
    struct nc_server_error *e;
    int wd_flag, data_flag;
    NC_WD_MODE nc_wd;


    /* get sysrepo connections for this session */
    sessions = (struct np2sr_sessions *)nc_session_get_data(ncs);

    /* get know which datastore is being affected */
    if (!strcmp(rpc->schema->name, "get")) {
        data_flag = LYD_OPT_GET;
        ds = sessions->running;
    } else { /* get-config */
        data_flag = LYD_OPT_GETCONFIG;
        nodeset = lyd_get_node(rpc, "/ietf-netconf:get-config/source/*");
        if (!strcmp(nodeset->set.d[0]->schema->name, "running")) {
            ds = sessions->running_config;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "startup")) {
            ds = sessions->startup;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
            ds = sessions->candidate;
        } else {
            ERR("Invalid <get-config> source (%s)", nodeset->set.d[0]->schema->name);
            ly_set_free(nodeset);
            goto error;
        }
        /* TODO URL capability */

        ly_set_free(nodeset);
    }

    /* create filters */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:*/filter");
    if (nodeset->number) {
        node = nodeset->set.d[0];
        ly_set_free(nodeset);
        LY_TREE_FOR(node->attr, attr) {
            if (!strcmp(attr->name, "type")) {
                if (!strcmp(attr->value, "xpath")) {
                    LY_TREE_FOR(node->attr, attr) {
                        if (!strcmp(attr->name, "select")) {
                            break;
                        }
                    }
                    if (!attr) {
                        ERR("RPC with an XPath filter without the \"select\" attribute.");
                        goto error;
                    }
                    break;
                } else if (!strcmp(attr->value, "subtree")) {
                    attr = NULL;
                    break;
                }
            }
        }

        if (!attr) {
            /* subtree */
            if (!((struct lyd_node_anyxml *)node)->value.str) {
                /* empty filter (checks both formats), fair enough */
                goto send_reply;
            }

            if (((struct lyd_node_anyxml *)node)->xml_struct) {
                subtree_filter = ((struct lyd_node_anyxml *)node)->value.xml;
            } else {
                subtree_filter = lyxml_parse_mem(np2srv.ly_ctx, ((struct lyd_node_anyxml *)node)->value.str, LYXML_PARSE_MULTIROOT);
            }
            if (!subtree_filter) {
                goto error;
            }

            if (build_xpath_from_subtree_filter(np2srv.ly_ctx, subtree_filter, &filters, &filter_count)) {
                goto error;
            }
        } else {
            /* xpath */
            if (!attr->value || !attr->value[0]) {
                /* empty select, okay, I guess... */
                goto send_reply;
            }
            path = strdup(attr->value);
            if (!path) {
                EMEM;
                goto error;
            }
            if (xpath_add_filter(path, &filters, &filter_count)) {
                free(path);
                goto error;
            }
        }
    } else {
        ly_set_free(nodeset);

        i = 0;
        while ((module = ly_ctx_get_module_iter(np2srv.ly_ctx, &i))) {
            LY_TREE_FOR(module->data, snode) {
                if (!(snode->nodetype & (LYS_GROUPING | LYS_NOTIF | LYS_RPC))) {
                    /* module with some actual data definitions */
                    break;
                }
            }

            /* TODO ietf-yang-library data should be created by us */

            if (snode) {
                asprintf(&path, "/%s:*", module->name);
                if (xpath_add_filter(path, &filters, &filter_count)) {
                    free(path);
                    goto error;
                }
            }
        }
    }

    /* get with-defaults mode */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:*/ietf-netconf-with-defaults:with-defaults");
    if (nodeset->number) {
        leaf = (struct lyd_node_leaf_list *)nodeset->set.d[0];
        if (!strcmp(leaf->value_str, "report-all")) {
            nc_wd = NC_WD_ALL;
        } else if (!strcmp(leaf->value_str, "report-all-tagged")) {
            nc_wd = NC_WD_ALL_TAG;
        } else if (!strcmp(leaf->value_str, "trim")) {
            nc_wd = NC_WD_TRIM;
        } else if (!strcmp(leaf->value_str, "explicit")) {
            nc_wd = NC_WD_EXPLICIT;
        } else {
            /* we received it, so it was validated, this cannot be */
            EINT;
            goto error;
        }
    } else {
        nc_server_get_capab_withdefaults(&nc_wd, NULL);
    }
    ly_set_free(nodeset);

    /* transform from NC_WD_ to LYD_WD_ */
    switch (nc_wd) {
    case NC_WD_ALL:
        wd_flag = LYD_WD_ALL;
        break;
    case NC_WD_ALL_TAG:
        wd_flag = LYD_WD_ALL_TAG;
        break;
    case NC_WD_TRIM:
        wd_flag = LYD_WD_TRIM;
        break;
    case NC_WD_EXPLICIT:
        /* TODO waiting for libyang support */
        //wd_flag = LYD_WD_EXPLICIT;
        wd_flag = 0;
        break;
    default:
        EINT;
        goto error;
    }

    /* refresh sysrepo data */
    sr_session_refresh(ds);

    /* create the data tree for the data reply */
    for (i = 0; (signed)i < filter_count; i++) {
        rc = sr_get_items(ds, filters[i], &values, &value_count);
        if ((rc == SR_ERR_UNKNOWN_MODEL) || (rc == SR_ERR_NOT_FOUND)) {
            /* skip internal modules not known to sysrepo and modules without data */
            continue;
        } else if (rc != SR_ERR_OK) {
            ERR("Getting items (%s) from sysrepo failed (%s).", filters[i], sr_strerror(rc));
            goto error;
        }

        for (j = 0; j < value_count; ++j) {
            /* create subtree root */
            ly_errno = LY_SUCCESS;
            node = lyd_new_path(root, np2srv.ly_ctx, values[j].xpath,
                                get_srval_value(np2srv.ly_ctx, &values[j], buf), LYD_PATH_OPT_UPDATE);
            if (ly_errno) {
                goto error;
            }

            if (!root) {
                root = node;
            }

            /* create the full subtree */
            if (build_subtree(ds, root, values[j].xpath)) {
                goto error;
            }
        }

        sr_free_values(values, value_count);
        value_count = 0;
        values = NULL;
    }

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    free(filters);

    /* debug
    lyd_print_file(stdout, root, LYD_XML_FORMAT, LYP_WITHSIBLINGS);
    debug */

send_reply:
    /* build RPC Reply */
    if (root) {
        if (lyd_wd_add(np2srv.ly_ctx, &root, wd_flag | data_flag)) {
            goto error;
        }
        lyd_print_mem(&data, root, LYD_XML, LYP_WITHSIBLINGS);
        lyd_free_withsiblings(root);
    }
    snode = ly_ctx_get_node(np2srv.ly_ctx, rpc->schema, "output/data");
    root = lyd_output_new_anyxml_str(snode, data);
    return nc_server_reply_data(root, NC_PARAMTYPE_FREE);

error:
    sr_free_values(values, value_count);

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    free(filters);

    lyd_free_withsiblings(root);

    e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
    nc_err_set_msg(e, np2log_lasterr(), "en");
    return nc_server_reply_err(e);
}

struct nc_server_reply *
op_lock(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct np2sr_sessions *sessions;
    sr_session_ctx_t *ds;
    struct nc_session **dsl;
    struct ly_set *nodeset;
    struct nc_server_error *e;
    const char *dsname;
    int rc;

    /* get sysrepo connections for this session */
    sessions = (struct np2sr_sessions *)nc_session_get_data(ncs);

    /* get know which datastore is being affected */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:lock/target/*");
    dsname = nodeset->set.d[0]->schema->name;
    ly_set_free(nodeset);

    if (!strcmp(dsname, "running")) {
        /* TODO additional requirements in case of supporting confirmed-commit */
        ds = sessions->running;
        dsl = &dslock.running;
    } else if (!strcmp(dsname, "startup")) {
        ds = sessions->startup;
        dsl = &dslock.startup;
    /* TODO sysrepo does not support candidate, RFC 6020 has some addition requirements here
    } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
        ds = sessions->candidate;
        dsl = &dslock.candidate;
    */
    } else {
        ERR("Invalid <lock> target (%s)", dsname);
        e = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_PROT);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    }

    pthread_rwlock_rdlock(&dslock_rwl);
    if (*dsl) {
lock_held:
        /* lock already held */
        pthread_rwlock_unlock(&dslock_rwl);
        ERR("Locking datastore %s by session %d failed (datastore is already locked by session %d).",
            dsname, nc_session_get_id(ncs), nc_session_get_id(*dsl));
        e = nc_err(NC_ERR_LOCK_DENIED, nc_session_get_id(*dsl));
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    }
    pthread_rwlock_unlock(&dslock_rwl);

    pthread_rwlock_wrlock(&dslock_rwl);
    /* check again dsl, it could change between unlock and wrlock */
    if (*dsl) {
        goto lock_held;
    }

    rc = sr_lock_datastore(ds);
    if (rc != SR_ERR_OK) {
        /* lock is held outside Netopeer */
        pthread_rwlock_unlock(&dslock_rwl);
        ERR("Locking datastore %s by session %d failed (%s).", dsname,
            nc_session_get_id(ncs), sr_strerror(rc));
        e = nc_err(NC_ERR_LOCK_DENIED, 0);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    }

    /* update local information about locks */
    *dsl = ncs;
    pthread_rwlock_unlock(&dslock_rwl);

    /* build positive RPC Reply */
    return nc_server_reply_ok();
}

struct nc_server_reply *
op_unlock(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct np2sr_sessions *sessions;
    sr_session_ctx_t *ds;
    struct nc_session **dsl;
    struct ly_set *nodeset;
    const char *dsname;
    struct nc_server_error *e;
    int rc;

    /* get sysrepo connections for this session */
    sessions = (struct np2sr_sessions *)nc_session_get_data(ncs);

    /* get know which datastore is being affected */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:unlock/target/*");
    dsname = nodeset->set.d[0]->schema->name;
    ly_set_free(nodeset);

    if (!strcmp(dsname, "running")) {
        ds = sessions->running;
        dsl = &dslock.running;
    } else if (!strcmp(dsname, "startup")) {
        ds = sessions->startup;
        dsl = &dslock.startup;
    /* TODO sysrepo does not support candidate
    } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
        ds = sessions->candidate;
        dsl = &dslock.candidate;
    */
    } else {
        ERR("Invalid <unlock> target (%s)", dsname);
        e = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_PROT);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    }

    pthread_rwlock_rdlock(&dslock_rwl);
    if (!(*dsl)) {
        /* lock is not held */
        pthread_rwlock_unlock(&dslock_rwl);
        ERR("Unlocking datastore %s by session %d failed (lock is not active).",
            dsname, nc_session_get_id(ncs));
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_PROT);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    } else {
        /* lock is held, but by who? */
        if ((*dsl) != ncs) {
            /* by someone else */
            pthread_rwlock_unlock(&dslock_rwl);
            ERR("Unlocking datastore %s by session %d failed (lock is held by session %d).",
                dsname, nc_session_get_id(ncs), nc_session_get_id(*dsl));
            e = nc_err(NC_ERR_LOCK_DENIED, nc_session_get_id(*dsl));
            nc_err_set_msg(e, np2log_lasterr(), "en");
            return nc_server_reply_err(e);
        }
    }
    pthread_rwlock_unlock(&dslock_rwl);
    pthread_rwlock_wrlock(&dslock_rwl);

    rc = sr_unlock_datastore(ds);
    if (rc != SR_ERR_OK) {
        /* lock is held outside Netopeer */
        pthread_rwlock_unlock(&dslock_rwl);
        ERR("Unlocking datastore %s by session %d failed (%s).", dsname,
            nc_session_get_id(ncs), sr_strerror(rc));
        e = nc_err(NC_ERR_LOCK_DENIED, 0);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        return nc_server_reply_err(e);
    }

    /* update local information about locks */
    *dsl = NULL;
    pthread_rwlock_unlock(&dslock_rwl);

    /* build positive RPC Reply */
    return nc_server_reply_ok();
}

static enum NP2_EDIT_OP
get_edit_op(struct lyd_node *node, enum NP2_EDIT_OP parentop, enum NP2_EDIT_DEFOP defop)
{
    struct lyd_attr *attr;

    assert(node);

    /* TODO check conflicts between parent and current operations */
    for (attr = node->attr; attr; attr = attr->next) {
        if (!strcmp(attr->name, "operation") &&
                !strcmp(attr->module->name, "ietf-netconf")) {
            /* NETCONF operation attribute */
            if (!strcmp(attr->value, "create")) {
                return NP2_EDIT_CREATE;
            } else if (!strcmp(attr->value, "delete")) {
                return NP2_EDIT_DELETE;
            } else if (!strcmp(attr->value, "remove")) {
                return NP2_EDIT_REMOVE;
            } else if (!strcmp(attr->value, "replace")) {
                return NP2_EDIT_REPLACE;
            } else if (!strcmp(attr->value, "merge")) {
                return NP2_EDIT_REPLACE;
            }
        }
    }

    if (parentop > 0) {
        return parentop;
    } else {
        return (enum NP2_EDIT_OP) defop;
    }
}

struct nc_server_reply *
op_editconfig(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct nc_server_error *e = NULL;
    struct np2sr_sessions *sessions;
    sr_session_ctx_t *ds;
    struct ly_set *nodeset;
    const char *value;
    enum NP2_EDIT_DEFOP defop;
    enum NP2_EDIT_TESTOPT testopt;
    struct lyxml_elem *config_xml;
    struct lyd_node *config = NULL, *next, *iter;
    char *str, path[1024];
    enum NP2_EDIT_OP *op = NULL, *op_new;
    int op_index, op_size, path_index = 0, missing_keys = 0;
    int ret;
    struct lys_node_container *cont;

    /* init */
    path[path_index] = '\0';

    /* get sysrepo connections for this session */
    sessions = (struct np2sr_sessions *)nc_session_get_data(ncs);

    /*
     * parse parameters
     */

    /* target */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:edit-config/target/*");
    value = nodeset->set.d[0]->schema->name;
    ly_set_free(nodeset);

    if (!strcmp(value, "running")) {
        ds = sessions->running;
    /* TODO sysrepo does not support candidate
    } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
        ds = sessions->candidate;
        dsl = &dslock.candidate;
    */
    }

    /* default-operation */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:edit-config/default-operation");
    if (nodeset->number) {
        value = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value_str;
        if (!strcmp(value, "merge")) {
            defop = NP2_EDIT_DEFOP_MERGE;
        } else if (!strcmp(value, "replace")) {
            defop = NP2_EDIT_DEFOP_REPLACE;
        } else if (!strcmp(value, "none")) {
            defop = NP2_EDIT_DEFOP_NONE;
        }
    } else {
        /* default value for default-operation is "merge" */
        defop = NP2_EDIT_DEFOP_MERGE;
    }
    ly_set_free(nodeset);

    /* test-option */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:edit-config/test-otion");
    if (nodeset->number) {
        value = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value_str;
        if (!strcmp(value, "test-then-set")) {
            testopt = NP2_EDIT_TESTOPT_TESTANDSET;
        } else if (!strcmp(value, "set")) {
            testopt = NP2_EDIT_TESTOPT_SET;
        } else if (!strcmp(value, "test-only")) {
            testopt = NP2_EDIT_TESTOPT_TEST;
        }
    } else {
        /* default value for test-option is "test-then-set" */
        testopt = NP2_EDIT_TESTOPT_TESTANDSET;
    }
    ly_set_free(nodeset);

    /* error-option is ignored, rollback is always done */

    /* config */
    nodeset = lyd_get_node(rpc, "/ietf-netconf:edit-config/config");
    if (nodeset->number) {
        config_xml = ((struct lyd_node_anyxml *)nodeset->set.d[0])->value.xml;
        ly_set_free(nodeset);

        config = lyd_parse_xml(np2srv.ly_ctx, &config_xml, LYD_OPT_EDIT | LYD_OPT_DESTRUCT);
        if (ly_errno) {
            return nc_server_reply_err(nc_err_libyang());
        } else if (!config) {
            /* nothing to do */
            return nc_server_reply_ok();
        }
    } else {
        /* TODO support for :url capability */
        ly_set_free(nodeset);
        goto internalerror;
    }

    lyd_print_mem(&str, config, LYD_XML, LYP_WITHSIBLINGS | LYP_FORMAT);
    VRB("EDIT-CONFIG: ds %d, defop %d, testopt %d, config:\n%s", ds, defop, testopt, str);
    free(str);

    /*
     * data manipulation
     */

    op_size = 16;
    op = malloc(op_size * sizeof *op);
    op[0] = NP2_EDIT_NONE;
    op_index = 0;
    LY_TREE_DFS_BEGIN(config, next, iter) {

        /* maintain list of operations */
        op_index++;
        if (op_index == op_size) {
            op_size += 16;
            op_new = realloc(op, op_size * sizeof *op);
            if (!op_new) {
                ERR("%s: memory allocation failed (%s)", __func__, strerror(errno));
                goto internalerror;
            }
            op = op_new;
        }
        op[op_index] = get_edit_op(iter, op[op_index - 1], defop);

        /* maintain path */
        if (!iter->parent || lyd_node_module(iter) != lyd_node_module(iter->parent)) {
            /* with prefix */
            path_index += sprintf(&path[path_index], "/%s:%s", lyd_node_module(iter)->name, iter->schema->name);
        } else {
            /* without prefix */
            if (missing_keys) {
                path_index += sprintf(&path[path_index], "[%s=\'%s\']", iter->schema->name,
                                      ((struct lyd_node_leaf_list *)iter)->value_str);
            } else {
                path_index += sprintf(&path[path_index], "/%s", iter->schema->name);
            }
        }

        /* specific work for different node types */
        ret = -1;
        switch(iter->schema->nodetype) {
        case LYS_CONTAINER:
            cont = (struct lys_node_container *)iter->schema;
            if (!cont->presence) {
                /* do nothing */
                break;
            }

            VRB("EDIT_CONFIG: presence container %s, operation %d", path, op[op_index]);
            break;
        case LYS_LEAF:
            if (missing_keys) {
                /* still processing list keys */
                missing_keys--;
                if (!missing_keys) {
                    /* the last key, create the list instance */
                    VRB("EDIT_CONFIG: list %s, operation %d", path, op[op_index]);
                    /* TODO */
                }

                /* make sure that LY_TREE_DFS_END does not remove the predicate */
                path[path_index++] = '/';
                path[path_index] = '\0';
                goto dfs_continue;
            }

            /* regular leaf */
            VRB("EDIT_CONFIG: leaf %s, operation %d", path, op[op_index]);

            break;
        case LYS_LEAFLIST:
            /* TODO process insert, if any, and after regular creation apply sr_move_item() */
            break;
        case LYS_LIST:
            missing_keys = ((struct lys_node_list *)iter->schema)->keys_size;
            /* must be processed later when we get know keys */
            goto dfs_continue;
        default:
            break;
        }

        /* apply change to sysrepo */
        switch (op[op_index]) {
        case NP2_EDIT_MERGE:
        case NP2_EDIT_REPLACE:
            /* create the node */
            ret = sr_set_item(ds, path, NULL, 0);
            break;
        case NP2_EDIT_CREATE:
            /* create the node, but it must not exists */
            ret = sr_set_item(ds, path, NULL, SR_EDIT_STRICT);
            break;
        case NP2_EDIT_DELETE:
            /* remove the node, but it must exists */
            ret = sr_delete_item(ds, path, SR_EDIT_STRICT);
            break;
        case NP2_EDIT_REMOVE:
            /* remove the node */
            ret = sr_delete_item(ds, path, 0);
            break;
        default:
            /* do nothing */
            break;
        }

        /* check the result */
        switch (ret) {
        case SR_ERR_OK:
            VRB("EDIT_CONFIG: success (%s)", path);
            /* no break */
        case -1:
            /* do nothing */
            break;
        case SR_ERR_UNAUTHORIZED:
            e = nc_err(NC_ERR_ACCESS_DENIED, NC_ERR_TYPE_PROT);
            nc_err_set_path(e, path);
            goto cleanup;
        case SR_ERR_DATA_EXISTS:
            e = nc_err(NC_ERR_DATA_EXISTS, NC_ERR_TYPE_PROT);
            nc_err_set_path(e, path);
            goto cleanup;
        case SR_ERR_DATA_MISSING:
            e = nc_err(NC_ERR_DATA_MISSING, NC_ERR_TYPE_PROT);
            nc_err_set_path(e, path);
            goto cleanup;
        default:
            /* not covered error */
            goto internalerror;
        }

dfs_continue:
        /* where go next? - modified LY_TREE_DFS_END */
        if (iter->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML)) {
            next = NULL;
        } else {
            next = iter->child;
        }
        if (!next) {
            /* no children, try siblings */
            next = iter->next;

            /* maintain "stack" variables */
            op_index--;
            str = strrchr(path, '/');
            if (str) {
                *str = '\0';
                path_index = str - path;
            } else {
                path[0] = '\0';
                path_index = 0;
            }
        }
        while (!next) {
            iter = iter->parent;

            /* parent is already processed, go to its sibling */
            if (!iter) {
                /* we are done */
                break;
            }
            next = iter->next;

            /* maintain "stack" variables */
            op_index--;
            str = strrchr(path, '/');
            if (str) {
                *str = '\0';
                path_index = str - path;
            } else {
                path[0] = '\0';
                path_index = 0;
            }

        }
        /* end of modified LY_TREE_DFS_END */
    }

cleanup:
    /* cleanup */
    free(op);
    lyd_free_withsiblings(config);

    if (e) {
        /* send error reply */
        return nc_server_reply_err(e);
    } else {
        /* build positive RPC Reply */
        return nc_server_reply_ok();
    }

internalerror:
    free(op);
    lyd_free_withsiblings(config);

    e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
    nc_err_set_msg(e, np2log_lasterr(), "en");
    return nc_server_reply_err(e);
}


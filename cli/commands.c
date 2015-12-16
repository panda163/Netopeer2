/**
 * @file commands.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief libyang's yanglint tool commands
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <pwd.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

#ifdef ENABLE_TLS
#   include <openssl/pem.h>
#   include <openssl/x509v3.h>
#endif

#include <libyang/libyang.h>
#include <nc_client.h>

#include "commands.h"
#include "configuration.h"
#include "completion.h"

#define CLI_CH_TIMEOUT 60 /* 1 minute */

#define NC_CAP_WRITABLERUNNING_ID "urn:ietf:params:netconf:capability:writable-running"
#define NC_CAP_CANDIDATE_ID       "urn:ietf:params:netconf:capability:candidate"
#define NC_CAP_CONFIRMEDCOMMIT_ID "urn:ietf:params:netconf:capability:confirmed-commit:1.1"
#define NC_CAP_ROLLBACK_ID        "urn:ietf:params:netconf:capability:rollback-on-error"
#define NC_CAP_VALIDATE10_ID      "urn:ietf:params:netconf:capability:validate:1.0"
#define NC_CAP_VALIDATE11_ID      "urn:ietf:params:netconf:capability:validate:1.1"
#define NC_CAP_STARTUP_ID         "urn:ietf:params:netconf:capability:startup"
#define NC_CAP_URL_ID             "urn:ietf:params:netconf:capability:url"
#define NC_CAP_XPATH_ID           "urn:ietf:params:netconf:capability:xpath"
#define NC_CAP_WITHDEFAULTS_ID    "urn:ietf:params:netconf:capability:with-defaults"
#define NC_CAP_NOTIFICATION_ID    "urn:ietf:params:netconf:capability:notification"
#define NC_CAP_INTERLEAVE_ID      "urn:ietf:params:netconf:capability:interleave"

COMMAND commands[];
extern int done;
extern char *search_path;

char *config_editor;
struct nc_session *session;
volatile pthread_t ntf_tid;
volatile int interleave;
struct ly_ctx *ctx;

struct arglist {
    char** list;
    int count;
    int size;
};

static void
init_arglist(struct arglist *args)
{
    if (args != NULL) {
        args->list = NULL;
        args->count = 0;
        args->size = 0;
    }
}

static void
clear_arglist(struct arglist *args)
{
    int i = 0;

    if (args && args->list) {
        for (i = 0; i < args->count; i++) {
            if (args->list[i]) {
                free(args->list[i]);
            }
        }
        free(args->list);
    }

    init_arglist(args);
}

static void
addargs(struct arglist *args, char *format, ...)
{
    va_list arguments;
    char *aux = NULL, *aux1 = NULL;
    int len;

    if (args == NULL) {
        return;
    }

    /* store arguments to aux string */
    va_start(arguments, format);
    if ((len = vasprintf(&aux, format, arguments)) == -1) {
        perror("addargs - vasprintf");
    }
    va_end(arguments);

    /* parse aux string and store it to the arglist */
    /* find \n and \t characters and replace them by space */
    while ((aux1 = strpbrk(aux, "\n\t")) != NULL) {
        *aux1 = ' ';
    }
    /* remember the begining of the aux string to free it after operations */
    aux1 = aux;

    /*
     * get word by word from given string and store words separately into
     * the arglist
     */
    for (aux = strtok(aux, " "); aux; aux = strtok(NULL, " ")) {
        if (!strcmp(aux, ""))
        continue;

        if (!args->list) { /* initial memory allocation */
            if ((args->list = (char **)malloc(8 * sizeof(char *))) == NULL) {
                perror("Fatal error while allocating memory");
            }
            args->size = 8;
            args->count = 0;
        } else if (args->count + 2 >= args->size) {
            /*
             * list is too short to add next to word so we have to
             * extend it
             */
            args->size += 8;
            args->list = realloc(args->list, args->size * sizeof(char *));
        }
        /* add word in the end of the list */
        if ((args->list[args->count] = malloc((strlen(aux) + 1) * sizeof(char))) == NULL) {
            perror("Fatal error while allocating memory");
        }
        strcpy(args->list[args->count], aux);
        args->list[++args->count] = NULL; /* last argument */
    }

    /* clean up */
    free(aux1);
}

static void *
cli_ntf_thread(void *arg)
{
    NC_MSG_TYPE msgtype;
    struct nc_notif *notif;
    FILE *output = (FILE *)arg;
    int was_rawmode;

    while (1) {
        msgtype = nc_recv_notif(session, 0, &notif);

        if (!ntf_tid) {
            break;
        } else if (msgtype == NC_MSG_WOULDBLOCK) {
            usleep(1000);
        } else if (msgtype == NC_MSG_NOTIF) {
            if (output == stdout) {
                if (ls.rawmode) {
                    was_rawmode = 1;
                    linenoiseDisableRawMode(ls.ifd);
                    printf("\n");
                } else {
                    was_rawmode = 0;
                }
            }

            /* TODO print datetime */
            lyd_print_file(output, notif->tree, LYD_JSON);
            fprintf(output, "\n");
            fflush(output);

            if ((output == stdout) && was_rawmode) {
                linenoiseEnableRawMode(ls.ifd);
                linenoiseRefreshLine();
            }

            nc_notif_free(notif);
        }
    }

    if (output != stdout) {
        fclose(output);
    }
    ntf_tid = 0;
    interleave = 1;
    return NULL;
}

static int
cli_send_recv(struct nc_rpc *rpc, FILE *output)
{
    char *str, *model_data, *ptr, *ptr2;
    int i, j, ret;
    uint64_t msgid;
    NC_MSG_TYPE msgtype;
    struct nc_reply *reply;
    struct nc_reply_data *data_rpl;
    struct nc_reply_error *error;

    msgtype = nc_send_rpc(session, rpc, 1000, &msgid);
    if (msgtype == NC_MSG_ERROR) {
        ERROR(__func__, "Failed to send the RPC.");
        return -1;
    } else if (msgtype == NC_MSG_WOULDBLOCK) {
        ERROR(__func__, "Timeout for sending the RPC expired.");
        return -1;
    }

    msgtype = nc_recv_reply(session, rpc, msgid, 1000, &reply);
    if (msgtype == NC_MSG_ERROR) {
        ERROR(__func__, "Failed to receive a reply.");
        return -1;
    } else if (msgtype == NC_MSG_WOULDBLOCK) {
        ERROR(__func__, "Timeout for receiving a reply expired.");
        return -1;
    }

    switch (reply->type) {
    case NC_REPLY_OK:
        fprintf(output, "OK\n");
        ret = 0;
        break;
    case NC_REPLY_DATA:
        data_rpl = (struct nc_reply_data *)reply;

        /* special case */
        if (nc_rpc_get_type(rpc) == NC_RPC_GETSCHEMA) {
            if (output == stdout) {
                fprintf(output, "MODULE\n");
            }
            str = lyxml_serialize(((struct lyd_node_anyxml *)data_rpl->data)->value);
            if (!str) {
                ERROR(__func__, "Failed to get the model data from the reply.\n");
                nc_reply_free(reply);
                return EXIT_FAILURE;
            }

            ptr = strchr(str, '>');
            ++ptr;
            ptr2 = strrchr(str, '<');

            model_data = strndup(ptr, strlen(ptr) - strlen(ptr2));
            free(str);

            fputs(model_data, output);
            free(model_data);
            if (output == stdout) {
                fprintf(output, "\n");
            }
            break;
        }

        if (output == stdout) {
            fprintf(output, "DATA\n");
        }
        lyd_print_file(output, data_rpl->data, LYD_JSON);
        if (output == stdout) {
            fprintf(output, "\n");
        }
        ret = 0;
        break;
    case NC_REPLY_ERROR:
        fprintf(output, "ERROR\n");
        error = (struct nc_reply_error *)reply;
        for (i = 0; i < error->err_count; ++i) {
            if (error->err[i].type) {
                fprintf(output, "\ttype:     %s\n", error->err[i].type);
            }
            if (error->err[i].tag) {
                fprintf(output, "\ttag:      %s\n", error->err[i].tag);
            }
            if (error->err[i].severity) {
                fprintf(output, "\tseverity: %s\n", error->err[i].severity);
            }
            if (error->err[i].apptag) {
                fprintf(output, "\tapp-tag:  %s\n", error->err[i].apptag);
            }
            if (error->err[i].path) {
                fprintf(output, "\tpath:     %s\n", error->err[i].path);
            }
            if (error->err[i].message) {
                fprintf(output, "\tmessage:  %s\n", error->err[i].message);
            }
            if (error->err[i].sid) {
                fprintf(output, "\tSID:      %s\n", error->err[i].sid);
            }
            for (j = 0; j < error->err[i].attr_count; ++j) {
                fprintf(output, "\tbad-attr #%d: %s\n", j + 1, error->err[i].attr[j]);
            }
            for (j = 0; j < error->err[i].elem_count; ++j) {
                fprintf(output, "\tbad-elem #%d: %s\n", j + 1, error->err[i].elem[j]);
            }
            for (j = 0; j < error->err[i].ns_count; ++j) {
                fprintf(output, "\tbad-ns #%d:   %s\n", j + 1, error->err[i].ns[j]);
            }
            for (j = 0; j < error->err[i].other_count; ++j) {
                str = lyxml_serialize(error->err[i].other[j]);
                fprintf(output, "\tother #%d:\n%s\n", j + 1, str);
                free(str);
            }
            fprintf(output, "\n");
        }
        ret = 1;
        break;
    default:
        ERROR(__func__, "Internal error.");
        nc_reply_free(reply);
        return -1;
    }

    nc_reply_free(reply);
    return ret;
}

void
cmd_searchpath_help(void)
{
    printf("searchpath <model-dir-path>\n");
}

void
cmd_verb_help(void)
{
    printf("verb (error/0 | warning/1 | verbose/2 | debug/3)\n");
}

void
cmd_connect_help(void)
{
#if defined(ENABLE_SSH) && defined(ENABLE_TLS)
    printf("connect [--help] [--host <hostname>] [--port <num>]\n");
    printf("    SSH [--ssh] [--login <username>]\n");
    printf("    TLS  --tls  [--cert <cert_path> [--key <key_path>]] [--trusted <trusted_CA_store.pem>]\n");
#elif defined(ENABLE_SSH)
    printf("connect [--help] [--ssh] [--host <hostname>] [--port <num>] [--login <username>]\n");
#elif defined(ENABLE_TLS)
    printf("connect [--help] [--tls] [--host <hostname>] [--port <num>] [--cert <cert_path> [--key <key_path>]] [--trusted <trusted_CA_store.pem>]\n");
#endif
}

void
cmd_listen_help(void)
{
#if defined(ENABLE_SSH) && defined(ENABLE_TLS)
    printf("listen [--help] [--timeout <sec>] [--port <num>]\n");
    printf("   SSH [--ssh] [--login <username>]\n");
    printf("   TLS  --tls  [--cert <cert_path> [--key <key_path>]] [--trusted <trusted_CA_store.pem>]\n");
#elif defined(ENABLE_SSH)
    printf("listen [--help] [--ssh] [--timeout <sec>] [--port <num>] [--login <username>]\n");
#elif defined(ENABLE_TLS)
    printf("listen [--help] [--tls] [--timeout <sec>] [--port <num>] [--cert <cert_path> [--key <key_path>]] [--trusted <trusted_CA_store.pem>]\n");
#endif
}

void
cmd_editor_help(void)
{
    printf("editor [--help] [<path/name_of_the_editor> | --none]\n");
}

void
cmd_cancelcommit_help(void)
{
    if (session && !nc_session_cpblt(session, NC_CAP_CONFIRMEDCOMMIT_ID)) {
        printf("cancel-commit is not supported by the current session.\n");
    } else {
        printf("cancel-commit [--help] --persist-id <commit-id>\n");
    }
}

void
cmd_commit_help(void)
{
    const char *confirmed;

    if (session && !nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        printf("commit is not supported by the current session.\n");
        return;
    }

    if (!session || nc_session_cpblt(session, NC_CAP_CONFIRMEDCOMMIT_ID)) {
        confirmed = " [--confirmed] [--confirm-timeout <sec>] [--persist <new-commit-id>] [--persist-id <commit-id>]";
    } else {
        confirmed = "";
    }
    printf("commit [--help]%s\n", confirmed);
}

void
cmd_copyconfig_help(void)
{
    int ds = 0;
    const char *running, *startup, *candidate, *url, *defaults;

    if (!session) {
        /* if session not established, print complete help for all capabilities */
        running = "running";
        startup = "|startup";
        candidate = "|candidate";
        url = "|url:<url>";
        defaults = " [--defaults report-all|report-all-tagged|trim|explicit]";
    } else {
        if (nc_session_cpblt(session, NC_CAP_WRITABLERUNNING_ID)) {
            running = "running";
            ds = 1;
        } else {
            running = "";
        }
        if (nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
            if (ds) {
                startup = "|startup";
            } else {
                startup = "startup";
                ds = 1;
            }
        } else {
            startup = "";
        }
        if (nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
            if (ds) {
                candidate = "|candidate";
            } else {
                candidate = "candidate";
                ds = 1;
            }
        } else {
            candidate = "";
        }
        if (nc_session_cpblt(session, NC_CAP_URL_ID)) {
            if (ds) {
                url = "|url:<url>";
            } else {
                url = "url:<url>";
                ds = 1;
            }
        } else {
            url = "";
        }

        if (!ds) {
            printf("copy-config is not supported by the current session.\n");
            return;
        }

        if (nc_session_cpblt(session, NC_CAP_WITHDEFAULTS_ID)) {
            defaults = " [--defaults report-all|report-all-tagged|trim|explicit]";
        } else {
            defaults = "";
        }
    }

    printf("copy-config [--help] --target %s%s%s%s (--source %s%s%s%s | --src-config [<file>])%s\n",
           running, startup, candidate, url,
           running, startup, candidate, url, defaults);
}

void
cmd_deleteconfig_help(void)
{
    const char *startup, *url;

    if (!session) {
        startup = "startup";
        url = "|url:<url>";
    } else {
        if (nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
            startup = "startup";
        } else {
            startup = "";
        }

        if (nc_session_cpblt(session, NC_CAP_URL_ID)) {
            url = strlen(startup) ? "|url:<url>" : "url:<url>";
        } else {
            url = "";
        }
    }

    if ((strlen(startup) + strlen(url)) == 0) {
        printf("delete-config is not supported by the current session.\n");
        return;
    }

    printf("delete-config [--help] --target %s%s\n", startup, url);
}

void
cmd_discardchanges_help(void)
{
    if (!session || nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        printf("discard-changes [--help]\n");
    } else {
        printf("discard-changes is not supported by the current session.\n");
    }
}

void
cmd_editconfig_help(void)
{
    const char *rollback, *validate, *running, *candidate, *url, *bracket;

    if (!session || nc_session_cpblt(session, NC_CAP_WRITABLERUNNING_ID)) {
        running = "running";
    } else {
        running = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        if (running[0]) {
            candidate = "|candidate";
        } else {
            candidate = "candidate";
        }
    } else {
        candidate = "";
    }

    if (!running[0] && !candidate[0]) {
        printf("edit-config is not supported by the current session.\n");
        return;
    }

    if (!session || nc_session_cpblt(session, NC_CAP_ROLLBACK_ID)) {
        rollback = "|rollback";
    } else {
        rollback = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_VALIDATE11_ID)) {
        validate = "[--test set|test-only|test-then-set] ";
    } else if (!session || nc_session_cpblt(session, NC_CAP_VALIDATE10_ID)) {
        validate = "[--test set|test-then-set] ";
    } else {
        validate = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_URL_ID)) {
        url = " | --url <url>)";
        bracket = "(";
    } else {
        url = "";
        bracket = "";
    }

    printf("edit-config [--help] --target %s%s %s--config [<file>]%s [--defop merge|replace|none] "
           "%s[--error stop|continue%s]\n", running, candidate, bracket, url, validate, rollback);
}

void
cmd_get_help(void)
{
    const char *defaults, *xpath;

    if (!session || nc_session_cpblt(session, NC_CAP_WITHDEFAULTS_ID)) {
        defaults = "[--defaults report-all|report-all-tagged|trim|explicit] ";
    } else {
        defaults = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_XPATH_ID)) {
        xpath = " | --filter-xpath <XPath>";
    } else {
        xpath = "";
    }

    fprintf(stdout, "get [--help] [--filter-subtree [<file>]%s] %s[--out <file>]\n", xpath, defaults);
}

void
cmd_getconfig_help(void)
{
    const char *defaults, *xpath, *candidate, *startup;

    /* if session not established, print complete help for all capabilities */
    if (!session || nc_session_cpblt(session, NC_CAP_WITHDEFAULTS_ID)) {
        defaults = "[--defaults report-all|report-all-tagged|trim|explicit] ";
    } else {
        defaults = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_XPATH_ID)) {
        xpath = " | --filter-xpath <XPath>";
    } else {
        xpath = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
        startup = "|startup";
    } else {
        startup = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        candidate = "|candidate";
    } else {
        candidate = "";
    }

    printf("get-config [--help] --source running%s%s [--filter-subtree [<file>]%s] %s[--out <file>]\n",
           startup, candidate, xpath, defaults);
}

void
cmd_killsession_help(void)
{
    printf("killsession [--help] --sid <sesion-ID>\n");
}

void
cmd_lock_help(void)
{
    const char *candidate, *startup;

    if (!session || nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
        startup = "|startup";
    } else {
        startup = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        candidate = "|candidate";
    } else {
        candidate = "";
    }

    printf("lock [--help] --target running%s%s\n", startup, candidate);
}

void
cmd_unlock_help(void)
{
    const char *candidate, *startup;

    if (!session || nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
        startup = "|startup";
    } else {
        startup = "";
    }

    if (!session || nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
        candidate = "|candidate";
    } else {
        candidate = "";
    }

    printf("unlock [--help] --target running%s%s\n", startup, candidate);
}

void
cmd_validate_help(void)
{
    const char *startup, *candidate, *url;

    if (session && !nc_session_cpblt(session, NC_CAP_VALIDATE10_ID)
            && !nc_session_cpblt(session, NC_CAP_VALIDATE11_ID)) {
        printf("validate is not supported by the current session.\n");
        return;
    }

    if (!session) {
        /* if session not established, print complete help for all capabilities */
        startup = "|startup";
        candidate = "|candidate";
        url = "|url:<url>";
    } else {
        if (nc_session_cpblt(session, NC_CAP_STARTUP_ID)) {
            startup = "|startup";
        } else {
            startup = "";
        }
        if (nc_session_cpblt(session, NC_CAP_CANDIDATE_ID)) {
            candidate = "|candidate";
        } else {
            candidate = "";
        }
        if (nc_session_cpblt(session, NC_CAP_URL_ID)) {
            url = "|url:<dsturl>";
        } else {
            url = "";
        }
    }
    printf("validate [--help] (--source running%s%s%s | --src-config [<file>])\n",
           startup, candidate, url);
}

void
cmd_subscribe_help(void)
{
    const char *xpath;

    if (session && !nc_session_cpblt(session, NC_CAP_NOTIFICATION_ID)) {
        printf("subscribe not supported by the current session.\n");
        return;
    }

    if (!session || nc_session_cpblt(session, NC_CAP_XPATH_ID)) {
        xpath = " | --filter-xpath <XPath>";
    } else {
        xpath = "";
    }

    printf("subscribe [--help] [--filter-subtree [<file>]%s] [--begin <time>] [--end <time>] [--stream <stream>] [--out <file>]\n", xpath);
    printf("\t<time> has following format:\n");
    printf("\t\t+<num>  - current time plus the given number of seconds.\n");
    printf("\t\t<num>   - absolute time as number of seconds since 1970-01-01.\n");
    printf("\t\t-<num>  - current time minus the given number of seconds.\n");
}

void
cmd_getschema_help(void)
{
    if (session && !ly_ctx_get_module(ctx, "ietf-netconf-monitoring", NULL)) {
        printf("get-schema is not supported by the current session.\n");
        return;
    }

    printf("get-schema [--help] --model <identifier> [--version <version>] [--format <format>] [--out <file>]\n");
}

void
cmd_userrpc_help(void)
{
    printf("user-rpc [--help] [--content <file>] [--out <file>]\n");
}

#ifdef ENABLE_SSH

void
cmd_auth_help(void)
{
    printf("auth (--help | pref [(publickey | interactive | password) <preference>] | keys [add <private_key_path>] [remove <key_index>])\n");
}

void
cmd_knownhosts_help(void)
{
    printf("knownhosts [--help] [--del <key_index>]\n");
}

#endif /* ENABLE_SSH */

#ifdef ENABLE_TLS

void
cmd_cert_help(void)
{
    printf("cert [--help | display | add <cert_path> | remove <cert_name> | displayown | replaceown (<cert_path.pem> | <cert_path.crt> <key_path.key>)]\n");
}

void
cmd_crl_help(void)
{
    printf("crl [--help | display | add <crl_path> | remove <crl_name>]\n");
}

#endif /* ENABLE_TLS */

#ifdef ENABLE_SSH

int
cmd_auth(const char *arg)
{
    int i;
    short int pref;
    char *args = strdupa(arg);
    char *cmd = NULL, *ptr = NULL, *str;
    const char *pub_key, *priv_key;

    cmd = strtok_r(args, " ", &ptr);
    cmd = strtok_r(NULL, " ", &ptr);
    if (cmd == NULL || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        cmd_auth_help();

    } else if (strcmp(cmd, "pref") == 0) {
        cmd = strtok_r(NULL, " ", &ptr);
        if (cmd == NULL) {
            printf("The SSH authentication method preferences:\n");
            if ((pref = nc_ssh_get_auth_pref(NC_SSH_AUTH_PUBLICKEY)) < 0) {
                printf("\t'publickey':   disabled\n");
            } else {
                printf("\t'publickey':   %d\n", pref);
            }
            if ((pref = nc_ssh_get_auth_pref(NC_SSH_AUTH_PASSWORD)) < 0) {
                printf("\t'password':    disabled\n");
            } else {
                printf("\t'password':    %d\n", pref);
            }
            if ((pref = nc_ssh_get_auth_pref(NC_SSH_AUTH_INTERACTIVE)) < 0) {
                printf("\t'interactive': disabled\n");
            } else {
                printf("\t'interactive': %d\n", pref);
            }

        } else if (strcmp(cmd, "publickey") == 0) {
            cmd = strtok_r(NULL, " ", &ptr);
            if (cmd == NULL) {
                ERROR("auth pref publickey", "Missing the preference argument");
                return EXIT_FAILURE;
            } else {
                nc_ssh_set_auth_pref(NC_SSH_AUTH_PUBLICKEY, atoi(cmd));
            }
        } else if (strcmp(cmd, "interactive") == 0) {
            cmd = strtok_r(NULL, " ", &ptr);
            if (cmd == NULL) {
                ERROR("auth pref interactive", "Missing the preference argument");
                return EXIT_FAILURE;
            } else {
                nc_ssh_set_auth_pref(NC_SSH_AUTH_INTERACTIVE, atoi(cmd));
            }
        } else if (strcmp(cmd, "password") == 0) {
            cmd = strtok_r(NULL, " ", &ptr);
            if (cmd == NULL) {
                ERROR("auth pref password", "Missing the preference argument");
                return EXIT_FAILURE;
            } else {
                nc_ssh_set_auth_pref(NC_SSH_AUTH_PASSWORD, atoi(cmd));
            }
        } else {
            ERROR("auth pref", "Unknown authentication method (%s)", cmd);
            return EXIT_FAILURE;
        }

    } else if (strcmp(cmd, "keys") == 0) {
        cmd = strtok_r(NULL, " ", &ptr);
        if (cmd == NULL) {
            printf("The keys used for SSH authentication:\n");
            if (nc_ssh_get_keypair_count() == 0) {
                printf("(none)\n");
            } else {
                for (i = 0; i < nc_ssh_get_keypair_count(); ++i) {
                    nc_ssh_get_keypair(i, &pub_key, &priv_key);
                    printf("#%d: %s (private %s)\n", i, pub_key, priv_key);
                }
            }
        } else if (strcmp(cmd, "add") == 0) {
            cmd = strtok_r(NULL, " ", &ptr);
            if (cmd == NULL) {
                ERROR("auth keys add", "Missing the key path");
                return EXIT_FAILURE;
            }

            asprintf(&str, "%s.pub", cmd);
            if (nc_ssh_add_keypair(str, cmd) != EXIT_SUCCESS) {
                ERROR("auth keys add", "Failed to add key");
                free(str);
                return EXIT_FAILURE;
            }

            if (eaccess(cmd, R_OK) != 0) {
                ERROR("auth keys add", "The new private key is not accessible (%s), but added anyway", strerror(errno));
            }
            if (eaccess(str, R_OK) != 0) {
                ERROR("auth keys add", "The public key for the new private key is not accessible (%s), but added anyway", strerror(errno));
            }
            free(str);

        } else if (strcmp(cmd, "remove") == 0) {
            cmd = strtok_r(NULL, " ", &ptr);
            if (cmd == NULL) {
                ERROR("auth keys remove", "Missing the key index");
                return EXIT_FAILURE;
            }

            i = strtol(cmd, &ptr, 10);
            if (ptr[0] || nc_ssh_del_keypair(i)) {
                ERROR("auth keys remove", "Wrong index");
                return EXIT_FAILURE;
            }
        } else {
            ERROR("auth keys", "Unknown argument %s", cmd);
            return EXIT_FAILURE;
        }

    } else {
        ERROR("auth", "Unknown argument %s", cmd);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int
cmd_knownhosts(const char *arg)
{
    char* ptr, *kh_file, *line = NULL, **pkeys = NULL, *text;
    int del_idx = -1, i, j, pkey_len = 0, written;
    size_t line_len, text_len;
    FILE* file;
    struct passwd* pwd;
    struct arglist cmd;
    struct option long_options[] = {
        {"help", 0, 0, 'h'},
        {"del", 1, 0, 'd'},
        {0, 0, 0, 0}
    };
    int option_index = 0, c;

    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hd:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_knownhosts_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
            break;
        case 'd':
            del_idx = strtol(optarg, &ptr, 10);
            if (*ptr != '\0' || del_idx < 0) {
                ERROR("knownhosts", "Wrong index");
                clear_arglist(&cmd);
                return EXIT_FAILURE;
            }
            break;
        default:
            ERROR("knownhosts", "Unknown option -%c", c);
            cmd_knownhosts_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }

    clear_arglist(&cmd);

    errno = 0;
    pwd = getpwuid(getuid());
    if (pwd == NULL) {
        if (errno == 0) {
            ERROR("knownhosts", "Failed to get the home directory of UID %d, it does not exist", getuid());
        } else {
            ERROR("knownhosts", "Failed to get a pwd entry (%s)", strerror(errno));
        }
        return EXIT_FAILURE;
    }

    asprintf(&kh_file, "%s/.ssh/known_hosts", pwd->pw_dir);

    if ((file = fopen(kh_file, "r+")) == NULL) {
        ERROR("knownhosts", "Cannot open \"%s\" (%s)", kh_file, strerror(errno));
        free(kh_file);
        return EXIT_FAILURE;
    }
    free(kh_file);

    /* list */
    if (del_idx == -1) {
        printf("ID Hostname Algorithm Key\n\n");

        errno = 0;
        i = 0;
        while (getline(&line, &line_len, file) > 0) {
            /* host number */
            printf("%d: ", i);

            /* host name */
            ptr = strtok(line, " ");
            if (ptr == NULL) {
                printf("INVALID\n");
                ++i;
                continue;
            }
            if (ptr[0] == '|' && ptr[2] == '|') {
                printf("(hashed hostname) ");
            } else {
                printf("%s ", ptr);
            }

            /* host key algorithm */
            ptr = strtok(NULL, " ");
            if (ptr == NULL) {
                printf("INVALID\n");
                ++i;
                continue;
            }
            printf("%s: ", ptr);

            /* host key */
            ptr = strtok(NULL, " ");
            if (ptr == NULL) {
                printf("INVALID\n");
                ++i;
                continue;
            }
            for (j = 0; j < pkey_len; ++j) {
                if (strcmp(ptr, pkeys[j]) == 0) {
                    break;
                }
            }
            if (j == pkey_len) {
                ++pkey_len;
                pkeys = realloc(pkeys, pkey_len*sizeof(char*));
                pkeys[j] = strdup(ptr);
            }
            printf("(key %d)\n", j);

            ++i;
        }

        if (i == 0) {
            printf("(none)\n");
        }
        printf("\n");

        for (j = 0; j < pkey_len; ++j) {
            free(pkeys[j]);
        }
        free(pkeys);
        free(line);

    /* delete */
    } else {
        fseek(file, 0, SEEK_END);
        text_len = ftell(file);
        if (text_len < 0) {
            ERROR("knownhosts", "ftell on the known hosts file failed (%s)", strerror(errno));
            fclose(file);
            return EXIT_FAILURE;
        }
        fseek(file, 0, SEEK_SET);

        text = malloc(text_len + 1);
        text[text_len] = '\0';

        if (fread(text, 1, text_len, file) < text_len) {
            ERROR("knownhosts", "Cannot read known hosts file (%s)", strerror(ferror(file)));
            free(text);
            fclose(file);
            return EXIT_FAILURE;
        }
        fseek(file, 0, SEEK_SET);

        for (i = 0, ptr = text; (i < del_idx) && ptr; ++i, ptr = strchr(ptr + 1, '\n'));

        if (!ptr || (strlen(ptr) < 2)) {
            ERROR("knownhosts", "Key index %d does not exist", del_idx);
            free(text);
            fclose(file);
            return EXIT_FAILURE;
        }

        if (ptr[0] == '\n') {
            ++ptr;
        }

        /* write the old beginning */
        written = fwrite(text, 1, ptr - text, file);
        if (written < ptr-text) {
            ERROR("knownhosts", "Failed to write to known hosts file (%s)", strerror(ferror(file)));
            free(text);
            fclose(file);
            return EXIT_FAILURE;
        }

        ptr = strchr(ptr, '\n');
        if (ptr) {
            ++ptr;

            /* write the rest */
            if (fwrite(ptr, 1, strlen(ptr), file) < strlen(ptr)) {
                ERROR("knownhosts", "Failed to write to known hosts file (%s)", strerror(ferror(file)));
                free(text);
                fclose(file);
                return EXIT_FAILURE;
            }
            written += strlen(ptr);
        }
        free(text);

        ftruncate(fileno(file), written);
    }

    fclose(file);
    return EXIT_SUCCESS;
}

static int
cmd_connect_listen_ssh(struct arglist *cmd, int is_connect)
{
    const char *func_name = (is_connect ? "cmd_connect" : "cmd_listen");
    static unsigned short listening = 0;
    char *host = NULL, *user = NULL;
    struct passwd *pw;
    unsigned short port = 0;
    int c, timeout = 0;
    struct option *long_options;
    int option_index = 0;

    if (is_connect) {
        struct option connect_long_options[] = {
            {"ssh", 0, 0, 's'},
            {"host", 1, 0, 'o'},
            {"port", 1, 0, 'p'},
            {"login", 1, 0, 'l'},
            {0, 0, 0, 0}
        };
        long_options = connect_long_options;
    } else {
        struct option listen_long_options[] = {
            {"ssh", 0, 0, 's'},
            {"timeout", 1, 0, 'i'},
            {"port", 1, 0, 'p'},
            {"login", 1, 0, 'l'},
            {0, 0, 0, 0}
        };
        long_options = listen_long_options;
    }

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    while ((c = getopt_long(cmd->count, cmd->list, (is_connect ? "so:p:l:" : "si:p:l:"), long_options, &option_index)) != -1) {
        switch (c) {
        case 's':
            /* we know already */
            break;
        case 'o':
            host = optarg;
            break;
        case 'i':
            timeout = atoi(optarg);
            break;
        case 'p':
            port = (unsigned short)atoi(optarg);
            if (!is_connect && listening && (listening != port)) {
                //nc_callhome_listen_stop();
                listening = 0;
            }
            break;
        case 'l':
            user = optarg;
            break;
        default:
            ERROR(func_name, "Unknown option -%c.", c);
            if (is_connect) {
                cmd_connect_help();
            } else {
                cmd_listen_help();
            }
            return EXIT_FAILURE;
        }
    }

    /* default port */
    if (!port) {
        port = (is_connect ? NC_PORT_SSH : NC_PORT_CH_SSH);
    }

    /* default user */
    if (!user) {
        pw = getpwuid(getuid());
        if (pw) {
            user = pw->pw_name;
        }
    }

    if (ctx) {
        ly_ctx_destroy(ctx);
    }
    ctx = ly_ctx_new(search_path);

    if (is_connect) {
        /* default hostname */
        if (!host) {
            host = "localhost";
        }

        /* create the session */
        session = nc_connect_ssh(host, port, user, ctx);
        if (session == NULL) {
            ERROR(func_name, "Connecting to the %s:%d as user \"%s\" failed.", host, port, user);
            ly_ctx_destroy(ctx);
            ctx = NULL;
            return EXIT_FAILURE;
        }
    } else {
        /* default timeout */
        if (!timeout) {
            timeout = CLI_CH_TIMEOUT;
        }

        /* create the session */
        ERROR(func_name, "Waiting %ds for an SSH Call Home connection on port %u...", timeout, port);
        session = nc_callhome_accept_ssh(port, user, timeout * 1000, ctx);
        if (!session) {
            ERROR(func_name, "Receiving SSH Call Home on port %d as user \"%s\" failed.", port, user);
            ly_ctx_destroy(ctx);
            ctx = NULL;
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

#endif /* ENABLE_SSH */

#ifdef ENABLE_TLS

static int
cp(const char *to, const char *from)
{
    int fd_to, fd_from;
    struct stat st;
    ssize_t from_len;
    int saved_errno;

    fd_from = open(from, O_RDONLY);
    if (fd_from < 0) {
        return -1;
    }

    fd_to = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_to < 0) {
        goto out_error;
    }

    if (fstat(fd_from, &st) < 0) {
        goto out_error;
    }

    from_len = st.st_size;

    if (sendfile(fd_to, fd_from, NULL, from_len) < from_len) {
        goto out_error;
    }
    return 0;

out_error:
    saved_errno = errno;

    close(fd_from);
    if (fd_to >= 0)
        close(fd_to);

    errno = saved_errno;
    return -1;
}

static void
parse_cert(const char *name, const char *path)
{
    int i, j, has_san, first_san;
    ASN1_OCTET_STRING *ip;
    ASN1_INTEGER *bs;
    BIO *bio_out;
    FILE *fp;
    X509 *cert;
    STACK_OF(GENERAL_NAME) *san_names = NULL;
    GENERAL_NAME *san_name;

    fp = fopen(path, "r");
    if (fp == NULL) {
        ERROR("parse_cert", "Unable to open: %s", path);
        return;
    }
    cert = PEM_read_X509(fp, NULL, NULL, NULL);
    if (cert == NULL) {
        ERROR("parse_cert", "Unable to parse certificate: %s", path);
        fclose(fp);
        return;
    }

    bio_out = BIO_new_fp(stdout, BIO_NOCLOSE);

    bs = X509_get_serialNumber(cert);
    BIO_printf(bio_out, "-----%s----- serial: ", name);
    for (i = 0; i < bs->length; i++) {
        BIO_printf(bio_out, "%02x", bs->data[i]);
    }
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "Subject: ");
    X509_NAME_print(bio_out, X509_get_subject_name(cert), 0);
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "Issuer:  ");
    X509_NAME_print(bio_out, X509_get_issuer_name(cert), 0);
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "Valid until: ");
    ASN1_TIME_print(bio_out, X509_get_notAfter(cert));
    BIO_printf(bio_out, "\n");

    has_san = 0;
    first_san = 1;
    san_names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (san_names != NULL) {
        for (i = 0; i < sk_GENERAL_NAME_num(san_names); ++i) {
            san_name = sk_GENERAL_NAME_value(san_names, i);
            if (san_name->type == GEN_EMAIL || san_name->type == GEN_DNS || san_name->type == GEN_IPADD) {
                if (!has_san) {
                    BIO_printf(bio_out, "X509v3 Subject Alternative Name:\n\t");
                    has_san = 1;
                }
                if (!first_san) {
                    BIO_printf(bio_out, ", ");
                }
                if (first_san) {
                    first_san = 0;
                }
                if (san_name->type == GEN_EMAIL) {
                    BIO_printf(bio_out, "RFC822:%s", (char*) ASN1_STRING_data(san_name->d.rfc822Name));
                }
                if (san_name->type == GEN_DNS) {
                    BIO_printf(bio_out, "DNS:%s", (char*) ASN1_STRING_data(san_name->d.dNSName));
                }
                if (san_name->type == GEN_IPADD) {
                    BIO_printf(bio_out, "IP:");
                    ip = san_name->d.iPAddress;
                    if (ip->length == 4) {
                        BIO_printf(bio_out, "%d.%d.%d.%d", ip->data[0], ip->data[1], ip->data[2], ip->data[3]);
                    } else if (ip->length == 16) {
                        for (j = 0; j < ip->length; ++j) {
                            if (j > 0 && j < 15 && j%2 == 1) {
                                BIO_printf(bio_out, "%02x:", ip->data[j]);
                            } else {
                                BIO_printf(bio_out, "%02x", ip->data[j]);
                            }
                        }
                    }
                }
            }
        }
        sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
    }
    if (has_san) {
        BIO_printf(bio_out, "\n");
    }
    BIO_printf(bio_out, "\n");

    X509_free(cert);
    BIO_vfree(bio_out);
    fclose(fp);
}

void
parse_crl(const char *name, const char *path)
{
    int i;
    BIO *bio_out;
    FILE *fp;
    X509_CRL *crl;
    ASN1_INTEGER* bs;
    X509_REVOKED* rev;

    fp = fopen(path, "r");
    if (fp == NULL) {
        ERROR("parse_crl", "Unable to open \"%s\": %s", path, strerror(errno));
        return;
    }
    crl = PEM_read_X509_CRL(fp, NULL, NULL, NULL);
    if (crl == NULL) {
        ERROR("parse_crl", "Unable to parse certificate: %s", path);
        fclose(fp);
        return;
    }

    bio_out = BIO_new_fp(stdout, BIO_NOCLOSE);

    BIO_printf(bio_out, "-----%s-----\n", name);

    BIO_printf(bio_out, "Issuer: ");
    X509_NAME_print(bio_out, X509_CRL_get_issuer(crl), 0);
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "Last update: ");
    ASN1_TIME_print(bio_out, X509_CRL_get_lastUpdate(crl));
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "Next update: ");
    ASN1_TIME_print(bio_out, X509_CRL_get_nextUpdate(crl));
    BIO_printf(bio_out, "\n");

    BIO_printf(bio_out, "REVOKED:\n");

    if ((rev = sk_X509_REVOKED_pop(X509_CRL_get_REVOKED(crl))) == NULL) {
        BIO_printf(bio_out, "\tNone\n");
    }
    while (rev != NULL) {
        bs = rev->serialNumber;
        BIO_printf(bio_out, "\tSerial no.: ");
        for (i = 0; i < bs->length; i++) {
            BIO_printf(bio_out, "%02x", bs->data[i]);
        }
        BIO_printf(bio_out, "  Date: ");

        ASN1_TIME_print(bio_out, rev->revocationDate);
        BIO_printf(bio_out, "\n");

        X509_REVOKED_free(rev);
        rev = sk_X509_REVOKED_pop(X509_CRL_get_REVOKED(crl));
    }

    X509_CRL_free(crl);
    BIO_vfree(bio_out);
    fclose(fp);
}

int
cmd_cert(const char *arg)
{
    int ret;
    char* args = strdupa(arg);
    char* cmd = NULL, *ptr = NULL, *path, *path2, *dest;
    char* trusted_dir, *netconf_dir, *c_rehash_cmd;
    DIR* dir = NULL;
    struct dirent *d;

    cmd = strtok_r(args, " ", &ptr);
    cmd = strtok_r(NULL, " ", &ptr);
    if (!cmd || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        cmd_cert_help();

    } else if (!strcmp(cmd, "display")) {
        int none = 1;
        char *name;

        if (!(trusted_dir = get_default_trustedCA_dir(NULL))) {
            ERROR("cert display", "Could not get the default trusted CA directory");
            return EXIT_FAILURE;
        }

        dir = opendir(trusted_dir);
        while ((d = readdir(dir))) {
            if (!strcmp(d->d_name + strlen(d->d_name) - 4, ".pem")) {
                none = 0;
                name = strdup(d->d_name);
                name[strlen(name) - 4] = '\0';
                asprintf(&path, "%s/%s", trusted_dir, d->d_name);
                parse_cert(name, path);
                free(name);
                free(path);
            }
        }
        closedir(dir);
        if (none) {
            printf("No certificates found in the default trusted CA directory.\n");
        }
        free(trusted_dir);

    } else if (!strcmp(cmd, "add")) {
        path = strtok_r(NULL, " ", &ptr);
        if (!path || (strlen(path) < 5)) {
            ERROR("cert add", "Missing or wrong path to the certificate");
            return EXIT_FAILURE;
        }
        if (eaccess(path, R_OK)) {
            ERROR("cert add", "Cannot access certificate \"%s\": %s", path, strerror(errno));
            return EXIT_FAILURE;
        }

        trusted_dir = get_default_trustedCA_dir(NULL);
        if (!trusted_dir) {
            ERROR("cert add", "Could not get the default trusted CA directory");
            return EXIT_FAILURE;
        }

        if ((asprintf(&dest, "%s/%s", trusted_dir, strrchr(path, '/') + 1) == -1)
                || (asprintf(&c_rehash_cmd, "c_rehash %s &> /dev/null", trusted_dir) == -1)) {
            ERROR("cert add", "Memory allocation failed");
            free(trusted_dir);
            return EXIT_FAILURE;
        }
        free(trusted_dir);

        if (strcmp(dest + strlen(dest) - 4, ".pem")) {
            ERROR("cert add", "CA certificates are expected to be in *.pem format");
            strcpy(dest + strlen(dest) - 4, ".pem");
        }

        if (cp(dest, path)) {
            ERROR("cert add", "Could not copy the certificate: %s", strerror(errno));
            free(dest);
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }
        free(dest);

        if (((ret = system(c_rehash_cmd)) == -1) || WEXITSTATUS(ret)) {
            ERROR("cert add", "c_rehash execution failed");
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }

        free(c_rehash_cmd);

    } else if (!strcmp(cmd, "remove")) {
        path = strtok_r(NULL, " ", &ptr);
        if (!path) {
            ERROR("cert remove", "Missing the certificate name");
            return EXIT_FAILURE;
        }

        /* delete ".pem" if the user unnecessarily included it */
        if ((strlen(path) > 4) && !strcmp(path + strlen(path) - 4, ".pem")) {
            path[strlen(path) - 4] = '\0';
        }

        trusted_dir = get_default_trustedCA_dir(NULL);
        if (!trusted_dir) {
            ERROR("cert remove", "Could not get the default trusted CA directory");
            return EXIT_FAILURE;
        }

        if ((asprintf(&dest, "%s/%s.pem", trusted_dir, path) == -1)
                || (asprintf(&c_rehash_cmd, "c_rehash %s &> /dev/null", trusted_dir) == -1)) {
            ERROR("cert remove", "Memory allocation failed");
            free(trusted_dir);
            return EXIT_FAILURE;
        }
        free(trusted_dir);

        if (remove(dest)) {
            ERROR("cert remove", "Cannot remove certificate \"%s\": %s (use the name from \"cert display\" output)",
                  path, strerror(errno));
            free(dest);
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }
        free(dest);

        if (((ret = system(c_rehash_cmd)) == -1) || WEXITSTATUS(ret)) {
            ERROR("cert remove", "c_rehash execution failed");
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }

        free(c_rehash_cmd);

    } else if (!strcmp(cmd, "displayown")) {
        int crt = 0, key = 0, pem = 0;

        netconf_dir = get_netconf_dir();
        if (!netconf_dir) {
            ERROR("cert displayown", "Could not get the client home directory");
            return EXIT_FAILURE;
        }

        if (asprintf(&dest, "%s/client.pem", netconf_dir) == -1) {
            ERROR("cert displayown", "Memory allocation failed");
            free(netconf_dir);
            return EXIT_FAILURE;
        }
        free(netconf_dir);
        if (!eaccess(dest, R_OK)) {
            pem = 1;
        }

        strcpy(dest + strlen(dest) - 4, ".key");
        if (!eaccess(dest, R_OK)) {
            key = 1;
        }

        strcpy(dest + strlen(dest) - 4, ".crt");
        if (!eaccess(dest, R_OK)) {
            crt = 1;
        }

        if (!crt && !key && !pem) {
            printf("FAIL: No client certificate found, use \"cert replaceown\" to set some.\n");
        } else if (crt && !key && !pem) {
            printf("FAIL: Client *.crt certificate found, but is of no use without its private key *.key.\n");
        } else if (!crt && key && !pem) {
            printf("FAIL: Private key *.key found, but is of no use without a certificate.\n");
        } else if (!crt && !key && pem) {
            printf("OK: Using *.pem client certificate with the included private key.\n");
        } else if (crt && key && !pem) {
            printf("OK: Using *.crt certificate with a separate private key.\n");
        } else if (crt && !key && pem) {
            printf("WORKING: Using *.pem client certificate with the included private key (leftover certificate *.crt detected).\n");
        } else if (!crt && key && pem) {
            printf("WORKING: Using *.pem client certificate with the included private key (leftover private key detected).\n");
        } else if (crt && key && pem) {
            printf("WORKING: Using *.crt certificate with a separate private key (lower-priority *.pem certificate with a private key detected).\n");
        }

        if (crt) {
            parse_cert("CRT", dest);
        }
        if (pem) {
            strcpy(dest + strlen(dest) - 4, ".pem");
            parse_cert("PEM", dest);
        }
        free(dest);

    } else if (!strcmp(cmd, "replaceown")) {
        path = strtok_r(NULL, " ", &ptr);
        if (!path || (strlen(path) < 5)) {
            ERROR("cert replaceown", "Missing the certificate or invalid path.");
            return EXIT_FAILURE;
        }
        if (eaccess(path, R_OK)) {
            ERROR("cert replaceown", "Cannot access the certificate \"%s\": %s", path, strerror(errno));
            return EXIT_FAILURE;
        }

        path2 = strtok_r(NULL, " ", &ptr);
        if (path2) {
            if (strlen(path2) < 5) {
                ERROR("cert replaceown", "Invalid private key path.");
                return EXIT_FAILURE;
            }
            if (eaccess(path2, R_OK)) {
                ERROR("cert replaceown", "Cannot access the private key \"%s\": %s", path2, strerror(errno));
                return EXIT_FAILURE;
            }
        }

        netconf_dir = get_netconf_dir();
        if (!netconf_dir) {
            ERROR("cert replaceown", "Could not get the client home directory");
            return EXIT_FAILURE;
        }
        if (asprintf(&dest, "%s/client.XXX", netconf_dir) == -1) {
            ERROR("cert replaceown", "Memory allocation failed");
            free(netconf_dir);
            return EXIT_FAILURE;
        }
        free(netconf_dir);

        if (path2) {
            /* CRT & KEY */
            strcpy(dest + strlen(dest) - 4, ".pem");
            errno = 0;
            if (remove(dest) && (errno == EACCES)) {
                ERROR("cert replaceown", "Could not remove old certificate (*.pem)");
            }

            strcpy(dest + strlen(dest) - 4, ".crt");
            if (cp(dest, path)) {
                ERROR("cert replaceown", "Could not copy the certificate \"%s\": %s", path, strerror(errno));
                free(dest);
                return EXIT_FAILURE;
            }
            strcpy(dest + strlen(dest) - 4, ".key");
            if (cp(dest, path2)) {
                ERROR("cert replaceown", "Could not copy the private key \"%s\": %s", path, strerror(errno));
                free(dest);
                return EXIT_FAILURE;
            }
        } else {
            /* PEM */
            strcpy(dest + strlen(dest) - 4, ".key");
            errno = 0;
            if (remove(dest) && (errno == EACCES)) {
                ERROR("cert replaceown", "Could not remove old private key");
            }
            strcpy(dest + strlen(dest) - 4, ".crt");
            if (remove(dest) && (errno == EACCES)) {
                ERROR("cert replaceown", "Could not remove old certificate (*.crt)");
            }

            strcpy(dest + strlen(dest) - 4, ".pem");
            if (cp(dest, path)) {
                ERROR("cert replaceown", "Could not copy the certificate \"%s\": %s", path, strerror(errno));
                free(dest);
                return EXIT_FAILURE;
            }
        }

        free(dest);

    } else {
        ERROR("cert", "Unknown argument %s", cmd);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int
cmd_crl(const char *arg)
{
    int ret;
    char *args = strdupa(arg);
    char *cmd = NULL, *ptr = NULL, *path, *dest;
    char *crl_dir, *c_rehash_cmd;
    DIR *dir = NULL;
    struct dirent *d;

    cmd = strtok_r(args, " ", &ptr);
    cmd = strtok_r(NULL, " ", &ptr);
    if (!cmd || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        cmd_crl_help();

    } else if (!strcmp(cmd, "display")) {
        int none = 1;
        char *name;

        if (!(crl_dir = get_default_CRL_dir(NULL))) {
            ERROR("crl display", "Could not get the default CRL directory");
            return EXIT_FAILURE;
        }

        dir = opendir(crl_dir);
        while ((d = readdir(dir))) {
            if (!strcmp(d->d_name + strlen(d->d_name) - 4, ".pem")) {
                none = 0;
                name = strdup(d->d_name);
                name[strlen(name) - 4] = '\0';
                asprintf(&path, "%s/%s", crl_dir, d->d_name);
                parse_crl(name, path);
                free(name);
                free(path);
            }
        }
        closedir(dir);
        if (none) {
            printf("No CRLs found in the default CRL directory.\n");
        }
        free(crl_dir);

    } else if (!strcmp(cmd, "add")) {
        path = strtok_r(NULL, " ", &ptr);
        if (!path || (strlen(path) < 5)) {
            ERROR("crl add", "Missing or wrong path to the certificate");
            return EXIT_FAILURE;
        }
        if (eaccess(path, R_OK)) {
            ERROR("crl add", "Cannot access certificate \"%s\": %s", path, strerror(errno));
            return EXIT_FAILURE;
        }

        crl_dir = get_default_CRL_dir(NULL);
        if (!crl_dir) {
            ERROR("crl add", "Could not get the default CRL directory");
            return EXIT_FAILURE;
        }

        if ((asprintf(&dest, "%s/%s", crl_dir, strrchr(path, '/') + 1) == -1)
                || (asprintf(&c_rehash_cmd, "c_rehash %s &> /dev/null", crl_dir) == -1)) {
            ERROR("crl add", "Memory allocation failed");
            free(crl_dir);
            return EXIT_FAILURE;
        }
        free(crl_dir);

        if (strcmp(dest + strlen(dest) - 4, ".pem")) {
            ERROR("crl add", "CRLs are expected to be in *.pem format");
            strcpy(dest + strlen(dest) - 4, ".pem");
        }

        if (cp(dest, path)) {
            ERROR("crl add", "Could not copy the CRL \"%s\": %s", path, strerror(errno));
            free(dest);
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }
        free(dest);

        if (((ret = system(c_rehash_cmd)) == -1) || WEXITSTATUS(ret)) {
            ERROR("crl add", "c_rehash execution failed");
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }

        free(c_rehash_cmd);

    } else if (!strcmp(cmd, "remove")) {
        path = strtok_r(NULL, " ", &ptr);
        if (!path) {
            ERROR("crl remove", "Missing the certificate name");
            return EXIT_FAILURE;
        }

        // delete ".pem" if the user unnecessarily included it
        if ((strlen(path) > 4) && !strcmp(path + strlen(path) - 4, ".pem")) {
            path[strlen(path) - 4] = '\0';
        }

        crl_dir = get_default_CRL_dir(NULL);
        if (!crl_dir) {
            ERROR("crl remove", "Could not get the default CRL directory");
            return EXIT_FAILURE;
        }

        if ((asprintf(&dest, "%s/%s.pem", crl_dir, path) == -1)
                || (asprintf(&c_rehash_cmd, "c_rehash %s &> /dev/null", crl_dir) == -1)) {
            ERROR("crl remove", "Memory allocation failed");
            free(crl_dir);
            return EXIT_FAILURE;
        }
        free(crl_dir);

        if (remove(dest)) {
            ERROR("crl remove", "Cannot remove CRL \"%s\": %s (use the name from \"crl display\" output)",
                  path, strerror(errno));
            free(dest);
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }
        free(dest);

        if (((ret = system(c_rehash_cmd)) == -1) || WEXITSTATUS(ret)) {
            ERROR("crl remove", "c_rehash execution failed");
            free(c_rehash_cmd);
            return EXIT_FAILURE;
        }

        free(c_rehash_cmd);

    } else {
        ERROR("crl", "Unknown argument %s", cmd);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int
cmd_connect_listen_tls(struct arglist *cmd, int is_connect)
{
    const char *func_name = (is_connect ? "cmd_connect" : "cmd_listen");
    static unsigned short listening = 0;
    char *host = NULL;
    DIR *dir = NULL;
    struct dirent* d;
    int c, n, timeout = 0;
    char *cert = NULL, *key = NULL, *trusted_dir = NULL, *crl_dir = NULL, *trusted_store = NULL;
    unsigned short port = 0;
    struct option *long_options;
    int option_index = 0;

    if (is_connect) {
        struct option connect_long_options[] = {
            {"tls", 0, 0, 't'},
            {"host", 1, 0, 'o'},
            {"port", 1, 0, 'p'},
            {"cert", 1, 0, 'c'},
            {"key", 1, 0, 'k'},
            {"trusted", 1, 0, 'r'},
            {0, 0, 0, 0}
        };
        long_options = connect_long_options;
    } else {
        struct option listen_long_options[] = {
            {"tls", 0, 0, 't'},
            {"timeout", 1, 0, 'i'},
            {"port", 1, 0, 'p'},
            {"cert", 1, 0, 'c'},
            {"key", 1, 0, 'k'},
            {"trusted", 1, 0, 'r'},
            {0, 0, 0, 0}
        };
        long_options = listen_long_options;
    }

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    while ((c = getopt_long(cmd->count, cmd->list, (is_connect ? "to:p:c:k:r:" : "ti:p:c:k:r:"), long_options, &option_index)) != -1) {
        switch (c) {
        case 't':
            /* we know already */
            break;
        case 'o':
            host = optarg;
            break;
        case 'i':
            timeout = atoi(optarg);
            break;
        case 'p':
            port = (unsigned short)atoi(optarg);
            if (!is_connect && listening && (listening != port)) {
                //nc_callhome_listen_stop();
                listening = 0;
            }
            break;
        case 'c':
            asprintf(&cert, "%s", optarg);
            break;
        case 'k':
            asprintf(&key, "%s", optarg);
            break;
        case 'r':
            trusted_store = optarg;
            break;
        default:
            ERROR(func_name, "Unknown option -%c.", c);
            if (is_connect) {
                cmd_connect_help();
            } else {
                cmd_listen_help();
            }
            return EXIT_FAILURE;
        }
    }

    if (!cert) {
        if (key) {
            ERROR(func_name, "Key specified without a certificate.");
            goto error_cleanup;
        }
        get_default_client_cert(&cert, &key);
        if (!cert) {
            ERROR(func_name, "Could not find the default client certificate, check with \"cert displayown\" command.");
            goto error_cleanup;
        }
    }
    if (!trusted_store) {
        trusted_dir = get_default_trustedCA_dir(NULL);
        if (!(dir = opendir(trusted_dir))) {
            ERROR(func_name, "Could not use the trusted CA directory.");
            goto error_cleanup;
        }

        /* check whether we have any trusted CA, verification should fail otherwise */
        n = 0;
        while ((d = readdir(dir))) {
            if (++n > 2) {
                break;
            }
        }
        closedir(dir);
        if (n <= 2) {
            ERROR(func_name, "Trusted CA directory empty, use \"cert add\" command to add certificates.");
        }
    } else {
        if (eaccess(trusted_store, R_OK)) {
            ERROR(func_name, "Could not access trusted CA store \"%s\": %s", trusted_store, strerror(errno));
            goto error_cleanup;
        }
        if ((strlen(trusted_store) < 5) || strcmp(trusted_store + strlen(trusted_store) - 4, ".pem")) {
            ERROR(func_name, "Trusted CA store in an unknown format.");
            goto error_cleanup;
        }
    }
    if (!(crl_dir = get_default_CRL_dir(NULL))) {
        ERROR(func_name, "Could not use the CRL directory.");
        goto error_cleanup;
    }

    if (nc_tls_client_init(cert, key, trusted_store, trusted_dir, NULL, crl_dir)) {
        ERROR(func_name, "Initiating TLS failed.");
        goto error_cleanup;
    }

    /* default port */
    if (!port) {
        port = (is_connect ? NC_PORT_TLS : NC_PORT_CH_TLS);
    }

    if (ctx) {
        ly_ctx_destroy(ctx);
    }
    ctx = ly_ctx_new(search_path);

    if (is_connect) {
        /* default host */
        if (!host) {
            host = "localhost";
        }

        /* create the session */
        session = nc_connect_tls(host, port, ctx);
        if (session == NULL) {
            ERROR(func_name, "Connecting to the %s:%d failed.", host, port);
            goto error_cleanup;
        }
    } else {
        /* default timeout */
        if (!timeout) {
            timeout = CLI_CH_TIMEOUT;
        }

        /* create the session */
        ERROR(func_name, "Waiting %ds for a TLS Call Home connection on port %u...", timeout, port);
        session = nc_callhome_accept_tls(port, timeout * 1000, ctx);
        if (!session) {
            ERROR(func_name, "Receiving TLS Call Home on port %d failed.", port);
            goto error_cleanup;
        }
    }

    free(trusted_dir);
    free(crl_dir);
    free(cert);
    free(key);
    return EXIT_SUCCESS;

error_cleanup:
    free(trusted_dir);
    free(crl_dir);
    free(cert);
    free(key);
    ly_ctx_destroy(ctx);
    ctx = NULL;
    return EXIT_FAILURE;
}

#endif /* ENABLE_TLS */

int
cmd_searchpath(const char *arg)
{
    const char *path;
    struct stat st;

    if (strchr(arg, ' ') == NULL) {
        fprintf(stderr, "Missing the search path.\n");
        return 1;
    }
    path = strchr(arg, ' ')+1;

    if (!strcmp(path, "-h") || !strcmp(path, "--help")) {
        cmd_searchpath_help();
        return 0;
    }

    if (stat(path, &st) == -1) {
        fprintf(stderr, "Failed to stat the search path (%s).\n", strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "\"%s\" is not a directory.\n", path);
        return 1;
    }

    free(search_path);
    search_path = strdup(path);

    return 0;
}

int
cmd_verb(const char *arg)
{
    const char *verb;
    if (strlen(arg) < 5) {
        cmd_verb_help();
        return 1;
    }

    verb = arg + 5;
    if (!strcmp(verb, "error") || !strcmp(verb, "0")) {
        nc_verbosity(0);
    } else if (!strcmp(verb, "warning") || !strcmp(verb, "1")) {
        nc_verbosity(1);
    } else if (!strcmp(verb, "verbose")  || !strcmp(verb, "2")) {
        nc_verbosity(2);
    } else if (!strcmp(verb, "debug")  || !strcmp(verb, "3")) {
        nc_verbosity(3);
    } else {
        fprintf(stderr, "Unknown verbosity \"%s\"\n", verb);
        return 1;
    }

    return 0;
}

int
cmd_disconnect(const char *UNUSED(arg))
{
    if (session == NULL) {
        ERROR("disconnect", "Not connected to any NETCONF server.");
    } else {
        /* possible data race, but let's be optimistic */
        ntf_tid = 0;
        nc_session_free(session);
        session = NULL;
        ly_ctx_destroy(ctx);
        ctx = NULL;
    }

    return EXIT_SUCCESS;
}

int
cmd_status(const char *UNUSED(arg))
{
    const char *s, **cpblts;
    int i;

    if (!session) {
        printf("Client is not connected to any NETCONF server.\n");
    } else {
        printf("Current NETCONF session:\n");
        printf("  ID          : %u\n", nc_session_get_id(session));
        printf("  Host        : %s\n", nc_session_get_host(session));
        printf("  Port        : %u\n", nc_session_get_port(session));
        printf("  User        : %s\n", nc_session_get_username(session));
        switch (nc_session_get_ti(session)) {
        case NC_TI_LIBSSH:
            s = "SSH";
            break;
        case NC_TI_OPENSSL:
            s = "TLS";
            break;
        case NC_TI_FD:
            s = "FD";
        default:
            s = "Unknown";
            break;
        }
        printf("  Transport   : %s\n", s);
        printf("  Capabilities:\n");
        cpblts = nc_session_get_cpblts(session);
        for (i = 0; cpblts[i]; ++i) {
            printf("\t%s\n", cpblts[i]);
        }
    }

    return EXIT_SUCCESS;
}

static int
cmd_connect_listen(const char *arg, int is_connect)
{
    const char *func_name = (is_connect ? "cmd_connect" : "cmd_listen");
    int c, ret;
    const char *optstring;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
#ifdef ENABLE_SSH
            {"ssh", 0, 0, 's'},
#endif
#ifdef ENABLE_TLS
            {"tls", 0, 0, 't'},
#endif
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    if (session) {
        ERROR(func_name, "Already connected to %s.", nc_session_get_host(session));
        return EXIT_FAILURE;
    }

    /* process given arguments */
    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    ret = -1;

#if defined(ENABLE_SSH) && defined(ENABLE_TLS)
    optstring = "hst";
#elif defined(ENABLE_SSH)
    optstring = "hs";
#elif defined(ENABLE_TLS)
    optstring = "ht";
#endif

    while ((c = getopt_long(cmd.count, cmd.list, optstring, long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            if (is_connect) {
                cmd_connect_help();
            } else {
                cmd_listen_help();
            }
            clear_arglist(&cmd);
            ret = EXIT_SUCCESS;
            break;
#ifdef ENABLE_SSH
        case 's':
            ret = cmd_connect_listen_ssh(&cmd, is_connect);
            break;
#endif
#ifdef ENABLE_TLS
        case 't':
            ret = cmd_connect_listen_tls(&cmd, is_connect);
            break;
#endif
        default:
            ERROR(func_name, "Unknown option -%c.", c);
            if (is_connect) {
                cmd_connect_help();
            } else {
                cmd_listen_help();
            }
            ret = EXIT_FAILURE;
        }
    }

    if (ret == -1) {
#ifdef ENABLE_SSH
        ret = cmd_connect_listen_ssh(&cmd, is_connect);
#elif defined(ENABLE_TLS)
        ret = cmd_connect_listen_tls(&cmd, is_connect);
#endif
    }

    if (!ret) {
        interleave = 1;
    }

    clear_arglist(&cmd);
    return ret;
}

int
cmd_connect(const char *arg)
{
    return cmd_connect_listen(arg, 1);
}

int
cmd_listen(const char *arg)
{
    return cmd_connect_listen(arg, 0);
}

int
cmd_quit(const char *UNUSED(arg))
{
    done = 1;
    return 0;
}

int
cmd_help(const char *arg)
{
    int i;
    char *args = strdupa(arg);
    char *cmd = NULL;

    strtok(args, " ");
    if ((cmd = strtok(NULL, " ")) == NULL) {

generic_help:
        fprintf(stdout, "Available commands:\n");

        for (i = 0; commands[i].name; i++) {
            if (commands[i].helpstring != NULL) {
                fprintf(stdout, "  %-15s %s\n", commands[i].name, commands[i].helpstring);
            }
        }
    } else {
        /* print specific help for the selected command */

        /* get the command of the specified name */
        for (i = 0; commands[i].name; i++) {
            if (strcmp(cmd, commands[i].name) == 0) {
                break;
            }
        }

        /* execute the command's help if any valid command specified */
        if (commands[i].name) {
            if (commands[i].help_func != NULL) {
                commands[i].help_func();
            } else {
                printf("%s\n", commands[i].helpstring);
            }
        } else {
            /* if unknown command specified, print the list of commands */
            printf("Unknown command \'%s\'\n", cmd);
            goto generic_help;
        }
    }

    return 0;
}

int
cmd_editor(const char *arg)
{
    char *cmd, *args = strdupa(arg), *ptr = NULL;

    cmd = strtok_r(args, " ", &ptr);
    cmd = strtok_r(NULL, " ", &ptr);
    if (cmd == NULL) {
        printf("Current editor: ");
        if (strcmp(config_editor, "NONE") == 0) {
            printf("(none)\n");
        } else {
            printf("%s\n", config_editor);
        }
    } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        cmd_editor_help();
    } else if (strcmp(cmd, "--none") == 0) {
        free(config_editor);
        config_editor = strdup("NONE");
    } else {
        free(config_editor);
        config_editor = strdup(cmd);
    }

    return EXIT_SUCCESS;
}

int
cmd_cancelcommit(const char *arg)
{
    struct nc_rpc *rpc;
    int c, ret;
    char *persist_id = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"persist-id", 1, 0, 'i'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    /* process given arguments */
    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hi:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_cancelcommit_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 'i':
            persist_id = strdup(optarg);
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_cancelcommit_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    rpc = nc_rpc_cancel(persist_id, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_commit(const char *arg)
{
    struct nc_rpc *rpc;
    int c, ret, confirmed = 0;
    int32_t confirm_timeout = 0;
    char *persist = NULL, *persist_id = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"confirmed", 0, 0, 'c'},
            {"confirm-timeout", 1, 0, 't'},
            {"persist", 1, 0, 'p'},
            {"persist-id", 1, 0, 'i'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    /* process given arguments */
    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hct:p:i:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_commit_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 'c':
            confirmed = 1;
            break;
        case 't':
            confirm_timeout = atoi(optarg);
            break;
        case 'p':
            persist = strdup(optarg);
            break;
        case 'i':
            persist_id = strdup(optarg);
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_commit_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    rpc = nc_rpc_commit(confirmed, confirm_timeout, persist, persist_id, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_copyconfig(const char *arg)
{
    int c, config_fd, ret;
    struct stat config_stat;
    char *src = NULL, *config_m = NULL, *trg = NULL;
    NC_DATASTORE target = NC_DATASTORE_ERROR, source = NC_DATASTORE_ERROR;
    struct nc_rpc *rpc;
    NC_WD_MODE wd = NC_WD_UNKNOWN;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"target", 1, 0, 't'},
            {"source", 1, 0, 's'},
            {"src-config", 2, 0, 'c'},
            {"defaults", 1, 0, 'd'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:s:c::d:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_copyconfig_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 't':
            /* validate argument */
            if (!strcmp(optarg, "running")) {
                target = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                target = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                target = NC_DATASTORE_CANDIDATE;
            } else if (!strncmp(optarg, "url:", 4)) {
                target = NC_DATASTORE_URL;
                trg = strdup(&(optarg[4]));
            } else {
                ERROR(__func__, "Invalid target datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        case 's':
            /* check if -c was not used */
            if (source != NC_DATASTORE_ERROR) {
                ERROR(__func__, "Mixing --source, and --src-config parameters is not allowed.");
                goto fail;
            }

            /* validate argument */
            if (!strcmp(optarg, "running")) {
                source = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                source = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                source = NC_DATASTORE_CANDIDATE;
            } else if (!strncmp(optarg, "url:", 4)) {
                source = NC_DATASTORE_URL;
                src = strdup(&(optarg[4]));
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'c':
            /* check if -s was not used */
            if (source != NC_DATASTORE_ERROR) {
                ERROR(__func__, "Mixing --source and --src-config parameters is not allowed.");
                goto fail;
            }

            source = NC_DATASTORE_CONFIG;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                src = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        case 'd':
            if (!strcmp(optarg, "report-all")) {
                wd = NC_WD_ALL;
            } else if (!strcmp(optarg, "report-all-tagged")) {
                wd = NC_WD_ALL_TAG;
            } else if (!strcmp(optarg, "trim")) {
                wd = NC_WD_TRIM;
            } else if (!strcmp(optarg, "explicit")) {
                wd = NC_WD_EXPLICIT;
            } else {
                ERROR(__func__, "Unknown with-defaults mode \"%s\".", optarg);
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_copyconfig_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if ((source == NC_DATASTORE_CONFIG) && !src) {
        /* let user write edit data interactively */
        src = readinput("Type the content of a configuration datastore.");
        if (!src) {
            ERROR(__func__, "Reading configuration data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_copy(target, trg, source, src, wd, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    free(src);
    free(trg);
    return EXIT_FAILURE;
}

int
cmd_deleteconfig(const char *arg)
{
    int c, ret;
    char *trg = NULL;
    struct nc_rpc *rpc;
    NC_DATASTORE target = NC_DATASTORE_ERROR;;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"target", 1, 0, 't'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_deleteconfig_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 't':
            if (!strcmp(optarg, "startup")) {
                target = NC_DATASTORE_STARTUP;
            } else if (!strncmp(optarg, "url:", 4)) {
                target = NC_DATASTORE_URL;
                trg = strdup(optarg + 4);
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_deleteconfig_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* create requests */
    rpc = nc_rpc_delete(target, trg, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    free(trg);
    return EXIT_FAILURE;
}

int
cmd_discardchanges(const char *arg)
{
    struct nc_rpc *rpc;
    int c, ret;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    /* process given arguments */
    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "h", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_discardchanges_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_discardchanges_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }

    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    rpc = nc_rpc_discard();
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_editconfig(const char *arg)
{
    int c, config_fd, ret, content_param = 0;
    struct stat config_stat;
    char *content = NULL, *config_m = NULL;
    NC_DATASTORE target = NC_DATASTORE_ERROR;
    struct nc_rpc *rpc;
    NC_RPC_EDIT_DFLTOP op = NC_RPC_EDIT_DFLTOP_UNKNOWN;
    NC_RPC_EDIT_TESTOPT test = NC_RPC_EDIT_TESTOPT_UNKNOWN;
    NC_RPC_EDIT_ERROPT err = NC_RPC_EDIT_ERROPT_UNKNOWN;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"target", 1, 0, 't'},
            {"defop", 1, 0, 'o'},
            {"test", 1, 0, 'e'},
            {"error", 1, 0, 'r'},
            {"config", 2, 0, 'c'},
            {"url", 1, 0, 'u'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:o:e:r:c::u:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_editconfig_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 't':
            /* validate argument */
            if (!strcmp(optarg, "running")) {
                target = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "candidate")) {
                target = NC_DATASTORE_CANDIDATE;
            } else {
                ERROR(__func__, "Invalid target datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'o':
            if (!strcmp(optarg, "merge")) {
                op = NC_RPC_EDIT_DFLTOP_MERGE;
            } else if (!strcmp(optarg, "replace")) {
                op = NC_RPC_EDIT_DFLTOP_REPLACE;
            } else if (!strcmp(optarg, "none")) {
                op = NC_RPC_EDIT_DFLTOP_NONE;
            } else {
                ERROR(__func__, "Invalid default operation specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'e':
            if (!strcmp(optarg, "set")) {
                test = NC_RPC_EDIT_TESTOPT_SET;
            } else if (!strcmp(optarg, "test-only")) {
                test = NC_RPC_EDIT_TESTOPT_TEST;
            } else if (!strcmp(optarg, "test-then-set")) {
                test = NC_RPC_EDIT_TESTOPT_TESTSET;
            } else {
                ERROR(__func__, "Invalid test option specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'r':
            if (!strcmp(optarg, "stop")) {
                err = NC_RPC_EDIT_ERROPT_STOP;
            } else if (!strcmp(optarg, "continue")) {
                err = NC_RPC_EDIT_ERROPT_CONTINUE;
            } else if (!strcmp(optarg, "rollback")) {
                err = NC_RPC_EDIT_ERROPT_ROLLBACK;
            } else {
                ERROR(__func__, "Invalid error option specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'c':
            /* check if -u was not used */
            if (content_param) {
                ERROR(__func__, "Mixing --url and --config parameters is not allowed.");
                goto fail;
            }

            content_param = 1;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                content = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        case 'u':
            /* check if -c was not used */
            if (content_param) {
                ERROR(__func__, "Mixing --url and --config parameters is not allowed.");
                goto fail;
            }

            content_param = 1;

            content = strdup(optarg);
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_editconfig_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if (content_param && !content) {
        /* let user write edit data interactively */
        content = readinput("Type the content of the <edit-config>.");
        if (!content) {
            ERROR(__func__, "Reading configuration data failed.");
            goto fail;
        }
    }

    rpc = nc_rpc_edit(target, op, test, err, content, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    free(content);
    return EXIT_FAILURE;
}

int
cmd_get(const char *arg)
{
    int c, config_fd, ret, filter_param = 0;
    struct stat config_stat;
    char *filter = NULL, *config_m = NULL;
    struct nc_rpc *rpc;
    NC_WD_MODE wd = NC_WD_UNKNOWN;
    FILE *output = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"filter-subtree", 2, 0, 's'},
            {"filter-xpath", 1, 0, 'x'},
            {"defaults", 1, 0, 'd'},
            {"out", 1, 0, 'o'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hs::x:d:o:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_get_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 's':
            /* check if -x was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                filter = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        case 'x':
            /* check if -s was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            filter = strdup(optarg);
            break;
        case 'd':
            if (!strcmp(optarg, "report-all")) {
                wd = NC_WD_ALL;
            } else if (!strcmp(optarg, "report-all-tagged")) {
                wd = NC_WD_ALL_TAG;
            } else if (!strcmp(optarg, "trim")) {
                wd = NC_WD_TRIM;
            } else if (!strcmp(optarg, "explicit")) {
                wd = NC_WD_EXPLICIT;
            } else {
                ERROR(__func__, "Unknown with-defaults mode \"%s\".", optarg);
                goto fail;
            }
            break;
        case 'o':
            output = fopen(optarg, "w");
            if (!output) {
                ERROR(__func__, "Failed to open file \"%s\" (%s).", optarg, strerror(errno));
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_get_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    /* check if edit configuration data were specified */
    if (filter_param && !filter) {
        /* let user write edit data interactively */
        filter = readinput("Type the content of the subtree filter.");
        if (!filter) {
            ERROR(__func__, "Reading filter data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_get(filter, wd, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    if (output) {
        ret = cli_send_recv(rpc, output);
        fclose(output);
    } else {
        ret = cli_send_recv(rpc, stdout);
    }

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    if (output) {
        fclose(output);
    }
    free(filter);
    return EXIT_FAILURE;
}

int
cmd_getconfig(const char *arg)
{
    int c, config_fd, ret, filter_param = 0;
    struct stat config_stat;
    char *filter = NULL, *config_m = NULL;
    struct nc_rpc *rpc;
    NC_WD_MODE wd = NC_WD_UNKNOWN;
    NC_DATASTORE source = NC_DATASTORE_ERROR;
    FILE *output = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"source", 1, 0, 'u'},
            {"filter-subtree", 2, 0, 's'},
            {"filter-xpath", 1, 0, 'x'},
            {"defaults", 1, 0, 'd'},
            {"out", 1, 0, 'o'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hu:s::x:d:o:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_getconfig_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 'u':
            if (!strcmp(optarg, "running")) {
                source = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                source = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                source = NC_DATASTORE_CANDIDATE;
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        case 's':
            /* check if -x was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                filter = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        case 'x':
            /* check if -s was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            filter = strdup(optarg);
            break;
        case 'd':
            if (!strcmp(optarg, "report-all")) {
                wd = NC_WD_ALL;
            } else if (!strcmp(optarg, "report-all-tagged")) {
                wd = NC_WD_ALL_TAG;
            } else if (!strcmp(optarg, "trim")) {
                wd = NC_WD_TRIM;
            } else if (!strcmp(optarg, "explicit")) {
                wd = NC_WD_EXPLICIT;
            } else {
                ERROR(__func__, "Unknown with-defaults mode \"%s\".", optarg);
                goto fail;
            }
            break;
        case 'o':
            output = fopen(optarg, "w");
            if (!output) {
                ERROR(__func__, "Failed to open file \"%s\" (%s).", optarg, strerror(errno));
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_getconfig_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if (filter_param && !filter) {
        /* let user write edit data interactively */
        filter = readinput("Type the content of the subtree filter.");
        if (!filter) {
            ERROR(__func__, "Reading filter data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_getconfig(source, filter, wd, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    if (output) {
        ret = cli_send_recv(rpc, output);
        fclose(output);
    } else {
        ret = cli_send_recv(rpc, stdout);
    }

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    if (output) {
        fclose(output);
    }
    free(filter);
    return EXIT_FAILURE;
}

int
cmd_killsession(const char *arg)
{
    struct nc_rpc *rpc;
    int c, ret;
    uint32_t sid = 0;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"sid", 1, 0, 's'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    /* process given arguments */
    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hs:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_killsession_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 's':
            sid = atoi(optarg);
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_killsession_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }

    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    if (!sid) {
        ERROR(__func__, "Session ID was not specififed or not a number.");
        return EXIT_FAILURE;
    }

    rpc = nc_rpc_kill(sid);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_lock(const char *arg)
{
    int c, ret;
    struct nc_rpc *rpc;
    NC_DATASTORE target = NC_DATASTORE_ERROR;;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"target", 1, 0, 't'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_lock_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 't':
            if (!strcmp(optarg, "running")) {
                target = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                target = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                target = NC_DATASTORE_CANDIDATE;
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                clear_arglist(&cmd);
                return EXIT_FAILURE;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_lock_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    /* create requests */
    rpc = nc_rpc_lock(target);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_unlock(const char *arg)
{
    int c, ret;
    struct nc_rpc *rpc;
    NC_DATASTORE target = NC_DATASTORE_ERROR;;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"target", 1, 0, 't'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_unlock_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 't':
            if (!strcmp(optarg, "running")) {
                target = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                target = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                target = NC_DATASTORE_CANDIDATE;
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                clear_arglist(&cmd);
                return EXIT_FAILURE;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_unlock_help();
            clear_arglist(&cmd);
            return EXIT_FAILURE;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        return EXIT_FAILURE;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        return EXIT_FAILURE;
    }

    /* create requests */
    rpc = nc_rpc_unlock(target);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        return EXIT_FAILURE;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;
}

int
cmd_validate(const char *arg)
{
    int c, config_fd, ret;
    struct stat config_stat;
    char *src = NULL, *config_m = NULL;
    NC_DATASTORE source = NC_DATASTORE_ERROR;
    struct nc_rpc *rpc;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"source", 1, 0, 's'},
            {"src-config", 2, 0, 'c'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hs:c::", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_validate_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 's':
            /* check if -c was not used */
            if (source != NC_DATASTORE_ERROR) {
                ERROR(__func__, "Mixing --source, and --src-config parameters is not allowed.");
                goto fail;
            }

            /* validate argument */
            if (!strcmp(optarg, "running")) {
                source = NC_DATASTORE_RUNNING;
            } else if (!strcmp(optarg, "startup")) {
                source = NC_DATASTORE_STARTUP;
            } else if (!strcmp(optarg, "candidate")) {
                source = NC_DATASTORE_CANDIDATE;
            } else if (!strncmp(optarg, "url:", 4)) {
                source = NC_DATASTORE_URL;
                src = strdup(&(optarg[4]));
            } else {
                ERROR(__func__, "Invalid source datastore specified (%s).", optarg);
                goto fail;
            }
            break;
        case 'c':
            /* check if -s was not used */
            if (source != NC_DATASTORE_ERROR) {
                ERROR(__func__, "Mixing --source and --src-config parameters is not allowed.");
                goto fail;
            }

            source = NC_DATASTORE_CONFIG;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                src = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_validate_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if ((source == NC_DATASTORE_CONFIG) && !src) {
        /* let user write edit data interactively */
        src = readinput("Type the content of a configuration datastore.");
        if (!src) {
            ERROR(__func__, "Reading configuration data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_validate(source, src, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    ret = cli_send_recv(rpc, stdout);

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    free(src);
    return EXIT_FAILURE;
}

int
cmd_subscribe(const char *arg)
{
    int c, config_fd, ret, filter_param = 0;
    struct stat config_stat;
    char *filter = NULL, *config_m = NULL, *stream = NULL, *start = NULL, *stop = NULL;
    struct nc_rpc *rpc;
    time_t t;
    FILE *output = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"filter-subtree", 2, 0, 's'},
            {"filter-xpath", 1, 0, 'x'},
            {"begin", 1, 0, 'b'},
            {"end", 1, 0, 'e'},
            {"stream", 1, 0, 't'},
            {"out", 1, 0, 'o'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hs::x:b:e:t:o:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_subscribe_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 's':
            /* check if -x was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            if (optarg) {
                /* open edit configuration data from the file */
                config_fd = open(optarg, O_RDONLY);
                if (config_fd == -1) {
                    ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                    goto fail;
                }

                /* map content of the file into the memory */
                if (fstat(config_fd, &config_stat) != 0) {
                    ERROR(__func__, "fstat failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }
                config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
                if (config_m == MAP_FAILED) {
                    ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                    close(config_fd);
                    goto fail;
                }

                /* make a copy of the content to allow closing the file */
                filter = strdup(config_m);

                /* unmap local datastore file and close it */
                munmap(config_m, config_stat.st_size);
                close(config_fd);
            }
            break;
        case 'x':
            /* check if -s was not used */
            if (filter_param) {
                ERROR(__func__, "Mixing --filter-subtree, and --filter-xpath parameters is not allowed.");
                goto fail;
            }

            filter_param = 1;

            filter = strdup(optarg);
            break;
        case 'b':
        case 'e':
            if (optarg[0] == '-' || optarg[0] == '+') {
                t = time(NULL);
                t += atol(optarg);
            } else {
                t = atol(optarg);
            }

            if (c == 'b') {
                if (t > time(NULL)) {
                    /* begin time is in future */
                    ERROR(__func__, "Begin time cannot be set to future.");
                    goto fail;
                }
                start = nc_time2datetime(t, NULL);
            } else { /* c == 'e' */
                stop = nc_time2datetime(t, NULL);
            }
            break;
        case 't':
            stream = strdup(optarg);
            break;
        case 'o':
            output = fopen(optarg, "w");
            if (!output) {
                ERROR(__func__, "Failed to open file \"%s\" (%s).", optarg, strerror(errno));
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_subscribe_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (ntf_tid) {
        ERROR(__func__, "Already subscribed to a notification stream.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if (filter_param && !filter) {
        /* let user write edit data interactively */
        filter = readinput("Type the content of the subtree filter.");
        if (!filter) {
            ERROR(__func__, "Reading filter data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_subscribe(stream, filter, start, stop, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    ret = cli_send_recv(rpc, stdout);
    nc_rpc_free(rpc);
    filter = NULL;
    stream = NULL;
    start = NULL;
    stop = NULL;

    if (ret) {
        goto fail;
    }

    /* create notification thread */
    if (!output) {
        output = stdout;
    }
    ret = pthread_create((pthread_t *)&ntf_tid, NULL, cli_ntf_thread, output);
    if (ret) {
        ERROR(__func__, "Failed to create notification thread (%s).", strerror(ret));
        ntf_tid = 0;
        goto fail;
    }
    pthread_detach(ntf_tid);
    output = NULL;

    if (!nc_session_cpblt(session, NC_CAP_INTERLEAVE_ID)) {
        fprintf(output, "NETCONF server does not support interleave, you\n"
                        "cannot issue any RPCs during the subscription.\n"
                        "Close the session with \"disconnect\".\n");
        interleave = 0;
    }

    return ret;

fail:
    clear_arglist(&cmd);
    if (output && (output != stdout)) {
        fclose(output);
    }
    free(filter);
    free(stream);
    free(start);
    free(stop);
    return EXIT_FAILURE;
}

int
cmd_getschema(const char *arg)
{
    int c, ret;
    char *model = NULL, *version = NULL, *format = NULL;
    struct nc_rpc *rpc;
    FILE *output = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"model", 1, 0, 'm'},
            {"version", 1, 0, 'v'},
            {"format", 1, 0, 'f'},
            {"out", 1, 0, 'o'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "hm:v:f:o:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_getschema_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 'm':
            model = strdup(optarg);
            break;
        case 'v':
            version = strdup(optarg);
            break;
        case 'f':
            format = strdup(optarg);
            break;
        case 'o':
            output = fopen(optarg, "w");
            if (!output) {
                ERROR(__func__, "Failed to open file \"%s\" (%s).", optarg, strerror(errno));
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_getschema_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    rpc = nc_rpc_getschema(model, version, format, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    if (output) {
        ret = cli_send_recv(rpc, output);
        fclose(output);
    } else {
        ret = cli_send_recv(rpc, stdout);
    }

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    if (output) {
        fclose(output);
    }
    free(model);
    free(version);
    free(format);
    return EXIT_FAILURE;
}

int
cmd_userrpc(const char *arg)
{
    int c, config_fd, ret;
    struct stat config_stat;
    char *content = NULL, *config_m = NULL;
    struct nc_rpc *rpc;
    FILE *output = NULL;
    struct arglist cmd;
    struct option long_options[] = {
            {"help", 0, 0, 'h'},
            {"content", 1, 0, 'c'},
            {"out", 1, 0, 'o'},
            {0, 0, 0, 0}
    };
    int option_index = 0;

    /* set back to start to be able to use getopt() repeatedly */
    optind = 0;

    init_arglist(&cmd);
    addargs(&cmd, "%s", arg);

    while ((c = getopt_long(cmd.count, cmd.list, "ht:s:c::d:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'h':
            cmd_userrpc_help();
            clear_arglist(&cmd);
            return EXIT_SUCCESS;
        case 'c':
            /* open edit configuration data from the file */
            config_fd = open(optarg, O_RDONLY);
            if (config_fd == -1) {
                ERROR(__func__, "Unable to open the local datastore file (%s).", strerror(errno));
                goto fail;
            }

            /* map content of the file into the memory */
            if (fstat(config_fd, &config_stat) != 0) {
                ERROR(__func__, "fstat failed (%s).", strerror(errno));
                close(config_fd);
                goto fail;
            }
            config_m = mmap(NULL, config_stat.st_size, PROT_READ, MAP_PRIVATE, config_fd, 0);
            if (config_m == MAP_FAILED) {
                ERROR(__func__, "mmap of the local datastore file failed (%s).", strerror(errno));
                close(config_fd);
                goto fail;
            }

            /* make a copy of the content to allow closing the file */
            content = strdup(config_m);

            /* unmap local datastore file and close it */
            munmap(config_m, config_stat.st_size);
            close(config_fd);
            break;
        case 'o':
            output = fopen(optarg, "w");
            if (!output) {
                ERROR(__func__, "Failed to open file \"%s\" (%s).", optarg, strerror(errno));
                goto fail;
            }
            break;
        default:
            ERROR(__func__, "Unknown option -%c.", c);
            cmd_userrpc_help();
            goto fail;
        }
    }
    clear_arglist(&cmd);

    if (!session) {
        ERROR(__func__, "Not connected to a NETCONF server, no RPCs can be sent.");
        goto fail;
    }

    if (!interleave) {
        ERROR(__func__, "NETCONF server does not support interleaving RPCs and notifications.");
        goto fail;
    }

    /* check if edit configuration data were specified */
    if (!content) {
        /* let user write edit data interactively */
        content = readinput("Type the content of a configuration datastore.");
        if (!content) {
            ERROR(__func__, "Reading configuration data failed.");
            goto fail;
        }
    }

    /* create requests */
    rpc = nc_rpc_generic_xml(content, NC_RPC_PARAMTYPE_FREE);
    if (!rpc) {
        ERROR(__func__, "RPC creation failed.");
        goto fail;
    }

    if (output) {
        ret = cli_send_recv(rpc, output);
        fclose(output);
    } else {
        ret = cli_send_recv(rpc, stdout);
    }

    nc_rpc_free(rpc);
    return ret;

fail:
    clear_arglist(&cmd);
    if (output) {
        fclose(output);
    }
    free(content);
    return EXIT_FAILURE;
}

COMMAND commands[] = {
#ifdef ENABLE_SSH
        {"auth", cmd_auth, cmd_auth_help, "Manage SSH authentication options"},
        {"knownhosts", cmd_knownhosts, cmd_knownhosts_help, "Manage the user knownhosts file"},
#endif
#ifdef ENABLE_TLS
        {"cert", cmd_cert, cmd_cert_help, "Manage trusted or your own certificates"},
        {"crl", cmd_crl, cmd_crl_help, "Manage Certificate Revocation List directory"},
#endif
        {"searchpath", cmd_searchpath, cmd_searchpath_help, "Set the search path for models"},
        {"verb", cmd_verb, cmd_verb_help, "Change verbosity"},
        {"disconnect", cmd_disconnect, NULL, "Disconnect from a NETCONF server"},
        {"status", cmd_status, NULL, "Display information about the current NETCONF session"},
        {"connect", cmd_connect, cmd_connect_help, "Connect to a NETCONF server"},
        {"listen", cmd_listen, cmd_listen_help, "Wait for a Call Home connection from a NETCONF server"},
        {"quit", cmd_quit, NULL, "Quit the program"},
        {"help", cmd_help, NULL, "Display commands description"},
        {"editor", cmd_editor, cmd_editor_help, "Set the text editor for working with XML data"},
        {"cancel-commit", cmd_cancelcommit, cmd_cancelcommit_help, "ietf-netconf <cancel-commit> operation"},
        {"commit", cmd_commit, cmd_commit_help, "ietf-netconf <commit> operation"},
        {"copy-config", cmd_copyconfig, cmd_copyconfig_help, "ietf-netconf <copy-config> operation"},
        {"delete-config", cmd_deleteconfig, cmd_deleteconfig_help, "ietf-netconf <delete-config> operation"},
        {"discard-changes", cmd_discardchanges, cmd_discardchanges_help, "ietf-netconf <discard-changes> operation"},
        {"edit-config", cmd_editconfig, cmd_editconfig_help, "ietf-netconf <edit-config> operation"},
        {"get", cmd_get, cmd_get_help, "ietf-netconf <get> operation"},
        {"get-config", cmd_getconfig, cmd_getconfig_help, "ietf-netconf <get-config> operation"},
        {"kill-session", cmd_killsession, cmd_killsession_help, "ietf-netconf <kill-session> operation"},
        {"lock", cmd_lock, cmd_lock_help, "ietf-netconf <lock> operation"},
        {"unlock", cmd_unlock, cmd_unlock_help, "ietf-netconf <unlock> operation"},
        {"validate", cmd_validate, cmd_validate_help, "ietf-netconf <validate> operation"},
        {"subscribe", cmd_subscribe, cmd_subscribe_help, "notifications <create-subscription> operation"},
        {"get-schema", cmd_getschema, cmd_getschema_help, "ietf-netconf-monitoring <get-schema> operation"},
        {"user-rpc", cmd_userrpc, cmd_userrpc_help, "Send your own content in an RPC envelope (for DEBUG purposes)"},
        /* synonyms for previous commands */
        {"?", cmd_help, NULL, "Display commands description"},
        {"exit", cmd_quit, NULL, "Quit the program"},
        {NULL, NULL, NULL, NULL}
};
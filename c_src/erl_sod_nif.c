/*
 * Copyright 2011 - 2017 Maas-Maarten Zeeman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/


#include <string.h>
#include <stdio.h>

#include "erl_nif.h"
#include "enif_util.h"
#include "queue.h"
#include "sod.h"

#define MAX_PATHNAME 512
#define UNUSED(x) (void)(x)

static ErlNifResourceType *erl_sod_type = NULL;
typedef struct {
    ErlNifTid tid;
    ErlNifThreadOpts* opts;
    ErlNifPid notification_pid;
    queue *commands;
    sod_cnn *net;
} erl_sod_connection;

typedef enum {
    cmd_unknown,
    cmd_stop,
    cmd_net_open,
    cmd_net_close
} command_type;

typedef struct {
    command_type type;

    ErlNifEnv *env;
    ERL_NIF_TERM ref;
    ErlNifPid pid;
    ERL_NIF_TERM arg;
} erl_sod_command;

static ERL_NIF_TERM atom_erl_sod;

static ERL_NIF_TERM push_command(ErlNifEnv *env, erl_sod_connection *conn, erl_sod_command *cmd);

static void
command_destroy(void *obj)
{
    erl_sod_command *cmd = (erl_sod_command *) obj;

    if(cmd->env != NULL)
	   enif_free_env(cmd->env);

    enif_free(cmd);
}

static erl_sod_command*
command_create()
{
    erl_sod_command *cmd = (erl_sod_command *) enif_alloc(sizeof(erl_sod_command));
    if(cmd == NULL)
	   return NULL;

    cmd->env = enif_alloc_env();
    if(cmd->env == NULL) {
	    command_destroy(cmd);
        return NULL;
    }

    cmd->type = cmd_unknown;
    cmd->ref = 0;
    cmd->arg = 0;
    return cmd;
}

static ERL_NIF_TERM
do_net_open(ErlNifEnv *env, erl_sod_connection* conn, const ERL_NIF_TERM arg)
{
    char filename[MAX_PATHNAME];
    unsigned int size;
    int rc;

    size = enif_get_string(env, arg, filename, MAX_PATHNAME, ERL_NIF_LATIN1);
    if(size <= 0)
        return make_error_tuple(env, "invalid_filename");

    const char *net_error; /* Error log if any */
    enif_fprintf(stderr, "HERE\r\n");
    rc = sod_cnn_create(&conn->net, ":fast", filename, &net_error);

    if (rc != SOD_OK) {
        conn->net = NULL;
        return make_error_tuple(env, net_error);
    }
    return make_atom(env, "ok");
}

static ERL_NIF_TERM
do_net_close(ErlNifEnv *env, erl_sod_connection* conn)
{
    if(conn->net) {
        sod_cnn_destroy(conn->net);
        conn->net = NULL;
    }
    return make_atom(env, "ok");
}

static ERL_NIF_TERM
evaluate_command(erl_sod_command *cmd, erl_sod_connection *conn)
{
    UNUSED(conn);
    switch(cmd->type) {
      case cmd_net_open:
        return do_net_open(cmd->env, conn, cmd->arg);
      case cmd_net_close:
        return do_net_close(cmd->env, conn);
      default:
        return make_error_tuple(cmd->env, "invalid_command");
    }
}

static ERL_NIF_TERM
push_command(ErlNifEnv *env, erl_sod_connection *conn, erl_sod_command *cmd) {
    if(!queue_push(conn->commands, cmd))
        return make_error_tuple(env, "command_push_failed");

    return make_atom(env, "ok");
}

static ERL_NIF_TERM
make_answer(erl_sod_command *cmd, ERL_NIF_TERM answer)
{
    return enif_make_tuple3(cmd->env, atom_erl_sod, cmd->ref, answer);
}

static void *
erl_sod_connection_run(void *arg)
{
    erl_sod_connection *conn = (erl_sod_connection *) arg;
    erl_sod_command *cmd;
    int continue_running = 1;

    while(continue_running) {
	    cmd = (erl_sod_command*)queue_pop(conn->commands);

        if(cmd->type == cmd_stop) {
	        continue_running = 0;
        } else {
	        enif_send(NULL, &cmd->pid, cmd->env, make_answer(cmd, evaluate_command(cmd, conn)));
        }

	    command_destroy(cmd);
    }

    return NULL;
}

/*
 * Start the processing thread
 */
static ERL_NIF_TERM
erl_sod_start(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    UNUSED(argc);
    UNUSED(argv);
    erl_sod_connection *conn;
    ERL_NIF_TERM conn_resource;

    /* Initialize the resource */
    conn = (erl_sod_connection *) enif_alloc_resource(erl_sod_type, sizeof(erl_sod_connection));
    if(!conn)
	    return make_error_tuple(env, "no_memory");

    /* Create command queue */
    conn->commands = queue_create();
    if(!conn->commands) {
	    enif_release_resource(conn);
	    return make_error_tuple(env, "command_queue_create_failed");
    }

    /* Start command processing thread */
    conn->opts = enif_thread_opts_create((char*) "erl_video_capture_thread_opts");
    if(enif_thread_create((char*) "erl_sod_connection", &conn->tid, erl_sod_connection_run, conn, conn->opts) != 0) {
	    enif_release_resource(conn);
	    return make_error_tuple(env, (char*)"thread_create_failed");
    }

    conn_resource = enif_make_resource(env, conn);
    enif_release_resource(conn);

    return make_ok_tuple(env, conn_resource);
}

static ERL_NIF_TERM
erl_sod_net_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    erl_sod_connection *conn;
    erl_sod_command *cmd = NULL;
    ErlNifPid pid;

    if(argc != 4)
	    return enif_make_badarg(env);
    if(!enif_get_resource(env, argv[0], erl_sod_type, (void **) &conn))
	    return enif_make_badarg(env);
    if(!enif_is_ref(env, argv[1]))
	    return make_error_tuple(env, "invalid_ref");
    if(!enif_get_local_pid(env, argv[2], &pid))
	    return make_error_tuple(env, "invalid_pid");
    if(!enif_is_list(env, argv[3]))
	    return make_error_tuple(env, "invalid_arg");

    /* Note, no check is made for the type of the argument */
    cmd = command_create();
    if(!cmd)
	    return make_error_tuple(env, "command_create_failed");

    cmd->type = cmd_net_open;
    cmd->ref = enif_make_copy(cmd->env, argv[1]);
    cmd->pid = pid;
    cmd->arg = enif_make_copy(cmd->env, argv[3]);

    return push_command(env, conn, cmd);
}

static void
destruct_sod_connection(ErlNifEnv* env, void *arg)
{
    UNUSED(env);
    enif_fprintf(stderr, "destruct sod_conn\r\n");
    erl_sod_connection *conn = (erl_sod_connection *) arg;
    erl_sod_command *close_cmd = command_create();
    erl_sod_command *stop_cmd = command_create();

    /* Send the close command
    */
    close_cmd->type = cmd_net_close;
    queue_push(conn->commands, close_cmd);

        /* Send the stop command
    */
    stop_cmd->type = cmd_stop;
    queue_push(conn->commands, stop_cmd);

    /* Wait for the thread to finish
     */
    enif_thread_join(conn->tid, NULL);

    enif_thread_opts_destroy(conn->opts);

    while(queue_has_item(conn->commands)) {
        command_destroy(queue_pop(conn->commands));
    }
    queue_destroy(conn->commands);
}

/*
 * Load the nif. Initialize some stuff and such
 */
static int
on_load(ErlNifEnv* env, void** priv, ERL_NIF_TERM term)
{
    UNUSED(priv);
    UNUSED(term);
    ErlNifResourceType *rt;

    rt = enif_open_resource_type(env, "erl_sod_nif", "erl_sod_type",
				destruct_sod_connection, ERL_NIF_RT_CREATE, NULL);
    if(!rt)
	    return -1;
    erl_sod_type = rt;

    atom_erl_sod = make_atom(env, "erl_sod_nif");
    return 0;
}

static int on_reload(ErlNifEnv* env, void** priv, ERL_NIF_TERM term)
{
    UNUSED(env);
    UNUSED(priv);
    UNUSED(term);
    return 0;
}

static int on_upgrade(ErlNifEnv* env, void** old, void** new, ERL_NIF_TERM term)
{
    UNUSED(env);
    UNUSED(old);
    UNUSED(new);
    UNUSED(term);
    return 0;
}

static ErlNifFunc nif_funcs[] = {
    {"start", 0, erl_sod_start, 0},
    {"net_open", 4, erl_sod_net_open, 0}
};
ERL_NIF_INIT(erl_sod_nif, nif_funcs, on_load, on_reload, on_upgrade, NULL);

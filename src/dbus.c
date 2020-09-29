#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <systemd/sd-bus.h>

#include "_features.h"

// LCOV_EXCL_START
/* XXX: (cyphermox)
 * This file  is completely excluded from coverage on purpose. Tests should
 * still include code in here, but sadly coverage does not appear to
 * correctly capture tests being run over a DBus bus.
 */

static gint _try_child_stdin = -1;
static GPid _try_child_pid = -1;
static guint _try_child_timeout = 0;

static int method_apply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    g_autoptr(GError) err = NULL;
    g_autofree gchar *stdout = NULL;
    g_autofree gchar *stderr = NULL;
    gint exit_status = 0;

    gchar *argv[] = {SBINDIR "/" "netplan", "apply", NULL};

    // for tests only: allow changing what netplan to run
    if (getuid() != 0 && getenv("DBUS_TEST_NETPLAN_CMD") != 0) {
       argv[0] = getenv("DBUS_TEST_NETPLAN_CMD");
    }

    g_spawn_sync("/", argv, NULL, 0, NULL, NULL, &stdout, &stderr, &exit_status, &err);
    if (err != NULL) {
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "cannot run netplan apply: %s", err->message);
    }
    g_spawn_check_exit_status(exit_status, &err);
    if (err != NULL) {
       return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "netplan apply failed: %s\nstdout: '%s'\nstderr: '%s'", err->message, stdout, stderr);
    }
    
    return sd_bus_reply_method_return(m, "b", true);
}

static int method_info(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    sd_bus_message *reply = NULL;
    g_autoptr(GError) err = NULL;
    g_autofree gchar *stdout = NULL;
    g_autofree gchar *stderr = NULL;
    gint exit_status = 0;

    exit_status = sd_bus_message_new_method_return(m, &reply);
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_open_container(reply, 'a', "(sv)");
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_open_container(reply, 'r', "sv");
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_append(reply, "s", "Features");
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_open_container(reply, 'v', "as");
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_append_strv(reply, (char**)feature_flags);
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_close_container(reply);
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_close_container(reply);
    if (exit_status < 0)
       return exit_status;

    exit_status = sd_bus_message_close_container(reply);
    if (exit_status < 0)
       return exit_status;

    return sd_bus_send(NULL, reply, NULL);
}

//XXX: choose more telling function name, like _netplan_try_child_still alive?
static int
check_netplan_try_child(sd_bus_message *m, sd_bus_error *ret_error)
{
    int status = -1;
    int r = -1;
    //guint now = (guint) time(NULL);
    if (_try_child_pid < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "Invalid netplan try PID");

    r = (int)waitpid(_try_child_pid, &status, WNOHANG)
    if (r > 0 && status == 0) {
        /* The child already exited. Cannot send signal. Cleanup. */
        _try_child_stdin = -1;
        _try_child_pid = -1;
        _try_child_timeout = 0;
        return sd_bus_reply_method_return(m, "b", false);
    }
    //TODO: handle error cases
#ifdef 0
    if (now >= _try_child_timeout) {
        /* Time is up. Cancel 'netplan try' child process (if still running) */
        kill(_try_child_pid, SIGINT);
        g_spawn_close_pid (_try_child_pid);
        /* cleanup */
        _try_child_stdin = -1;
        _try_child_pid = -1;
        _try_child_timeout = 0;
        return sd_bus_reply_method_return(m, "b", false);
    }
#endif

    return 0;
}

static int method_try(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    g_autoptr(GError) err = NULL;
    g_autofree gchar *timeout = NULL;
    guint seconds = 0;

    if (_try_child_timeout > 0 || _try_child_pid > 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "cannot run netplan try: already running");

    if (sd_bus_message_read_basic (m, 'u', &seconds) < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "cannot extract timeout_seconds");
    if (seconds > 0)
        timeout = g_strdup_printf("--timeout=%u", seconds);
    gchar *argv[] = {SBINDIR "/" "netplan", "try", timeout, NULL};

    // for tests only: allow changing what netplan to run
    //if (getuid() != 0 && getenv("DBUS_TEST_NETPLAN_CMD") != 0)
       argv[0] = getenv("DBUS_TEST_NETPLAN_CMD");

    // TODO: stdout & stderr to /dev/null flags
    g_spawn_async_with_pipes("/", argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &_try_child_pid, &_try_child_stdin, NULL, NULL, &err);
    if (err != NULL)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "cannot run netplan try: %s", err->message);

    _try_child_timeout = (guint)time(NULL) + seconds;
    // TODO: set a timer (via event-loop) to send a Changed() signal via DBus and cancel the try command

    return sd_bus_reply_method_return(m, "b", true);
}

static int method_try_cancel(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    GError *error = NULL;
    GIOChannel *stdin = NULL;
    int status = -1;
    int r = check_netplan_try_child(m, ret_error);

    if (r != 0)
        return r;

    /* Child does not exist or already exited ... */
    // XXX: isn't this checked in check_netplan_try_child() already?
    if (_try_child_pid < 0 || waitpid (_try_child_pid, &status, WNOHANG) < 0)
        return sd_bus_reply_method_return(m, "b", false);

    /* Send cancel signal to 'netplan try' process */
    kill(_try_child_pid, SIGINT);
    waitpid (_try_child_pid, &status, 0);
    g_spawn_check_exit_status(status, &error);
    if (error != NULL)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "netplan try failed: %s", error->message);
    if (!WIFEXITED(status))
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "netplan try failed: %s", "IFEXITED");
    printf("Child exited with status code %d\n", WEXITSTATUS(status));

    g_spawn_close_pid (_try_child_pid);
    _try_child_stdin = -1;
    _try_child_pid = -1;
    _try_child_timeout = 0;

    return sd_bus_reply_method_return(m, "b", true);
}

static int method_try_confirm(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    // TODO: move this into Apply()
    GError *error = NULL;
    int status = -1;
    int r = check_netplan_try_child(m, ret_error);

    if (r != 0)
        return r;

    if (_try_child_pid < 0 || waitpid (_try_child_pid, &status, WNOHANG) < 0) {
        /* Child does not exist or already exited ... */
        return sd_bus_reply_method_return(m, "b", false);
    }

    kill(_try_child_pid, SIGUSR1);
    waitpid(_try_child_pid, &status, 0);
    g_spawn_check_exit_status(status, &error);
    if (error != NULL)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "netplan try failed: %s", error->message);
    if (!WIFEXITED(status))
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "netplan try failed: %s", "IFEXITED");
    printf("Child exited with status code %d\n", WEXITSTATUS(status));

    g_spawn_close_pid (_try_child_pid);

    return sd_bus_reply_method_return(m, "b", true);
}

static const sd_bus_vtable netplan_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Apply", "", "b", method_apply, 0),
    SD_BUS_METHOD("Info", "", "a(sv)", method_info, 0),
    SD_BUS_METHOD("Try", "u", "b", method_try, 0),
    SD_BUS_METHOD("Cancel", "", "b", method_try_cancel, 0),
    SD_BUS_METHOD("Confirm", "", "b", method_try_confirm, 0),
    SD_BUS_VTABLE_END
};

int main(int argc, char *argv[]) {
    sd_bus_slot *slot = NULL;
    sd_bus *bus = NULL;
    int r;
   
    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
        goto finish;
    }

    r = sd_bus_add_object_vtable(bus,
                                     &slot,
                                     "/io/netplan/Netplan",  /* object path */
                                     "io.netplan.Netplan",   /* interface name */
                                     netplan_vtable,
                                     NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
        goto finish;
    }

    r = sd_bus_request_name(bus, "io.netplan.Netplan", 0);
    if (r < 0) {
        fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-r));
        goto finish;
    }
    for (;;) {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
            goto finish;
        }
        if (r > 0)
            continue;

        /* Wait for the next request to process */
        r = sd_bus_wait(bus, (uint64_t) -1);
        if (r < 0) {
            fprintf(stderr, "Failed to wait on bus: %s\n", strerror(-r));
            goto finish;
        }
    }

finish:
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);

    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

// LCOV_EXCL_STOP

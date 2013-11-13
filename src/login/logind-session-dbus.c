/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <string.h>

#include "util.h"
#include "strv.h"
#include "bus-util.h"

#include "logind.h"
#include "logind-session.h"
#include "logind-session-device.h"

static int property_get_user(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        _cleanup_free_ char *p = NULL;
        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        p = user_bus_path(s->user);
        if (!p)
                return -ENOMEM;

        return sd_bus_message_append(reply, "(uo)", (uint32_t) s->user->uid, p);
}

static int property_get_name(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        return sd_bus_message_append(reply, "s", s->user->name);
}

static int property_get_seat(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        _cleanup_free_ char *p = NULL;
        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        p = s->seat ? seat_bus_path(s->seat) : strdup("/");
        if (!p)
                return -ENOMEM;

        return sd_bus_message_append(reply, "(so)", s->seat ? s->seat->id : "", p);
}

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_type, session_type, SessionType);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_class, session_class, SessionClass);

static int property_get_active(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        return sd_bus_message_append(reply, "b", session_is_active(s));
}

static int property_get_state(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        return sd_bus_message_append(reply, "s", session_state_to_string(session_get_state(s)));
}

static int property_get_idle_hint(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Session *s = userdata;

        assert(bus);
        assert(reply);
        assert(s);

        return sd_bus_message_append(reply, "b", session_get_idle_hint(s, NULL) > 0);
}

static int property_get_idle_since_hint(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Session *s = userdata;
        dual_timestamp t;
        uint64_t u;
        int r;

        assert(bus);
        assert(reply);
        assert(s);

        r = session_get_idle_hint(s, &t);
        if (r < 0)
                return r;

        u = streq(property, "IdleSinceHint") ? t.realtime : t.monotonic;

        return sd_bus_message_append(reply, "t", u);
}

static int method_terminate(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = session_stop(s);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_activate(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = session_activate(s);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_lock(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = session_send_lock(s, streq(sd_bus_message_get_member(message), "Lock"));
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_set_idle_hint(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        uid_t uid;
        int r, b;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "b", &b);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        r = sd_bus_get_owner_uid(bus, sd_bus_message_get_sender(message), &uid);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (uid != 0 && uid != s->user->uid)
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_ACCESS_DENIED, "Only owner of session my set idle hint");

        session_set_idle_hint(s, b);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_kill(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        const char *swho;
        int32_t signo;
        KillWho who;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "si", &swho, &signo);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (isempty(swho))
                who = KILL_ALL;
        else {
                who = kill_who_from_string(swho);
                if (who < 0)
                        return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid kill parameter '%s'", swho);
        }

        if (signo <= 0 || signo >= _NSIG)
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid signal %i", signo);

        r = session_kill(s, who, signo);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_take_control(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        int r, force;
        uid_t uid;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "b", &force);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        r = sd_bus_get_owner_uid(bus, sd_bus_message_get_sender(message), &uid);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (uid != 0 && (force || uid != s->user->uid))
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_ACCESS_DENIED, "Only owner of session may take control");

        r = session_set_controller(s, sd_bus_message_get_sender(message), force);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_release_control(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;

        assert(bus);
        assert(message);
        assert(s);

        if (!session_is_controller(s, sd_bus_message_get_sender(message)))
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NOT_IN_CONTROL, "You are not in control of this session");

        session_drop_controller(s);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_take_device(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        uint32_t major, minor;
        SessionDevice *sd;
        dev_t dev;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "uu", &major, &minor);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (!session_is_controller(s, sd_bus_message_get_sender(message)))
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NOT_IN_CONTROL, "You are not in control of this session");

        dev = makedev(major, minor);
        sd = hashmap_get(s->devices, &dev);
        if (sd)
                /* We don't allow retrieving a device multiple times.
                 * The related ReleaseDevice call is not ref-counted.
                 * The caller should use dup() if it requires more
                 * than one fd (it would be functionally
                 * equivalent). */
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_DEVICE_IS_TAKEN, "Device already taken");

        r = session_device_new(s, dev, &sd);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        r = sd_bus_reply_method_return(bus, message, "hb", sd->fd, !sd->active);
        if (r < 0)
                session_device_free(sd);

        return r;
}

static int method_release_device(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        uint32_t major, minor;
        SessionDevice *sd;
        dev_t dev;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "uu", &major, &minor);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (!session_is_controller(s, sd_bus_message_get_sender(message)))
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NOT_IN_CONTROL, "You are not in control of this session");

        dev = makedev(major, minor);
        sd = hashmap_get(s->devices, &dev);
        if (!sd)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_DEVICE_NOT_TAKEN, "Device not taken");

        session_device_free(sd);
        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_pause_device_complete(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Session *s = userdata;
        uint32_t major, minor;
        SessionDevice *sd;
        dev_t dev;
        int r;

        assert(bus);
        assert(message);
        assert(s);

        r = sd_bus_message_read(message, "uu", &major, &minor);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (!session_is_controller(s, sd_bus_message_get_sender(message)))
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NOT_IN_CONTROL, "You are not in control of this session");

        dev = makedev(major, minor);
        sd = hashmap_get(s->devices, &dev);
        if (!sd)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_DEVICE_NOT_TAKEN, "Device not taken");

        session_device_complete_pause(sd);

        return sd_bus_reply_method_return(bus, message, NULL);
}

const sd_bus_vtable session_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Id", "s", NULL, offsetof(Session, id), 0),
        SD_BUS_PROPERTY("User", "(uo)", property_get_user, 0, 0),
        SD_BUS_PROPERTY("Name", "s", property_get_name, 0, 0),
        SD_BUS_PROPERTY("Timestamp", "t", NULL, offsetof(Session, timestamp.realtime), 0),
        SD_BUS_PROPERTY("TimestampMonotonic", "t", NULL, offsetof(Session, timestamp.monotonic), 0),
        SD_BUS_PROPERTY("VTNr", "u", NULL, offsetof(Session, vtnr), 0),
        SD_BUS_PROPERTY("Seat", "(so)", property_get_seat, 0, 0),
        SD_BUS_PROPERTY("TTY", "s", NULL, offsetof(Session, tty), 0),
        SD_BUS_PROPERTY("Display", "s", NULL, offsetof(Session, display), 0),
        SD_BUS_PROPERTY("Remote", "b", bus_property_get_bool, offsetof(Session, remote), 0),
        SD_BUS_PROPERTY("RemoteHost", "s", NULL, offsetof(Session, remote_host), 0),
        SD_BUS_PROPERTY("RemoteUser", "s", NULL, offsetof(Session, remote_user), 0),
        SD_BUS_PROPERTY("Service", "s", NULL, offsetof(Session, service), 0),
        SD_BUS_PROPERTY("Scope", "s", NULL, offsetof(Session, scope), 0),
        SD_BUS_PROPERTY("Leader", "u", bus_property_get_pid, offsetof(Session, leader), 0),
        SD_BUS_PROPERTY("Audit", "u", NULL, offsetof(Session, audit_id), 0),
        SD_BUS_PROPERTY("Type", "s", property_get_type, offsetof(Session, type), 0),
        SD_BUS_PROPERTY("Class", "s", property_get_class, offsetof(Session, class), 0),
        SD_BUS_PROPERTY("Active", "b", property_get_active, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("State", "s", property_get_state, 0, 0),
        SD_BUS_PROPERTY("IdleHint", "b", property_get_idle_hint, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("IdleSinceHint", "t", property_get_idle_since_hint, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("IdleSinceHintMonotonic", "t", property_get_idle_since_hint, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

        SD_BUS_METHOD("Terminate", NULL, NULL, method_terminate, 0),
        SD_BUS_METHOD("Activate", NULL, NULL, method_activate, 0),
        SD_BUS_METHOD("Lock", NULL, NULL, method_lock, 0),
        SD_BUS_METHOD("Unlock", NULL, NULL, method_lock, 0),
        SD_BUS_METHOD("SetIdleHint", "b", NULL, method_set_idle_hint, 0),
        SD_BUS_METHOD("Kill", "si", NULL, method_kill, 0),
        SD_BUS_METHOD("TakeControl", "b", NULL, method_take_control, 0),
        SD_BUS_METHOD("ReleaseControl", NULL, NULL, method_release_control, 0),
        SD_BUS_METHOD("TakeDevice", "uu", "hb", method_take_device, 0),
        SD_BUS_METHOD("ReleaseDevice", "uu", NULL, method_release_device, 0),
        SD_BUS_METHOD("PauseDeviceComplete", "uu", NULL, method_pause_device_complete, 0),

        SD_BUS_SIGNAL("PauseDevice", "uus", 0),
        SD_BUS_SIGNAL("ResumeDevice", "uuh", 0),
        SD_BUS_SIGNAL("Lock", NULL, 0),
        SD_BUS_SIGNAL("Unlock", NULL, 0),

        SD_BUS_VTABLE_END
};

int session_object_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        Session *session;
        int r;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        if (streq(path, "/org/freedesktop/login1/session/self")) {
                sd_bus_message *message;
                pid_t pid;

                message = sd_bus_get_current(bus);
                if (!message)
                        return 0;

                r = sd_bus_get_owner_pid(bus, sd_bus_message_get_sender(message), &pid);
                if (r < 0)
                        return 0;

                r = manager_get_session_by_pid(m, pid, &session);
                if (r <= 0)
                        return 0;
        } else {
                _cleanup_free_ char *e = NULL;
                const char *p;

                p = startswith(path, "/org/freedesktop/login1/session/");
                if (!p)
                        return 0;

                e = bus_path_unescape(p);
                if (!e)
                        return -ENOMEM;

                session = hashmap_get(m->sessions, e);
                if (!session)
                        return 0;
        }

        *found = session;
        return 1;
}

char *session_bus_path(Session *s) {
        _cleanup_free_ char *t = NULL;

        assert(s);

        t = bus_path_escape(s->id);
        if (!t)
                return NULL;

        return strappend("/org/freedesktop/login1/session/", t);
}

int session_node_enumerator(sd_bus *bus, const char *path, char ***nodes, void *userdata) {
        _cleanup_strv_free_ char **l = NULL;
        Manager *m = userdata;
        Session *session;
        Iterator i;
        int r;

        assert(bus);
        assert(path);
        assert(nodes);

        HASHMAP_FOREACH(session, m->sessions, i) {
                char *p;

                p = session_bus_path(session);
                if (!p)
                        return -ENOMEM;

                r = strv_push(&l, p);
                if (r < 0) {
                        free(p);
                        return r;
                }
        }

        *nodes = l;
        l = NULL;

        return 1;
}

int session_send_signal(Session *s, bool new_session) {
        _cleanup_free_ char *p = NULL;

        assert(s);

        p = session_bus_path(s);
        if (!p)
                return -ENOMEM;

        return sd_bus_emit_signal(
                        s->manager->bus,
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        new_session ? "SessionNew" : "SessionRemoved",
                        "so", s->id, p);
}

int session_send_changed(Session *s, const char *properties, ...) {
        _cleanup_free_ char *p = NULL;
        char **l;

        assert(s);

        if (!s->started)
                return 0;

        p = session_bus_path(s);
        if (!p)
                return -ENOMEM;

        l = strv_from_stdarg_alloca(properties);

        return sd_bus_emit_properties_changed_strv(s->manager->bus, p, "org.freedesktop.login1.Session", l);
}

int session_send_lock(Session *s, bool lock) {
        _cleanup_free_ char *p = NULL;

        assert(s);

        p = session_bus_path(s);
        if (!p)
                return -ENOMEM;

        return sd_bus_emit_signal(
                        s->manager->bus,
                        p,
                        "org.freedesktop.login1.Session",
                        lock ? "Lock" : "Unlock",
                        NULL);
}

int session_send_lock_all(Manager *m, bool lock) {
        Session *session;
        Iterator i;
        int r = 0;

        assert(m);

        HASHMAP_FOREACH(session, m->sessions, i) {
                int k;

                k = session_send_lock(session, lock);
                if (k < 0)
                        r = k;
        }

        return r;
}

int session_send_create_reply(Session *s, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *c = NULL;
        _cleanup_close_ int fifo_fd = -1;
        _cleanup_free_ char *p = NULL;

        assert(s);

        /* This is called after the session scope was successfully
         * created, and finishes where bus_manager_create_session()
         * left off. */

        if (!s->create_message)
                return 0;

        c = s->create_message;
        s->create_message = NULL;

        if (error)
                return sd_bus_reply_method_error(s->manager->bus, c, error);

        fifo_fd = session_create_fifo(s);
        if (fifo_fd < 0)
                return fifo_fd;

        /* Update the session state file before we notify the client
         * about the result. */
        session_save(s);

        p = session_bus_path(s);
        if (!p)
                return -ENOMEM;

        log_debug("Sending reply about created session: "
                  "id=%s object_path=%s runtime_path=%s session_fd=%d seat=%s vtnr=%u",
                  s->id,
                  p,
                  s->user->runtime_path,
                  fifo_fd,
                  s->seat ? s->seat->id : "",
                  (uint32_t) s->vtnr);

        return sd_bus_reply_method_return(
                        s->manager->bus, c,
                        "soshsub",
                        s->id,
                        p,
                        s->user->runtime_path,
                        fifo_fd,
                        s->seat ? s->seat->id : "",
                        (uint32_t) s->vtnr,
                        false);
}

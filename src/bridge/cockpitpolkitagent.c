/*
 * Copyright (C) 2008 Red Hat, Inc.
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 *         Cockpit Authors
 */

#include "config.h"

#include "cockpitpolkitagent.h"
#include "cockpitrouter.h"

#include "common/cockpithex.h"
#include "common/cockpitjson.h"
#include "common/cockpittransport.h"

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1
#include <polkitagent/polkitagent.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>

#define COCKPIT_TYPE_POLKIT_AGENT          (cockpit_polkit_agent_get_type())
#define COCKPIT_POLKIT_AGENT(o)            (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_POLKIT_AGENT, CockpitPolkitAgent))
#define COCKPIT_IS_POLKIT_AGENT(o)         (G_TYPE_CHECK_INSTANCE_TYPE ((o), COCKPIT_TYPE_POLKIT_AGENT))

typedef struct {
  PolkitAgentListener parent_instance;
  CockpitTransport *transport;
  CockpitRouter *router;

  /* Polkit helper sessions active */
  GHashTable *callers;
} CockpitPolkitAgent;

typedef struct {
  gchar *cookie;
  gchar *user;

  GTask *result;
  CockpitPolkitAgent *self;

  PolkitAgentSession *session;
  gulong completed_sig;
  gulong request_sig;
  gulong info_sig;
  gulong error_sig;

  GCancellable *cancellable;
  gulong cancel_sig;
} ReauthorizeCaller;

typedef struct {
  PolkitAgentListenerClass parent_class;
} CockpitPolkitAgentClass;

enum {
    PROP_0,
    PROP_TRANSPORT,
    PROP_ROUTER,
};

static GType cockpit_polkit_agent_get_type (void) G_GNUC_CONST;

G_DEFINE_TYPE (CockpitPolkitAgent, cockpit_polkit_agent, POLKIT_AGENT_TYPE_LISTENER);

static void
caller_free (gpointer data)
{
  ReauthorizeCaller *caller = data;
  if (caller->cancel_sig)
    g_signal_handler_disconnect (caller->cancellable, caller->cancel_sig);
  if (caller->cancellable)
    g_object_unref (caller->cancellable);

  if (caller->session)
    {
      g_signal_handler_disconnect (caller->session, caller->completed_sig);
      g_signal_handler_disconnect (caller->session, caller->request_sig);
      g_signal_handler_disconnect (caller->session, caller->info_sig);
      g_signal_handler_disconnect (caller->session, caller->error_sig);
      polkit_agent_session_cancel (caller->session);
      g_object_unref (caller->session);
    }

  if (caller->result)
    {
      g_debug ("cancelling agent authentication");
      g_task_return_new_error (caller->result, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "Operation was cancelled");
      g_object_unref (caller->result);
    }

  if (caller->self->router)
    cockpit_router_prompt_cancel (caller->self->router, caller);

  g_free (caller->cookie);
  g_free (caller->user);
  g_free (caller);
}

static void
cockpit_polkit_agent_init (CockpitPolkitAgent *self)
{
  self->callers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, caller_free);
}

static void
cockpit_polkit_agent_constructed (GObject *object)
{
  G_OBJECT_CLASS (cockpit_polkit_agent_parent_class)->constructed (object);
}

 static void
cockpit_polkit_agent_set_property (GObject *object,
                                   guint prop_id,
                                   const GValue *value,
                                   GParamSpec *pspec)
{
  CockpitPolkitAgent *self = COCKPIT_POLKIT_AGENT (object);
  switch (prop_id)
    {
      case PROP_TRANSPORT:
        self->transport = g_value_dup_object (value);
        break;
      case PROP_ROUTER:
        self->router = g_value_get_object (value);
        if (self->router)
          g_object_add_weak_pointer (G_OBJECT(self->router), (gpointer *)&self->router);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
cockpit_polkit_agent_dispose (GObject *object)
{
  CockpitPolkitAgent *self = COCKPIT_POLKIT_AGENT (object);

  g_hash_table_remove_all (self->callers);

  G_OBJECT_CLASS (cockpit_polkit_agent_parent_class)->dispose (object);
}

static void
cockpit_polkit_agent_finalize (GObject *object)
{
  CockpitPolkitAgent *self = COCKPIT_POLKIT_AGENT (object);

  if (self->transport)
    g_object_unref (self->transport);
  if (self->router)
    g_object_remove_weak_pointer (G_OBJECT(self->router), (gpointer *)&self->router);

  g_hash_table_destroy (self->callers);

  G_OBJECT_CLASS (cockpit_polkit_agent_parent_class)->finalize (object);
}

static void
on_completed (PolkitAgentSession *session,
              gboolean gained_authorization,
              gpointer user_data)
{
  ReauthorizeCaller *caller = user_data;

  g_debug ("polkit authentication completed");

  g_warn_if_fail (g_hash_table_steal (caller->self->callers, caller->cookie));
  g_task_return_boolean (caller->result, TRUE);

  g_object_unref (caller->result);
  caller->result = NULL;

  caller_free (caller);
}

static void
on_answer (const gchar *value,
           gpointer user_data)
{
  ReauthorizeCaller *caller = user_data;
  polkit_agent_session_response (caller->session, value ? value : "");
}

static void
on_request (PolkitAgentSession *session,
            const gchar *request,
            gboolean echo_on,
            gpointer user_data)
{
  ReauthorizeCaller *caller = user_data;

  if (echo_on)
    {
      g_message ("ignoring polkit helper request: %s", request);
      polkit_agent_session_response (session, "");
    }
  else if (caller->self->router)
    cockpit_router_prompt (caller->self->router, caller->user, NULL, on_answer, caller);
}

static void
on_show_error (PolkitAgentSession *session,
               const gchar *text,
               gpointer user_data)
{
  g_message ("polkit helper error: %s", text);
}

static void
on_show_info (PolkitAgentSession *session,
              const gchar *text,
              gpointer user_data)
{
  g_message ("polkit helper info: %s", text);
}

static void
on_cancelled (GCancellable *cancellable,
              gpointer user_data)
{
  ReauthorizeCaller *caller = user_data;
  g_debug ("cancelled agent authentication");
  g_hash_table_remove (caller->self->callers, caller->cookie);
}

static void
cockpit_polkit_agent_initiate_authentication (PolkitAgentListener *listener,
                                              const gchar *action_id,
                                              const gchar *message,
                                              const gchar *icon_name,
                                              PolkitDetails *details,
                                              const gchar *cookie,
                                              GList *identities,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data)
{
  CockpitPolkitAgent *self = COCKPIT_POLKIT_AGENT (listener);
  PolkitIdentity *identity = NULL;
  GTask *result = NULL;
  GString *unsupported = NULL;
  ReauthorizeCaller *caller;
  const gchar *name = NULL;
  gchar *string;
  uid_t uid;
  GList *l;

  g_debug ("polkit is requesting authentication");

  result = g_task_new (G_OBJECT (self), NULL, callback, user_data);

  if (!g_str_equal (action_id, "org.cockpit-project.cockpit.root-bridge"))
    {
      g_task_return_new_error (result, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                               "Unsupported action");
      goto out;
    }

  uid = getuid ();

  unsupported = g_string_new ("");
  for (l = identities; l != NULL; l = g_list_next (l))
    {
      if (POLKIT_IS_UNIX_USER (l->data))
        {
          if (polkit_unix_user_get_uid (l->data) == uid)
            {
              identity = g_object_ref (l->data);
              name = polkit_unix_user_get_name (l->data);
              break;
            }
        }

      string = polkit_identity_to_string (l->data);
      g_string_append_printf (unsupported, "%s ", string);
      g_free (string);
    }

  if (!identity)
    {
      g_message ("cannot reauthorize identity(s): %s", unsupported->str);
      g_task_return_new_error (result, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                               "Reauthorization not supported for identity");
      goto out;
    }

  caller = g_new0 (ReauthorizeCaller, 1);
  caller->cookie = g_strdup (cookie);
  caller->user = g_strdup (name);
  caller->session = polkit_agent_session_new (identity, cookie);
  caller->completed_sig = g_signal_connect (caller->session, "completed", G_CALLBACK (on_completed), caller);
  caller->request_sig = g_signal_connect (caller->session, "request", G_CALLBACK (on_request), caller);
  caller->info_sig = g_signal_connect (caller->session, "show-info", G_CALLBACK (on_show_info), NULL);
  caller->error_sig = g_signal_connect (caller->session, "show-error", G_CALLBACK (on_show_error), NULL);
  caller->cancellable = g_object_ref (cancellable);
  caller->cancel_sig = g_cancellable_connect (cancellable, G_CALLBACK (on_cancelled), caller, NULL);

  caller->result = g_object_ref (result);
  caller->self = self;

  polkit_agent_session_initiate (caller->session);
  g_hash_table_replace (self->callers, caller->cookie, caller);

  g_debug ("polkit helper starting");

out:
  if (unsupported)
    g_string_free (unsupported, TRUE);
  g_object_unref (result);
  if (identity)
    g_object_unref (identity);
}

static gboolean
cockpit_polkit_agent_initiate_authentication_finish (PolkitAgentListener *listener,
                                                     GAsyncResult *res,
                                                     GError **error)
{
  g_warn_if_fail (g_task_is_valid (res, listener));

  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cockpit_polkit_agent_class_init (CockpitPolkitAgentClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PolkitAgentListenerClass *listener_class = POLKIT_AGENT_LISTENER_CLASS (klass);

  gobject_class->constructed = cockpit_polkit_agent_constructed;
  gobject_class->set_property = cockpit_polkit_agent_set_property;
  gobject_class->dispose = cockpit_polkit_agent_dispose;
  gobject_class->finalize = cockpit_polkit_agent_finalize;

  listener_class->initiate_authentication = cockpit_polkit_agent_initiate_authentication;
  listener_class->initiate_authentication_finish = cockpit_polkit_agent_initiate_authentication_finish;

  g_object_class_install_property (gobject_class, PROP_TRANSPORT,
             g_param_spec_object ("transport", "transport", "transport", COCKPIT_TYPE_TRANSPORT,
                                  G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ROUTER,
             g_param_spec_object ("router", "router", "router", COCKPIT_TYPE_ROUTER,
                                  G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

typedef struct {
    PolkitAgentListener *listener;
    gpointer registration_handle;
} CockpitPolkitRegistered;

gpointer
cockpit_polkit_agent_register (CockpitTransport *transport,
                               CockpitRouter *router,
                               GCancellable *cancellable)
{
  CockpitPolkitRegistered *registered;
  PolkitAgentListener *listener = NULL;
  PolkitAuthority *authority = NULL;
  PolkitSubject *subject = NULL;
  GVariant *options;
  GError *error = NULL;
  gpointer handle = NULL;
  gchar *string;

  g_return_val_if_fail (transport != NULL, NULL);

  authority = polkit_authority_get_sync (cancellable, &error);
  if (authority == NULL)
    {
      g_message ("couldn't get polkit authority: %s", error->message);
      goto out;
    }

  subject = polkit_unix_session_new_for_process_sync (getpid (), cancellable, &error);
  if (subject == NULL)
    {
      /*
       * This can happen if there's a race between the polkit request and closing of
       * Cockpit. So it's not unheard of. We can complain, but not too loudly.
       */
      g_message ("couldn't create polkit session subject: %s", error->message);
      goto out;
    }

  listener = g_object_new (COCKPIT_TYPE_POLKIT_AGENT, "transport", transport, "router", router, NULL);
  options = NULL;

  handle = polkit_agent_listener_register_with_options (listener,
                                                        POLKIT_AGENT_REGISTER_FLAGS_NONE,
                                                        subject, NULL, options, cancellable, &error);

  if (error != NULL)
    {
      if ((g_error_matches (error, POLKIT_ERROR, POLKIT_ERROR_FAILED) &&
           error->message && strstr (error->message, "already exists")) ||
          g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
        {
          g_debug ("couldn't register polkit agent: %s", error->message);
        }
      else
        {
          g_dbus_error_strip_remote_error (error);
          g_message ("couldn't register polkit authentication agent: %s", error->message);
        }
      goto out;
    }

  string = polkit_subject_to_string (subject);

  g_debug ("registered polkit authentication agent for subject: %s", string);
  g_free (string);

out:
  if (subject)
    g_object_unref (subject);
  if (authority)
    g_object_unref (authority);
  g_clear_error (&error);

  if (handle)
    {
      registered = g_new0 (CockpitPolkitRegistered, 1);
      registered->registration_handle = handle;
      registered->listener = listener;
      return registered;
    }
  else
    {
      if (listener)
        g_object_unref (listener);
      return NULL;
    }
}

void
cockpit_polkit_agent_unregister (gpointer data)
{
  CockpitPolkitRegistered *registered = data;
  guint handler = 0;

  if (!registered)
    return;

  /* Explicitly cancel all pending operations */
  g_object_run_dispose (G_OBJECT (registered->listener));
  g_object_unref (registered->listener);

  /* Now unregister with polkit */
  polkit_agent_listener_unregister (registered->registration_handle);

  g_log_remove_handler (NULL, handler);

  g_free (registered);
}

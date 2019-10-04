/*
 * Copyright (C) 2018, Matthias Clasen
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gio/gunixfdlist.h>

#include "portal-private.h"
#include "session-private.h"
#include "utils-private.h"

/**
 * SECTION:screencast
 * @title: Screencast
 * @short_description: create screencast sessions
 *
 * A screencast session makes the content of a monitor or window
 * available as a pipewire stream.
 */

/**
 * SECTION:remote
 * @title: Remote desktop
 * @short_description: allow remote control of the session
 *
 * A remote desktop session allows to inject events into the input stream.
 */

typedef struct {
  XdpPortal *portal;
  char *id;
  XdpSessionType type;
  XdpDeviceType devices;
  XdpOutputType outputs;
  gboolean multiple;
  guint signal_id;
  GTask *task;
  char *request_path;
  guint cancelled_id;
} CreateCall;

static void
create_call_free (CreateCall *call)
{
  if (call->signal_id)
    g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);

  if (call->cancelled_id)
    g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);

  g_free (call->request_path);

  g_object_unref (call->portal);
  g_object_unref (call->task);

  g_free (call->id);

  g_free (call);
}

static void
sources_selected (GDBusConnection *bus,
                  const char *sender_name,
                  const char *object_path,
                  const char *interface_name,
                  const char *signal_name,
                  GVariant *parameters,
                  gpointer data)
{
  CreateCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    {
       XdpSession *session;

       session = _xdp_session_new (call->portal, call->id, call->type);
       g_task_return_pointer (call->task, session, g_object_unref);
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Remote desktop canceled");
  else if (response == 2)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Remote desktop failed");

  create_call_free (call);
}

static void
select_sources (CreateCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  g_autofree char *handle = NULL;

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  handle = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        handle,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        sources_selected,
                                                        call,
                                                        NULL);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_add (&options, "{sv}", "types", g_variant_new_uint32 (call->outputs));
  g_variant_builder_add (&options, "{sv}", "multiple", g_variant_new_boolean (call->multiple));
  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.ScreenCast",
                          "SelectSources",
                          g_variant_new ("(oa{sv})", call->id, &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          g_task_get_cancellable (call->task),
                          NULL,
                          NULL);
}

static void
devices_selected (GDBusConnection *bus,
                  const char *sender_name,
                  const char *object_path,
                  const char *interface_name,
                  const char *signal_name,
                  GVariant *parameters,
                  gpointer data)
{
  CreateCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    {
      g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);
      call->signal_id = 0;

      if (call->outputs != 0)
        select_sources (call);
      else
       {
         g_task_return_pointer (call->task, _xdp_session_new (call->portal, call->id, call->type), g_object_unref);
         create_call_free (call);
       }
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Remote desktop canceled");
  else if (response == 2)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Remote desktop failed");

  if (response != 0)
    create_call_free (call);
}

static void
select_devices (CreateCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  g_autofree char *handle = NULL;

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  handle = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        handle,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        devices_selected,
                                                        call,
                                                        NULL);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_add (&options, "{sv}", "type", g_variant_new_uint32 (call->devices));
  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "SelectDevices",
                          g_variant_new ("(oa{sv})", call->id, &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          g_task_get_cancellable (call->task),
                          NULL,
                          NULL);
}

static void
session_created (GDBusConnection *bus,
                 const char *sender_name,
                 const char *object_path,
                 const char *interface_name,
                 const char *signal_name,
                 GVariant *parameters,
                 gpointer data)
{
  CreateCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    {
      g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);
      call->signal_id = 0;

      if (call->type == XDP_SESSION_REMOTE_DESKTOP)
        select_devices (call);
      else
        select_sources (call);
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "CreateSession canceled");
  else if (response == 2)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "CreateSession failed");

  if (response != 0)
    create_call_free (call);
}

static void
create_cancelled_cb (GCancellable *cancellable,
                     gpointer data)
{
  CreateCall *call = data;

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          call->request_path,
                          REQUEST_INTERFACE,
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
create_session (CreateCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  g_autofree char *session_token = NULL;
  GCancellable *cancellable;

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  call->request_path = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        call->request_path,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        session_created,
                                                        call,
                                                        NULL);

  session_token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  call->id = g_strconcat (SESSION_PATH_PREFIX, call->portal->sender, "/", session_token, NULL);

  cancellable = g_task_get_cancellable (call->task);
  if (cancellable)
    call->cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (create_cancelled_cb), call);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_add (&options, "{sv}", "session_handle_token", g_variant_new_string (session_token));
  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          call->type == XDP_SESSION_REMOTE_DESKTOP ?
                              "org.freedesktop.portal.RemoteDesktop" :
                              "org.freedesktop.portal.ScreenCast",
                          "CreateSession",
                          g_variant_new ("(a{sv})", &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          NULL,
                          NULL);
}

/**
 * xdp_portal_create_screencast_session:
 * @portal: a #XdpPortal
 * @outputs: which kinds of source to offer in the dialog
 * @multiple: whether to allow selecting multiple sources
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Creates a session for a screencast.
 *
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_create_screencast_session_finish() to get the results.
 */
void
xdp_portal_create_screencast_session (XdpPortal *portal,
                                      XdpOutputType outputs,
                                      gboolean multiple,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer data)
{
  CreateCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (CreateCall, 1);
  call->portal = g_object_ref (portal);
  call->type = XDP_SESSION_SCREENCAST;
  call->devices = XDP_DEVICE_NONE;
  call->outputs = outputs;
  call->multiple = multiple;
  call->task = g_task_new (portal, cancellable, callback, data);

  create_session (call);
}

/**
 * xdp_portal_create_screencast_session_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes the create-screencast request, and returns an #XdpSession.
 *
 * Returns: (transfer full): a #XdpSession
 */
XdpSession *
xdp_portal_create_screencast_session_finish (XdpPortal *portal,
                                             GAsyncResult *result,
                                             GError **error)
{
  XdpSession *session;

  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  session = g_task_propagate_pointer (G_TASK (result), error);
  if (session)
    return g_object_ref (session);
  else
    return NULL;
}

/**
 * xdp_portal_create_remote_desktop_session:
 * @portal: a #XdpPortal
 * @devices: which kinds of input devices to ofer in the new dialog
 * @outputs: which kinds of source to offer in the dialog
 * @multiple: whether to allow selecting multiple sources
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Creates a session for remote desktop.
 *
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_create_remote_desktop_session_finish() to get the results.
 */
void
xdp_portal_create_remote_desktop_session (XdpPortal *portal,
                                          XdpDeviceType devices,
                                          XdpOutputType outputs,
                                          gboolean multiple,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer data)
{
  CreateCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (CreateCall, 1);
  call->portal = g_object_ref (portal);
  call->type = XDP_SESSION_REMOTE_DESKTOP;
  call->devices = devices;
  call->outputs = outputs;
  call->multiple = multiple;
  call->task = g_task_new (portal, cancellable, callback, data);

  create_session (call);
}

/**
 * xdp_portal_create_remote_desktop_session_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes the create-remote-desktop request, and returns an #XdpSession.
 *
 * Returns: (transfer full): a #XdpSession
 */
XdpSession *
xdp_portal_create_remote_desktop_session_finish (XdpPortal *portal,
                                                 GAsyncResult *result,
                                                 GError **error)
{
  XdpSession *session;

  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  session = g_task_propagate_pointer (G_TASK (result), error);

  if (session)
    return g_object_ref (session);
  else
    return NULL;
}


typedef struct {
  XdpPortal *portal;
  XdpSession *session;
  XdpParent *parent;
  char *parent_handle;
  guint signal_id;
  GTask *task;
  char *request_path;
  guint cancelled_id;
} StartCall;

static void
start_call_free (StartCall *call)
{
  if (call->parent)
    {
      call->parent->unexport (call->parent);
      _xdp_parent_free (call->parent);
    }
  g_free (call->parent_handle);
  
  if (call->signal_id)
    g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);

  if (call->cancelled_id)
    g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);

  g_free (call->request_path);

  g_object_unref (call->session);
  g_object_unref (call->portal);
  g_object_unref (call->task);

  g_free (call);
}

static void
session_started (GDBusConnection *bus,
                 const char *sender_name,
                 const char *object_path,
                 const char *interface_name,
                 const char *signal_name,
                 GVariant *parameters,
                 gpointer data)
{
  StartCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    {
      guint32 devices;
      GVariant *streams;

      if (g_variant_lookup (ret, "devices", "u", &devices))
        _xdp_session_set_devices (call->session, devices);
      if (g_variant_lookup (ret, "streams", "@a(ua{sv})", &streams))
        _xdp_session_set_streams (call->session, streams);

      g_task_return_boolean (call->task, TRUE);
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Screencast canceled");
  else if (response == 2)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Screencast failed");

  _xdp_session_set_session_state (call->session, response == 0 ? XDP_SESSION_ACTIVE
                                                               : XDP_SESSION_CLOSED);
 
  start_call_free (call);
}

static void start_session (StartCall *call);

static void
parent_exported (XdpParent *parent,
                 const char *handle,
                 gpointer data)
{
  StartCall *call = data;
  call->parent_handle = g_strdup (handle);
  start_session (call);
}

static void
start_cancelled_cb (GCancellable *cancellable,
                    gpointer data)
{
  StartCall *call = data;

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          call->request_path,
                          REQUEST_INTERFACE,
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
start_session (StartCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  GCancellable *cancellable;

  if (call->parent_handle == NULL)
    {
      call->parent->export (call->parent, parent_exported, call);
      return;
    }

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  call->request_path = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        call->request_path,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        session_started,
                                                        call,
                                                        NULL);

  cancellable = g_task_get_cancellable (call->task);
  if (cancellable)
    call->cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (start_cancelled_cb), call);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          call->session->type == XDP_SESSION_REMOTE_DESKTOP
                             ? "org.freedesktop.portal.RemoteDesktop"
                             : "org.freedesktop.portal.ScreenCast",
                          "Start",
                          g_variant_new ("(osa{sv})",
                                         call->session->id,
                                         call->parent_handle,
                                         &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          NULL,
                          NULL);
}

/**
 * xdp_session_start:
 * @session: a #XdpSession in initial state
 * @parent: (nullable): parent window information
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Starts the session.
 * 
 * When the request is done, @callback will be called. You can then
 * call xdp_session_start_finish() to get the results.
 */ 
void
xdp_session_start (XdpSession *session,
                   XdpParent *parent,
                   GCancellable *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer data)
{
  StartCall *call = NULL;

  g_return_if_fail (XDP_IS_SESSION (session));

  call = g_new0 (StartCall, 1);
  call->portal = g_object_ref (session->portal);
  call->session = g_object_ref (session);
  if (parent)
    call->parent = _xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->task = g_task_new (session, cancellable, callback, data);

  start_session (call);
}

/**
 * xdp_session_start_finish:
 * @session: a #XdpSession
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes the session-start request.
 *
 * Returns: %TRUE if the session was started successfully.
 */
gboolean
xdp_session_start_finish (XdpSession *session,
                          GAsyncResult *result,
                          GError **error)
{
  g_return_val_if_fail (XDP_IS_SESSION (session), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, session), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * xdp_session_close:
 * @session: an active #XdpSession
 *
 * Closes the session.
 */
void
xdp_session_close (XdpSession *session)
{
  g_return_if_fail (XDP_IS_SESSION (session));

  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          session->id,
                          SESSION_INTERFACE,
                          "Close",
                          NULL,
                          NULL, 0, -1, NULL, NULL, NULL);

  _xdp_session_set_session_state (session, XDP_SESSION_CLOSED);
  g_signal_emit_by_name (session, "closed");
}

/**
 * xdp_session_open_pipewire_remote:
 * @session: a #XdpSession
 *
 * Opens a file descriptor to the pipewire remote where the screencast
 * streams are available. The file descriptor should be used to create
 * a pw_remote object, by using pw_remote_connect_fd(). Only the
 * screencast stream nodes will be available from this pipewire node.
 *
 * Returns: the file ddescriptor
 */
int
xdp_session_open_pipewire_remote (XdpSession *session)
{
  GVariantBuilder options;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  int fd_out;

  g_return_val_if_fail (XDP_IS_SESSION (session), -1);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  ret = g_dbus_connection_call_with_unix_fd_list_sync (session->portal->bus,
                                                       PORTAL_BUS_NAME,
                                                       PORTAL_OBJECT_PATH,
                                                       "org.freedesktop.portal.ScreenCast",
                                                       "OpenPipeWireRemote",
                                                       g_variant_new ("(oa{sv})", session->id, &options),
                                                       G_VARIANT_TYPE ("(h)"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       -1,
                                                       NULL,
                                                       &fd_list,
                                                       NULL,
                                                       &error);
  if (ret == NULL)
    {
      g_warning ("Failed to get pipewire fd: %s", error->message);
      return -1;
    }

  g_variant_get (ret, "(h)", &fd_out);

  return g_unix_fd_list_get (fd_list, fd_out, NULL);
}

/**
 * xdp_session_pointer_motion:
 * @session: a #XdpSession
 * @dx: relative horizontal movement
 * @dy: relative vertical movement
 *
 * Moves the pointer from its current position.
 * 
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_POINTER access.
 */
void
xdp_session_pointer_motion (XdpSession *session,
                            double dx,
                            double dy)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_POINTER) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyPointerMotion",
                          g_variant_new ("(oa{sv}dd)", session->id, &options, dx, dy),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_pointer_position:
 * @session: a #XdpSession
 * @stream: the node ID of the pipewire stream the position is relative to
 * @x: new X position
 * @y: new Y position
 *
 * Moves the pointer to a new position in the given streams logical
 * coordinate space.
 * 
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_POINTER access.
 */
void
xdp_session_pointer_position (XdpSession *session,
                              guint stream,
                              double x,
                              double y)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_POINTER) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyPointerMotionAbsolute",
                          g_variant_new ("(oa{sv}udd)", session->id, &options, stream, x, y),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_pointer_button:
 * @session: a #XdpSession
 * @button: the button
 * @state: the new state
 *
 * Changes the state of the button to @state.
 * 
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_POINTER access.
 */
void
xdp_session_pointer_button (XdpSession *session,
                            int button,
                            XdpButtonState state)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_POINTER) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyPointerButton",
                          g_variant_new ("(oa{sv}iu)", session->id, &options, button, state),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_pointer_axis:
 * @session: a #XdpSession
 * @finish: whether this is the last in a series of related events
 * @dx: relative axis movement on the X axis
 * @dy: relative axis movement on the Y axis
 *
 * The axis movement from a smooth scroll device, such as a touchpad.
 * When applicable, the size of the motion delta should be equivalent to
 * the motion vector of a pointer motion done using the same advice.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_POINTER access.
 */
void
xdp_session_pointer_axis (XdpSession *session,
                          gboolean finish,
                          double dx,
                          double dy)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_POINTER) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "finish", g_variant_new_boolean (finish));
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyPointerAxis",
                          g_variant_new ("(oa{sv}dd)", session->id, &options, dx, dy),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_pointer_axis_discrete:
 * @session: a #XdpSession
 * @axis: the axis to change
 * @steps: number of steps scrolled
 *
 * The axis movement from a discrete scroll device.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_POINTER access.
 */
void
xdp_session_pointer_axis_discrete (XdpSession *session,
                                   XdpDiscreteAxis axis,
                                   int steps)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_POINTER) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyPointerAxisDiscrete",
                          g_variant_new ("(oa{sv}ui)", session->id, &options, axis, steps),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_keyboard_key:
 * @session: a remote desktop #XdpSession
 * @keysym: whether to interpret @key as a keysym instead of a keycode
 * @key: the keysym or keycode to change
 * @state: the new state
 * 
 * Changes the state of the key to @state.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_KEYBOARD access.
 */
void
xdp_session_keyboard_key (XdpSession *session,
                          gboolean keysym,
                          int key,
                          XdpKeyState state)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_KEYBOARD) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          keysym ? "NotifyKeyboardKeysym" : "NotifyKeyboardKeycode",
                          g_variant_new ("(oa{sv}iu)", session->id, &options, key, state),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_touch_down:
 * @session: a #XdpSession
 * @stream: the node ID of the pipewire stream the position is relative to
 * @slot: touch slot where the touch point appeared
 * @x: new X position
 * @y: new Y position
 *
 * Notify about a new touch down event. The (x, y) position
 * represents the new touch point position in the streams logical
 * coordinate space.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_TOUCHSCREEN access.
 */
void
xdp_session_touch_down (XdpSession *session,
                        guint stream,
                        guint slot,
                        double x,
                        double y)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_TOUCHSCREEN) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyTouchDown",
                          g_variant_new ("(oa{sv}uudd)", session->id, &options, stream, slot, x, y),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_touch_position:
 * @session: a #XdpSession
 * @stream: the node ID of the pipewire stream the position is relative to
 * @slot: touch slot that is changing position
 * @x: new X position
 * @y: new Y position
 *
 * Notify about a new touch motion event. The (x, y) position
 * represents where the touch point position in the streams logical
 * coordinate space moved.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_TOUCHSCREEN access.
 */
void
xdp_session_touch_position (XdpSession *session,
                            guint stream,
                            guint slot,
                            double x,
                            double y)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_TOUCHSCREEN) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyTouchMotion",
                          g_variant_new ("(oa{sv}uudd)", session->id, &options, stream, slot, x, y),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/**
 * xdp_session_touch_up:
 * @session: a #XdpSession
 * @slot: touch slot that changed
 *
 * Notify about a new touch up event.
 *
 * May only be called on a remote desktop session
 * with %XDP_DEVICE_TOUCHSCREEN access.
 */
void
xdp_session_touch_up (XdpSession *session,
                      guint slot)
{
  GVariantBuilder options;

  g_return_if_fail (XDP_IS_SESSION (session) &&
                    session->type == XDP_SESSION_REMOTE_DESKTOP &&
                    session->state == XDP_SESSION_ACTIVE &&
                    ((session->devices & XDP_DEVICE_TOUCHSCREEN) != 0));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call (session->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.RemoteDesktop",
                          "NotifyTouchMotion",
                          g_variant_new ("(oa{sv}u)", session->id, &options, slot),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

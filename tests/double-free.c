/* This file is part of GTK+ Sapwood Engine
 *
 * AUTHORS
 *     Sven Herzberg  <sven@imendio.com>
 *
 * Copyright (C) 2008  Nokia Corporation
 *
 * This work is provided "as is"; redistribution and modification
 * in whole or in part, in any medium, physical or electronic is
 * permitted without restriction.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * In no event shall the authors or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 */

#include <config.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gtk/gtktestutils.h>
#include "sapwood-pixmap.h"
#include "sapwood-proto.h"

#undef SAPWOOD_PIXMAP_ERROR
#define SAPWOOD_PIXMAP_ERROR 0
#define SAPWOOD_PIXMAP_ERROR_FAILED 0
#define SAPWOOD_SERVER "sapwood-server"

struct _SapwoodPixmap {
  guint32    id;
  gint       width;
  gint       height;
  GdkPixmap *pixmap[3][3];
  GdkBitmap *pixmask[3][3];
};

/* FIXME: refactor this out into a libproto-client.la library for the engine
 * and the test cases */
static int
pixbuf_proto_get_socket (GError **err)
{
  struct sockaddr_un  sun;
  const char         *sock_path;
  int                 fd;

  fd = socket (PF_LOCAL, SOCK_STREAM, 0);
  if (fd < 0)
    {
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "socket: %s", strerror (errno));
      return -1;
    }

  sock_path = sapwood_socket_path_get_default ();

  memset (&sun, '\0', sizeof(sun));
  sun.sun_family = AF_LOCAL;
#ifdef HAVE_ABSTRACT_SOCKETS
  strcpy (&sun.sun_path[1], sock_path);
#else
  strcpy (&sun.sun_path[0], sock_path);
#endif
  if (connect (fd, (struct sockaddr *)&sun, sizeof (sun)) < 0)
    {
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "Failed to connect to sapwood server using `%s': %s\n\n"
		   "\t`%s' MUST be started before applications",
		   sock_path, strerror (errno),
		   SAPWOOD_SERVER);
      close (fd);
      return -1;
    }

  return fd;
}

static gboolean
pixbuf_proto_request (const char *req,
                      ssize_t     reqlen,
                      char       *rep,
                      ssize_t     replen,
                      GError    **err)
{
  static int fd = -1;
  ssize_t    n;

  if (fd == -1)
    {
      fd = pixbuf_proto_get_socket (err);
      if (fd == -1)
	return FALSE;
    }

  n = write (fd, req, reqlen);
  if (n < 0)
    {
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "write: %s", g_strerror (errno));
      return FALSE;
    }
  else if (n != reqlen)
    {
      /* FIXME */
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "wrote %d of %d bytes", n, reqlen);
      return FALSE;
    }

  if (!rep)
    return TRUE;

  n = read (fd, rep, replen);
  if (n < 0)
    {
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "read: %s", g_strerror (errno));
      return FALSE;
    }
  else if (n != replen)
    {
      /* FIXME */
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "read %d, expected %d bytes", n, replen);
      return FALSE;
    }
  else if (n != replen)
    {
      /* FIXME */
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "read %d, expected %d bytes", n, replen);
      return FALSE;
    }

  return TRUE;
}

SapwoodPixmap *
sapwood_pixmap_get_for_file (const char *filename,
                             int         border_left,
                             int         border_right,
                             int         border_top,
                             int         border_bottom,
                             GError    **err)
{
  SapwoodPixmap     *self;
  char               buf[ sizeof(PixbufOpenRequest) + PATH_MAX + 1 ] = {0};
  PixbufOpenRequest *req = (PixbufOpenRequest *) buf;
  PixbufOpenResponse rep;
  int                flen;

  /* marshal request */
  flen = g_strlcpy (req->filename, filename, PATH_MAX);
  if (flen > PATH_MAX)
    {
      g_set_error (err, SAPWOOD_PIXMAP_ERROR, SAPWOOD_PIXMAP_ERROR_FAILED,
		   "%s: filename too long", filename);
      return NULL;
    }

  req->base.op       = PIXBUF_OP_OPEN;
  req->base.length   = sizeof(*req) + flen + 1;
  req->border_left   = border_left;
  req->border_right  = border_right;
  req->border_top    = border_top;
  req->border_bottom = border_bottom;

  if (!pixbuf_proto_request ((char*)req,  req->base.length,
			     (char*)&rep, sizeof(rep), err))
    return NULL;

  /* unmarshal response */
  self = g_new0 (SapwoodPixmap, 1);
  self->id     = rep.id;
  self->width  = rep.width;
  self->height = rep.height;

  return self;
}

static void
pixbuf_proto_unref_pixmap (guint32 id)
{
  PixbufCloseRequest  req;
  GError             *err = NULL;

  req.base.op     = PIXBUF_OP_CLOSE;
  req.base.length = sizeof(PixbufCloseRequest);
  req.id          = id;
  if (!pixbuf_proto_request ((char*)&req, req.base.length, NULL, 0, &err))
    {
      g_warning ("close(0x%x): %s", id, err->message);
      g_error_free (err);
      return;
    }
}

static gboolean
false_func (void)
{
  GFile* top_srcdir = g_file_new_for_commandline_arg (g_getenv ("top_srcdir"));
  GFile* image = g_file_resolve_relative_path (top_srcdir, "demos/images/gradient.png");
  SapwoodPixmap* pixmap;
  gchar* path;
  GError* error = NULL;

  path = g_file_get_path (image);
  pixmap = sapwood_pixmap_get_for_file (path, 0, 0, 0, 0, &error);
  g_free (path);

  if (!pixmap)
    {
      g_warning ("Error creating pixmap: %s",
                 error->message);
      g_clear_error (&error);
    }
  else
    {
      pixbuf_proto_unref_pixmap (pixmap->id);
      g_free (pixmap);
    }

  g_object_unref (image);
  g_object_unref (top_srcdir);
  return FALSE;
}

int
main (int   argc,
      char**argv)
{
  GMainLoop* loop;
  GError* error = NULL;
  int fd;

  g_type_init ();

  /* gtester disables this if it doesn't find xvfb */
  g_setenv ("DISPLAY", ":0.0", TRUE);

  gdk_init (&argc, &argv);

  fd = pixbuf_proto_get_socket (&error);
  if (fd == -1)
    {
      g_warning ("%s", error->message);
      g_clear_error (&error);
      return 1;
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add_full (G_PRIORITY_LOW, 100,
                      (GSourceFunc)false_func, loop,
                      (GDestroyNotify)g_main_loop_quit);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  close (fd);
  return 0;
}

/*
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Video Redirection Virtual Channel - GStreamer Decoder X11 specifics
 *
 * (C) Copyright 2014 Thincast Technologies GmbH
 * (C) Copyright 2014 Armin Novak <armin.novak@thincast.com>
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

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <winpr/thread.h>
#include <winpr/string.h>

#include <gst/gst.h>
#if GST_VERSION_MAJOR > 0
#include <gst/video/videooverlay.h>
#else
#include <gst/interfaces/xoverlay.h>
#endif

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <freerdp/channels/tsmf.h>

#include "tsmf_platform.h"
#include "tsmf_constants.h"
#include "tsmf_decoder.h"

#if !defined(WITH_XEXT)
#warning "Building TSMF without shape extension support"
#endif

struct X11Handle
{
	int shmid;
	int *xfwin;
#if defined(WITH_XEXT)
	BOOL has_shape;
#endif
	Display *disp;
	Window subwin;
	BOOL subwinMapped;
	GstXOverlay *overlay;
	int subwinWidth;
	int subwinHeight;
	int subwinX;
	int subwinY;
};

static const char* get_shm_id()
{
	static char shm_id[128];
	sprintf_s(shm_id, sizeof(shm_id), "/com.freerdp.xfreerdp.tsmf_%016X", GetCurrentProcessId());
	return shm_id;
}

static GstBusSyncReply tsmf_platform_bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data)
{
	struct X11Handle* hdl;

	TSMFGstreamerDecoder* decoder = user_data;

	if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
		return GST_BUS_PASS;

	if (!gst_structure_has_name (message->structure, "prepare-xwindow-id"))
		return GST_BUS_PASS;

	hdl = (struct X11Handle*) decoder->platform;

	if (hdl->subwin)
	{
		hdl->overlay = GST_X_OVERLAY (GST_MESSAGE_SRC (message));

#if GST_VERSION_MAJOR > 0
		gst_video_overlay_set_window_handle(hdl->overlay, hdl->subwin);
#else
		gst_x_overlay_set_window_handle(hdl->overlay, hdl->subwin);
#endif
#if GST_VERSION_MAJOR > 0
		gst_video_overlay_handle_events(hdl->overlay, TRUE);
#else
		gst_x_overlay_handle_events(hdl->overlay, TRUE);
#endif

		if (hdl->subwinWidth != -1 && hdl->subwinHeight != -1 && hdl->subwinX != -1 && hdl->subwinY != -1)
		{
#if GST_VERSION_MAJOR > 0

			if (!gst_video_overlay_set_render_rectangle(hdl->overlay, 0, 0, hdl->swubwinWidth, hdl->subwinHeight))
			{
				WLog_ERR(TAG, "Could not resize overlay!");
			}

			gst_video_overlay_expose(hdl->overlay);
#else
			if (!gst_x_overlay_set_render_rectangle(hdl->overlay, 0, 0, hdl->subwinWidth, hdl->subwinHeight))
			{
				WLog_ERR(TAG, "Could not resize overlay!");
			}

			gst_x_overlay_expose(hdl->overlay);
#endif
			XMoveResizeWindow(hdl->disp, hdl->subwin, hdl->subwinX, hdl->subwinY, hdl->subwinWidth, hdl->subwinHeight);
			XSync(hdl->disp, FALSE);
		}
	} else {
		g_warning ("Window was not available before retrieving the overlay!");
	}

	gst_message_unref (message);

	return GST_BUS_DROP;
}

const char* tsmf_platform_get_video_sink(void)
{
	return "autovideosink";
}

const char* tsmf_platform_get_audio_sink(void)
{
	//return "alsasink";
	return "autoaudiosink";
}

int tsmf_platform_create(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl;

	if (!decoder)
		return -1;

	if (decoder->platform)
		return -1;

	hdl = calloc(1, sizeof(struct X11Handle));
	if (!hdl)
	{
		WLog_ERR(TAG, "Could not allocate handle.");
		return -1;
	}

	decoder->platform = hdl;
	hdl->shmid = shm_open(get_shm_id(), (O_RDWR | O_CREAT), (PROT_READ | PROT_WRITE));
	if (hdl->shmid == -1)
	{
		WLog_ERR(TAG, "failed to get access to shared memory - shmget(%s): %i - %s", get_shm_id(), errno, strerror(errno));
		return -2;
	}

	hdl->xfwin = mmap(0, sizeof(void *), PROT_READ | PROT_WRITE, MAP_SHARED, hdl->shmid, 0);
	if (hdl->xfwin == MAP_FAILED)
	{
		WLog_ERR(TAG, "shmat failed!");
		return -3;
	}

	hdl->disp = XOpenDisplay(NULL);
	if (!hdl->disp)
	{
		WLog_ERR(TAG, "Failed to open display");
		return -4;
	}

	hdl->subwinMapped = FALSE;
	hdl->subwinX = -1;
	hdl->subwinY = -1;
	hdl->subwinWidth = -1;
	hdl->subwinHeight = -1;

	return 0;
}

int tsmf_platform_set_format(TSMFGstreamerDecoder* decoder)
{
	if (!decoder)
		return -1;

	if (decoder->media_type == TSMF_MAJOR_TYPE_VIDEO)
	{

	}

	return 0;
}

int tsmf_platform_register_handler(TSMFGstreamerDecoder* decoder)
{
	GstBus* bus;

	if (!decoder)
		return -1;

	if (!decoder->pipe)
		return -1;

	bus = gst_pipeline_get_bus(GST_PIPELINE(decoder->pipe));

	gst_bus_set_sync_handler (bus, (GstBusSyncHandler) tsmf_platform_bus_sync_handler, decoder);

	if (!bus)
	{
		WLog_ERR(TAG, "gst_pipeline_get_bus failed!");
		return 1;
	}

	gst_object_unref (bus);

	return 0;
}

int tsmf_platform_free(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl = decoder->platform;

	if (!hdl)
		return -1;

	if (hdl->disp)
		XCloseDisplay(hdl->disp);

	if (hdl->xfwin)
		munmap(0, sizeof(void*));

	if (hdl->shmid >= 0)
		close(hdl->shmid);

	free(hdl);
	decoder->platform = NULL;

	return 0;
}

int tsmf_window_create(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl;

	if (decoder->media_type != TSMF_MAJOR_TYPE_VIDEO)
	{
		decoder->ready = TRUE;
		return -3;
	}
	else
	{
		if (!decoder)
			return -1;

		if (!decoder->platform)
			return -1;

		hdl = (struct X11Handle*) decoder->platform;

		if (!hdl->subwin)
		{
			hdl->subwin = XCreateSimpleWindow(hdl->disp, *(int *)hdl->xfwin, 0, 0, 1, 1, 0, 0, 0);

			if (!hdl->subwin)
			{
				WLog_ERR(TAG, "Could not create subwindow!");
			}
		}

		tsmf_window_map(decoder);

		decoder->ready = TRUE;
#if defined(WITH_XEXT)
	int event, error;
	hdl->has_shape = XShapeQueryExtension(hdl->disp, &event, &error);
#endif
	}

	return 0;
}

int tsmf_window_resize(TSMFGstreamerDecoder* decoder, int x, int y, int width,
				   int height, int nr_rects, RDP_RECT *rects)
{
	struct X11Handle* hdl;

	if (decoder->media_type != TSMF_MAJOR_TYPE_VIDEO)
	{
		return -3;
	}

#if GST_VERSION_MAJOR > 0
	GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(decoder->outsink);
#else
	GstXOverlay *overlay = GST_X_OVERLAY(decoder->outsink);
#endif
	if (!decoder)
		return -1;

	if (!decoder->platform)
		return -1;

	hdl = (struct X11Handle*) decoder->platform;
	DEBUG_TSMF("resize: x=%d, y=%d, w=%d, h=%d", x, y, width, height);

	if (hdl->overlay)
	{
#if GST_VERSION_MAJOR > 0

		if (!gst_video_overlay_set_render_rectangle(overlay, 0, 0, width, height))
		{
			WLog_ERR(TAG, "Could not resize overlay!");
		}

		gst_video_overlay_expose(overlay);
#else
		if (!gst_x_overlay_set_render_rectangle(overlay, 0, 0, width, height))
		{
			WLog_ERR(TAG, "Could not resize overlay!");
		}

		gst_x_overlay_expose(overlay);
#endif
	}

	if (hdl->subwin)
	{
		hdl->subwinX = x;
		hdl->subwinY = y;
		hdl->subwinWidth = width;
		hdl->subwinHeight = height;
		XMoveResizeWindow(hdl->disp, hdl->subwin, hdl->subwinX, hdl->subwinY, hdl->subwinWidth, hdl->subwinHeight);
#if defined(WITH_XEXT)

		if (hdl->has_shape)
		{
			int i;
			XRectangle *xrects = calloc(nr_rects, sizeof(XRectangle));
			if (!xrects)
				return -1;

			for (i = 0; i < nr_rects; i++)
			{
				xrects[i].x = rects[i].x - x;
				xrects[i].y = rects[i].y - y;
				xrects[i].width = rects[i].width;
				xrects[i].height = rects[i].height;
			}

			XShapeCombineRectangles(hdl->disp, hdl->subwin, ShapeBounding, x, y, xrects, nr_rects, ShapeSet, 0);
			free(xrects);
		}
#endif
		XSync(hdl->disp, FALSE);
	}

	return 0;
}

int tsmf_window_pause(TSMFGstreamerDecoder* decoder)
{
	if (!decoder)
		return -1;

	return 0;
}

int tsmf_window_resume(TSMFGstreamerDecoder* decoder)
{
	if (!decoder)
		return -1;

	return 0;
}

int tsmf_window_map(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl;
	if (!decoder)
		return -1;

	hdl = (struct X11Handle*) decoder->platform;

	// Only need to map the window if it is not currently mapped
	if ((hdl->subwin) && (!hdl->subwinMapped))
	{
		XLockDisplay(hdl->disp);
		XMapWindow(hdl->disp, hdl->subwin);
		hdl->subwinMapped = TRUE;
		XSync(hdl->disp, FALSE);
		XUnlockDisplay(hdl->disp);
	}

	return 0;
}

int tsmf_window_unmap(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl;
	if (!decoder)
		return -1;

	hdl = (struct X11Handle*) decoder->platform;

	// only need to unmap window if it is currently mapped
	if ((hdl->subwin) && (hdl->subwinMapped))
	{
		XLockDisplay(hdl->disp);
		XUnmapWindow(hdl->disp, hdl->subwin);
		hdl->subwinMapped = FALSE; 
		XSync(hdl->disp, FALSE); 
		XUnlockDisplay(hdl->disp);
	}

	return 0;
}


int tsmf_window_destroy(TSMFGstreamerDecoder* decoder)
{
	struct X11Handle* hdl;
	decoder->ready = FALSE;

	if (decoder->media_type != TSMF_MAJOR_TYPE_VIDEO)
		return -3;

	if (!decoder)
		return -1;

	if (!decoder->platform)
		return -1;

	hdl = (struct X11Handle*) decoder->platform;

	if (hdl->subwin)
	{
		XDestroyWindow(hdl->disp, hdl->subwin);
		XSync(hdl->disp, FALSE);
	}

	hdl->overlay = NULL;
	hdl->subwin = 0;
	hdl->subwinMapped = FALSE;
	hdl->subwinX = -1;
	hdl->subwinY = -1;
	hdl->subwinWidth = -1;
	hdl->subwinHeight = -1;
	return 0;
}


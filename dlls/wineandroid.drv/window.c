/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001, 2013-2017 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "android.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

/* private window data */
struct android_win_data
{
    HWND           hwnd;           /* hwnd that this private data belongs to */
    HWND           parent;         /* parent hwnd for child windows */
    RECT           window_rect;    /* USER window rectangle relative to parent */
    RECT           whole_rect;     /* X window rectangle for the whole window relative to parent */
    RECT           client_rect;    /* client area relative to parent */
    ANativeWindow *window;         /* native window wrapper that forwards calls to the desktop process */
    struct window_surface *surface;
};

#define SWP_AGG_NOPOSCHANGE (SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOZORDER)

static CRITICAL_SECTION win_data_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &win_data_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": win_data_section") }
};
static CRITICAL_SECTION win_data_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static struct android_win_data *win_data_context[32768];

static inline int context_idx( HWND hwnd )
{
    return LOWORD( hwnd ) >> 1;
}

static void set_surface_region( struct window_surface *window_surface, HRGN win_region );

/* only for use on sanitized BITMAPINFO structures */
static inline int get_dib_info_size( const BITMAPINFO *info, UINT coloruse )
{
    if (info->bmiHeader.biCompression == BI_BITFIELDS)
        return sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD);
    if (coloruse == DIB_PAL_COLORS)
        return sizeof(BITMAPINFOHEADER) + info->bmiHeader.biClrUsed * sizeof(WORD);
    return FIELD_OFFSET( BITMAPINFO, bmiColors[info->bmiHeader.biClrUsed] );
}

static inline int get_dib_stride( int width, int bpp )
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size( const BITMAPINFO *info )
{
    return get_dib_stride( info->bmiHeader.biWidth, info->bmiHeader.biBitCount )
        * abs( info->bmiHeader.biHeight );
}


/***********************************************************************
 *           alloc_win_data
 */
static struct android_win_data *alloc_win_data( HWND hwnd )
{
    struct android_win_data *data;

    if ((data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data))))
    {
        data->hwnd = hwnd;
        data->window = create_ioctl_window( hwnd );
        EnterCriticalSection( &win_data_section );
        win_data_context[context_idx(hwnd)] = data;
    }
    return data;
}


/***********************************************************************
 *           free_win_data
 */
static void free_win_data( struct android_win_data *data )
{
    win_data_context[context_idx( data->hwnd )] = NULL;
    LeaveCriticalSection( &win_data_section );
    if (data->window) release_ioctl_window( data->window );
    HeapFree( GetProcessHeap(), 0, data );
}


/***********************************************************************
 *           get_win_data
 *
 * Lock and return the data structure associated with a window.
 */
static struct android_win_data *get_win_data( HWND hwnd )
{
    struct android_win_data *data;

    if (!hwnd) return NULL;
    EnterCriticalSection( &win_data_section );
    if ((data = win_data_context[context_idx(hwnd)]) && data->hwnd == hwnd) return data;
    LeaveCriticalSection( &win_data_section );
    return NULL;
}


/***********************************************************************
 *           release_win_data
 *
 * Release the data returned by get_win_data.
 */
static void release_win_data( struct android_win_data *data )
{
    if (data) LeaveCriticalSection( &win_data_section );
}


/***********************************************************************
 *           get_ioctl_window
 */
static struct ANativeWindow *get_ioctl_window( HWND hwnd )
{
    struct ANativeWindow *ret;
    struct android_win_data *data = get_win_data( hwnd );

    if (!data || !data->window) return NULL;
    ret = grab_ioctl_window( data->window );
    release_win_data( data );
    return ret;
}


/* Handling of events coming from the Java side */

struct java_event
{
    struct list      entry;
    union event_data data;
};

static struct list event_queue = LIST_INIT( event_queue );
static struct java_event *current_event;
static int event_pipe[2];
static DWORD desktop_tid;

/***********************************************************************
 *           send_event
 */
int send_event( const union event_data *data )
{
    int res;

    if ((res = write( event_pipe[1], data, sizeof(*data) )) != sizeof(*data))
    {
        p__android_log_print( ANDROID_LOG_ERROR, "wine", "failed to send event" );
        return -1;
    }
    return 0;
}


/***********************************************************************
 *           desktop_changed
 *
 * JNI callback, runs in the context of the Java thread.
 */
void desktop_changed( JNIEnv *env, jobject obj, jint width, jint height )
{
    union event_data data;

    memset( &data, 0, sizeof(data) );
    data.type = DESKTOP_CHANGED;
    data.desktop.width = width;
    data.desktop.height = height;
    p__android_log_print( ANDROID_LOG_INFO, "wine", "desktop_changed: %ux%u", width, height );
    send_event( &data );
}


/***********************************************************************
 *           surface_changed
 *
 * JNI callback, runs in the context of the Java thread.
 */
void surface_changed( JNIEnv *env, jobject obj, jint win, jobject surface )
{
    union event_data data;

    memset( &data, 0, sizeof(data) );
    data.surface.hwnd = LongToHandle( win );
    if (surface)
    {
        int width, height;
        ANativeWindow *win = pANativeWindow_fromSurface( env, surface );

        if (win->query( win, NATIVE_WINDOW_WIDTH, &width ) < 0) width = 0;
        if (win->query( win, NATIVE_WINDOW_HEIGHT, &height ) < 0) height = 0;
        data.surface.window = win;
        data.surface.width = width;
        data.surface.height = height;
        p__android_log_print( ANDROID_LOG_INFO, "wine", "surface_changed: %p %ux%u",
                              data.surface.hwnd, width, height );
    }
    data.type = SURFACE_CHANGED;
    send_event( &data );
}


/***********************************************************************
 *           init_event_queue
 */
static void init_event_queue(void)
{
    HANDLE handle;
    int ret;

    if (pipe2( event_pipe, O_CLOEXEC | O_NONBLOCK ) == -1)
    {
        ERR( "could not create data\n" );
        ExitProcess(1);
    }
    if (wine_server_fd_to_handle( event_pipe[0], GENERIC_READ | SYNCHRONIZE, 0, &handle ))
    {
        ERR( "Can't allocate handle for event fd\n" );
        ExitProcess(1);
    }
    SERVER_START_REQ( set_queue_fd )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (ret)
    {
        ERR( "Can't store handle for event fd %x\n", ret );
        ExitProcess(1);
    }
    CloseHandle( handle );
    desktop_tid = GetCurrentThreadId();
}


/***********************************************************************
 *           pull_events
 *
 * Pull events from the event pipe and add them to the queue
 */
static void pull_events(void)
{
    struct java_event *event;
    int res;

    for (;;)
    {
        if (!(event = HeapAlloc( GetProcessHeap(), 0, sizeof(*event) ))) break;

        res = read( event_pipe[0], &event->data, sizeof(event->data) );
        if (res != sizeof(event->data)) break;
        list_add_tail( &event_queue, &event->entry );
    }
    HeapFree( GetProcessHeap(), 0, event );
}


/***********************************************************************
 *           process_events
 */
static int process_events( DWORD mask )
{
    struct java_event *event, *next, *previous;
    unsigned int count = 0;

    assert( GetCurrentThreadId() == desktop_tid );

    pull_events();

    previous = current_event;

    LIST_FOR_EACH_ENTRY_SAFE( event, next, &event_queue, struct java_event, entry )
    {
        switch (event->data.type)
        {
        case SURFACE_CHANGED:
            break;  /* always process it to unblock other threads */
        default:
            if (mask & QS_SENDMESSAGE) break;
            continue;  /* skip it */
        }

        /* remove it first, in case we process events recursively */
        list_remove( &event->entry );
        current_event = event;

        switch (event->data.type)
        {
        case DESKTOP_CHANGED:
            TRACE( "DESKTOP_CHANGED %ux%u\n", event->data.desktop.width, event->data.desktop.height );
            screen_width = event->data.desktop.width;
            screen_height = event->data.desktop.height;
            init_monitors( screen_width, screen_height );
            SetWindowPos( GetDesktopWindow(), 0, 0, 0, screen_width, screen_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW );
            break;

        case SURFACE_CHANGED:
            TRACE("SURFACE_CHANGED %p %p size %ux%u\n", event->data.surface.hwnd,
                  event->data.surface.window, event->data.surface.width, event->data.surface.height );

            register_native_window( event->data.surface.hwnd, event->data.surface.window );
            break;

        default:
            FIXME( "got event %u\n", event->data.type );
        }
        HeapFree( GetProcessHeap(), 0, event );
        count++;
    }
    current_event = previous;
    return count;
}


/***********************************************************************
 *           wait_events
 */
static int wait_events( int timeout )
{
    assert( GetCurrentThreadId() == desktop_tid );

    for (;;)
    {
        struct pollfd pollfd;
        int ret;

        pollfd.fd = event_pipe[0];
        pollfd.events = POLLIN | POLLHUP;
        ret = poll( &pollfd, 1, timeout );
        if (ret == -1 && errno == EINTR) continue;
        if (ret && (pollfd.revents & (POLLHUP | POLLERR))) ret = -1;
        return ret;
    }
}


/* Window surface support */

struct android_window_surface
{
    struct window_surface header;
    HWND                  hwnd;
    ANativeWindow        *window;
    RECT                  bounds;
    BOOL                  byteswap;
    RGNDATA              *region_data;
    HRGN                  region;
    BYTE                  alpha;
    COLORREF              color_key;
    void                 *bits;
    CRITICAL_SECTION      crit;
    BITMAPINFO            info;   /* variable size, must be last */
};

static struct android_window_surface *get_android_surface( struct window_surface *surface )
{
    return (struct android_window_surface *)surface;
}

static inline void reset_bounds( RECT *bounds )
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

static inline void add_bounds_rect( RECT *bounds, const RECT *rect )
{
    if (rect->left >= rect->right || rect->top >= rect->bottom) return;
    bounds->left   = min( bounds->left, rect->left );
    bounds->top    = min( bounds->top, rect->top );
    bounds->right  = max( bounds->right, rect->right );
    bounds->bottom = max( bounds->bottom, rect->bottom );
}

/* store the palette or color mask data in the bitmap info structure */
static void set_color_info( BITMAPINFO *info, BOOL has_alpha )
{
    DWORD *colors = (DWORD *)info->bmiColors;

    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biBitCount = 32;
    if (has_alpha)
    {
        info->bmiHeader.biCompression = BI_RGB;
        return;
    }
    info->bmiHeader.biCompression = BI_BITFIELDS;
    colors[0] = 0xff0000;
    colors[1] = 0x00ff00;
    colors[2] = 0x0000ff;
}

/* apply the window region to a single line of the destination image. */
static void apply_line_region( DWORD *dst, int width, int x, int y, const RECT *rect, const RECT *end )
{
    while (rect < end && rect->top <= y && width > 0)
    {
        if (rect->left > x)
        {
            memset( dst, 0, min( rect->left - x, width ) * sizeof(*dst) );
            dst += rect->left - x;
            width -= rect->left - x;
            x = rect->left;
        }
        if (rect->right > x)
        {
            dst += rect->right - x;
            width -= rect->right - x;
            x = rect->right;
        }
        rect++;
    }
    if (width > 0) memset( dst, 0, width * sizeof(*dst) );
}

/***********************************************************************
 *           android_surface_lock
 */
static void android_surface_lock( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    EnterCriticalSection( &surface->crit );
}

/***********************************************************************
 *           android_surface_unlock
 */
static void android_surface_unlock( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    LeaveCriticalSection( &surface->crit );
}

/***********************************************************************
 *           android_surface_get_bitmap_info
 */
static void *android_surface_get_bitmap_info( struct window_surface *window_surface, BITMAPINFO *info )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    memcpy( info, &surface->info, get_dib_info_size( &surface->info, DIB_RGB_COLORS ));
    return surface->bits;
}

/***********************************************************************
 *           android_surface_get_bounds
 */
static RECT *android_surface_get_bounds( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    return &surface->bounds;
}

/***********************************************************************
 *           android_surface_set_region
 */
static void android_surface_set_region( struct window_surface *window_surface, HRGN region )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    TRACE( "updating surface %p hwnd %p with %p\n", surface, surface->hwnd, region );

    window_surface->funcs->lock( window_surface );
    if (!region)
    {
        if (surface->region) DeleteObject( surface->region );
        surface->region = 0;
    }
    else
    {
        if (!surface->region) surface->region = CreateRectRgn( 0, 0, 0, 0 );
        CombineRgn( surface->region, region, 0, RGN_COPY );
    }
    window_surface->funcs->unlock( window_surface );
    set_surface_region( &surface->header, (HRGN)1 );
}

/***********************************************************************
 *           android_surface_flush
 */
static void android_surface_flush( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    ANativeWindow_Buffer buffer;
    ARect rc;
    RECT rect;
    BOOL needs_flush;

    window_surface->funcs->lock( window_surface );
    SetRect( &rect, 0, 0, surface->header.rect.right - surface->header.rect.left,
             surface->header.rect.bottom - surface->header.rect.top );
    needs_flush = IntersectRect( &rect, &rect, &surface->bounds );
    reset_bounds( &surface->bounds );
    window_surface->funcs->unlock( window_surface );
    if (!needs_flush) return;

    TRACE( "flushing %p hwnd %p surface %s rect %s bits %p alpha %02x key %08x region %u rects\n",
           surface, surface->hwnd, wine_dbgstr_rect( &surface->header.rect ),
           wine_dbgstr_rect( &rect ), surface->bits, surface->alpha, surface->color_key,
           surface->region_data ? surface->region_data->rdh.nCount : 0 );

    rc.left   = rect.left;
    rc.top    = rect.top;
    rc.right  = rect.right;
    rc.bottom = rect.bottom;

    if (!surface->window->perform( surface->window, NATIVE_WINDOW_LOCK, &buffer, &rc ))
    {
        const RECT *rgn_rect = NULL, *end = NULL;
        unsigned int *src, *dst;
        int x, y, width;

        rect.left   = rc.left;
        rect.top    = rc.top;
        rect.right  = rc.right;
        rect.bottom = rc.bottom;
        IntersectRect( &rect, &rect, &surface->header.rect );

        if (surface->region_data)
        {
            rgn_rect = (RECT *)surface->region_data->Buffer;
            end = rgn_rect + surface->region_data->rdh.nCount;
        }
        src = (unsigned int *)surface->bits
            + (rect.top - surface->header.rect.top) * surface->info.bmiHeader.biWidth
            + (rect.left - surface->header.rect.left);
        dst = (unsigned int *)buffer.bits + rect.top * buffer.stride + rect.left;
        width = min( rect.right - rect.left, buffer.stride );

        for (y = rect.top; y < min( buffer.height, rect.bottom); y++)
        {
            if (surface->info.bmiHeader.biCompression == BI_RGB)
                memcpy( dst, src, width * sizeof(*dst) );
            else if (surface->alpha == 255)
                for (x = 0; x < width; x++) dst[x] = src[x] | 0xff000000;
            else
                for (x = 0; x < width; x++)
                    dst[x] = ((surface->alpha << 24) |
                              (((BYTE)(src[x] >> 16) * surface->alpha / 255) << 16) |
                              (((BYTE)(src[x] >> 8) * surface->alpha / 255) << 8) |
                              (((BYTE)src[x] * surface->alpha / 255)));

            if (surface->color_key != CLR_INVALID)
                for (x = 0; x < width; x++) if ((src[x] & 0xffffff) == surface->color_key) dst[x] = 0;

            if (rgn_rect)
            {
                while (rgn_rect < end && rgn_rect->bottom <= y) rgn_rect++;
                apply_line_region( dst, width, rect.left, y, rgn_rect, end );
            }

            src += surface->info.bmiHeader.biWidth;
            dst += buffer.stride;
        }
        surface->window->perform( surface->window, NATIVE_WINDOW_UNLOCK_AND_POST );
    }
    else TRACE( "Unable to lock surface %p window %p buffer %p\n",
                surface, surface->hwnd, surface->window );
}

/***********************************************************************
 *           android_surface_destroy
 */
static void android_surface_destroy( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    TRACE( "freeing %p bits %p\n", surface, surface->bits );

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &surface->crit );
    HeapFree( GetProcessHeap(), 0, surface->region_data );
    if (surface->region) DeleteObject( surface->region );
    release_ioctl_window( surface->window );
    HeapFree( GetProcessHeap(), 0, surface->bits );
    HeapFree( GetProcessHeap(), 0, surface );
}

static const struct window_surface_funcs android_surface_funcs =
{
    android_surface_lock,
    android_surface_unlock,
    android_surface_get_bitmap_info,
    android_surface_get_bounds,
    android_surface_set_region,
    android_surface_flush,
    android_surface_destroy
};

static BOOL is_argb_surface( struct window_surface *surface )
{
    return surface && surface->funcs == &android_surface_funcs &&
        get_android_surface( surface )->info.bmiHeader.biCompression == BI_RGB;
}

/***********************************************************************
 *           set_color_key
 */
static void set_color_key( struct android_window_surface *surface, COLORREF key )
{
    if (key == CLR_INVALID)
        surface->color_key = CLR_INVALID;
    else if (surface->info.bmiHeader.biBitCount <= 8)
        surface->color_key = CLR_INVALID;
    else if (key & (1 << 24))  /* PALETTEINDEX */
        surface->color_key = 0;
    else if (key >> 16 == 0x10ff)  /* DIBINDEX */
        surface->color_key = 0;
    else if (surface->info.bmiHeader.biBitCount == 24)
        surface->color_key = key;
    else
        surface->color_key = (GetRValue(key) << 16) | (GetGValue(key) << 8) | GetBValue(key);
}

/***********************************************************************
 *           set_surface_region
 */
static void set_surface_region( struct window_surface *window_surface, HRGN win_region )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    struct android_win_data *win_data;
    HRGN region = win_region;
    RGNDATA *data = NULL;
    DWORD size;
    int offset_x, offset_y;

    if (window_surface->funcs != &android_surface_funcs) return;  /* we may get the null surface */

    if (!(win_data = get_win_data( surface->hwnd ))) return;
    offset_x = win_data->window_rect.left - win_data->whole_rect.left;
    offset_y = win_data->window_rect.top - win_data->whole_rect.top;
    release_win_data( win_data );

    if (win_region == (HRGN)1)  /* hack: win_region == 1 means retrieve region from server */
    {
        region = CreateRectRgn( 0, 0, win_data->window_rect.right - win_data->window_rect.left,
                                win_data->window_rect.bottom - win_data->window_rect.top );
        if (GetWindowRgn( surface->hwnd, region ) == ERROR && !surface->region) goto done;
    }

    OffsetRgn( region, offset_x, offset_y );
    if (surface->region) CombineRgn( region, region, surface->region, RGN_AND );

    if (!(size = GetRegionData( region, 0, NULL ))) goto done;
    if (!(data = HeapAlloc( GetProcessHeap(), 0, size ))) goto done;

    if (!GetRegionData( region, size, data ))
    {
        HeapFree( GetProcessHeap(), 0, data );
        data = NULL;
    }

done:
    window_surface->funcs->lock( window_surface );
    HeapFree( GetProcessHeap(), 0, surface->region_data );
    surface->region_data = data;
    *window_surface->funcs->get_bounds( window_surface ) = surface->header.rect;
    window_surface->funcs->unlock( window_surface );
    if (region != win_region) DeleteObject( region );
}

/***********************************************************************
 *           create_surface
 */
static struct window_surface *create_surface( HWND hwnd, const RECT *rect,
                                              BYTE alpha, COLORREF color_key, BOOL src_alpha )
{
    struct android_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;

    surface = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                         FIELD_OFFSET( struct android_window_surface, info.bmiColors[3] ));
    if (!surface) return NULL;
    set_color_info( &surface->info, src_alpha );
    surface->info.bmiHeader.biWidth       = width;
    surface->info.bmiHeader.biHeight      = -height; /* top-down */
    surface->info.bmiHeader.biPlanes      = 1;
    surface->info.bmiHeader.biSizeImage   = get_dib_image_size( &surface->info );

    InitializeCriticalSection( &surface->crit );
    surface->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": surface");

    surface->header.funcs = &android_surface_funcs;
    surface->header.rect  = *rect;
    surface->header.ref   = 1;
    surface->hwnd         = hwnd;
    surface->window       = get_ioctl_window( hwnd );
    surface->alpha        = alpha;
    set_color_key( surface, color_key );
    set_surface_region( &surface->header, (HRGN)1 );
    reset_bounds( &surface->bounds );

    if (!(surface->bits = HeapAlloc( GetProcessHeap(), 0, surface->info.bmiHeader.biSizeImage )))
        goto failed;

    TRACE( "created %p hwnd %p %s bits %p-%p\n", surface, hwnd, wine_dbgstr_rect(rect),
           surface->bits, (char *)surface->bits + surface->info.bmiHeader.biSizeImage );

    return &surface->header;

failed:
    android_surface_destroy( &surface->header );
    return NULL;
}

/***********************************************************************
 *           set_surface_layered
 */
static void set_surface_layered( struct window_surface *window_surface, BYTE alpha, COLORREF color_key )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    COLORREF prev_key;
    BYTE prev_alpha;

    if (window_surface->funcs != &android_surface_funcs) return;  /* we may get the null surface */

    window_surface->funcs->lock( window_surface );
    prev_key = surface->color_key;
    prev_alpha = surface->alpha;
    surface->alpha = alpha;
    set_color_key( surface, color_key );
    if (alpha != prev_alpha || surface->color_key != prev_key)  /* refresh */
        *window_surface->funcs->get_bounds( window_surface ) = surface->header.rect;
    window_surface->funcs->unlock( window_surface );
}


static WNDPROC desktop_orig_wndproc;

static LRESULT CALLBACK desktop_wndproc_wrapper( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_PARENTNOTIFY:
        if (LOWORD(wp) == WM_DESTROY) destroy_ioctl_window( (HWND)lp );
        break;
    }
    return desktop_orig_wndproc( hwnd, msg, wp, lp );
}


/***********************************************************************
 *           ANDROID_MsgWaitForMultipleObjectsEx
 */
DWORD CDECL ANDROID_MsgWaitForMultipleObjectsEx( DWORD count, const HANDLE *handles,
                                                 DWORD timeout, DWORD mask, DWORD flags )
{
    if (GetCurrentThreadId() == desktop_tid)
    {
        /* don't process nested events */
        if (current_event) mask = 0;
        if (process_events( mask )) return count - 1;
    }
    return WaitForMultipleObjectsEx( count, handles, flags & MWMO_WAITALL,
                                     timeout, flags & MWMO_ALERTABLE );
}

/**********************************************************************
 *           ANDROID_CreateWindow
 */
BOOL CDECL ANDROID_CreateWindow( HWND hwnd )
{
    TRACE( "%p\n", hwnd );

    if (hwnd == GetDesktopWindow())
    {
        struct android_win_data *data;

        init_event_queue();
        start_android_device();
        desktop_orig_wndproc = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC,
                                                           (LONG_PTR)desktop_wndproc_wrapper );
        if (!(data = alloc_win_data( hwnd ))) return FALSE;
        release_win_data( data );
    }
    return TRUE;
}


/***********************************************************************
 *           ANDROID_DestroyWindow
 */
void CDECL ANDROID_DestroyWindow( HWND hwnd )
{
    struct android_win_data *data;

    if (!(data = get_win_data( hwnd ))) return;

    if (data->surface) window_surface_release( data->surface );
    data->surface = NULL;
    free_win_data( data );
}


/***********************************************************************
 *           create_win_data
 *
 * Create a data window structure for an existing window.
 */
static struct android_win_data *create_win_data( HWND hwnd, const RECT *window_rect,
                                                 const RECT *client_rect )
{
    struct android_win_data *data;
    HWND parent;

    if (!(parent = GetAncestor( hwnd, GA_PARENT ))) return NULL;  /* desktop or HWND_MESSAGE */

    if (parent != GetDesktopWindow())
    {
        if (!(data = get_win_data( parent )) &&
            !(data = create_win_data( parent, NULL, NULL )))
            return NULL;
        release_win_data( data );
    }

    if (!(data = alloc_win_data( hwnd ))) return NULL;

    data->parent = (parent == GetDesktopWindow()) ? 0 : parent;

    if (window_rect)
    {
        data->whole_rect = data->window_rect = *window_rect;
        data->client_rect = *client_rect;
    }
    else
    {
        GetWindowRect( hwnd, &data->window_rect );
        MapWindowPoints( 0, parent, (POINT *)&data->window_rect, 2 );
        data->whole_rect = data->window_rect;
        GetClientRect( hwnd, &data->client_rect );
        MapWindowPoints( hwnd, parent, (POINT *)&data->client_rect, 2 );
        ioctl_window_pos_changed( hwnd, &data->window_rect, &data->client_rect, &data->whole_rect,
                                  GetWindowLongW( hwnd, GWL_STYLE ), SWP_NOACTIVATE,
                                  GetWindow( hwnd, GW_HWNDPREV ), GetWindow( hwnd, GW_OWNER ));
    }
    return data;
}


static inline RECT get_surface_rect( const RECT *visible_rect )
{
    RECT rect;

    IntersectRect( &rect, visible_rect, &virtual_screen_rect );
    OffsetRect( &rect, -visible_rect->left, -visible_rect->top );
    rect.left &= ~31;
    rect.top  &= ~31;
    rect.right  = max( rect.left + 32, (rect.right + 31) & ~31 );
    rect.bottom = max( rect.top + 32, (rect.bottom + 31) & ~31 );
    return rect;
}


/***********************************************************************
 *           ANDROID_WindowPosChanging
 */
void CDECL ANDROID_WindowPosChanging( HWND hwnd, HWND insert_after, UINT swp_flags,
                                     const RECT *window_rect, const RECT *client_rect, RECT *visible_rect,
                                     struct window_surface **surface )
{
    struct android_win_data *data = get_win_data( hwnd );
    RECT surface_rect;
    DWORD flags;
    COLORREF key;
    BYTE alpha;
    BOOL layered = GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_LAYERED;

    TRACE( "win %p window %s client %s style %08x flags %08x\n",
           hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
           GetWindowLongW( hwnd, GWL_STYLE ), swp_flags );

    if (!data && !(data = create_win_data( hwnd, window_rect, client_rect ))) return;

    *visible_rect = *window_rect;

    /* create the window surface if necessary */

    if (data->parent) goto done;
    if (swp_flags & SWP_HIDEWINDOW) goto done;
    if (is_argb_surface( data->surface )) goto done;

    surface_rect = get_surface_rect( visible_rect );
    if (data->surface)
    {
        if (!memcmp( &data->surface->rect, &surface_rect, sizeof(surface_rect) ))
        {
            /* existing surface is good enough */
            window_surface_add_ref( data->surface );
            if (*surface) window_surface_release( *surface );
            *surface = data->surface;
            goto done;
        }
    }
    if (!(swp_flags & SWP_SHOWWINDOW) && !(GetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE)) goto done;

    if (!layered || !GetLayeredWindowAttributes( hwnd, &key, &alpha, &flags )) flags = 0;
    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    if (*surface) window_surface_release( *surface );
    *surface = create_surface( data->hwnd, &surface_rect, alpha, key, FALSE );

done:
    release_win_data( data );
}


/***********************************************************************
 *           ANDROID_WindowPosChanged
 */
void CDECL ANDROID_WindowPosChanged( HWND hwnd, HWND insert_after, UINT swp_flags,
                                    const RECT *window_rect, const RECT *client_rect,
                                    const RECT *visible_rect, const RECT *valid_rects,
                                    struct window_surface *surface )
{
    struct android_win_data *data;
    DWORD new_style = GetWindowLongW( hwnd, GWL_STYLE );
    HWND owner = 0;

    if (!(data = get_win_data( hwnd ))) return;

    data->window_rect = *window_rect;
    data->whole_rect  = *visible_rect;
    data->client_rect = *client_rect;

    if (!is_argb_surface( data->surface ))
    {
        if (surface) window_surface_add_ref( surface );
        if (data->surface) window_surface_release( data->surface );
        data->surface = surface;
    }
    if (!data->parent) owner = GetWindow( hwnd, GW_OWNER );
    release_win_data( data );

    TRACE( "win %p window %s client %s style %08x owner %p flags %08x\n", hwnd,
           wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect), new_style, owner, swp_flags );

    ioctl_window_pos_changed( hwnd, window_rect, client_rect, visible_rect,
                              new_style, swp_flags, insert_after, owner );
}


/***********************************************************************
 *           ANDROID_ShowWindow
 */
UINT CDECL ANDROID_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp )
{
    if (IsRectEmpty( rect )) return swp;
    if (!IsIconic( hwnd )) return swp;
    /* always hide icons off-screen */
    if (rect->left != -32000 || rect->top != -32000)
    {
        OffsetRect( rect, -32000 - rect->left, -32000 - rect->top );
        swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
    }
    return swp;
}


/***********************************************************************
 *           ANDROID_SetWindowStyle
 */
void CDECL ANDROID_SetWindowStyle( HWND hwnd, INT offset, STYLESTRUCT *style )
{
    struct android_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == GetDesktopWindow()) return;
    if (!(data = get_win_data( hwnd ))) return;

    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED)) /* changing WS_EX_LAYERED resets attributes */
    {
        if (is_argb_surface( data->surface ))
        {
            if (data->surface) window_surface_release( data->surface );
            data->surface = NULL;
        }
        else if (data->surface) set_surface_layered( data->surface, 255, CLR_INVALID );
    }
    release_win_data( data );
}


/***********************************************************************
 *           ANDROID_SetWindowRgn
 */
void CDECL ANDROID_SetWindowRgn( HWND hwnd, HRGN hrgn, BOOL redraw )
{
    struct android_win_data *data;

    if ((data = get_win_data( hwnd )))
    {
        if (data->surface) set_surface_region( data->surface, hrgn );
        release_win_data( data );
    }
    else FIXME( "not supported on other process window %p\n", hwnd );
}


/***********************************************************************
 *	     ANDROID_SetLayeredWindowAttributes
 */
void CDECL ANDROID_SetLayeredWindowAttributes( HWND hwnd, COLORREF key, BYTE alpha, DWORD flags )
{
    struct android_win_data *data;

    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    if ((data = get_win_data( hwnd )))
    {
        if (data->surface) set_surface_layered( data->surface, alpha, key );
        release_win_data( data );
    }
}


/*****************************************************************************
 *           ANDROID_UpdateLayeredWindow
 */
BOOL CDECL ANDROID_UpdateLayeredWindow( HWND hwnd, const UPDATELAYEREDWINDOWINFO *info,
                                        const RECT *window_rect )
{
    struct window_surface *surface;
    struct android_win_data *data;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
    COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    void *src_bits, *dst_bits;
    RECT rect;
    HDC hdc = 0;
    HBITMAP dib;
    BOOL ret = FALSE;

    if (!(data = get_win_data( hwnd ))) return FALSE;

    rect = *window_rect;
    OffsetRect( &rect, -window_rect->left, -window_rect->top );

    surface = data->surface;
    if (!is_argb_surface( surface ))
    {
        if (surface) window_surface_release( surface );
        surface = NULL;
    }

    if (!surface || memcmp( &surface->rect, &rect, sizeof(RECT) ))
    {
        data->surface = create_surface( data->hwnd, &rect, 255, color_key, TRUE );
        if (surface) window_surface_release( surface );
        surface = data->surface;
    }
    else set_surface_layered( surface, 255, color_key );

    if (surface) window_surface_add_ref( surface );
    release_win_data( data );

    if (!surface) return FALSE;
    if (!info->hdcSrc)
    {
        window_surface_release( surface );
        return TRUE;
    }

    dst_bits = surface->funcs->get_info( surface, bmi );

    if (!(dib = CreateDIBSection( info->hdcDst, bmi, DIB_RGB_COLORS, &src_bits, NULL, 0 ))) goto done;
    if (!(hdc = CreateCompatibleDC( 0 ))) goto done;

    SelectObject( hdc, dib );

    surface->funcs->lock( surface );

    if (info->prcDirty)
    {
        IntersectRect( &rect, &rect, info->prcDirty );
        memcpy( src_bits, dst_bits, bmi->bmiHeader.biSizeImage );
        PatBlt( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, BLACKNESS );
    }
    ret = GdiAlphaBlend( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         info->hdcSrc,
                         rect.left + (info->pptSrc ? info->pptSrc->x : 0),
                         rect.top + (info->pptSrc ? info->pptSrc->y : 0),
                         rect.right - rect.left, rect.bottom - rect.top,
                         (info->dwFlags & ULW_ALPHA) ? *info->pblend : blend );
    if (ret)
    {
        memcpy( dst_bits, src_bits, bmi->bmiHeader.biSizeImage );
        add_bounds_rect( surface->funcs->get_bounds( surface ), &rect );
    }

    surface->funcs->unlock( surface );
    surface->funcs->flush( surface );

done:
    window_surface_release( surface );
    if (hdc) DeleteDC( hdc );
    if (dib) DeleteObject( dib );
    return ret;
}


/**********************************************************************
 *           ANDROID_WindowMessage
 */
LRESULT CDECL ANDROID_WindowMessage( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct android_win_data *data;

    switch (msg)
    {
    case WM_ANDROID_REFRESH:
        if ((data = get_win_data( hwnd )))
        {
            struct window_surface *surface = data->surface;
            if (surface)
            {
                surface->funcs->lock( surface );
                *surface->funcs->get_bounds( surface ) = surface->rect;
                surface->funcs->unlock( surface );
                if (is_argb_surface( surface )) surface->funcs->flush( surface );
            }
            release_win_data( data );
        }
        return 0;
    default:
        FIXME( "got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp );
        return 0;
    }
}


/***********************************************************************
 *           ANDROID_create_desktop
 */
BOOL CDECL ANDROID_create_desktop( UINT width, UINT height )
{
    /* wait until we receive the surface changed event */
    while (!screen_width)
    {
        if (wait_events( 2000 ) != 1)
        {
            ERR( "wait timed out\n" );
            break;
        }
        process_events( QS_ALLINPUT );
    }
    return TRUE;
}

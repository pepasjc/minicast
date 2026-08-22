#include "types.h"

u8* emu_vram;
u32 FrameCount;


#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

Window x11_win;
Display* x11_disp;
Visual* x11_vis;

int x11_width;
int x11_height;
Atom wmDeleteMessage;

void SetREP(u32 render_end_pending_cycles);
void RenderCORE();

void x11_window_create()
{
    XInitThreads();
    // X11 variables
    Window       x11Window = 0;
    Display*     x11Display = 0;
    long         x11Screen = 0;
    XVisualInfo* x11Visual = 0;
    Colormap     x11Colormap = 0;

    /*
    Step 0 - Create a NativeWindowType that we can use it for OpenGL ES output
    */
    Window sRootWindow;
    XSetWindowAttributes sWA;
    unsigned int ui32Mask;
    int i32Depth;

    // Initializes the display and screen
    x11Display = XOpenDisplay(NULL);
    if (!x11Display && !(x11Display = XOpenDisplay(":0")))
    {
        verify(false && "Error: Unable to open X display\n");
        return;
    }
    x11Screen = XDefaultScreen(x11Display);
    // float xdpi = (float)DisplayWidth(x11Display, x11Screen) / DisplayWidthMM(x11Display, x11Screen) * 25.4;
    // float ydpi = (float)DisplayHeight(x11Display, x11Screen) / DisplayHeightMM(x11Display, x11Screen) * 25.4;
    // auto screen_dpi = fmax(xdpi, ydpi);

    // Gets the window parameters
    sRootWindow = RootWindow(x11Display, x11Screen);

    int depth = CopyFromParent;

    i32Depth = DefaultDepth(x11Display, x11Screen);
    x11Visual = new XVisualInfo;
    XMatchVisualInfo(x11Display, x11Screen, i32Depth, TrueColor, x11Visual);
    if (!x11Visual)
    {
        printf("Error: Unable to acquire visual\n");
        return;
    }

    x11Colormap = XCreateColormap(x11Display, sRootWindow, x11Visual->visual, AllocNone);

    sWA.colormap = x11Colormap;

    // Add to these for handling other events
    sWA.event_mask = StructureNotifyMask | ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask;
    sWA.event_mask |= PointerMotionMask | FocusChangeMask;
    ui32Mask = CWBackPixel | CWBorderPixel | CWEventMask | CWColormap;

    x11_width = 640;
    x11_height = 480;

    if (x11_width < 0 || x11_height < 0)
    {
        x11_width = XDisplayWidth(x11Display, x11Screen);
        x11_height = XDisplayHeight(x11Display, x11Screen);
    }

    // Creates the X11 window
    x11Window = XCreateWindow(x11Display, RootWindow(x11Display, x11Screen), 640, 480, x11_width, x11_height,
        0, depth, InputOutput, x11Visual->visual, ui32Mask, &sWA);

    // Capture the close window event
    wmDeleteMessage = XInternAtom(x11Display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x11Display, x11Window, &wmDeleteMessage, 1);

    XMapWindow(x11Display, x11Window);

    XFlush(x11Display);

    //(EGLNativeDisplayType)x11Display;
    x11_disp = x11Display;
    x11_win = x11Window;
    x11_vis = x11Visual->visual;
}

void x11_window_destroy()
{
	// close XWindow
	if (x11_win)
	{
		XDestroyWindow((Display*)x11_disp, (Window)x11_win);
		x11_win = 0;
	}
	if (x11_disp)
	{
		XCloseDisplay((Display*)x11_disp);
		x11_disp = 0;
	}
}


void rend_init_renderer(u8* vram) {
    emu_vram = vram;
    // initialize renderer
    printf("rend_init_renderer\n");

    x11_window_create();
}
void rend_term_renderer() {
    // terminate renderer
    printf("rend_term_renderer\n");

    x11_window_destroy();
}


void rend_start_render(u8* vram) {
    // kick off render
    printf("rend_start_render\n");
    SetREP(2 * 1000 * 1000); // in 2 mhz = 10 ms at 200 mhz
    RenderCORE();
}

void rend_end_render() {
    // wait for render to end
    FrameCount++;
    printf("rend_end_render\n");
}

bool rend_render_done(u32 GuestRenderCycles) {
    return true; // RenderCORE is synchronous
}

void rend_vblank() {
    // present framebuffer to video out
    // printf("rend_vblank\n");

        if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
        return;

    int width = (FB_R_SIZE.fb_x_size + 1) << 1; // in 16-bit words
    int height = FB_R_SIZE.fb_y_size + 1;
    int modulus = (FB_R_SIZE.fb_modulus - 1) << 1;

    int bpp;
    switch (FB_R_CTRL.fb_depth)
    {
    case fbde_0555:
    case fbde_565:
        bpp = 2;
        break;
    case fbde_888:
        bpp = 3;
        //width = (width * 2) / 3;     // in pixels
        modulus = (modulus * 2) / 3; // in pixels
        break;
    case fbde_C888:
        bpp = 4;
        //width /= 2;   // in pixels
        modulus /= 2; // in pixels
        break;
    default:
        die("Invalid framebuffer format\n");
        bpp = 4;
        break;
    }

    u32 addr = SCALER_CTL.interlace && SCALER_CTL.fieldselect ? FB_R_SOF2 : FB_R_SOF1;

    // printf("Presenting: %X\n", addr);
    addr &= ~3;

    size_t pb_len = width * (SPG_CONTROL.interlace ? (height * 2 + 1) : height) * 4;
    void* pb = calloc(1, pb_len);

    u8 *dst = (u8 *)pb;

    if (SPG_CONTROL.interlace & SPG_STATUS.fieldnum) {
        dst += width * 4;
    }

#define RED_5 10
#define BLUE_5 0
#define RED_6 11
#define BLUE_6 0
#define RED_8 16
#define BLUE_8 0

    switch (FB_R_CTRL.fb_depth)
    {
    case fbde_0555: // 555 RGB
        for (int y = 0; y < height; y++)
        {
            for (int i = 0; i < width; i++)
            {
                u16 src = pvr_read_area1_16(emu_vram, addr);
                *dst++ = (((src >> BLUE_5) & 0x1F) << 3) + FB_R_CTRL.fb_concat;
                *dst++ = (((src >> 5) & 0x1F) << 3) + FB_R_CTRL.fb_concat;
                *dst++ = (((src >> RED_5) & 0x1F) << 3) + FB_R_CTRL.fb_concat;
                *dst++ = 0xFF;
                addr += bpp;
            }
            addr += modulus * bpp;
            if (SPG_CONTROL.interlace) {
                dst += width * 4;
            }
        }
        break;

    case fbde_565: // 565 RGB
        for (int y = 0; y < height; y++)
        {
            for (int i = 0; i < width; i++)
            {
                u16 src = pvr_read_area1_16(emu_vram, addr);
                *dst++ = (((src >> BLUE_6) & 0x1F) << 3) + FB_R_CTRL.fb_concat;
                *dst++ = (((src >> 5) & 0x3F) << 2) + (FB_R_CTRL.fb_concat >> 1);
                *dst++ = (((src >> RED_6) & 0x1F) << 3) + FB_R_CTRL.fb_concat;
                *dst++ = 0xFF;
                addr += bpp;
            }
            addr += modulus * bpp;

            if (SPG_CONTROL.interlace) {
                dst += width * 4;
            }
        }
        break;
    case fbde_888: // 888 RGB
        for (int y = 0; y < height; y++)
        {
            for (int i = 0; i < width; i++)
            {
                if (addr & 1)
                {
                    u32 src = pvr_read_area1_32(emu_vram, addr - 1);
                    *dst++ = src >> RED_8;
                    *dst++ = src >> 8;
                    *dst++ = src >> BLUE_8;
                }
                else
                {
                    u32 src = pvr_read_area1_32(emu_vram, addr);
                    *dst++ = src >> (RED_8 + 8);
                    *dst++ = src >> 16;
                    *dst++ = src >> (BLUE_8 + 8);
                }
                *dst++ = 0xFF;
                addr += bpp;
            }
            addr += modulus * bpp;

            if (SPG_CONTROL.interlace) {
                dst += width * 4;
            }
        }
        break;
    case fbde_C888: // 0888 RGB
        for (int y = 0; y < height; y++)
        {
            for (int i = 0; i < width; i++)
            {
                u32 src = pvr_read_area1_32(emu_vram, addr);
                *dst++ = src >> RED_8;
                *dst++ = src >> 8;
                *dst++ = src >> BLUE_8;
                *dst++ = 0xFF;
                addr += bpp;
            }
            addr += modulus * bpp;
                
            if (SPG_CONTROL.interlace) {
                dst += width * 4;
            }
        }
        break;
    }
    {
        extern Window x11_win;
        extern Display* x11_disp;
        extern Visual* x11_vis;

        extern int x11_width;
        extern int x11_height;
        XImage* ximage = XCreateImage(x11_disp, x11_vis, 24, ZPixmap, 0, (char*)pb, width, SPG_CONTROL.interlace ? height * 2 : height, 32, width * 4);

        GC gc = XCreateGC(x11_disp, x11_win, 0, 0);
        XPutImage(x11_disp, x11_win, gc, ximage, 0, 0, (x11_width - width) / 2, (x11_height - (SPG_CONTROL.interlace ? height * 2 : height)) / 2, width, SPG_CONTROL.interlace ? height * 2 : height);
        XFree(ximage);
        XFreeGC(x11_disp, gc);
    }
    free(pb);
}
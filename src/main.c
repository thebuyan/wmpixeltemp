#define _POSIX_C_SOURCE 200809L
#include "pixel.h"
#include "sensors.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t quitting;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static Reading latest = {-1, -1};
static int stop_worker, refresh;
static int interval = 2;
static SensorConfig config = {"/sys", NULL, NULL, 0};
static void stop_signal(int signum) { (void)signum; quitting = 1; }

static void *sample(void *unused)
{
    (void)unused;
    for (;;) {
        Reading r = sensors_read(&config);
        pthread_mutex_lock(&lock);
        latest = r;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += interval;
        while (!stop_worker && !refresh) {
            int err = pthread_cond_timedwait(&wake, &lock, &deadline);
            if (err == ETIMEDOUT) break;
            if (err) { stop_worker = 1; break; }
        }
        refresh = 0;
        int done = stop_worker;
        pthread_mutex_unlock(&lock);
        if (done) break;
    }
    sensors_close();
    return NULL;
}
static void request_refresh(void)
{
    pthread_mutex_lock(&lock);
    refresh = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}
static void usage(FILE *out)
{
    fputs("wmpixeltemp 1.0.0 — CPU + GPU pixel dockapp for Window Maker\n"
          "Usage: wmpixeltemp [options]\n"
          "  --windowed           Normal X11 window instead of a dock tile\n"
          "  --scale N            Integer pixel scale 1..8 (N>1 implies --windowed)\n"
          "  --interval N         Sampling interval in seconds, 1..3600 (default 2)\n"
          "  --gpu-index N        NVIDIA device index / fallback DRM card index\n"
          "  --cpu-file PATH      Explicit CPU sensor file in millidegrees Celsius\n"
          "  --gpu-file PATH      Explicit GPU sensor file in millidegrees Celsius\n"
          "  --sysfs-root PATH    Alternative sysfs root (default /sys)\n"
          "  --once               Print CPU/GPU readings and exit; no X11 required\n"
          "  --demo               Simulated CPU 48 C / GPU 62 C, amber top LED\n"
          "  --snapshot PATH      Save PPM image and exit; no X11 required\n"
          "  --help | --version\n"
          "Left click or R: refresh. Right click: Window Maker menu. Q/Escape: quit.\n", out);
}
static int number(const char *s, int low, int high)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < low || v > high) {
        fprintf(stderr, "Invalid numeric argument: %s (expected %d..%d)\n", s, low, high);
        exit(2);
    }
    return (int)v;
}
static unsigned long color(Display *d, unsigned int rgb)
{
    XColor c = {0};
    c.red = (unsigned short)(((rgb >> 16) & 255) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 255) * 257);
    c.blue = (unsigned short)((rgb & 255) * 257);
    if (!XAllocColor(d, DefaultColormap(d, DefaultScreen(d)), &c)) {
        fputs("Cannot allocate X11 palette\n", stderr); exit(1);
    }
    return c.pixel;
}
static void dock_menu(Display *d, const XButtonEvent *button)
{
    Window root, parent, *children = NULL, child;
    unsigned count;
    if (!XQueryTree(d, button->window, &root, &parent, &children, &count)) return;
    if (children) XFree(children);
    /* Window Maker owns the frame that contains our icon window. */
    if (parent == None || parent == root) return;
    XEvent forwarded = {0};
    forwarded.xbutton = *button;
    forwarded.xbutton.window = parent;
    forwarded.xbutton.subwindow = None;
    XTranslateCoordinates(d, root, parent, button->x_root, button->y_root,
                          &forwarded.xbutton.x, &forwarded.xbutton.y, &child);
    /* Release the implicit client grab so the WM can grab its native menu. */
    XUngrabPointer(d, button->time);
    XSendEvent(d, parent, False, ButtonPressMask, &forwarded);
    XFlush(d);
}
static void draw(Display *d, Window main, Window icon, GC gc, XImage *image,
                 const unsigned long *colors, Reading r, int demo, int scale)
{
    unsigned char pixels[TILE * TILE];
    pixel_render(pixels, r, demo);
    for (int y = 0; y < TILE * scale; ++y)
        for (int x = 0; x < TILE * scale; ++x)
            XPutPixel(image, x, y, colors[pixels[(y / scale) * TILE + x / scale]]);
    XPutImage(d, main, gc, image, 0, 0, 0, 0, TILE * scale, TILE * scale);
    if (icon != main)
        XPutImage(d, icon, gc, image, 0, 0, 0, 0, TILE * scale, TILE * scale);
    char cpu[16] = "N/A", gpu[16] = "N/A", title[128];
    if (r.cpu >= 0) snprintf(cpu, sizeof cpu, "%d C", r.cpu);
    if (r.gpu >= 0) snprintf(gpu, sizeof gpu, "%d C", r.gpu);
    snprintf(title, sizeof title, "wmpixeltemp%s | CPU %s | GPU %s", demo ? " [DEMO]" : "", cpu, gpu);
    XStoreName(d, main, title);
    if (icon != main) XStoreName(d, icon, title);
    XFlush(d);
}
int main(int argc, char **argv)
{
    int windowed = 0, scale = 1, demo = 0, once = 0;
    const char *snapshot = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) { usage(stdout); return 0; }
        if (!strcmp(arg, "--version")) { puts("wmpixeltemp 1.0.0"); return 0; }
        if (!strcmp(arg, "--windowed")) { windowed = 1; continue; }
        if (!strcmp(arg, "--demo")) { demo = 1; continue; }
        if (!strcmp(arg, "--once")) { once = 1; continue; }
        if (strcmp(arg, "--scale") && strcmp(arg, "--interval") &&
            strcmp(arg, "--gpu-index") && strcmp(arg, "--cpu-file") &&
            strcmp(arg, "--gpu-file") && strcmp(arg, "--sysfs-root") &&
            strcmp(arg, "--snapshot")) {
            fprintf(stderr, "Unknown option: %s\n", arg); return 2;
        }
        if (++i >= argc) { fprintf(stderr, "Missing value for %s\n", arg); return 2; }
        if (!strcmp(arg, "--scale")) scale = number(argv[i], 1, 8);
        else if (!strcmp(arg, "--interval")) interval = number(argv[i], 1, 3600);
        else if (!strcmp(arg, "--gpu-index")) config.gpu_index = (unsigned)number(argv[i], 0, INT_MAX);
        else if (!strcmp(arg, "--cpu-file")) config.cpu_file = argv[i];
        else if (!strcmp(arg, "--gpu-file")) config.gpu_file = argv[i];
        else if (!strcmp(arg, "--sysfs-root")) config.root = argv[i];
        else snapshot = argv[i];
    }
    if (once || snapshot) {
        Reading r = demo ? (Reading){48, 62} : sensors_read(&config);
        sensors_close();
        if (once) {
            char cpu[16] = "N/A", gpu[16] = "N/A";
            if (r.cpu >= 0) snprintf(cpu, sizeof cpu, "%d", r.cpu);
            if (r.gpu >= 0) snprintf(gpu, sizeof gpu, "%d", r.gpu);
            printf("CPU=%s GPU=%s unit=C%s\n", cpu, gpu, demo ? " DEMO" : "");
        }
        if (snapshot) {
            unsigned char pixels[TILE * TILE];
            pixel_render(pixels, r, demo);
            if (!pixel_save(snapshot, pixels, scale)) { perror(snapshot); return 1; }
        }
        return 0;
    }
    if (scale > 1) windowed = 1;
    Display *d = XOpenDisplay(NULL);
    if (!d) { fputs("Cannot open X11 display. Use --once for sensor diagnostics.\n", stderr); return 1; }
    int screen = DefaultScreen(d), size = TILE * scale;
    Window root = RootWindow(d, screen);
    Window main = XCreateSimpleWindow(d, root, 0, 0, size, size, 0, 0, 0);
    Window icon = windowed ? main : XCreateSimpleWindow(d, root, 0, 0, size, size, 0, 0, 0);
    XClassHint class = {"wmpixeltemp", "WMPixelTemp"};
    XSetClassHint(d, main, &class);
    XSetCommand(d, main, argv, argc);
    XWMHints hints = {0};
    hints.flags = StateHint | InputHint | WindowGroupHint;
    hints.initial_state = windowed ? NormalState : WithdrawnState;
    hints.input = windowed;
    hints.window_group = main;
    if (!windowed) { hints.flags |= IconWindowHint; hints.icon_window = icon; }
    XSetWMHints(d, main, &hints);
    XSizeHints sizes = {0};
    sizes.flags = PMinSize | PMaxSize | USSize;
    sizes.width = sizes.min_width = sizes.max_width = size;
    sizes.height = sizes.min_height = sizes.max_height = size;
    XSetWMNormalHints(d, main, &sizes);
    if (icon != main) XSetWMNormalHints(d, icon, &sizes);
    Atom wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
    Atom wm_protocols = XInternAtom(d, "WM_PROTOCOLS", False);
    XSetWMProtocols(d, main, &wm_delete, 1);
    if (icon != main) XSetWMProtocols(d, icon, &wm_delete, 1);
    long events = ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask;
    XSelectInput(d, main, events);
    if (icon != main) XSelectInput(d, icon, events);
    GC gc = XCreateGC(d, main, 0, NULL);
    unsigned long colors[COLORS];
    for (int i = 0; i < COLORS; ++i) colors[i] = color(d, palette[i]);
    XImage *image = XCreateImage(d, DefaultVisual(d, screen), (unsigned)DefaultDepth(d, screen),
                                 ZPixmap, 0, NULL, (unsigned)size, (unsigned)size, 32, 0);
    if (!image) { fputs("Cannot create X11 image\n", stderr); XCloseDisplay(d); return 1; }
    image->data = calloc((size_t)image->bytes_per_line, (size_t)size);
    if (!image->data) { XDestroyImage(image); XCloseDisplay(d); return 1; }
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    pthread_t worker;
    if (!demo && pthread_create(&worker, NULL, sample, NULL)) {
        fputs("Cannot start sensor worker\n", stderr);
        XDestroyImage(image); XCloseDisplay(d); return 1;
    }
    XMapWindow(d, main);
    Reading previous = {-2, -2};
    int dirty = 1;
    while (!quitting) {
        while (XPending(d)) {
            XEvent event;
            XNextEvent(d, &event);
            if (event.type == Expose) dirty = 1;
            else if (event.type == DestroyNotify) quitting = 1;
            else if (event.type == ClientMessage && event.xclient.message_type == wm_protocols &&
                     (Atom)event.xclient.data.l[0] == wm_delete) quitting = 1;
            else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button1 && !demo) request_refresh();
                if (event.xbutton.button == Button3 && !windowed)
                    dock_menu(d, &event.xbutton);
            } else if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_q || key == XK_Q || key == XK_Escape) quitting = 1;
                if ((key == XK_r || key == XK_R) && !demo) request_refresh();
            }
        }
        if (quitting) break;
        pthread_mutex_lock(&lock);
        Reading r = demo ? (Reading){48, 62} : latest;
        pthread_mutex_unlock(&lock);
        if (dirty || previous.cpu != r.cpu || previous.gpu != r.gpu) {
            draw(d, main, icon, gc, image, colors, r, demo, scale);
            previous = r; dirty = 0;
        }
        struct pollfd fd = {ConnectionNumber(d), POLLIN, 0};
        int result = poll(&fd, 1, 100);
        if (result < 0 && errno != EINTR) break;
        if (result > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    }
    if (!demo) {
        pthread_mutex_lock(&lock); stop_worker = 1;
        pthread_cond_signal(&wake); pthread_mutex_unlock(&lock);
        pthread_join(worker, NULL);
    }
    XDestroyImage(image); XFreeGC(d, gc);
    if (icon != main) XDestroyWindow(d, icon);
    XDestroyWindow(d, main); XCloseDisplay(d);
    return 0;
}

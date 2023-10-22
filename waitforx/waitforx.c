
#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <X11/extensions/Xrandr.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <unistd.h>

#include "arch.h"
#include "os_calls.h"

#define DISPLAY_ATTEMPTS 10
#define RANDR_ATTEMPTS 2
#define ALARM_WAIT 30

void
alarm_handler(int signal_num)
{
    g_writeln("waitforx: Unable to find RandR outputs after %d seconds", ALARM_WAIT);
    exit(1);
}

int
main(int argc, char **argv)
{
    char *display = NULL;
    int error_base = 0;
    int event_base = 0;
    int n = 0;
    int outputs = 0;
    XEvent ev;

    Display *dpy = NULL;
    XRRScreenResources *res = NULL;

    display = getenv("DISPLAY");

    signal(SIGALRM, alarm_handler);
    alarm(ALARM_WAIT);

    if (!display)
    {
        g_writeln("waitforx: DISPLAY is null");
        return 1;
    }

    for (n = 1; n <= DISPLAY_ATTEMPTS; ++n)
    {
        dpy = XOpenDisplay(display);
        g_writeln("waitforx: Opening display %s. Attempt %d of %d", display, n, DISPLAY_ATTEMPTS);
        if (dpy != NULL)
        {
            g_writeln("waitforx: Opened display %s", display);
            break;
        }
        sleep(1);
    }

    if (!dpy)
    {
        g_writeln("waitforx: Unable to open display %s", display);
        return 1;
    }

    for (n = 1; n <= RANDR_ATTEMPTS; ++n)
    {
        g_writeln("waitforx: Waiting for RandR extension.  Attempt %d of %d", n, RANDR_ATTEMPTS);
        if (XRRQueryExtension(dpy, &event_base, &error_base))
        {
            g_writeln("waitforx: RandR supported on display %s", display);
            break;
        }
        event_base = 0;
        sleep(1);
    }
    if (event_base == 0)
    {
        g_writeln("waitforx: RandR not supported on display %s", display);
        return 0; /* ok */
    }

    XRRSelectInput(dpy, DefaultRootWindow(dpy), RROutputChangeNotifyMask);

    for (n = 0; 1; ++n)
    {
        res = XRRGetScreenResources(dpy, DefaultRootWindow(dpy));
        g_writeln("waitforx: Waiting for outputs. Attempt %d", n);
        if (res != NULL)
        {
            if (res->noutput > 0)
            {
                outputs = res->noutput;
                XRRFreeScreenResources(res);
                g_writeln("waitforx: Found %d output[s]", outputs);
                break;
            }
            XRRFreeScreenResources(res);
        }
        while (XNextEvent(dpy, &ev))
        {
            if (ev.type == event_base + RRNotify)
            {
                break;
            }
        }
    }

    if (outputs > 0)
    {
        g_writeln("waitforx: display %s ready with %d outputs", display, res->noutput);
    }
    else
    {
        g_writeln("waitforx: Unable to find any outputs");
        return 1;
    }

    return 0;
}

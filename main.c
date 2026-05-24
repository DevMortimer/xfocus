#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>

int main() {
  Display *d = XOpenDisplay(NULL);
  if (!d) {
    fprintf(stderr, "Cannot open display\n");
    exit(1);
  }

  int screen = DefaultScreen(d);
  Window root = RootWindow(d, screen);

  XWindowAttributes wa;
  XGetWindowAttributes(d, root, &wa);
  int width = wa.width, height = wa.height;

  XSetWindowAttributes attr;
  attr.override_redirect = True;
  attr.background_pixel = BlackPixel(d, screen);
  attr.event_mask = KeyPressMask;

  if (!XShmQueryExtension(d)) {
    fprintf(stderr, "XShm extension not supported!\n");
    exit(1);
  }

  XShmSegmentInfo shminfo;
  XImage *img =
      XShmCreateImage(d, DefaultVisual(d, screen), DefaultDepth(d, screen),
                      ZPixmap, NULL, &shminfo, width, height);

  shminfo.shmid =
      shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);

  if (shminfo.shmid < 0) {
    fprintf(stderr, "Failed to allocate shared memory\n");
    exit(1);
  }

  shminfo.shmaddr = img->data = (char *)shmat(shminfo.shmid, 0, 0);
  shminfo.readOnly = False;

  XShmAttach(d, &shminfo);
  shmctl(shminfo.shmid, IPC_RMID, 0);

  XShmGetImage(d, root, img, 0, 0, AllPlanes);

  Window overlay = XCreateWindow(
      d, root, 0, 0, width, height, 0, CopyFromParent, InputOutput,
      CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
  XMapWindow(d, overlay);

  while (XGrabKeyboard(d, overlay, 1, GrabModeAsync, GrabModeAsync,
                       CurrentTime) != GrabSuccess) {
    struct timespec ts = {0, 10000000L};
    nanosleep(&ts, NULL);
  }

  XEvent ev;
  int running = 1;
  while (running) {
    XNextEvent(d, &ev);

    if (ev.type == KeyPress) {
      KeySym key = XKeycodeToKeysym(d, ev.xkey.keycode, 0);

      if (key == XK_q || key == XK_Escape) {
	running = 0;
      }
    }
  }

  XUngrabKeyboard(d, CurrentTime);
  XDestroyWindow(d, overlay);
  XShmDetach(d, &shminfo);
  XDestroyImage(img);
  shmdt(shminfo.shmaddr);
  XCloseDisplay(d);

  exit(0);
}

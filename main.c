#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <time.h>

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

  GLint att[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None};
  XVisualInfo *vi = glXChooseVisual(d, 0, att);
  if (!vi) {
    fprintf(stderr, "No appropriate GLX visual found\n");
    exit(1);
  }
  Colormap cmap = XCreateColormap(d, root, vi->visual, AllocNone);

  XSetWindowAttributes attr;
  attr.override_redirect = True;
  attr.background_pixel = BlackPixel(d, screen);
  attr.colormap = cmap;
  attr.event_mask = KeyPressMask;
  attr.border_pixel = 0;

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
      d, root, 0, 0, width, height, 0, vi->depth, InputOutput, vi->visual,
      CWOverrideRedirect | CWColormap | CWEventMask | CWBorderPixel, &attr);
  XMapWindow(d, overlay);

  GLXContext glc = glXCreateContext(d, vi, NULL, GL_TRUE);
  glXMakeCurrent(d, overlay, glc);

  while (XGrabKeyboard(d, overlay, 1, GrabModeAsync, GrabModeAsync,
                       CurrentTime) != GrabSuccess) {
    struct timespec ts = {0, 10000000L};
    nanosleep(&ts, NULL);
  }

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, img->data);
  glEnable(GL_TEXTURE_2D);

  XEvent ev;
  int running = 1;
  while (running) {
    while (XPending(d)) {
      XNextEvent(d, &ev);
      if (ev.type == KeyPress) {
        KeySym key = XKeycodeToKeysym(d, ev.xkey.keycode, 0);

        if (key == XK_q || key == XK_Escape) {
          running = 0;
        }
      }
    }

    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2d(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2d(1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2d(1.0f, 1.0f);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2d(-1.0f, 1.0f);
    glEnd();

    glXSwapBuffers(d, overlay);
  }

  XUngrabKeyboard(d, CurrentTime);
  glXMakeCurrent(d, None, NULL);
  glXDestroyContext(d, glc);
  XDestroyWindow(d, overlay);
  XShmDetach(d, &shminfo);
  XDestroyImage(img);
  shmdt(shminfo.shmaddr);
  XCloseDisplay(d);

  exit(0);
}

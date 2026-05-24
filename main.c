#define GL_GLEXT_PROTOTYPES 1

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

const char *vtx_src = "#version 120\n"
                      "void main() {\n"
                      "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
                      "    gl_Position = ftransform();\n"
                      "}\n";

const char *frag_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 mouse;\n"
    "uniform vec2 res;\n"
    "uniform float zoom;\n"
    "uniform float radius;\n"
    "void main() {\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    vec2 m_uv = mouse / res;\n"
    "    \n"
    "    // Pull UV coordinates towards the mouse to zoom\n"
    "    vec2 zoomed_uv = mix(m_uv, uv, 1.0 / zoom);\n"
    "    vec4 color = texture2D(tex, zoomed_uv);\n"
    "    \n"
    "    // Calculate distance in real pixels for a perfect circle\n"
    "    float dist = distance(uv * res, mouse);\n"
    "    \n"
    "    // Smooth transition from cursor\n"
    "    float mask = smoothstep(radius, radius+50.0, dist);\n"
    "    \n"
    "    // Darken everything outside the spotlight to 40% brightness\n"
    "    vec4 dark = color * 0.4;\n"
    "    gl_FragColor = mix(color, dark, mask);\n"
    "}\n";

GLuint compile_shader(const char *vtx, const char *frag) {
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vtx, NULL);
  glCompileShader(vs);

  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &frag, NULL);
  glCompileShader(fs);

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  return prog;
}

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
  attr.event_mask = KeyPressMask | ButtonPressMask;
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

  GLuint shader = compile_shader(vtx_src, frag_src);
  glUseProgram(shader);

  GLint loc_mouse = glGetUniformLocation(shader, "mouse");
  GLint loc_res = glGetUniformLocation(shader, "res");
  GLint loc_zoom = glGetUniformLocation(shader, "zoom");
  GLint loc_radius = glGetUniformLocation(shader, "radius");

  glUniform2f(loc_res, (float)width, (float)height);

  float current_zoom = 1.0f;
  float target_zoom = 1.0f;
  float current_radius = 150.0f;
  float target_radius = 1920.0f;

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
      } else if (ev.type == ButtonPress) {
        if (ev.xbutton.state & ShiftMask) {
          if (ev.xbutton.button == 4) {
            target_radius += 200.0f;
            if (target_radius > 1920.0f) target_radius = 1920.0f;
          } else if (ev.xbutton.button == 5) {
            target_radius -= 200.0f;
            if (target_radius < 50.0f) target_radius = 50.0f;
          }
        } else {
          if (ev.xbutton.button == 4) {
            target_zoom += 0.5f;
            if (target_zoom > 10.0f)
              target_zoom = 10.0f;
          } else if (ev.xbutton.button == 5) {
            target_zoom -= 0.3f;
            if (target_zoom < 1.0f)
              target_zoom = 1.0f;
          }
        }
      }
    }

    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    XQueryPointer(d, root, &root_ret, &child_ret, &root_x, &root_y, &win_x,
                  &win_y, &mask);

    current_zoom += (target_zoom - current_zoom) * 0.08f;
    current_radius += (target_radius - current_radius) * 0.15f;

    glUniform2f(loc_mouse, (float)root_x, (float)root_y);
    glUniform1f(loc_zoom, current_zoom);
    glUniform1f(loc_radius, current_radius);

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

    struct timespec ts = {0, 8000000L};
    nanosleep(&ts, NULL);
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

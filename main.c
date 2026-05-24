
#define GL_GLEXT_PROTOTYPES 1
#define MAX_POINTS 100000

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <time.h>

typedef struct {
  float x, y;
  int start;
} Point;

const char *vtx_src = "#version 120\n"
                      "void main() {\n"
                      "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
                      "    gl_Position = ftransform();\n"
                      "}\n";

const char *frag_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 camera;\n"
    "uniform vec2 mouse;\n"
    "uniform vec2 res;\n"
    "uniform float zoom;\n"
    "uniform float radius;\n"
    "void main() {\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    vec2 m_uv = mouse / res;\n"
    "    vec2 cam_uv = camera / res;\n"
    "    \n"
    "    vec2 center = clamp(cam_uv, vec2(0.5 / zoom), vec2(1.0 - 0.5 / zoom));\n"
    "    vec2 zoomed_uv = center + (uv - vec2(0.5)) / zoom;\n"
    "    \n"
    "    vec4 color = texture2D(tex, zoomed_uv);\n"
    "    \n"
    "    float dist = distance(uv * res, mouse);\n"
    "    float mask = smoothstep(radius, radius+50.0, dist);\n"
    "    \n"
    "    vec4 fog = vec4(0.89, 0.89, 0.89, 1.0);\n"
    "    vec4 outside = mix(color, fog, 0.35); // 35% opacity fog\n"
    "    \n"
    "    gl_FragColor = mix(color, outside, mask);\n"
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
  GLint loc_camera = glGetUniformLocation(shader, "camera");
  GLint loc_res = glGetUniformLocation(shader, "res");
  GLint loc_zoom = glGetUniformLocation(shader, "zoom");
  GLint loc_radius = glGetUniformLocation(shader, "radius");

  glUniform2f(loc_res, (float)width, (float)height);

  float current_zoom = 1.0f;
  float target_zoom = 1.0f;
  float current_radius = 1920.0f;
  float target_radius = 1920.0f;

  int is_frozen = 0;
  float cam_x = width / 2.0f;
  float cam_y = height / 2.0f;

  // for drawing
  Point *strokes = malloc(sizeof(Point) * MAX_POINTS);
  int num_points = 0;
  int was_drawing = 0;
  int wants_save = 0;
  float last_cam_x = -1, last_cam_y = -1;

  XEvent ev;
  int last_x = -1, last_y = -1, running = 1;
  while (running) {
    while (XPending(d)) {
      XNextEvent(d, &ev);
      if (ev.type == KeyPress) {
        KeySym key = XKeycodeToKeysym(d, ev.xkey.keycode, 0);

        if (key == XK_q || key == XK_Escape)
          running = 0;
        if (key == XK_f)
          is_frozen = !is_frozen;
        if (key == XK_s)
          wants_save = 1;

      } else if (ev.type == ButtonPress) {
        if (ev.xbutton.state & ShiftMask) {
          if (ev.xbutton.button == 4) {
            target_radius += 200.0f;
            if (target_radius > 1920.0f)
              target_radius = 1920.0f;
          } else if (ev.xbutton.button == 5) {
            target_radius -= 200.0f;
            if (target_radius < 50.0f)
              target_radius = 50.0f;
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

    if (!is_frozen) {
      cam_x += (root_x - cam_x) * 0.15f;
      cam_y += (root_y - cam_y) * 0.15f;
    }

    float cx = cam_x / width;
    float cy = cam_y / height;
    float min_c = 0.5f / current_zoom;
    float max_c = 1.0f - min_c;
    if (cx < min_c)
      cx = min_c;
    if (cx > max_c)
      cx = max_c;
    if (cy < min_c)
      cy = min_c;
    if (cy > max_c)
      cy = max_c;

    float actual_cam_x = cx * width;
    float actual_cam_y = cy * height;

    int is_drawing = (mask & Button1Mask) ? 1 : 0;
    if (is_drawing && num_points < MAX_POINTS) {
      float world_x = actual_cam_x + (root_x - width / 2.0f) / current_zoom;
      float world_y = actual_cam_y + (root_y - height / 2.0f) / current_zoom;

      strokes[num_points].x = world_x;
      strokes[num_points].y = world_y;
      strokes[num_points].start = !was_drawing;
      num_points++;
    }
    was_drawing = is_drawing;

    // save some cpu cycles
    if (fabs(current_zoom - target_zoom) < 0.001f &&
        fabs(current_radius - target_radius) < 0.001f && root_x == last_x &&
        root_y == last_y && !is_drawing && !wants_save && XPending(d) == 0) {
      struct timespec ts = {0, 16000000L};
      nanosleep(&ts, NULL);
      continue;
    }

    last_x = root_x;
    last_y = root_y;

    last_cam_x = cam_x;
    last_cam_y = cam_y;

    current_zoom += (target_zoom - current_zoom) * 0.08f;
    current_radius += (target_radius - current_radius) * 0.15f;

    glUniform2f(loc_camera, cam_x, cam_y);
    glUniform2f(loc_mouse, (float)root_x, (float)root_y);
    glUniform1f(loc_zoom, current_zoom);
    glUniform1f(loc_radius, current_radius);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

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

    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float half_w = (width / 2.0f) / current_zoom;
    float half_h = (height / 2.0f) / current_zoom;

    glOrtho(actual_cam_x - half_w, actual_cam_x + half_w, actual_cam_y + half_h,
            actual_cam_y - half_h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor3f(1.0f, 0.0f, 0.0f);
    glLineWidth(4.0f);

    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < num_points; i++) {
      if (strokes[i].start && i > 0) {
        glEnd();
        glBegin(GL_LINE_STRIP);
      }
      glVertex2f(strokes[i].x, strokes[i].y);
    }
    glEnd();
    glEnable(GL_TEXTURE_2D);
    glUseProgram(shader);

    if (wants_save) {
      wants_save = 0;
      unsigned char *pixels = malloc(width * height * 3);

      glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);

      char filename[128];
      snprintf(filename, sizeof(filename), "~/xfocus_save_%ld.ppm",
               time(NULL));
      FILE *f = fopen(filename, "wb");
      if (f) {
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int y = height - 1; y >= 0; y--) {
          fwrite(pixels + y * width * 3, 3, width, f);
        }
        fclose(f);
        printf("Screenshot saved to %s\n", filename);
      }
      free(pixels);
    }

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

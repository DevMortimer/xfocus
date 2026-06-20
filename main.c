
#define GL_GLEXT_PROTOTYPES 1
#define MAX_POINTS 100000
#define SMOOTH_STEPS 6
#define SNAP_ANGLE_STEP 0.78539816339f

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/XShm.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <time.h>

typedef struct {
  float x, y;
  int start;
} Point;

typedef struct {
  int x, y;
  int width, height;
} Monitor;

static void constrain_line(float anchor_x, float anchor_y, float *x, float *y) {
  float dx = *x - anchor_x;
  float dy = *y - anchor_y;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f)
    return;

  float angle = atan2f(dy, dx);
  float snapped = roundf(angle / SNAP_ANGLE_STEP) * SNAP_ANGLE_STEP;
  *x = anchor_x + cosf(snapped) * len;
  *y = anchor_y + sinf(snapped) * len;
}

static float catmull_rom(float p0, float p1, float p2, float p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static void draw_stroke_range(const Point *strokes, int start, int end) {
  if (end <= start)
    return;

  if (end - start < 3) {
    for (int i = start; i < end; i++) {
      glVertex2f(strokes[i].x, strokes[i].y);
    }
    return;
  }

  glVertex2f(strokes[start].x, strokes[start].y);
  for (int i = start; i < end - 1; i++) {
    Point p0 = strokes[i > start ? i - 1 : i];
    Point p1 = strokes[i];
    Point p2 = strokes[i + 1];
    Point p3 = strokes[i + 2 < end ? i + 2 : i + 1];

    for (int step = 1; step <= SMOOTH_STEPS; step++) {
      float t = (float)step / (float)SMOOTH_STEPS;
      float x = catmull_rom(p0.x, p1.x, p2.x, p3.x, t);
      float y = catmull_rom(p0.y, p1.y, p2.y, p3.y, t);
      glVertex2f(x, y);
    }
  }
}

static void draw_strokes(const Point *strokes, int num_points, float width,
                         float r, float g, float b) {
  glColor3f(r, g, b);
  glLineWidth(width);

  int start = 0;
  while (start < num_points) {
    int end = start + 1;
    while (end < num_points && !strokes[end].start) {
      end++;
    }

    glBegin(GL_LINE_STRIP);
    draw_stroke_range(strokes, start, end);
    glEnd();

    start = end;
  }

  glEnable(GL_POINT_SMOOTH);
  glPointSize(width);
  glBegin(GL_POINTS);
  for (int i = 0; i < num_points; i++) {
    glVertex2f(strokes[i].x, strokes[i].y);
  }
  glEnd();
  glDisable(GL_POINT_SMOOTH);
}

static const char *vtx_src = "#version 120\n"
                             "void main() {\n"
                             "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
                             "    gl_Position = ftransform();\n"
                             "}\n";

static const char *frag_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 camera;\n"
    "uniform vec2 mouse;\n"
    "uniform vec2 res;\n"
    "uniform float zoom;\n"
    "uniform float radius;\n"
    "uniform float time;\n"
    "\n"
    "float hash(vec2 p) {\n"
    "    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "\n"
    "vec2 hash2(vec2 p) {\n"
    "    return vec2(hash(p), hash(p + 37.17));\n"
    "}\n"
    "\n"
    "float noise(vec2 p) {\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    float a = hash(i);\n"
    "    float b = hash(i + vec2(1.0, 0.0));\n"
    "    float c = hash(i + vec2(0.0, 1.0));\n"
    "    float d = hash(i + vec2(1.0, 1.0));\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "\n"
    "float fbm(vec2 p) {\n"
    "    float value = 0.0;\n"
    "    float amp = 0.5;\n"
    "    mat2 spin = mat2(1.62, 1.18, -1.18, 1.62);\n"
    "    for (int i = 0; i < 5; i++) {\n"
    "        value += amp * noise(p);\n"
    "        p = spin * p + vec2(12.7, 8.3);\n"
    "        amp *= 0.52;\n"
    "    }\n"
    "    return value;\n"
    "}\n"
    "\n"
    "vec3 star_layer(vec2 p, float scale, float speed, float size) {\n"
    "    vec2 grid = p * scale;\n"
    "    vec2 id = floor(grid);\n"
    "    vec2 cell = fract(grid) - 0.5;\n"
    "    vec2 jitter = hash2(id) - 0.5;\n"
    "    vec2 pos = cell - jitter * 0.62;\n"
    "    float h = hash(id + scale);\n"
    "    float rare = smoothstep(0.76, 0.99, h);\n"
    "    float dist = length(pos);\n"
    "    float twinkle = 0.72 + 0.28 * sin(time * speed + h * 43.0);\n"
    "    float core = smoothstep(size, 0.0, dist);\n"
    "    float halo = smoothstep(size * 8.0, 0.0, dist) * 0.18;\n"
    "    float cross = smoothstep(size * 0.55, 0.0, abs(pos.x)) *\n"
    "                  smoothstep(size * 9.0, 0.0, abs(pos.y));\n"
    "    cross += smoothstep(size * 0.55, 0.0, abs(pos.y)) *\n"
    "             smoothstep(size * 7.0, 0.0, abs(pos.x));\n"
    "    vec3 cold = vec3(0.62, 0.82, 1.0);\n"
    "    vec3 warm = vec3(1.0, 0.78, 0.54);\n"
    "    vec3 tint = mix(cold, warm, smoothstep(0.78, 1.0, hash(id + 19.4)));\n"
    "    return tint * rare * twinkle * (core * 1.8 + halo + cross * 0.16);\n"
    "}\n"
    "\n"
    "vec3 comet_layer(vec2 p) {\n"
    "    vec2 grid = p * 5.0;\n"
    "    vec2 id = floor(grid);\n"
    "    vec2 cell = fract(grid) - 0.5;\n"
    "    float h = hash(id * 2.31);\n"
    "    float rare = smoothstep(0.965, 1.0, h);\n"
    "    cell.x += sin(time * 0.12 + h * 12.0) * 0.22;\n"
    "    cell.y += cos(time * 0.09 + h * 19.0) * 0.08;\n"
    "    float streak = exp(-abs(cell.y) * 45.0) * exp(-abs(cell.x) * 7.0);\n"
    "    float head = smoothstep(0.08, 0.0, length(cell + vec2(0.28, 0.0)));\n"
    "    return vec3(0.48, 0.72, 1.0) * rare * (streak * 0.18 + head * 1.35);\n"
    "}\n"
    "\n"
    "vec3 void_color(vec2 uv, vec2 zoomed_uv) {\n"
    "    float aspect = res.x / res.y;\n"
    "    vec2 p = (zoomed_uv - 0.5) * vec2(aspect, 1.0) * zoom;\n"
    "    vec2 drift = vec2(time * 0.018, -time * 0.012);\n"
    "    vec2 flow = vec2(fbm(p * 0.55 + vec2(time * 0.018, 4.1)),\n"
    "                     fbm(p * 0.55 + vec2(9.2, -time * 0.016))) - 0.5;\n"
    "    vec2 q = p + flow * 1.1;\n"
    "    float veil = fbm(q * 1.15 + time * 0.018);\n"
    "    float filament = fbm(q * 3.6 - time * 0.025);\n"
    "    float nebula = pow(smoothstep(0.44, 1.0, veil * 0.82 + filament * 0.42), 2.2);\n"
    "    vec3 deep = mix(vec3(0.004, 0.006, 0.014), vec3(0.018, 0.014, 0.038), veil);\n"
    "    vec3 gas_a = vec3(0.02, 0.24, 0.34);\n"
    "    vec3 gas_b = vec3(0.38, 0.06, 0.28);\n"
    "    vec3 gas = mix(gas_a, gas_b, fbm(q * 0.72 + 13.0));\n"
    "    vec3 stars = vec3(0.0);\n"
    "    stars += star_layer(p + drift * 0.20, 28.0, 1.1, 0.055);\n"
    "    stars += star_layer(p * 1.35 - drift * 0.35, 52.0, 1.8, 0.038);\n"
    "    stars += star_layer(p * 2.10 + drift * 0.70, 90.0, 2.7, 0.026) * 0.72;\n"
    "    stars += comet_layer(p + flow * 0.2);\n"
    "    vec2 clamped = clamp(zoomed_uv, vec2(0.0), vec2(1.0));\n"
    "    float edge_dist = length((zoomed_uv - clamped) * res);\n"
    "    float edge_glow = exp(-edge_dist / 72.0);\n"
    "    vec2 centered_uv = (uv - 0.5) * vec2(aspect, 1.0);\n"
    "    float vignette = smoothstep(1.15, 0.18, length(centered_uv));\n"
    "    vec3 color = deep + gas * nebula * 0.72 + stars;\n"
    "    color += vec3(0.16, 0.46, 0.95) * edge_glow * 0.26;\n"
    "    return color * (0.62 + vignette * 0.55);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    vec2 cam_uv = camera / res;\n"
    "    \n"
    "    // UNBOUNDED: Let the camera roam free!\n"
    "    vec2 zoomed_uv = cam_uv + (uv - vec2(0.5)) / zoom;\n"
    "    \n"
    "    // Check if we are inside the image\n"
    "    bool in_bounds = (zoomed_uv.x >= 0.0 && zoomed_uv.x <= 1.0 &&\n"
    "                      zoomed_uv.y >= 0.0 && zoomed_uv.y <= 1.0);\n"
    "    \n"
    "    if (in_bounds) {\n"
    "        // Normal image rendering\n"
    "        vec4 color = texture2D(tex, zoomed_uv);\n"
    "        float dist = distance(uv * res, mouse);\n"
    "        float mask = smoothstep(radius, radius+50.0, dist);\n"
    "        vec4 fog = vec4(0.89, 0.89, 0.89, 1.0);\n"
    "        vec4 outside = mix(color, fog, 0.35);\n"
    "        gl_FragColor = mix(color, outside, mask);\n"
    "    } else {\n"
    "        vec3 color = void_color(uv, zoomed_uv);\n"
    "        gl_FragColor = vec4(color, 1.0);\n"
    "    }\n"
    "}\n";

static int check_shader(GLuint shader, const char *label) {
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok)
    return 1;

  GLchar log[4096];
  GLsizei len = 0;
  glGetShaderInfoLog(shader, sizeof(log), &len, log);
  fprintf(stderr, "%s shader compilation failed:\n%.*s\n", label, len, log);
  return 0;
}

static int check_program(GLuint program) {
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok)
    return 1;

  GLchar log[4096];
  GLsizei len = 0;
  glGetProgramInfoLog(program, sizeof(log), &len, log);
  fprintf(stderr, "Shader program link failed:\n%.*s\n", len, log);
  return 0;
}

static GLuint compile_shader(const char *vtx, const char *frag) {
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vtx, NULL);
  glCompileShader(vs);
  if (!check_shader(vs, "Vertex")) {
    glDeleteShader(vs);
    return 0;
  }

  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &frag, NULL);
  glCompileShader(fs);
  if (!check_shader(fs, "Fragment")) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return 0;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  if (!check_program(prog)) {
    glDeleteProgram(prog);
    return 0;
  }

  return prog;
}

static Monitor current_monitor(Display *d, Window root, int root_width,
                               int root_height) {
  Monitor monitor = {0, 0, root_width, root_height};

  Window root_ret, child_ret;
  int root_x, root_y, win_x, win_y;
  unsigned int mask;
  if (!XQueryPointer(d, root, &root_ret, &child_ret, &root_x, &root_y, &win_x,
                     &win_y, &mask)) {
    return monitor;
  }

  int event_base, error_base;
  if (!XineramaQueryExtension(d, &event_base, &error_base) ||
      !XineramaIsActive(d)) {
    return monitor;
  }

  int screen_count = 0;
  XineramaScreenInfo *screens = XineramaQueryScreens(d, &screen_count);
  if (!screens)
    return monitor;

  for (int i = 0; i < screen_count; i++) {
    int x = screens[i].x_org;
    int y = screens[i].y_org;
    int w = screens[i].width;
    int h = screens[i].height;
    if (root_x >= x && root_x < x + w && root_y >= y && root_y < y + h) {
      monitor.x = x;
      monitor.y = y;
      monitor.width = w;
      monitor.height = h;
      break;
    }
  }

  XFree(screens);
  return monitor;
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
  Monitor monitor = current_monitor(d, root, wa.width, wa.height);
  int width = monitor.width, height = monitor.height;

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

  XShmSegmentInfo shminfo = {0};
  XImage *img =
      XShmCreateImage(d, DefaultVisual(d, screen), DefaultDepth(d, screen),
                      ZPixmap, NULL, &shminfo, width, height);
  if (!img) {
    fprintf(stderr, "Failed to create XShm image\n");
    exit(1);
  }

  shminfo.shmid =
      shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);

  if (shminfo.shmid < 0) {
    perror("shmget");
    XDestroyImage(img);
    exit(1);
  }

  shminfo.shmaddr = (char *)shmat(shminfo.shmid, 0, 0);
  if (shminfo.shmaddr == (char *)-1) {
    perror("shmat");
    shmctl(shminfo.shmid, IPC_RMID, 0);
    XDestroyImage(img);
    exit(1);
  }

  img->data = shminfo.shmaddr;
  shminfo.readOnly = False;

  if (!XShmAttach(d, &shminfo)) {
    fprintf(stderr, "Failed to attach XShm segment\n");
    shmdt(shminfo.shmaddr);
    shmctl(shminfo.shmid, IPC_RMID, 0);
    XDestroyImage(img);
    exit(1);
  }
  shmctl(shminfo.shmid, IPC_RMID, 0);

  if (!XShmGetImage(d, root, img, monitor.x, monitor.y, AllPlanes)) {
    fprintf(stderr, "Failed to capture root window\n");
    XShmDetach(d, &shminfo);
    XDestroyImage(img);
    shmdt(shminfo.shmaddr);
    exit(1);
  }

  Window overlay = XCreateWindow(
      d, root, monitor.x, monitor.y, width, height, 0, vi->depth, InputOutput,
      vi->visual,
      CWOverrideRedirect | CWColormap | CWEventMask | CWBorderPixel, &attr);
  XMapWindow(d, overlay);

  GLXContext glc = glXCreateContext(d, vi, NULL, GL_TRUE);
  if (!glc) {
    fprintf(stderr, "Failed to create GLX context\n");
    XDestroyWindow(d, overlay);
    XShmDetach(d, &shminfo);
    XDestroyImage(img);
    shmdt(shminfo.shmaddr);
    exit(1);
  }
  glXMakeCurrent(d, overlay, glc);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, img->data);
  glEnable(GL_TEXTURE_2D);

  GLuint shader = compile_shader(vtx_src, frag_src);
  if (!shader) {
    fprintf(stderr, "Failed to create shader program\n");
    exit(1);
  }
  glUseProgram(shader);

  GLint loc_mouse = glGetUniformLocation(shader, "mouse");
  GLint loc_camera = glGetUniformLocation(shader, "camera");
  GLint loc_res = glGetUniformLocation(shader, "res");
  GLint loc_zoom = glGetUniformLocation(shader, "zoom");
  GLint loc_radius = glGetUniformLocation(shader, "radius");
  GLint loc_time = glGetUniformLocation(shader, "time");

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
  if (!strokes) {
    fprintf(stderr, "Failed to allocate drawing buffer\n");
    exit(1);
  }
  int num_points = 0;
  int was_drawing = 0;
  int was_straight_drawing = 0;
  int straight_line_index = -1;
  float straight_anchor_x = 0.0f;
  float straight_anchor_y = 0.0f;
  int wants_save = 0;

  while (XGrabKeyboard(d, overlay, 1, GrabModeAsync, GrabModeAsync,
                       CurrentTime) != GrabSuccess) {
    struct timespec ts = {0, 10000000L};
    nanosleep(&ts, NULL);
  }

  float time_val = 0.0f;
  XEvent ev;
  int running = 1;
  while (running) {
    while (XPending(d)) {
      XNextEvent(d, &ev);
      if (ev.type == KeyPress) {
        KeySym key = XLookupKeysym(&ev.xkey, 0);

        if (key == XK_q || key == XK_Escape)
          running = 0;
        if (key == XK_f)
          is_frozen = !is_frozen;
        if (key == XK_s)
          wants_save = 1;
        if (key == XK_c) {
          num_points = 0;
          was_drawing = 0;
          was_straight_drawing = 0;
          straight_line_index = -1;
        }
        if (key == XK_r) {
          target_zoom = 1.0f;
          current_zoom = 1.0f;
          target_radius = 1920.0f;
          current_radius = 1920.0f;
          is_frozen = 0;
          was_drawing = 0;
          was_straight_drawing = 0;
          straight_line_index = -1;
          cam_x = width / 2.0f;
          cam_y = height / 2.0f;
        }

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
          } else if (ev.xbutton.button == 5) {
            target_zoom -= 0.3f;
            if (target_zoom < 0.1f)
              target_zoom = 0.1f;
          }
        }
      }
    }

    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    XQueryPointer(d, root, &root_ret, &child_ret, &root_x, &root_y, &win_x,
                  &win_y, &mask);
    int mouse_x = root_x - monitor.x;
    int mouse_y = root_y - monitor.y;

    if (!is_frozen) {
      cam_x += (mouse_x - cam_x) * 0.15f;
      cam_y += (mouse_y - cam_y) * 0.15f;
    }

    float cx = cam_x / width;
    float cy = cam_y / height;

    float actual_cam_x = cx * width;
    float actual_cam_y = cy * height;

    int is_drawing = (is_frozen && (mask & Button1Mask)) ? 1 : 0;
    int is_straight_drawing = (is_drawing && (mask & ShiftMask)) ? 1 : 0;
    if (is_drawing) {
      float world_x = actual_cam_x + (mouse_x - width / 2.0f) / current_zoom;
      float world_y = actual_cam_y + (mouse_y - height / 2.0f) / current_zoom;

      if (is_straight_drawing) {
        if (!was_straight_drawing || straight_line_index < 0) {
          if (num_points <= MAX_POINTS - 2) {
            if (was_drawing && num_points > 0) {
              straight_anchor_x = strokes[num_points - 1].x;
              straight_anchor_y = strokes[num_points - 1].y;
            } else {
              straight_anchor_x = world_x;
              straight_anchor_y = world_y;
            }

            constrain_line(straight_anchor_x, straight_anchor_y, &world_x,
                           &world_y);
            strokes[num_points].x = straight_anchor_x;
            strokes[num_points].y = straight_anchor_y;
            strokes[num_points].start = 1;
            straight_line_index = num_points + 1;
            strokes[straight_line_index].x = world_x;
            strokes[straight_line_index].y = world_y;
            strokes[straight_line_index].start = 0;
            num_points += 2;
          }
        } else {
          constrain_line(straight_anchor_x, straight_anchor_y, &world_x,
                         &world_y);
          strokes[straight_line_index].x = world_x;
          strokes[straight_line_index].y = world_y;
        }
      } else {
        straight_line_index = -1;
        if (num_points < MAX_POINTS) {
          strokes[num_points].x = world_x;
          strokes[num_points].y = world_y;
          strokes[num_points].start = !was_drawing || was_straight_drawing;
          num_points++;
        }
      }
    } else {
      straight_line_index = -1;
    }
    was_drawing = is_drawing;
    was_straight_drawing = is_straight_drawing;

    current_zoom += (target_zoom - current_zoom) * 0.08f;
    current_radius += (target_radius - current_radius) * 0.15f;

    time_val += 0.016f; // ~60fps

    glUniform2f(loc_camera, cam_x, cam_y);
    glUniform2f(loc_mouse, (float)mouse_x, (float)mouse_y);
    glUniform1f(loc_zoom, current_zoom);
    glUniform1f(loc_radius, current_radius);
    glUniform1f(loc_time, time_val);

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

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    draw_strokes(strokes, num_points, 8.0f, 0.0f, 0.0f, 0.0f);
    draw_strokes(strokes, num_points, 4.0f, 1.0f, 0.0f, 0.0f);

    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glUseProgram(shader);

    if (wants_save) {
      wants_save = 0;
      size_t pixel_count = (size_t)width * (size_t)height;
      if (pixel_count > SIZE_MAX / 3) {
        fprintf(stderr, "Screenshot is too large to save\n");
      } else {
        unsigned char *pixels = malloc(pixel_count * 3);
        if (!pixels) {
          fprintf(stderr, "Failed to allocate screenshot buffer\n");
          continue;
        }

        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);

        char filename[128];
        const char *home = getenv("HOME");
        int written =
            snprintf(filename, sizeof(filename), "%s/xfocus_save_%ld.ppm",
                     home ? home : ".", time(NULL));
        if (written < 0 || (size_t)written >= sizeof(filename)) {
          fprintf(stderr, "Screenshot path is too long\n");
        } else {
          FILE *f = fopen(filename, "wb");
          if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (int y = height - 1; y >= 0; y--) {
              fwrite(pixels + y * width * 3, 3, width, f);
            }
            fclose(f);
            printf("Screenshot saved to %s\n", filename);
          } else {
            perror("fopen");
          }
        }
        free(pixels);
      }
    }

    glXSwapBuffers(d, overlay);

    struct timespec ts = {0, 8000000L};
    nanosleep(&ts, NULL);
  }

  XUngrabKeyboard(d, CurrentTime);
  glDeleteProgram(shader);
  glDeleteTextures(1, &texture);
  glXMakeCurrent(d, None, NULL);
  glXDestroyContext(d, glc);
  XDestroyWindow(d, overlay);
  XShmDetach(d, &shminfo);
  XDestroyImage(img);
  shmdt(shminfo.shmaddr);
  XFreeColormap(d, cmap);
  XFree(vi);
  free(strokes);
  XCloseDisplay(d);

  return 0;
}

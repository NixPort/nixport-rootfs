// SPDX-License-Identifier: MIT
// fxwmfit: keeps top-level X windows inside the screen. There is no window manager on the
// phone, so a client that asks for more than the virtual screen (a guest UI: 1280x800 on a
// 1280x720 display) is simply cut off. This is not a window manager: it takes no redirect,
// draws no decorations and leaves override-redirect windows (menus, tooltips) alone. It only
// moves/resizes normal top-level windows that do not fit, and gives the whole screen to
// windows that already take most of it (a client's main window, a game in a window): the
// virtual screen has the shape of the phone's screen, which clients do not expect.
//
// It also gives the input focus to the topmost normal window, as a window manager does when
// a window opens. With nobody setting it, the focus stays where the last client put it (or
// at PointerRoot), and whether a game that just started has it is a matter of timing.
//
// usage: fxwmfit [--once]      (DISPLAY from the environment)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

static int on_error(Display* d, XErrorEvent* e) {
  (void)d;
  (void)e;
  return 0; // Windows disappear while we look at them: not an error for us.
}

static void fit(Display* d, Window w, int sw, int sh) {
  XWindowAttributes a;
  if (!XGetWindowAttributes(d, w, &a) || a.override_redirect || a.class != InputOutput || a.map_state != IsViewable) {
    return;
  }
  if (a.width < 64 || a.height < 64) {
    return; // Helper windows (1x1, 10x10).
  }
  int nw = a.width > sw ? sw : a.width;
  int nh = a.height > sh ? sh : a.height;
  int nx = a.x, ny = a.y;
  if (nw * 10 >= sw * 6 && nh * 10 >= sh * 8) { // Main windows: maximize.
    nw = sw;
    nh = sh;
    nx = ny = 0;
  }
  if (nx + nw > sw) nx = sw - nw;
  if (ny + nh > sh) ny = sh - nh;
  if (nx < 0) nx = 0;
  if (ny < 0) ny = 0;
  if (nw != a.width || nh != a.height || nx != a.x || ny != a.y) {
    XMoveResizeWindow(d, w, nx, ny, (unsigned)nw, (unsigned)nh);
    fprintf(stderr, "fxwmfit: 0x%lx %dx%d+%d+%d -> %dx%d+%d+%d\n", w, a.width, a.height, a.x, a.y, nw, nh, nx, ny);
  }
}

// A window a window manager would give the focus to.
static int takes_focus(Display* d, Window w) {
  XWindowAttributes a;
  if (!XGetWindowAttributes(d, w, &a) || a.override_redirect || a.class != InputOutput || a.map_state != IsViewable ||
      a.width < 64 || a.height < 64) {
    return 0;
  }
  int ok = 1;
  XWMHints* h = XGetWMHints(d, w);
  if (h) {
    if ((h->flags & InputHint) && !h->input) ok = 0;
    XFree(h);
  }
  return ok;
}

// The child of the root window that `w` is in (or is), None when it is gone.
static Window toplevel_of(Display* d, Window root, Window w) {
  while (w != None && w != root && w != PointerRoot) {
    Window r, p, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(d, w, &r, &p, &kids, &n)) return None;
    if (kids) XFree(kids);
    if (p == root) return w;
    w = p;
  }
  return None;
}

static int same_client(Window a, Window b) {
  return (a & ~0x1fffffUL) == (b & ~0x1fffffUL); // Resource ids: client base above 21 bits.
}

static int viewable(Display* d, Window w, XWindowAttributes* a) {
  return XGetWindowAttributes(d, w, a) && a->map_state == IsViewable;
}

static int verbose;

// Focus to the topmost window that takes it, unless that window (or one inside it) has it.
static void refocus(Display* d, Window root) {
  Window cur = None;
  int revert;
  XGetInputFocus(d, &cur, &revert);
  Window holder = toplevel_of(d, root, cur); // Clients put the focus on their own subwindows.
  Window want = None, r, p, *kids = NULL;
  unsigned n = 0;
  if (XQueryTree(d, root, &r, &p, &kids, &n)) {
    for (unsigned i = n; i-- > 0 && want == None;) { // Children come bottom to top.
      if (takes_focus(d, kids[i])) want = kids[i];
    }
    if (kids) XFree(kids);
  }
  if (verbose) fprintf(stderr, "fxwmfit: focus 0x%lx (toplevel 0x%lx), topmost 0x%lx\n", cur, holder, want);
  if (want == None || want == holder) return;
  // A client moves the focus among its own windows: to a menu it just opened (an
  // override-redirect window, which closes again when it loses the focus), to a dialog.
  XWindowAttributes a;
  if (holder != None && same_client(holder, want) && viewable(d, holder, &a)) return;
  XSetInputFocus(d, want, RevertToPointerRoot, CurrentTime);
  XChangeProperty(d, root, XInternAtom(d, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace,
                  (unsigned char*)&want, 1);
  fprintf(stderr, "fxwmfit: focus 0x%lx -> 0x%lx\n", holder, want);
}

// A program that covers the whole screen (a game) leaves nothing of the windows below to
// see, but they keep drawing: a CPU-rendered guest interface took 40% of a
// core under a running game. They are unmapped, as a window manager does when it minimizes,
// and mapped again when the window above them is gone. Windows of the same client as the
// one on top stay (a game and its own windows).
#define MAX_HIDDEN 16
static Window hidden[MAX_HIDDEN];
static int nhidden;
static Window hidden_for; // The full-screen window they were hidden for.
static int hide_covered = 1;

static void hide_under(Display* d, Window root, int sw, int sh) {
  XWindowAttributes a;
  if (nhidden > 0) {
    if (viewable(d, hidden_for, &a)) return; // Still there: nothing to do.
    for (int i = 0; i < nhidden; i++) XMapWindow(d, hidden[i]);
    fprintf(stderr, "fxwmfit: 0x%lx is gone, %d window(s) shown again\n", hidden_for, nhidden);
    nhidden = 0;
    hidden_for = None;
    return;
  }
  Window r, p, *kids = NULL;
  unsigned n = 0;
  if (!hide_covered || !XQueryTree(d, root, &r, &p, &kids, &n)) return;
  Window top = None;
  for (unsigned i = n; i-- > 0 && top == None;) {
    if (takes_focus(d, kids[i])) top = kids[i];
  }
  if (top != None && XGetWindowAttributes(d, top, &a) && a.x <= 0 && a.y <= 0 && a.x + a.width >= sw &&
      a.y + a.height >= sh) {
    for (unsigned i = 0; i < n && nhidden < MAX_HIDDEN; i++) {
      if (kids[i] == top || same_client(kids[i], top) || !takes_focus(d, kids[i])) continue;
      XUnmapWindow(d, kids[i]);
      hidden[nhidden++] = kids[i];
    }
    if (nhidden > 0) {
      hidden_for = top;
      fprintf(stderr, "fxwmfit: 0x%lx covers the screen, %d window(s) below hidden\n", top, nhidden);
    }
  }
  if (kids) XFree(kids);
}

static void fit_all(Display* d, Window root, int sw, int sh) {
  Window r, p, *kids = NULL;
  unsigned n = 0;
  if (XQueryTree(d, root, &r, &p, &kids, &n)) {
    for (unsigned i = 0; i < n; i++) {
      fit(d, kids[i], sw, sh);
    }
    if (kids) XFree(kids);
  }
}

int main(int argc, char** argv) {
  Display* d = XOpenDisplay(NULL);
  if (!d) {
    fprintf(stderr, "fxwmfit: cannot open display\n");
    return 1;
  }
  XSetErrorHandler(on_error);
  int s = DefaultScreen(d);
  Window root = RootWindow(d, s);
  int sw = DisplayWidth(d, s), sh = DisplayHeight(d, s);
  fit_all(d, root, sw, sh);
  XSync(d, False);
  if (argc > 1 && strcmp(argv[1], "--focus-info") == 0) { // Prints who has the focus, changes nothing.
    Window cur = None;
    int revert;
    XGetInputFocus(d, &cur, &revert);
    Window top = toplevel_of(d, root, cur);
    char* name = NULL;
    if (top != None) XFetchName(d, top, &name);
    printf("focus 0x%lx, toplevel 0x%lx \"%s\"\n", cur, top, name ? name : "");
    return 0;
  }
  verbose = argc > 1 && strcmp(argv[1], "--verbose") == 0;
  const char* keep = getenv("FXD_KEEP_COVERED_WINDOWS"); // =1: leave covered windows mapped.
  hide_covered = !(keep && keep[0] == '1');
  if (argc > 1 && strcmp(argv[1], "--once") == 0) {
    return 0;
  }
  XSelectInput(d, root, SubstructureNotifyMask);
  for (;;) { // Ends with the X server (Xlib exits on the I/O error).
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == MapNotify && e.xmap.event == root) {
        fit(d, e.xmap.window, sw, sh);
      } else if (e.type == ConfigureNotify && e.xconfigure.event == root) {
        fit(d, e.xconfigure.window, sw, sh);
      }
    }
    hide_under(d, root, sw, sh);
    refocus(d, root);
    XFlush(d);
    // Windows open, close and restack without an event we could rely on: look once a second.
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ConnectionNumber(d), &fds);
    struct timeval tv = {1, 0};
    select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
  }
}

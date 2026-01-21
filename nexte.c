/*** includes ***/

// Feature test macro so the compiler stops yelling about `getline()`
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define NEXTE_VERSION "0.0.1"
#define NEXTE_TAB_STOP 8

// Mirrors Ctrl key behavior: clears bits 5-6, mapping 'a'-'z' to 1-26.
// ASCII designed so Ctrl+letter = letter & 0x1f
// (same as toggling case via bit 5).
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 1000, // starting at 1000 avoids collision with ASCII chars
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY
};

/*** data ***/

// Editor row type: stores a single line of text
typedef struct erow {
  int size;     // length of raw chars
  int rsize;    // length of rendered string
  char *chars;  // raw line content
  char *render; // rendered line with tabs expanded
} erow;

// Editor state: cursor, viewport, dimensions, and original terminal settings
struct editorConfig {
  int cx, cy;            // cursor position
  int rx;                // rendered cursor position
  int rowoff;            // vertical scroll
  int coloff;            // horizontal scroll
  int screenrows;        // terminal height
  int screencols;        // terminal width
  int numrows;           // number of rows in file
  erow *row;             // holds every row in a file
  char *filename;        // currently open file (NULL if untitled)
  char statusmsg[80];    // message to display in status bar
  time_t statusmsg_time; // timestamp when message was set (for expiration)
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/*
 * Print error message with errno context and exit.
 * perror() appends ": <system error string>" to the message.
 */
void die(const char *s) {
  // Clear screen before printing error for readability
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

/*
 * Restore terminal to canonical mode.
 * Registered via atexit() so it runs on normal exit or die().
 */
void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

/*
 * Switch terminal to raw mode by modifying termios flags:
 *   c_iflag: input preprocessing
 *   c_oflag: output postprocessing
 *   c_cflag: control (baud, char size)
 *   c_lflag: local (echo, canonical, signals)
 */
void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tsgetattr");
  }
  atexit(disable_raw_mode);

  struct termios raw = E.orig_termios;

  // ECHO   - don't echo key presses
  // ICANON - disable line buffering (read byte-by-byte)
  // IEXTEN - disable Ctrl+V/O
  // ISIG   - disable Ctrl+C (SIGINT), Ctrl+Z (SIGTSTP)
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // Disable output processing - \n no longer automatically becomes \r\n
  raw.c_oflag &= ~(OPOST);

  // BRKINT - no SIGINT on break
  // ICRNL  - don't map CR to NL (distinguish Enter from Ctrl+J)
  // INPCK  - no parity check
  // ISTRIP - don't strip 8th bit
  // IXON   - disable Ctrl+S/Q flow control
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // Ensure character size is 8 bits per byte
  raw.c_cflag |= (CS8);

  // VMIN: min bytes before read() returns (0 = return immediately)
  // VTIME: max wait time in 1/10s (1 = 100ms timeout)
  // Together: non-blocking read with 100ms timeout for idle loop
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

/*
 * Read a single keypress from stdin.
 * Returns ASCII character or enum value for special keys.
 * Parses ANSI escape sequences: ESC [ N ~ for special keys, ESC [ A/D/B/C for
 * arrows.
 */
int editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
      return '\x1b';
    }
    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
              return HOME_KEY;
            case '3':
              return DEL_KEY;
            case '4':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            case '7':
              return HOME_KEY;
            case '8':
              return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

/*
 * Query terminal for current cursor position using ANSI escape sequence.
 * Sends ESC [ 6 n (request) and reads response in format ESC [ ROW ; COL R.
 * Returns 0 on success, -1 on failure (timeout or malformed response).
 * Typical response: "\x1b[24;80R" meaning row 24, column 80.
 */
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  size_t i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  // Print newline to flush and force terminal to process the query
  printf("\r\n");

  // Read response byte by byte until 'R' terminator
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    i++;
  }
  buf[i] = '\0';

  // Validate response format: should start with ESC [
  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
  // Parse "ROW;COL" from response
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

/*
 * Get terminal window size via ioctl(TIOCGWINSZ).
 * Returns 0 on success, -1 on failure (fallback to default).
 * Outputs rows/cols through pointer arguments.
 */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // Fallback: move cursor to far right/bottom to force terminal to report
    // size
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

/*
 * Convert logical cursor column to rendered column.
 * Tabs take multiple screen columns but count as one character.
 */
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      // Tab stops align to every NEXTE_TAB_STOP columns
      // Calculate spaces needed to reach next tab stop
      rx += (NEXTE_TAB_STOP - 1) - (rx % NEXTE_TAB_STOP);
    }

    rx++;
  }
  return rx;
}

/*
 * Process tabs in a row: expand them to spaces for display.
 * Allocates render buffer large enough to hold expanded tabs.
 */
void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int i = 0; i < row->size; i++) {
    if (row->chars[i] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  // Each tab can expand to up to (NEXTE_TAB_STOP - 1) extra spaces
  row->render = malloc(row->size + tabs * (NEXTE_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int i = 0; i < row->size; i++) {
    if (row->chars[i] == '\t') {
      // Convert tab to spaces up to next tab stop
      row->render[idx++] = ' ';
      while (idx % NEXTE_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[i];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

/*
 * Append a new row to the editor's row buffer.
 * Reallocates the row array to fit one more erow struct.
 */
void editorAppendRow(char *s, size_t len) {
  // Grow array to hold new row (realloc handles NULL for first allocation)
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  // Initialize render fields and compute rendered tab expansion
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

/*** file i/o ***/

/*
 * Open and read a file into editor state.
 * Stores filename for status bar display.
 * strdup() allocates memory; freed on re-open.
 */
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename); // allocates and copies string

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  // getline() automatically reallocates line buffer as needed
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    // Strip trailing newline/carriage return
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }

    editorAppendRow(line, linelen);
  }

  free(line);
  fclose(fp);
}

/*** append buffer ***/

/*
 * Append buffer: dynamically growing string buffer for building output.
 * Avoids many small write() syscalls by collecting bytes in memory first.
 * Pattern: create struct, append pieces, write once, free.
 */
struct abuf {
  char *b;
  int len;
};

// Constructor-like initializer for empty append buffer
#define ABUF_INIT {NULL, 0}

/*
 * Append string `s` of length `len` to buffer `ab`.
 * Reallocates buffer to accommodate new bytes, copies data into position.
 * Silently fails (no-op) if realloc returns NULL (out of memory).
 */
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) {
    return;
  }

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/*
 * Free dynamically allocated buffer memory.
 * Called after write() to prevent memory leaks.
 */
void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

/*
 * Adjust viewport to keep cursor visible.
 * Scrolls when cursor would move outside the viewport.
 */
void editorScroll() {
  E.rx = 0;

  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  // Vertical scroll: viewport top follows cursor up
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  // Vertical scroll: viewport bottom follows cursor down
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  // Horizontal scroll: viewport left follows cursor left
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  // Horizontal scroll: viewport right follows cursor right
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

/*
 * Render editor content rows into buffer for display.
 * Each row displays a tilde (~) as placeholder for text.
 * At row 1/3 of screen height, displays welcome message centered.
 * Uses ANSI escape \x1b[K to clear from cursor to line end (erases leftover
 * content).
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    // Map screen row to file row (accounting for vertical scroll)
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && filerow == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Nexte editor -- version %s", NEXTE_VERSION);

        if (welcomelen > E.screencols) {
          welcomelen = E.screencols;
        }

        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding--) {
          abAppend(ab, " ", 1);
        }

        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      // Clamp rendering to horizontal scroll position
      int len = E.row[filerow].rsize - E.coloff;

      if (len < 0) {
        len = 0;
      }
      if (len > E.screencols) {
        len = E.screencols;
      }

      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

/*
 * Draw a status bar at the bottom of the screen with inverted colors.
 * Left side: filename and line count.
 * Right side: current position.
 * Truncates or pads with spaces to fit terminal width.
 */
void editorDrawStatusBar(struct abuf *ab) {
  // SGR 7: inverted colors for status bar
  abAppend(ab, "\x1b[7m", 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

  if (len > E.screencols) {
    len = E.screencols;
  }

  abAppend(ab, status, len);

  // Fill remaining space with spaces, right-align position display
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }

  // SGR 0: reset all text attributes (bold, underline, etc.)
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) {
    msglen = E.screencols;
  }
  if (msglen && time(NULL) - E.statusmsg_time < 5) {
    abAppend(ab, E.statusmsg, msglen);
  }
}

/*
 * Clear screen and redraw content using ANSI escape sequences.
 * Uses append buffer to batch all output into a single write() syscall.
 * Sequence: hide cursor -> home cursor -> draw rows -> status bar -> position
 * cursor -> show cursor.
 * \x1b[?25l = hide cursor (prevents flicker during redraw)
 * \x1b[H    = move cursor to home
 * \x1b[?25h = show cursor
 * \x1b[Y;XH = position cursor at rendered position (rx, not cx)
 * \x1b[m   = SGR 0: reset all text attributes
 */
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];

  // Convert viewport-relative coordinates to absolute terminal positions
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1);

  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*
 * Set a status message to display in the status bar.
 * Uses variadic arguments (like printf) for formatted messages.
 * Message expires after 5 seconds (checked elsewhere).
 */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt); // initialize variadic argument list
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap); // format using va_list
  va_end(ap);                    // clean up variadic argument list
  E.statusmsg_time = time(NULL); // record when message was set
}

/*** input ***/

/*
 * Update cursor position based on movement key.
 * Handles line wrapping at row boundaries.
 */
void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        // Wrap to end of previous line
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        // Wrap to beginning of next line
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  // Clamp cursor to end of line if line got shorter
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

/*
 * Process a single keypress from the user.
 * Reads key, dispatches to handler based on key value.
 */
void editorProcessKeyPress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if (E.cy < E.numrows) {
        E.cx = E.row[E.cy].size;
      }
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        // Jump to top/bottom of viewport first
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
        }

        // Then scroll by screen height to preserve relative position
        int times = E.screenrows;
        while (times--) {
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }

  E.screenrows -= 2; // reserve bottom row for status bar
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}

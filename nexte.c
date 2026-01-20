/*** includes ***/

// Feature test macro so the compiler stops yelling about `getline()`
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define NEXTE_VERSION "0.0.1"

// Mirrors Ctrl key behavior: clears bits 5-6, mapping 'a'-'z' to 1-26.
// ASCII designed so Ctrl+letter = letter & 0x1f (same as toggling case via bit 5).
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

// Editor row: dynamically allocated string holding one line of file content
typedef struct erow {
  int size;
  char *chars;
} erow;

// Editor state: cursor position, dimensions, and original terminal settings
struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  int numrows;
  erow row;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/*
 * Print error message with errno context and exit.
 * perror() appends ": <system error string>" to the message.
 */
void die(const char *s) {
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
 * Parses ANSI escape sequences: ESC [ N ~ for special keys, ESC [ A/D/B/C for arrows.
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

  printf("\r\n");

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

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
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

/*** file i/o ***/

/*
 * Open and read a file into editor state.
 * getline() handles dynamic buffer allocation.
 */
void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  linelen = getline(&line, &linecap, fp);
  if (linelen != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }

    E.row.size = linelen;
    E.row.chars = malloc(linelen + 1);
    memcpy(E.row.chars, line, linelen);
    E.row.chars[linelen] = '\0';
    E.numrows = 1;
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
 * Render editor content rows into buffer for display.
 * Each row displays a tilde (~) as placeholder for text.
 * At row 1/3 of screen height, displays welcome message centered.
 * Uses ANSI escape \x1b[K to clear from cursor to line end (erases leftover content).
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen =
            snprintf(welcome, sizeof(welcome), "Nexte editor -- version %s", NEXTE_VERSION);
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
      int len = E.row.size;

      if (len > E.screencols) {
        len = E.screencols;
      }

      abAppend(ab, E.row.chars, len);
    }

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/*
 * Clear screen and redraw content using ANSI escape sequences.
 * Uses append buffer to batch all output into a single write() syscall.
 * Sequence: hide cursor -> home cursor -> draw rows -> position cursor -> show cursor.
 * \x1b[?25l = hide cursor (prevents flicker during redraw)
 * \x1b[H    = move cursor to home
 * \x1b[?25h = show cursor
 * \x1b[Y;XH = position cursor at row Y, column X
 */
void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

/*
 * Update cursor position based on movement key.
 * Does not check bounds - cursor can move outside visible area.
 */

void editorMoveCursor(int key) {
  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    }
    break;
  case ARROW_RIGHT:
    if (E.cx != E.screencols - 1) {
      E.cx++;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--;
    }
    break;
  case ARROW_DOWN:
    if (E.cy != E.screenrows - 1) {
      E.cy++;
    }
    break;
  }
}

/*
 * Process a single keypress from the user.
 * Reads key, dispatches to handler based on key value.
 * Ctrl+Q exits; WASD keys move cursor.
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
    E.cx = E.screencols - 1;
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    int times = E.screenrows;
    while (times--) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
  } break;

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
  E.numrows = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
}

/*** main ***/

int main(int argc, char *argv[]) {
  enable_raw_mode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}

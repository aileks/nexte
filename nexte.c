/*** includes ***/

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// Mirrors Ctrl key behavior: clears bits 5-6, mapping 'a'-'z' to 1-26.
// ASCII designed so Ctrl+letter = letter & 0x1f (same as toggling case via bit 5).
#define CTRL_KEY(k) ((k) & 0x1f)

#define NEXTE_VERSION "0.0.1"

/*** data ***/

// Editor state: dimensions and original terminal settings
struct editorConfig {
  int screenrows;              // Terminal rows (from TIOCGWINSZ)
  int screencols;              // Terminal columns (from TIOCGWINSZ)
  struct termios orig_termios; // Saved terminal state for cleanup
};

struct editorConfig E;

/*** terminal ***/

/*
 * Print error message with errno context and exit.
 * perror() appends ": <system error string>" to the message.
 */
void die(const char *s) {
  // Clear screen before printing error so it's readable
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // Move cursor to home position
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
 * Returns the character read, blocking until input is available.
 * Uses read() syscall directly to bypass stdio buffering.
 */
char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }
  return c;
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
 * Append string s of length `len` to buffer `ab`.
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
 * Uses ANSI escape \x1b[K to clear from cursor to line end.
 * Appends \r\n between rows (except last) for proper line breaks.
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y == E.screenrows / 3) {
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

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/*
 * Clear screen and redraw content using ANSI escape sequences.
 * Uses append buffer to batch all output into a single write() syscall.
 * Sequence: hide cursor -> home cursor -> draw rows -> home cursor -> show cursor.
 * \x1b[?25l = hide cursor (l = low)
 * \x1b[H    = move cursor to home (top-left)
 * \x1b[?25h = show cursor (h = high)
 */
void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  abAppend(&ab, "\x1b[H", 3);
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

/*
 * Main input processing loop: read key and handle it.
 * Called repeatedly in main() to process each keystroke.
 * Currently handles only Ctrl+Q to quit the editor.
 */
void editorProcessKeyPress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    // Clear screen before exiting for a clean terminal
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

/*** init ***/

/*
 * Initialize editor state: get terminal dimensions.
 * Called once at startup; dies on failure.
 */
void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
}

int main(void) {
  enable_raw_mode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}

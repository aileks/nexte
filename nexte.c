/*** includes ***/

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// Mirrors Ctrl key behavior: clears bits 5-6, mapping 'a'-'z' to 1-26.
// ASCII designed so Ctrl+letter = letter & 0x1f (same as toggling case via bit 5).
#define CTRL_KEY(k) ((k) & 0x1f)

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

/*** output ***/

// Render editor content: one tilde (~) per row as placeholder.
void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDERR_FILENO, "~\r\n", 3);
  }
}

/*
 * Clear screen and redraw content using ANSI escape sequences.
 * \x1b[2J = clear entire screen
 * \x1b[H  = move cursor to home (top-left)
 */
void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  editorDrawRows();
  write(STDOUT_FILENO, "\x1b[H", 3);
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

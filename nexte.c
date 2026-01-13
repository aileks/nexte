/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

// Saved original settings; restored on exit so terminal isn't left broken
struct termios orig_termios;

/*** terminal ***/

/*
 * Print error message with errno context and exit.
 * perror() appends ": <system error string>" to the message.
 */
void die(const char *s) {
  perror(s);
  exit(1);
}

/*
 * Restore terminal to canonical mode.
 * Registered via atexit() so it runs on normal exit or die().
 */
void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
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
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    die("tsgetattr");
  }
  atexit(disable_raw_mode);

  struct termios raw = orig_termios;

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

/*** init ***/

int main(void) {
  enable_raw_mode();

  // Timeout-based read loop (VMIN=0, VTIME=1).
  // read() returns 0 if no input within 100ms, allowing idle processing.
  // EAGAIN may occur on some systems when read times out - not an error.
  // Use \r\n since OPOST is disabled (no auto carriage return).
  while (1) {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
      die("read");
    }

    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }

    if (c == 'q') {
      break;
    }
  }

  return 0;
}

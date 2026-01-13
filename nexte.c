#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void disable_raw_mode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode(void) {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disable_raw_mode);

  struct termios raw = orig_termios;

  /* `ECHO` is a bitflag, defined as 00000000000000000000000000001000 in binary
   *
   * For example, we use the bitwise-NOT operator on the ECHO lflag value to get
   * 11111111111111111111111111110111
   *
   * We then bitwise-AND this value with the local flags field, which forces the
   * fourth bit in the flags field to become 0
   *
   * Every other bit retains its current value */
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // Turn off all output processing
  raw.c_oflag &= ~(OPOST);

  // Disable carriage returns, new lines, ctrl+s/q
  raw.c_iflag &= ~(ICRNL | IXON);

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(void) {
  enable_raw_mode();

  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
  }

  return 0;
}

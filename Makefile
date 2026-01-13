nexte: nexte.c
	$(CC) nexte.c -o nexte -Wall -Wextra -pedantic -std=c11

clean:
	rm -f nexte

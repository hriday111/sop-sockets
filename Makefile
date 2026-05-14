override CFLAGS=-std=c17 -Wall -Wextra -Wshadow -Wvla -Wno-unused-parameter -Wno-unused-const-variable -g -O0 -fsanitize=address,undefined,leak

ifdef CI
override CFLAGS=-std=c17 -Wall -Wextra -Wshadow -Wvla -Werror -Wno-unused-parameter -Wno-unused-const-variable
endif

NAME=sop-werona
HELPER=helper-sop-werona

.PHONY: clean all helper

all: ${NAME}

helper: ${HELPER}

${NAME}: ${NAME}.c l7-common.h
	$(CC) $(CFLAGS) -o ${NAME} ${NAME}.c

${HELPER}: helper-sop-werona.c sop-sockets-helper.c sop-sockets-helper.h l7-common.h
	$(CC) $(CFLAGS) -o ${HELPER} helper-sop-werona.c sop-sockets-helper.c

clean:
	rm -f ${NAME} ${HELPER}

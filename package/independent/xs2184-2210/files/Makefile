SRCS=\
	libxs2184/xs2184.c \
	libxs2184/i2cbusses.c

CFLAGS += -Wall -Werror -O1 -s
LDFLAGS += -lpthread -ldl
LIBS= -luci
ifeq ($(CC),cc)
CC=${CROSS_COMPILE}gcc
endif

all:
	${CC} ${CFLAGS} ${SRCS} -o xs2184 ${LDFLAGS} ${LIBS}

clean:
	-rm -rf *.o xs2184

PROGS=jpegtopdf

CFLAGS=${shell pkg-config --cflags cairo}
LDLIBS=${shell pkg-config --libs cairo} -ljpeg

all: ${PROGS}

clean:
	rm -f ${PROGS}


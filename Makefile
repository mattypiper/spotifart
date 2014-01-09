CC = g++
CFLAGS = -g -std=gnu++0x
SRCS = spotifart.cpp appkey.cpp
LFLAGS = -L/usr/local/lib
LIBS = -lspotify
OBJS = $(SRCS:.cpp=.o)
MAIN = spotifart

.PHONY: depend clean

ALL: $(MAIN)

$(MAIN): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(MAIN) $(OBJS) $(LFLAGS) $(LIBS)

.cpp.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

clean:
	rm -f *.o $(MAIN)

depend: $(SRCS)
	makedepend $(INCLUDES) $^

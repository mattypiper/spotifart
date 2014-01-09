CC = g++
CXXFLAGS = -g

#OBJDIR=obj
#
#LIBS=-lm
#
#_DEPS = spotifart.h
#DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))
#
#_OBJ = spotifart.o
#OBJ = $(patsubst %,$(OBJDIR)/%,$(_OBJ))
#
#$(OBJDIR)/%.o: %.cpp $(DEPS)
#	$(GXX) -c -o $@ $< $(CFLAGS)

TARGET = spotifart

.PHONY: clean

all: $(TARGET)

clean:
	rm -f $(OBJDIR)/*.o spotifart 


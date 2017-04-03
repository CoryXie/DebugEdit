CC?=gcc
CFLAGS+=-lelf -lpopt -Wall
SOURCES=debugedit.c hashtab.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=debugedit

all: $(SOURCES) $(EXECUTABLE)
clean: 
	rm -f $(OBJECTS) *.exe $(EXECUTABLE)
	
$(EXECUTABLE): $(SOURCES)
	$(CC) -o $@ $(SOURCES) $(CFLAGS) 

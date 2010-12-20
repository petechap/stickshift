CXX=g++
CPPFLAGS=-O0 -g -I/usr/include/libxml2 -I/usr/include/fuse -D_FILE_OFFSET_BITS=64 
LDFLAGS=-O0 -g -lfuse -lxml2 -lpthread
SOURCES=stickshift.cpp waitpipe.cpp joymodel.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=stickshift

all: $(SOURCES) $(EXECUTABLE)
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@

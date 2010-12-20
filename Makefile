CXX=g++
PACKAGES=fuse libxml-2.0
CPPFLAGS=-O0 -g $(shell pkg-config --cflags $(PACKAGES))
LDFLAGS=-O0 -g $(shell pkg-config --libs $(PACKAGES))
SOURCES=stickshift.cpp waitpipe.cpp joymodel.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=stickshift

all: $(SOURCES) $(EXECUTABLE)
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@

CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread
TARGETS = server client
SRC = server.cpp client.cpp

all: $(TARGETS)

server: server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

client: client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
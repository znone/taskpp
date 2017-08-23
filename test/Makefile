TARGET=test
CC=g++
OBJ=test.o
CFLAGS=-g -DNDEBUG -O3 -I/usr/include -I/usr/local/include 
CXXFLAGS=-I../include -std=c++11
LDFLAGS= -L/usr/lib -L/usr/local/lib -lpthread -lcpptest -lboost_context -lboost_system -lboost_thread -lboost_chrono

all : $(TARGET)

test.o : test.cpp test.h ../include/task/task.h
	$(CC) -c $(CFLAGS) $(CXXFLAGS) -o $@ $< 
	
$(TARGET) : $(OBJ)
	libtool --tag=CXX --mode=link $(CC) $(LDFLAGS) -o $@ $^

clean:
	rm $(TARGET) $(OBJ) -f

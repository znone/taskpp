TARGET=echo_server echo_client
CC=g++
OBJ=echo_client.o echo_server.o
CFLAGS=-g -DNDEBUG -O3 -I/usr/include -I/usr/local/include 
CXXFLAGS=-I../include -std=c++11
LDFLAGS= -L/usr/lib -L/usr/local/lib -lpthread -lboost_thread -lboost_context -lboost_chrono -lboost_system

all : $(TARGET) 
  
.PHONY: clean  
  
-include $(OBJS:.o=.d) 
  
%.d: %.c  
	@set -e; rm -f $@; $(CC) -MM $< $(CFLAGS) $(CXXFLAGS) > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

%.o : %.cpp
	$(CC) -c $(CFLAGS) $(CXXFLAGS) -o $@ $< 

echo_client : echo_client.o
	libtool --tag=CXX --mode=link $(CC) $(LDFLAGS) -o $@ $^

echo_server : echo_server.o
	libtool --tag=CXX --mode=link $(CC) $(LDFLAGS) -o $@ $^

clean:
	rm $(TARGET) $(OBJ) -f

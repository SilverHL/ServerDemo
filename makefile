all: log.o fdwrapper.o conn.o mgr.o springsnail

log.o: log.cpp log.h
	g++ -c -g log.cpp -o log.o
fdwrapper.o: fdwrapper.cpp fdwrapper.h
	g++ -c -g fdwrapper.cpp -o fdwrapper.o
conn.o: conn.cpp conn.h
	g++ -c -g conn.cpp -o conn.o
mgr.o: mgr.cpp mgr.h
	g++ -c -g mgr.cpp -o mgr.o
springsnail: processpool.h main.cpp log.o fdwrapper.o conn.o mgr.o
	g++ -g processpool.h log.o fdwrapper.o conn.o mgr.o main.cpp -o springsnail

clean:
	rm *.o springsnail

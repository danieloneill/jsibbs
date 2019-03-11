JSIPATH="../jsi"

all:
	clang -o snoopybbs src/snoopybbs.c -I$(JSIPATH)/src -lz -lm -lutil -ldl -pthread $(JSIPATH)/libjsi.a -lmysqlclient -lsqlite3 $(JSIPATH)/websocket/build/unix/libwebsockets.a -DJSI__MAIN

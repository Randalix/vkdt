SERVER_O=server/main.o
SERVER_CIV_O=server/civetweb/civetweb.o
SERVER_H=
SERVER_CFLAGS=-Iserver/civetweb
SERVER_LDFLAGS=$(DYNAMIC) -lm -lpthread
# civetweb: minimal feature set, no TLS (Tailscale Serve terminates), websockets on
SERVER_CIV_CFLAGS=-Iserver/civetweb -DUSE_WEBSOCKET -DNO_SSL -DNO_CGI -w
# M1 probe (resident render-core benchmark), built via the vkdt-m1 target
SERVER_M1_O=server/m1_resident.o

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pthread.h>
#include <errno.h>

#include "libtelnet.h"

#include "miniio.h"
const int TELNET_PORT = 5666;

static void* conn_ctx = 0;
static void* conn_sock = 0;
static void* chime_data = 0;
static void* chime_wndsize = 0;

static struct termios orig_tios;
static telnet_t* the_telnet;

static int do_echo, wndx, wndy;

static const telnet_telopt_t telopts[] = {
    { TELNET_TELOPT_ECHO, TELNET_WONT, TELNET_DO},
    { TELNET_TELOPT_NAWS, TELNET_WILL, TELNET_DO},
    { TELNET_TELOPT_SGA, TELNET_WILL, TELNET_DO},
    { -1, 0, 0 }
};

static void
update_winsz(void){
    struct winsize ws;
    int r;
    r = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if(r){
        fprintf(stderr, "Couldn't get winsize\n");
        wndx = 80;
        wndy = 25;
    }else{
        wndx = ws.ws_col;
        wndy = ws.ws_row;
    }
    fprintf(stderr, "Wnd size = %d,%d\n", wndx, wndy);
}

static void
telnet_event(telnet_t* telnet, telnet_event_t* ev, void* bogus){
    (void)bogus;
    unsigned char naws_buf[4];
    void* buf;
    void* ptr;

    switch(ev->type){
        case TELNET_EV_DATA:
            if (ev->data.size && fwrite(ev->data.buffer, 1, ev->data.size, 
                                        stdout) != ev->data.size) {
                fprintf(stderr, "ERROR: Could not write complete buffer to stdout");
            }
            fflush(stdout);
            break;

        case TELNET_EV_SEND:
            buf = miniio_buffer_create(conn_ctx, ev->data.size, 0);
            ptr = miniio_buffer_lock(conn_ctx, buf, 0, ev->data.size);
            memcpy(ptr, ev->data.buffer, ev->data.size);
            miniio_buffer_unlock(conn_ctx, buf);
            miniio_write(conn_ctx, conn_sock, buf, 0, ev->data.size);
            break;

        case TELNET_EV_WILL:
            if(ev->neg.telopt == TELNET_TELOPT_ECHO){
                do_echo = 0;
            }
            break;

        case TELNET_EV_WONT:
            if(ev->neg.telopt == TELNET_TELOPT_ECHO){
                do_echo = 1;
            }
            break;

        case TELNET_EV_DO:
            if(ev->neg.telopt == TELNET_TELOPT_NAWS){
                update_winsz();
                naws_buf[0] = wndx >> 8;
                naws_buf[1] = wndx & 0xff;
                naws_buf[2] = wndy >> 8;
                naws_buf[3] = wndy & 0xff;
                telnet_subnegotiation(telnet, TELNET_TELOPT_NAWS,
                                      naws_buf, 4);
            }else{
                fprintf(stderr, "IGNORED telnet DO request %d\n", 
                        ev->neg.telopt);
            }
            break;

        default:
            /* Nothing to do */
            break;
    }
}

#define SENDBUF_SIZE 128
static char sendbuf[SENDBUF_SIZE];
static int sendcnt;
static pthread_mutex_t mtx_send;
static pthread_cond_t cnd_send;

static void
do_input(void){ /* LOCKED */
    static char crlf[] = {'\r', '\n' };
    int i;
    for(i=0; i!= sendcnt; i++){
        if(sendbuf[i] == '\r' || sendbuf[i] == '\n'){
            if(do_echo){
                printf("\r\n");
            }
            telnet_send(the_telnet, crlf, 2);
        }else{
            if(do_echo){
                putchar(sendbuf[i]);
            }
            telnet_send(the_telnet, &sendbuf[i], 1);
        }
    }
    if(do_echo){
        fflush(stdout);
    }
}

static void*
thr_reader(void* bogus){
    int r,e;
    (void)bogus;
    for(;;){
        errno = 0;
        r = read(STDIN_FILENO, sendbuf, SENDBUF_SIZE);
        if(r<0){
            e = errno;
            if(e == EINTR){
                continue;
            }else{
                fprintf(stderr, "Aborting read.\n");
                exit(1);
            }
        }
        if(r>0){
            pthread_mutex_lock(&mtx_send);
            sendcnt = r;
            miniio_chime_trigger(conn_ctx, chime_data);
            for(;;){
                pthread_cond_wait(&cnd_send, &mtx_send);
                if(!sendcnt){
                    break;
                }
            }
            pthread_mutex_unlock(&mtx_send);
        }
    }
    return 0;
}


static void
minitelnet_startup(void){
    struct termios tios;
    pthread_t thr;
    /* Register signal handler */

    /* Start reader thread */
    pthread_mutex_init(&mtx_send, 0);
    pthread_cond_init(&cnd_send, 0);
    pthread_create(&thr, 0, thr_reader, 0);

    /* Enter raw mode */
    tcgetattr(STDOUT_FILENO, &orig_tios);
    tios = orig_tios;
    cfmakeraw(&tios);
    tcsetattr(STDOUT_FILENO, TCSADRAIN, &tios);

    the_telnet = telnet_init(telopts, telnet_event, 0, 0);
}

enum loopstate_e {
    LS_START,
    LS_RESOLVING,
    LS_CONNECTING,
    LS_CONNECTED
};

enum loopstate_e loopstate;

static void
mainloop(void){
    int r;
    void* ctx = 0;
    void* chime = 0;
    void* param = 0;
    void* sock = 0;
    void* buf;
    void* ptr;
#define EVBUF_SIZE 512
    uintptr_t evbuf[EVBUF_SIZE];
    uintptr_t cev_code, cev_size;
    uintptr_t* cev_param;
    uint32_t evsiz, evcur, cev;

    ctx = miniio_ioctx_create();
    param = miniio_net_param_create(ctx, 0);
    miniio_net_param_hostname(ctx, param, "127.0.0.1");
    miniio_net_param_port(ctx, param, TELNET_PORT);
    miniio_net_param_name_resolve(ctx, param);
    loopstate = LS_RESOLVING;

    chime_data = miniio_chime_new(ctx, 0);
    chime_wndsize = miniio_chime_new(ctx, 0);

    for(;;){
        r = miniio_ioctx_process(ctx);
        r = miniio_get_events(ctx, evbuf, EVBUF_SIZE, &evsiz, &evcur);
        if(r){
            abort();
        }
        if(evcur >= EVBUF_SIZE){
            abort();
        }
        evcur = 0;
        cev = 0;
        for(;;){
            if(cev >= evsiz){
                break;
            }
            cev_size = evbuf[cev+0];
            cev_code = evbuf[cev+1];
            cev_param = &evbuf[cev+2];
            cev += cev_size;
            switch(loopstate){
                case LS_RESOLVING:
                    switch(cev_code){
                        case MINIIO_EVT_NETRESOLVE:
                            sock = miniio_tcp_create(ctx, 0, 0, 0);
                            // FIXME:
                            miniio_tcp_connect(ctx, sock, param, 0);
                            loopstate = LS_CONNECTING;
                            break;
                        default:
                            break;
                    }
                    break;
                case LS_CONNECTING:
                    switch(cev_code){
                        case MINIIO_EVT_CONNECT_OUTGOING:
                            if(cev_param[2]){
                                fprintf(stderr, "Connection error.\n");
                                goto done;
                            }
                            miniio_start_read(ctx, sock);
                            conn_ctx = ctx;
                            conn_sock = sock;
                            minitelnet_startup();
                            loopstate = LS_CONNECTED;
                            break;
                        default:
                            break;
                    }
                    break;
                case LS_CONNECTED:
                    switch(cev_code){
                        case MINIIO_EVT_WRITE_COMPLETE:
                            buf = (void*)cev_param[0];
                            miniio_buffer_destroy(ctx, buf);
                            break;
                        case MINIIO_EVT_READ_COMPLETE:
                            buf = (void*)cev_param[2];
                            ptr = miniio_buffer_lock(ctx, buf, cev_param[3],
                                                     cev_param[4]);
                            telnet_recv(the_telnet, ptr, cev_param[4]);
                            miniio_buffer_unlock(ctx, buf);
                            break;
                        case MINIIO_EVT_CHIME:
                            ptr = (void*)cev_param[0];
                            if(ptr == chime_data){
                                pthread_mutex_lock(&mtx_send);
                                if(sendcnt){
                                    do_input();
                                }
                                sendcnt = 0;
                                pthread_cond_signal(&cnd_send);
                                pthread_mutex_unlock(&mtx_send);
                            }else if(ptr == chime_wndsize){
                            }else{
                                abort();
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    abort();
                    break;
            }
        }
    }

done:

}

void
start_minitelnet(void){
    struct termios tios;
    do_echo = 1;
    loopstate = LS_START;

    tcgetattr(STDOUT_FILENO, &orig_tios); /* For leaving */
    mainloop();
}


#ifndef MINITELNET_EMBEDDED

static void
cleanup(void){
    /* Leave raw mode */
    tcsetattr(STDOUT_FILENO, TCSADRAIN, &orig_tios);
}

int
main(int ac, char** av){
    tcgetattr(STDOUT_FILENO, &orig_tios); /* For leaving */
    atexit(cleanup);
    start_minitelnet();
    return 0;
}

#endif

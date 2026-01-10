#pragma once

#include <stddef.h>

#define NAME_MAX_LEN 16
#define MAP_PATH_MAX 128

#define REGISTRY_PATH "../rec/snake_ipc_registry.txt"

/* Max rozmery na prenos board snapshotu (dá sa upraviť) */
#define MAX_W 120
#define MAX_H 60
#define BOARD_MAX (MAX_W * MAX_H)

/* Protokol */
enum Dir { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 };
enum GameMode { STANDARD = 0, TIMED = 1 };
enum WorldType { NO_OBS = 0, OBS_FILE = 1 };

enum MsgType {
  JOIN = 1,
  JOIN_OK,
  JOIN_REJECT,
  INPUT,
  PAUSE,
  RESUME,
  RESPAWN,
  LEAVE,
  SNAPSHOT,
  SCOREBOARD,
  GAME_END
};

typedef struct { int x; int y; } Pos;

typedef struct {
  int type;   /* enum MsgType */
  int len;    /* bytes v data */
  char data[8192];
} Msg;

typedef struct {
  int pid;
  int port;
} ServerInfo;

/* --- sockets helpers --- */
int net_listen(int port);                 /* port 0 => OS pridelí */
int net_port_of_listenfd(int listenfd);
int net_accept(int listenfd);
int net_connect(const char* host, int port);

int net_send_msg(int fd, const Msg* m);
int net_recv_msg(int fd, Msg* m);

/* --- registry --- */
int reg_add(const ServerInfo* s);
int reg_remove(int pid);
int reg_list(ServerInfo out[], int max);
int reg_prune_dead(void);

/* --- util --- */
long now_ms(void);
void sleep_ms(int ms);
int clampi(int v, int lo, int hi);

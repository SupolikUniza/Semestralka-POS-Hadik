#pragma once

#include "common.h"

#define MAX_PLAYERS 8

typedef struct {
  int used;
  int id;            /* 0..MAX_PLAYERS-1 */
  int fd;            /* socket fd, -1 ak odpojený */
  char name[NAME_MAX_LEN];
  int alive;         /* 1 žije, 0 zomrel */
  int paused;        /* 1 = stojí */
  int score;
  long alive_ms;     /* celkový čas v hre (sum) */
  long spawn_ms;     /* čas posledného spawnu */
  long resume_at;    /* po RESUME sa začne hýbať až po tomto čase */
} Player;

typedef struct {
  int used;
  int player_id;
  int dir;     /* enum Dir */
  int len;
  Pos body[BOARD_MAX]; /* max = w*h */
} Snake;

typedef struct {
  int used;
  Pos p;
} Fruit;

typedef struct {
  int w;
  int h;
  int wrap; /* 1 v NO_OBS, 0 v OBS */
  char cells[BOARD_MAX]; /* prekážky '#', inak ' ' */
} World;

typedef struct {
  int mode;         /* enum GameMode */
  int world_type;   /* enum WorldType */
  int max_players;
  int time_s;       /* TIMED */
  char map_path[MAP_PATH_MAX];

  int listenfd;
  int port;
  int running;

  World world;

  Player players[MAX_PLAYERS];
  Snake  snakes[MAX_PLAYERS];
  Fruit  fruits[MAX_PLAYERS];

  long start_ms;
  long end_ms;          /* TIMED: absolútny čas konca */
  long freeze_until;    /* po JOIN 3s stop pre všetkých */
  long empty_since_ms;  /* pre STANDARD: kedy sa hra vyprázdnila */
} Game;

/* spustí server */
int server_run(int w, int h, int mode, int time_s, int world_type, const char* map_path, int max_players, int port_hint);

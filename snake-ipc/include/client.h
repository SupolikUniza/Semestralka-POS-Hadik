#pragma once

#include "common.h"
#include <pthread.h>

typedef struct {
  int fd;
  int connected;
  int player_id;

  int paused;

  Msg last_snapshot;
  int has_snapshot;

  int game_end;
  long final_ms;
  int final_scores[8];

  pthread_mutex_t mx;
} Client;

int client_run(void);

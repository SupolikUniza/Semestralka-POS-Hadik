#include "../include/server.h"

#include <sys/socket.h>


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

static int idx(int x, int y, int w) { return y * w + x; }

static void world_clear(World* W) {
  for (int i = 0; i < BOARD_MAX; i++) W->cells[i] = ' ';
}

static int world_load_file(World* W, const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return -1;

  /* jednoduchý formát: textová mapa, riadky = h, stĺpce = w, '#' = prekážka */
  char line[512];
  int y = 0;

  world_clear(W);

  while (fgets(line, (int)sizeof(line), f) && y < W->h) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
    for (int x = 0; x < W->w && x < len; x++) {
      char c = line[x];
      W->cells[idx(x, y, W->w)] = (c == '#') ? '#' : ' ';
    }
    y++;
  }

  fclose(f);
  return 0;
}

static int cell_is_blocked(Game* g, int x, int y) {
  if (x < 0 || x >= g->world.w || y < 0 || y >= g->world.h) return 1;
  return g->world.cells[idx(x, y, g->world.w)] == '#';
}

static int cell_has_snake(Game* g, int x, int y) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g->snakes[i].used) continue;
    Snake* s = &g->snakes[i];
    for (int k = 0; k < s->len; k++) {
      if (s->body[k].x == x && s->body[k].y == y) return 1;
    }
  }
  return 0;
}

static int cell_has_fruit(Game* g, int x, int y) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->fruits[i].used && g->fruits[i].p.x == x && g->fruits[i].p.y == y) return 1;
  }
  return 0;
}

static int rand_int(int lo, int hi) { /* inclusive */
  int r = rand();
  int span = hi - lo + 1;
  if (span <= 0) return lo;
  return lo + (r % span);
}

static int find_empty_cell(Game* g, Pos* out) {
  int w = g->world.w, h = g->world.h;
  int tries = w * h * 4;
  while (tries-- > 0) {
    int x = rand_int(0, w - 1);
    int y = rand_int(0, h - 1);
    if (cell_is_blocked(g, x, y)) continue;
    if (cell_has_snake(g, x, y)) continue;
    if (cell_has_fruit(g, x, y)) continue;
    out->x = x; out->y = y;
    return 0;
  }
  /* fallback scan */
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (cell_is_blocked(g, x, y)) continue;
      if (cell_has_snake(g, x, y)) continue;
      if (cell_has_fruit(g, x, y)) continue;
      out->x = x; out->y = y;
      return 0;
    }
  }
  return -1;
}

static void ensure_fruits(Game* g) {
  int alive_snakes = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->snakes[i].used && g->players[i].alive) alive_snakes++;
  }

  int have = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (g->fruits[i].used) have++;

  while (have < alive_snakes) {
    for (int i = 0; i < MAX_PLAYERS && have < alive_snakes; i++) {
      if (g->fruits[i].used) continue;
      Pos p;
      if (find_empty_cell(g, &p) == 0) {
        g->fruits[i].used = 1;
        g->fruits[i].p = p;
        have++;
      } else {
        return;
      }
    }
  }

  while (have > alive_snakes) {
    for (int i = 0; i < MAX_PLAYERS && have > alive_snakes; i++) {
      if (g->fruits[i].used) {
        g->fruits[i].used = 0;
        have--;
      }
    }
  }
}

static void snake_kill(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  g->players[pid].alive = 0;
  g->snakes[pid].used = 0;
}

static void snake_spawn(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;

  Pos p;
  if (find_empty_cell(g, &p) != 0) return;

  Snake* s = &g->snakes[pid];
  memset(s, 0, sizeof(*s));
  s->used = 1;
  s->player_id = pid;
  s->dir = RIGHT;
  s->len = 3;

  for (int i = 0; i < s->len; i++) {
    int x = p.x - (s->len - 1 - i);
    int y = p.y;
    if (g->world.wrap) {
      while (x < 0) x += g->world.w;
      while (x >= g->world.w) x -= g->world.w;
    } else {
      if (x < 0) x = 0;
      if (x >= g->world.w) x = g->world.w - 1;
    }
    s->body[i].x = x;
    s->body[i].y = y;
  }

  g->players[pid].alive = 1;
  g->players[pid].spawn_ms = now_ms();
  g->players[pid].resume_at = 0;
}

static void apply_move(Game* g, Snake* s, Pos* next) {
  Pos head = s->body[s->len - 1];
  int x = head.x;
  int y = head.y;

  if (s->dir == UP) y--;
  else if (s->dir == DOWN) y++;
  else if (s->dir == LEFT) x--;
  else if (s->dir == RIGHT) x++;

  if (g->world.wrap) {
    if (x < 0) x = g->world.w - 1;
    if (x >= g->world.w) x = 0;
    if (y < 0) y = g->world.h - 1;
    if (y >= g->world.h) y = 0;
  }

  next->x = x;
  next->y = y;
}

static int fruit_at(Game* g, int x, int y, int* out_index) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->fruits[i].used && g->fruits[i].p.x == x && g->fruits[i].p.y == y) {
      if (out_index) *out_index = i;
      return 1;
    }
  }
  return 0;
}

static void snake_step(Game* g, int pid) {
  Snake* s = &g->snakes[pid];
  if (!s->used) return;
  if (!g->players[pid].alive) return;

  long t = now_ms();
  if (t < g->freeze_until) return;
  if (g->players[pid].paused) return;
  if (g->players[pid].resume_at > 0 && t < g->players[pid].resume_at) return;

  Pos next;
  apply_move(g, s, &next);

  if (!g->world.wrap) {
    if (next.x < 0 || next.x >= g->world.w || next.y < 0 || next.y >= g->world.h) {
      snake_kill(g, pid);
      return;
    }
  }

  if (cell_is_blocked(g, next.x, next.y)) {
    snake_kill(g, pid);
    return;
  }

  if (cell_has_snake(g, next.x, next.y)) {
    snake_kill(g, pid);
    return;
  }

  int fi = -1;
  int ate = fruit_at(g, next.x, next.y, &fi);

  if (ate) {
    if (s->len < (g->world.w * g->world.h)) {
      s->body[s->len] = next;
      s->len++;
    }
    g->players[pid].score += 1;
    if (fi >= 0) g->fruits[fi].used = 0;
  } else {
    for (int i = 0; i < s->len - 1; i++) s->body[i] = s->body[i + 1];
    s->body[s->len - 1] = next;
  }
}

static int any_alive_snakes(Game* g) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->players[i].used && g->players[i].alive) return 1;
  }
  return 0;
}

static void build_board(Game* g, char* out_board) {
  int w = g->world.w, h = g->world.h;
  for (int i = 0; i < w * h; i++) out_board[i] = (g->world.cells[i] == '#') ? '#' : ' ';

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->fruits[i].used) {
      int x = g->fruits[i].p.x, y = g->fruits[i].p.y;
      out_board[idx(x, y, w)] = '*';
    }
  }

  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    if (!g->snakes[pid].used) continue;
    Snake* s = &g->snakes[pid];
    for (int k = 0; k < s->len; k++) {
      int x = s->body[k].x, y = s->body[k].y;
      char c = (k == s->len - 1) ? 'A' : 'o';
      out_board[idx(x, y, w)] = c;
    }
  }
}

static void send_snapshot_to(Game* g, int fd) {
  char board[BOARD_MAX];
  build_board(g, board);

  Msg m;
  memset(&m, 0, sizeof(m));
  m.type = SNAPSHOT;

  int w = g->world.w, h = g->world.h;
  long game_ms = now_ms() - g->start_ms;

  int off = 0;
  memcpy(m.data + off, &w, (int)sizeof(int)); off += (int)sizeof(int);
  memcpy(m.data + off, &h, (int)sizeof(int)); off += (int)sizeof(int);
  memcpy(m.data + off, &game_ms, (int)sizeof(long)); off += (int)sizeof(long);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int sc = g->players[i].score;
    memcpy(m.data + off, &sc, (int)sizeof(int)); off += (int)sizeof(int);
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int al = (g->players[i].used && g->players[i].alive) ? 1 : 0;
    memcpy(m.data + off, &al, (int)sizeof(int)); off += (int)sizeof(int);
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int pa = (g->players[i].used && g->players[i].paused) ? 1 : 0;
    memcpy(m.data + off, &pa, (int)sizeof(int)); off += (int)sizeof(int);
  }

  int blen = w * h;
  memcpy(m.data + off, board, (size_t)blen); off += blen;

  m.len = off;
  net_send_msg(fd, &m);
}

static void broadcast_snapshot(Game* g) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g->players[i].used) continue;
    if (g->players[i].fd < 0) continue;
    send_snapshot_to(g, g->players[i].fd);
  }
}

static void send_game_end(Game* g) {
  Msg m;
  memset(&m, 0, sizeof(m));
  m.type = GAME_END;

  long game_ms = now_ms() - g->start_ms;
  int off = 0;
  memcpy(m.data + off, &game_ms, (int)sizeof(long)); off += (int)sizeof(long);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int sc = g->players[i].score;
    memcpy(m.data + off, &sc, (int)sizeof(int)); off += (int)sizeof(int);
  }
  m.len = off;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->players[i].used && g->players[i].fd >= 0) {
      net_send_msg(g->players[i].fd, &m);
    }
  }
}

static int alloc_player(Game* g) {
  for (int i = 0; i < g->max_players && i < MAX_PLAYERS; i++) {
    if (!g->players[i].used) return i;
  }
  return -1;
}

static int current_players(Game* g) {
  int c = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (g->players[i].used) c++;
  return c;
}

static void registry_update(Game* g) {
  ServerInfo s;
  memset(&s, 0, sizeof(s));
  s.pid = getpid();
  s.port = g->port;
  reg_add(&s);
}
static void handle_join(Game* g, int fd, const Msg* m) {
  (void)m;
  if (current_players(g) >= g->max_players) {
    Msg r; memset(&r, 0, sizeof(r));
    r.type = JOIN_REJECT;
    const char* reason = "FULL";
    r.len = (int)strlen(reason) + 1;
    memcpy(r.data, reason, (size_t)r.len);
    net_send_msg(fd, &r);
    return;
  }

  int pid = alloc_player(g);
  if (pid < 0) {
    Msg r; memset(&r, 0, sizeof(r));
    r.type = JOIN_REJECT;
    const char* reason = "NO_SLOT";
    r.len = (int)strlen(reason) + 1;
    memcpy(r.data, reason, (size_t)r.len);
    net_send_msg(fd, &r);
    return;
  }

  Player* p = &g->players[pid];
  memset(p, 0, sizeof(*p));
  p->used = 1;
  p->id = pid;
  p->fd = fd;
  p->alive = 1;
  p->paused = 0;
  p->score = 0;
  p->alive_ms = 0;
  p->spawn_ms = now_ms();
  p->resume_at = 0;
  snprintf(p->name, NAME_MAX_LEN, "P%d", pid + 1);

  snake_spawn(g, pid);
  g->freeze_until = now_ms() + 3000;
  if (!any_alive_snakes(g)) g->empty_since_ms = 0;

  Msg ok; memset(&ok, 0, sizeof(ok));
  ok.type = JOIN_OK;
  ok.len = (int)sizeof(int);
  memcpy(ok.data, &pid, sizeof(int));
  net_send_msg(fd, &ok);

  //registry_update(g);
}

static void handle_input(Game* g, int pid, int dir) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  if (!g->snakes[pid].used) return;

  int cur = g->snakes[pid].dir;
  if ((cur == UP && dir == DOWN) || (cur == DOWN && dir == UP) ||
      (cur == LEFT && dir == RIGHT) || (cur == RIGHT && dir == LEFT)) return;

  g->snakes[pid].dir = dir;
}

static void handle_pause(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  if (!g->players[pid].used) return;
  g->players[pid].paused = 1;
}

static void handle_resume(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  if (!g->players[pid].used) return;
  g->players[pid].paused = 0;
  g->players[pid].resume_at = now_ms() + 3000;
}

static void handle_leave(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  if (!g->players[pid].used) return;

  if (g->players[pid].fd >= 0) {
    close(g->players[pid].fd);
    g->players[pid].fd = -1;
  }
  g->players[pid].used = 0;
  g->players[pid].alive = 0;
  g->snakes[pid].used = 0;

  //registry_update(g);
}

static void handle_respawn(Game* g, int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  if (!g->players[pid].used) return;

  snake_spawn(g, pid);
  g->freeze_until = now_ms() + 3000;
}

static int recv_one_nonblock(int fd, Msg* out) {
  return net_recv_msg(fd, out);
}

static void server_tick(Game* g) {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    if (g->snakes[pid].used && g->players[pid].alive) {
      snake_step(g, pid);
    }
  }
  ensure_fruits(g);

  long t = now_ms();
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g->players[i].used && g->players[i].alive) {
      g->players[i].alive_ms = (t - g->players[i].spawn_ms);
    }
  }

  if (g->mode == TIMED) {
    if (now_ms() >= g->end_ms) g->running = 0;
  } else {
    if (!any_alive_snakes(g)) {
      if (g->empty_since_ms == 0) g->empty_since_ms = now_ms();
      if (now_ms() - g->empty_since_ms >= 10000) g->running = 0;
    } else {
      g->empty_since_ms = 0;
    }
  }
}

int server_run(int w, int h, int mode, int time_s, int world_type, const char* map_path, int max_players, int port_hint) {
  Game g;
  memset(&g, 0, sizeof(g));

  g.mode = mode;
  g.world_type = world_type;
  g.max_players = clampi(max_players, 1, MAX_PLAYERS);
  g.time_s = time_s;
  g.running = 1;

  g.world.w = clampi(w, 10, MAX_W);
  g.world.h = clampi(h, 10, MAX_H);
  g.world.wrap = (world_type == NO_OBS) ? 1 : 0;

  if (map_path) strncpy(g.map_path, map_path, MAP_PATH_MAX - 1);

  world_clear(&g.world);
  if (world_type == OBS_FILE) {
    if (g.map_path[0] == '\0') {
      fprintf(stderr, "OBS_FILE vyžaduje map_path\n");
      return 1;
    }
    if (world_load_file(&g.world, g.map_path) != 0) {
      fprintf(stderr, "Nepodarilo sa načítať mapu: %s\n", g.map_path);
      return 1;
    }
  }

  g.listenfd = net_listen(port_hint);
  if (g.listenfd < 0) {
    perror("net_listen");
    return 1;
  }
  g.port = net_port_of_listenfd(g.listenfd);
  if (g.port < 0) {
    perror("getsockname");
    close(g.listenfd);
    return 1;
  }

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 20000;
  setsockopt(g.listenfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

 ServerInfo si;
memset(&si, 0, sizeof(si));
si.pid = getpid();
si.port = g.port;
reg_add(&si);

  srand((unsigned int)(now_ms() ^ (long)getpid()));

  g.start_ms = now_ms();
  if (g.mode == TIMED) g.end_ms = g.start_ms + (long)g.time_s * 1000L;

  printf("SERVER PID=%d PORT=%d w=%d h=%d mode=%d world=%d max=%d\n",
         (int)getpid(), g.port, g.world.w, g.world.h, g.mode, g.world_type, g.max_players);
  fflush(stdout);

  long last_snap = 0;

  while (g.running) {
    int flags = fcntl(g.listenfd, F_GETFL, 0);
    fcntl(g.listenfd, F_SETFL, flags | O_NONBLOCK);

    int cfd = net_accept(g.listenfd);
    if (cfd >= 0) {
      Msg m;
      memset(&m, 0, sizeof(m));
      if (net_recv_msg(cfd, &m) == 0 && m.type == JOIN) {
        handle_join(&g, cfd, &m);
      } else {
        close(cfd);
      }
    }

    for (int pid = 0; pid < MAX_PLAYERS; pid++) {
      if (!g.players[pid].used) continue;
      int fd = g.players[pid].fd;
      if (fd < 0) continue;

      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      Msg m;
      memset(&m, 0, sizeof(m));
      int rc = recv_one_nonblock(fd, &m);
      if (rc == 0) {
        if (m.type == INPUT && m.len == (int)sizeof(int)) {
          int dir;
          memcpy(&dir, m.data, sizeof(int));
          handle_input(&g, pid, dir);
        } else if (m.type == PAUSE) {
          handle_pause(&g, pid);
        } else if (m.type == RESUME) {
          handle_resume(&g, pid);
        } else if (m.type == LEAVE) {
          handle_leave(&g, pid);
        } else if (m.type == RESPAWN) {
          handle_respawn(&g, pid);
        }
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          handle_leave(&g, pid);
        }
      }
    }

    server_tick(&g);

    long t = now_ms();
    if (t - last_snap >= 100) {
      broadcast_snapshot(&g);
      last_snap = t;
      //registry_update(&g);
    }

    sleep_ms(30);
  }

  send_game_end(&g);
  reg_remove(getpid());

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (g.players[i].used && g.players[i].fd >= 0) close(g.players[i].fd);
  }
  close(g.listenfd);

  return 0;
}

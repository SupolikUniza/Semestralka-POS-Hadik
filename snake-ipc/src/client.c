#include "client.h"
#include "common.h"

#include <curses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void ui_init(void) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);
}

static void ui_end(void) {
  endwin();
}

static void ui_draw_center(const char* s, int row) {
  int cols = COLS;
  int x = (cols - (int)strlen(s)) / 2;
  if (x < 0) x = 0;
  mvprintw(row, x, "%s", s);
}

static int ui_menu(void) {
  clear();
  ui_draw_center("SNAKE IPC (POSIX)", 2);
  ui_draw_center("1) Nova hra", 5);
  ui_draw_center("2) Pripojit k hre", 6);
  ui_draw_center("3) Koniec", 7);
  ui_draw_center("Stlac 1/2/3", 10);
  refresh();

  for (;;) {
    int ch = getch();
    if (ch == '1') return 1;
    if (ch == '2') return 2;
    if (ch == '3' || ch == 'q' || ch == 'Q') return 3;
    sleep_ms(20);
  }
}

static int ui_prompt_int(const char* label, int defv) {
  echo();
  nodelay(stdscr, FALSE);
  curs_set(1);

  clear();
  mvprintw(2, 2, "%s (default %d): ", label, defv);
  refresh();

  char buf[64];
  memset(buf, 0, sizeof(buf));
  getnstr(buf, 63);

  noecho();
  nodelay(stdscr, TRUE);
  curs_set(0);

  if (buf[0] == '\0') return defv;
  return atoi(buf);
}

static void ui_prompt_str(const char* label, char* out, int outsz, const char* defv) {
  echo();
  nodelay(stdscr, FALSE);
  curs_set(1);

  clear();
  mvprintw(2, 2, "%s (default %s): ", label, defv ? defv : "");
  refresh();

  char buf[256];
  memset(buf, 0, sizeof(buf));
  getnstr(buf, 255);

  noecho();
  nodelay(stdscr, TRUE);
  curs_set(0);

  if (buf[0] == '\0' && defv) strncpy(out, defv, (size_t)outsz - 1);
  else strncpy(out, buf, (size_t)outsz - 1);
}

static int spawn_server_process(int w, int h, int mode, int time_s, int world, const char* map_path, int maxp, int* out_server_pid) {
  pid_t pid = fork();
  if (pid < 0) return -1;

  if (pid == 0) {
    char sw[32], sh[32], smode[32], stime[32], sworld[32], smax[32], sport[32];
    snprintf(sw, sizeof(sw), "%d", w);
    snprintf(sh, sizeof(sh), "%d", h);
    snprintf(stime, sizeof(stime), "%d", time_s);
    snprintf(smax, sizeof(smax), "%d", maxp);
    snprintf(sport, sizeof(sport), "%d", 0);
    snprintf(smode, sizeof(smode), "%s", (mode == TIMED) ? "timed" : "standard");
    snprintf(sworld, sizeof(sworld), "%s", (world == OBS_FILE) ? "obs_file" : "no_obs");

    if (world == OBS_FILE && map_path && map_path[0]) {
      execl("./server", "./server",
            "--w", sw, "--h", sh,
            "--mode", smode,
            "--time", stime,
            "--world", sworld,
            "--map", map_path,
            "--max", smax,
            "--port", sport,
            (char*)NULL);
    } else {
      execl("./server", "./server",
            "--w", sw, "--h", sh,
            "--mode", smode,
            "--time", stime,
            "--world", sworld,
            "--max", smax,
            "--port", sport,
            (char*)NULL);
    }

    _exit(1);
  }

  /* parent */
  if (out_server_pid) *out_server_pid = (int)pid;
  return 0;
}

static int connect_and_join(Client* c, int port) {
  int fd = net_connect("127.0.0.1", port);
  if (fd < 0) return -1;

  Msg j;
  memset(&j, 0, sizeof(j));
  j.type = JOIN;
  j.len = 0;
  if (net_send_msg(fd, &j) != 0) { close(fd); return -1; }

  Msg r;
  memset(&r, 0, sizeof(r));
  if (net_recv_msg(fd, &r) != 0) { close(fd); return -1; }

  if (r.type == JOIN_OK && r.len == (int)sizeof(int)) {
    int pid;
    memcpy(&pid, r.data, sizeof(int));
    c->fd = fd;
    c->connected = 1;
    c->player_id = pid;
    return 0;
  }

  close(fd);
  return -1;
}

static void send_simple(Client* c, int type) {
  Msg m; memset(&m, 0, sizeof(m));
  m.type = type;
  m.len = 0;
  net_send_msg(c->fd, &m);
}

static void send_input(Client* c, int dir) {
  Msg m; memset(&m, 0, sizeof(m));
  m.type = INPUT;
  m.len = (int)sizeof(int);
  memcpy(m.data, &dir, sizeof(int));
  net_send_msg(c->fd, &m);
}

static void* net_thread(void* arg) {
  Client* c = (Client*)arg;
  while (c->connected) {
    Msg m;
    memset(&m, 0, sizeof(m));
    if (net_recv_msg(c->fd, &m) != 0) {
      break;
    }

    pthread_mutex_lock(&c->mx);
    if (m.type == SNAPSHOT) {
      c->last_snapshot = m;
      c->has_snapshot = 1;
    } else if (m.type == GAME_END) {
      long ms = 0;
      int off = 0;
      memcpy(&ms, m.data + off, sizeof(long)); off += (int)sizeof(long);
      c->final_ms = ms;
      for (int i = 0; i < 8; i++) {
        int sc = 0;
        memcpy(&sc, m.data + off, sizeof(int)); off += (int)sizeof(int);
        c->final_scores[i] = sc;
      }
      c->game_end = 1;
    }
    pthread_mutex_unlock(&c->mx);
  }

  c->connected = 0;
  return NULL;
}

static void draw_game_from_snapshot(const Msg* s, int my_pid) {
  int off = 0;
  int w = 0, h = 0;
  long game_ms = 0;

  memcpy(&w, s->data + off, sizeof(int)); off += (int)sizeof(int);
  memcpy(&h, s->data + off, sizeof(int)); off += (int)sizeof(int);
  memcpy(&game_ms, s->data + off, sizeof(long)); off += (int)sizeof(long);

  int scores[8], alive[8], paused[8];
  for (int i = 0; i < 8; i++) { memcpy(&scores[i], s->data + off, sizeof(int)); off += (int)sizeof(int); }
  for (int i = 0; i < 8; i++) { memcpy(&alive[i],  s->data + off, sizeof(int)); off += (int)sizeof(int); }
  for (int i = 0; i < 8; i++) { memcpy(&paused[i], s->data + off, sizeof(int)); off += (int)sizeof(int); }

  const char* board = s->data + off;

  clear();

  mvprintw(0, 2, "PID=%d  Time=%.1fs  (Arrows/WASD move, P pause/resume, Q leave, R respawn)",
           my_pid, (double)game_ms / 1000.0);

  int row = 2;
  for (int y = 0; y < h; y++) {
    move(row + y, 2);
    for (int x = 0; x < w; x++) {
      char c = board[y * w + x];
      addch(c);
    }
  }

  int info_row = row + h + 1;
  mvprintw(info_row, 2, "Scores:");
  for (int i = 0; i < 8; i++) {
    if (scores[i] != 0 || alive[i] != 0 || paused[i] != 0 || i == my_pid) {
      mvprintw(info_row + 1 + i, 2, "P%d score=%d %s %s",
               i + 1, scores[i],
               alive[i] ? "ALIVE" : "----",
               paused[i] ? "PAUSED" : "");
    }
  }

  refresh();
}




static int find_port_by_server_pid(int server_pid) {
  /* cak�me max ~3 sekundy, k�m server zap�e PID+port do registry */
  for (int tries = 0; tries < 60; tries++) {   // 60 * 50ms = 3000ms
    ServerInfo list[64];
    int n = reg_list(list, 64);
    if (n > 0) {
      for (int i = 0; i < n; i++) {
        if (list[i].pid == server_pid && list[i].port > 0) {
          return list[i].port;
        }
      }
    }
    sleep_ms(50);
  }
  return -1;
}




int client_run(void) {
  Client c;
  memset(&c, 0, sizeof(c));
  c.fd = -1;
  pthread_mutex_init(&c.mx, NULL);

  ui_init();

  for (;;) {
    int choice = ui_menu();
    if (choice == 3) break;

    if (choice == 1) {
      int w = ui_prompt_int("Sirka sveta", 40);
      int h = ui_prompt_int("Vyska sveta", 20);
      int mode = ui_prompt_int("Mode: 0=STANDARD, 1=TIMED", 0);
      int time_s = ui_prompt_int("Cas hry (sek) pre TIMED", 60);
      int world = ui_prompt_int("World: 0=NO_OBS, 1=OBS_FILE", 0);
      char map_path[MAP_PATH_MAX]; map_path[0] = '\0';
      if (world == OBS_FILE) {
        ui_prompt_str("Cesta k mape", map_path, MAP_PATH_MAX, "maps/example1.txt");
      }
      int maxp = ui_prompt_int("Max hracov", 4);

      
int server_pid = -1;
if (spawn_server_process(w, h, mode ? TIMED : STANDARD, time_s,
                         world ? OBS_FILE : NO_OBS, map_path, maxp,
                         &server_pid) != 0) {
  clear(); mvprintw(2,2,"Nepodarilo sa spustit server proces."); refresh();
  sleep_ms(800);
  continue;
}

int port = find_port_by_server_pid(server_pid);
if (port <= 0) {
  clear(); mvprintw(2,2,"Neviem najst server v registry (pid=%d).", server_pid); refresh();
  sleep_ms(1200);
  continue;
}





      if (connect_and_join(&c, port) != 0) {
        clear(); mvprintw(2,2,"Nepodarilo sa pripojit k serveru (port %d).", port); refresh();
        sleep_ms(800);
        continue;
      }
    }

    if (choice == 2) {
      ServerInfo list[32];
      int n = reg_list(list, 32);

      int port = 0;
      if (n > 0) {
        clear();
        mvprintw(2,2,"Dostupne hry:");
        for (int i = 0; i < n; i++) {
          mvprintw(4+i, 2, "%d) pid=%d port=%d", i+1, list[i].pid, list[i].port);
        }
        port = ui_prompt_int("Zadaj port (alebo skopiruj z listu)", list[0].port);
        refresh();
      } else {
        port = ui_prompt_int("Zadaj port servera", 0);
      }

      if (connect_and_join(&c, port) != 0) {
        clear(); 
        mvprintw(2,2,"Nepodarilo sa pripojit k serveru (port %d).", port); refresh();
        sleep_ms(800);
        continue;
      }
    }

    pthread_t th;
    pthread_create(&th, NULL, net_thread, &c);

    while (c.connected) {
      int ch = getch();
      if (ch == KEY_UP || ch == 'w' || ch == 'W') send_input(&c, UP);
      else if (ch == KEY_DOWN || ch == 's' || ch == 'S') send_input(&c, DOWN);
      else if (ch == KEY_LEFT || ch == 'a' || ch == 'A') send_input(&c, LEFT);
      else if (ch == KEY_RIGHT || ch == 'd' || ch == 'D') send_input(&c, RIGHT);
      else if (ch == 'p' || ch == 'P') {
        if (!c.paused) { send_simple(&c, PAUSE); c.paused = 1; }
        else { send_simple(&c, RESUME); c.paused = 0; }
      } else if (ch == 'r' || ch == 'R') {
        send_simple(&c, RESPAWN);
      } else if (ch == 'q' || ch == 'Q') {
        send_simple(&c, LEAVE);
        break;
      }

      pthread_mutex_lock(&c.mx);
      if (c.has_snapshot) {
        Msg snap = c.last_snapshot;
        pthread_mutex_unlock(&c.mx);
        draw_game_from_snapshot(&snap, c.player_id);
      } else {
        pthread_mutex_unlock(&c.mx);
      }

      pthread_mutex_lock(&c.mx);
      int end = c.game_end;
      long fms = c.final_ms;
      int fs[8];
      for (int i = 0; i < 8; i++) fs[i] = c.final_scores[i];
      pthread_mutex_unlock(&c.mx);

      if (end) {
        clear();
        mvprintw(2,2,"GAME END  time=%.1fs", (double)fms/1000.0);
        for (int i = 0; i < 8; i++) {
          mvprintw(4+i,2,"P%d score=%d", i+1, fs[i]);
        }
        mvprintw(14,2,"Stlac lubovolnu klavesu...");
        nodelay(stdscr, FALSE);
        getch();
        nodelay(stdscr, TRUE);
        break;
      }

      sleep_ms(20);
    }

    if (c.fd >= 0) close(c.fd);
    c.fd = -1;
    c.connected = 0;
    c.has_snapshot = 0;
    c.game_end = 0;
    c.paused = 0;

    pthread_join(th, NULL);
  }

  ui_end();
  pthread_mutex_destroy(&c.mx);
  return 0;
}

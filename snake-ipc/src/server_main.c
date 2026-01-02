#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* usage:
   server --w 40 --h 20 --mode standard|timed --time 60 --world no_obs|obs_file --map maps/example1.txt --max 4 --port 0
*/
int main(int argc, char** argv) {
  int w = 40, h = 20;
  int mode = STANDARD;
  int time_s = 60;
  int world = NO_OBS;
  char map[MAP_PATH_MAX]; map[0] = '\0';
  int maxp = 4;
  int port = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--w") && i + 1 < argc) w = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--h") && i + 1 < argc) h = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
      const char* s = argv[++i];
      mode = (!strcmp(s, "timed")) ? TIMED : STANDARD;
    } else if (!strcmp(argv[i], "--time") && i + 1 < argc) time_s = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--world") && i + 1 < argc) {
      const char* s = argv[++i];
      world = (!strcmp(s, "obs_file")) ? OBS_FILE : NO_OBS;
    } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
      strncpy(map, argv[++i], MAP_PATH_MAX - 1);
    } else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxp = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
  }

  const char* map_path = (map[0] ? map : NULL);
  return server_run(w, h, mode, time_s, world, map_path, maxp, port);
}

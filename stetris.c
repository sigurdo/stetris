#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <err.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/ioctl.h>
#include <asm/ioctl.h>
#include <linux/fb.h>
#include <errno.h>


// The game state can be used to detect what happens on the playfield
#define GAMEOVER   0
#define ACTIVE     (1 << 0)
#define ROW_CLEAR  (1 << 1)
#define TILE_ADDED (1 << 2)

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct {
  bool occupied;
} tile;

typedef struct {
  unsigned int x;
  unsigned int y;
} coord;

typedef struct {
  coord const grid;                     // playfield bounds
  unsigned long const uSecTickTime;     // tick rate
  unsigned long const rowsPerLevel;     // speed up after clearing rows
  unsigned long const initNextGameTick; // initial value of nextGameTick

  unsigned int tiles; // number of tiles played
  unsigned int rows;  // number of rows cleared
  unsigned int score; // game score
  unsigned int level; // game level

  tile *rawPlayfield; // pointer to raw memory of the playfield
  tile **playfield;   // This is the play field array
  unsigned int state;
  coord activeTile;                       // current tile

  unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                              // when reached 0, next game state calculated
  unsigned long nextGameTick; // sets when tick is wrapping back to zero
                              // lowers with increasing level, never reaches 0
} gameConfig;



gameConfig game = {
                   .grid = {8, 8},
                   .uSecTickTime = 10000,
                   .rowsPerLevel = 2,
                   .initNextGameTick = 50,
};


// Custom macros:
#define RD_VALUE _IO('a', 'b')


// Custom types:
// Framebuffer pixel struct type
typedef struct {
  int r:5;
  int g:6;
  int b:5;
} fb_pixel_t;

// Custom variables:
int                       fd;                    // Joystick events file descriptor
struct pollfd             pollfd_struct;         // Joystick events pollfd struct
char                     *eventX;                // Joystick events memory mapped
struct stat               statbuf;               // Joystick events statbuf
struct input_event        eventX_struct;         // Joystick events read struct
int                       fb_fd;                 // Framebuffer file descriptor
char                     *fb_dev_path;           // Framebuffer device file path
struct stat               fb_statbuf;            // Framebuffer statbuf
struct fb_fix_screeninfo  fb_fscreeninfo;        // Framebuffer fixed screeninfo struct
struct fb_var_screeninfo  fb_vscreeninfo;        // Framebuffer varialbe screeninfo struct
short                    *fb;                    // Framebuffer memory mapped
fb_pixel_t                fb_pixel;              // Framebuffer pixel
fb_pixel_t                fb_test;               // Framebuffer struct for testing
fb_pixel_t                colorfield[8 * 8 * sizeof(fb_pixel_t)]; // Array of colors for pixel at colorfield[x + (8 * y)]
int                       color_hue_next;        // Next color hue to apply
int                       double_input_toggler;  // Used to fix double input problem

// Test variables:
char *read_buf;


// Custom functions:
int color_from_hue(int hue, int offset, int hue_max, int max) {
  int hue_scale = max / hue_max;
  int res = 0;
  hue = (hue + offset) % hue_max;
  if (hue < hue_max / 2) res = max - 2 * hue_scale * hue;
  else res = 2 * hue_scale * hue - max;
  if (res < 0) res = 0;
  if (res > max) res = max;
  return res;
}

short color_short_from_struct(fb_pixel_t* pixel) {
  return (short) ((pixel->r & 0b11111) << (5 + 6)) | ((pixel->g & 0b111111) << 5) | ((pixel->b & 0b11111) << 0);
}


void testing() {
  printf("Skal prøve...\n");
  if ((fd = open("/dev/input/eventX", O_RDWR, 0)) == -1)
    err(1, "open failed\n");
  if (fstat(fd, &statbuf))
    err(1, "fstat failed");
  printf("fd: %d\n", fd);
  printf("size: %d\n", statbuf.st_size);
  // printf("venter litt...\n");
  // usleep(3000000);
  read_buf = malloc(1000);
  write(fd, "X", 1);
  read(fd, read_buf, statbuf.st_size);
  ioctl(fd, RD_VALUE, read_buf);
  printf("read byte: %d\n", read_buf[0]);
  eventX = (char*) mmap(NULL, statbuf.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  printf("eventX addr: 0x%x\n", eventX);
  if (eventX == MAP_FAILED)
    err(1, "mmap failed");
  close(fd);
  for (int i = 0; i < 10; i++) {
    printf("Teller: %d\n", i);
    printf("eventX: %c\n", *(eventX));
    usleep(1000000);
  }
  printf("Sånn!\n");
  err(1, "hahahahah\n");

  read_buf = malloc(statbuf.st_size);
  read_buf[0] = 5;
  for (int i = 0; i < 5; i++)
    printf("read_buf[%d]: %d\n", i, read_buf[i]);

  printf("venter litt...\n");
  usleep(3000000);

  int read_ret = read(fd, &eventX_struct, sizeof(eventX_struct));
  printf("read_ret: %d\n", read_ret);
  printf("errno: %d\n", errno);
  printf("event: %d, %d, %d, %d\n", eventX_struct.time, eventX_struct.type, eventX_struct.code, eventX_struct.value);
  usleep(1000);
}

int open_joystick(int *fd, char *wanted_name) {
  char device_path [1000];
  for (int i = 0; i < 32; i++) {
    sprintf(device_path, "/dev/input/event%d", i);
    *fd = open(device_path, O_RDONLY);
    char name[1000];
    int ioctl_ret = ioctl(*fd, EVIOCGNAME(1000), name);
    if (ioctl_ret < 0) {
      printf("Could not find %s\n", device_path);
      continue;
    }

    printf("name: %s\n", name);

    int name_correct = 1;
    for (int j = 0; 1; j++) {
      if ((name[j] == (char) 0) || (wanted_name[j] == (char) 0)) {
        break;
      }
      if (name[j] != wanted_name[j]) {
        name_correct = 0;
        break;
      }
    }

    if (name_correct) {
      break;
    }

    // Return error if not even number 31 is correct
    if (i == 31) {
      return -1;
    }
  }

  return 0;
}

void hello_joystick() {
  // Hello joystick
  while (1) {
    if (open_joystick(&fd, "Raspberry Pi Sense HAT Joystick") < 0) {
      printf("open_joystick failed");
      return;
    }
    
    printf("reading...\n");
    read(fd, &eventX_struct, sizeof(eventX_struct));
    printf("done reading %d\n", eventX_struct.code);

    pollfd_struct.fd = fd;
    pollfd_struct.events = POLLIN;
    printf("polling...\n");
    poll(&pollfd_struct, 1, 3000);
    printf("poll returned!\n");
    while (read(fd, &eventX_struct, sizeof(eventX_struct)) > 0) {
      // printf("event: %d, %d, %d, %d\n", eventX_struct.time, eventX_struct.type, eventX_struct.code, eventX_struct.value);
      // printf("KEY_UP: %d, event code: %d\n", KEY_UP, eventX_struct.code);
      switch (eventX_struct.code) {
      case KEY_UP:
        printf("KEY_UP\n");
        break;
      case KEY_RIGHT:
        printf("RIGHT\n");
        break;
      case KEY_DOWN:
        printf("DOWN\n");
        break;
      case KEY_LEFT:
        printf("LEFT\n");
        break;
      case KEY_ENTER:
        printf("ENTER\n");
        break;
      case 0:
        printf("RELEASED\n");
        break;
      default:
        printf("Unknown key\n");
      }
    }
    printf("no more events to read\n");
    /* EAGAIN is returned when the queue is empty */
    if (errno != EAGAIN) {
      /* error */
      printf("error\n");
    }
    printf("empty\n");
    /* do something interesting with processed events */
  }
}

int open_framebuffer(short **fb, char *id) {
  for (int i = 0; i < 32; i++) {
    fb_dev_path = (char*) malloc(1000 * sizeof(char));
    sprintf(fb_dev_path, "/dev/fb%d", i);
    printf("opening %s\n", fb_dev_path);
    fb_fd = open(fb_dev_path, O_RDWR);
    int ioctl_ret = ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_fscreeninfo);

    printf("ioctl_ret: %d\n", ioctl_ret);
    ioctl_ret = ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_vscreeninfo);
    printf("ioctl_ret: %d\n", ioctl_ret);

    switch (errno) {
      case EBADF: printf("EBADF\n"); break;
      case EFAULT: printf("EFAULT\n"); break;
      case EINVAL: printf("EINVAL\n"); break;
      case ENOTTY: printf("ENOTTY\n"); break;
      case 0: printf("SUCCESS\n"); break;
      default: printf("none above\n"); break;
    }

    if (ioctl_ret < 0) {
      printf("could not find %s\n", fb_dev_path);
      continue;
    }

    printf("id: %s\n", fb_fscreeninfo.id);

    // char expected_id[1000] = "RPi-Sense FB";
    int id_correct = 1;
    for (int j = 0; 1; j++) {
      if ((fb_fscreeninfo.id[j] == (char) 0) || (id[j] == 0)) {
        break;
      }
      if (fb_fscreeninfo.id[j] != (char) id[j]) {
        id_correct = 0;
        break;
      }
    }

    if (id_correct) {
      break;
    }

    // Return error if not even number 31 is correct
    if (i == 31) {
      return -1;
    }
  }

  *fb = (short*) mmap(NULL, 2 * 8 * 8, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
  if (*fb == MAP_FAILED) printf("fb mmap failed\n");
  close(fb_fd);
}

void hello_framebuffer() {
  // Hello framebuffer:
  printf("Hello frambuffer\n");
  printf("Size: %d\n", sizeof(fb_pixel_t));
  fb_test.r = 15;
  fb_test.g = 7;
  fb_test.b = 3;
  printf("test: %x %x\n", *((__u8*) &fb_test), *(((__u8*) &fb_test) + 1));

  // Memory map framebuffer
  int open_fb_ret = open_framebuffer(&fb, "RPi-Sense FB");
  if (open_fb_ret < 0) {
    printf("Could not open framebuffer\n");
  }

  printf("write to fb\n");
  for (int counter = 0; 1; counter = (counter + 1) % 31) {
    usleep(20000);
    printf("test: %d %d\n",counter, color_from_hue(counter, 0, 31, 31) * 1);
    for (int i = 0; i < 8 * 8; i++) {
      fb_pixel.r = color_from_hue(counter,  0, 31, 31) * 1;
      fb_pixel.g = color_from_hue(counter, 16, 31, 31) * 2;
      fb_pixel.b = color_from_hue(counter, 21, 31, 31) * 1;
      fb[i] = color_short_from_struct(&fb_pixel);
    }
  }

  printf("sleep(1000)\n");
  sleep(1000);
}

// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
bool initializeSenseHat() {
  // Open joystick input file as fd
  if (open_joystick(&fd, "Raspberry Pi Sense HAT Joystick") < 0) {
    printf("Could not open joystick");
    return false;
  }

  // Set up poll struct
  pollfd_struct.fd = fd;
  pollfd_struct.events = POLLIN;

  // Initialize double_input_toggler
  double_input_toggler = 0;

  // Memory map framebuffer as fb
  if (open_framebuffer(&fb, "RPi-Sense FB") < 0) {
    printf("Could not open framebuffer\n");
    return false;
  }

  // Initialize color_hue_next
  color_hue_next = 0;
  
  // exit(1);

  return true;
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat() {
  close(fd);
  munmap(fb, 2 * 8 * 8);
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick() {
  // Default return value should be 0
  int keycode = 0;

  while (1) {
    // Poll eventX file with timeout of 1 ms and break out if it times out or fails
    int poll_ret = poll(&pollfd_struct, 1, 1);
    if (poll_ret <= 0) break;

    // Read input and set keycode if recognized
    read(fd, &eventX_struct, sizeof(eventX_struct));
    switch (eventX_struct.code) {
      case KEY_UP:
      case KEY_RIGHT:
      case KEY_DOWN:
      case KEY_LEFT:
      case KEY_ENTER:
        keycode = eventX_struct.code;
      default:
        break;
    }
  }
  // Hacky fix of double input problem
  if (keycode) {
    double_input_toggler = !double_input_toggler;
    if (double_input_toggler) keycode = 0;
  }
  return keycode;
}


// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged) {
  if (!playfieldChanged) return;
  for (int i = 0; i < 8 * 8; i++) {
    fb[i] = color_short_from_struct(&(colorfield[i]));;
  }
}


// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target) {
  game.playfield[target.y][target.x].occupied = true;
  colorfield[target.x + 8 * target.y].r = color_from_hue(color_hue_next,  0, 31, 31) * 1;
  colorfield[target.x + 8 * target.y].g = color_from_hue(color_hue_next, 10, 31, 31) * 2;
  colorfield[target.x + 8 * target.y].b = color_from_hue(color_hue_next, 21, 31, 31) * 1;
  color_hue_next = (color_hue_next + 13) % 31;
}

static inline void copyTile(coord const to, coord const from) {
  memcpy((void *) &game.playfield[to.y][to.x], (void *) &game.playfield[from.y][from.x], sizeof(tile));
  memcpy((void *) &colorfield[to.x + 8 * to.y], (void *) &colorfield[from.x + 8 * from.y], sizeof(fb_pixel_t));
}

static inline void copyRow(unsigned int const to, unsigned int const from) {
  memcpy((void *) &game.playfield[to][0], (void *) &game.playfield[from][0], sizeof(tile) * game.grid.x);
  memcpy((void *) &colorfield[8 * to], (void *) &colorfield[8 * from], sizeof(fb_pixel_t) * game.grid.x);
}

static inline void resetTile(coord const target) {
  memset((void *) &game.playfield[target.y][target.x], 0, sizeof(tile));
  memset((void *) &colorfield[target.x + 8 * target.y], 0, sizeof(fb_pixel_t));
}

static inline void resetRow(unsigned int const target) {
  memset((void *) &game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
  memset((void *) &colorfield[8 * target], 0, sizeof(fb_pixel_t) * game.grid.x);
}

static inline bool tileOccupied(coord const target) {
  return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target) {
  for (unsigned int x = 0; x < game.grid.x; x++) {
    coord const checkTile = {x, target};
    if (!tileOccupied(checkTile)) {
      return false;
    }
  }
  return true;
}


static inline void resetPlayfield() {
  for (unsigned int y = 0; y < game.grid.y; y++) {
    resetRow(y);
  }
}

// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile() {
  game.activeTile.y = 0;
  game.activeTile.x = (game.grid.x - 1) / 2;
  if (tileOccupied(game.activeTile))
    return false;
  newTile(game.activeTile);
  return true;
}

bool moveRight() {
  coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
  if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}

bool moveLeft() {
  coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
  if (game.activeTile.x > 0 && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool moveDown() {
  coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
  if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool clearRow() {
  if (rowOccupied(game.grid.y - 1)) {
    for (unsigned int y = game.grid.y - 1; y > 0; y--) {
      copyRow(y, y - 1);
    }
    resetRow(0);
    return true;
  }
  return false;
}

void advanceLevel() {
  game.level++;
  switch(game.nextGameTick) {
  case 1:
    break;
  case 2 ... 10:
    game.nextGameTick--;
    break;
  case 11 ... 20:
    game.nextGameTick -= 2;
    break;
  default:
    game.nextGameTick -= 10;
  }
}

void newGame() {
  game.state = ACTIVE;
  game.tiles = 0;
  game.rows = 0;
  game.score = 0;
  game.tick = 0;
  game.level = 0;
  resetPlayfield();
}

void gameOver() {
  game.state = GAMEOVER;
  game.nextGameTick = game.initNextGameTick;
}


bool sTetris(int const key) {
  bool playfieldChanged = false;

  if (game.state & ACTIVE) {
    // Move the current tile
    if (key) {
      playfieldChanged = true;
      switch(key) {
      case KEY_LEFT:
        moveLeft();
        break;
      case KEY_RIGHT:
        moveRight();
        break;
      case KEY_DOWN:
        while (moveDown()) {};
        game.tick = 0;
        break;
      default:
        playfieldChanged = false;
      }
    }

    // If we have reached a tick to update the game
    if (game.tick == 0) {
      // We communicate the row clear and tile add over the game state
      // clear these bits if they were set before
      game.state &= ~(ROW_CLEAR | TILE_ADDED);

      playfieldChanged = true;
      // Clear row if possible
      if (clearRow()) {
        game.state |= ROW_CLEAR;
        game.rows++;
        game.score += game.level + 1;
        if ((game.rows % game.rowsPerLevel) == 0) {
          advanceLevel();
        }
      }

      // if there is no current tile or we cannot move it down,
      // add a new one. If not possible, game over.
      if (!tileOccupied(game.activeTile) || !moveDown()) {
        if (addNewTile()) {
          game.state |= TILE_ADDED;
          game.tiles++;
        } else {
          gameOver();
        }
      }
    }
  }

  // Press any key to start a new game
  if ((game.state == GAMEOVER) && key) {
    playfieldChanged = true;
    newGame();
    addNewTile();
    game.state |= TILE_ADDED;
    game.tiles++;
  }

  return playfieldChanged;
}

int readKeyboard() {
  struct pollfd pollStdin = {
       .fd = STDIN_FILENO,
       .events = POLLIN
  };
  int lkey = 0;

  if (poll(&pollStdin, 1, 0)) {
    lkey = fgetc(stdin);
    if (lkey != 27)
      goto exit;
    lkey = fgetc(stdin);
    if (lkey != 91)
      goto exit;
    lkey = fgetc(stdin);
  }
 exit:
    switch (lkey) {
      case 10: return KEY_ENTER;
      case 65: return KEY_UP;
      case 66: return KEY_DOWN;
      case 67: return KEY_RIGHT;
      case 68: return KEY_LEFT;
    }
  return 0;
}

void renderConsole(bool const playfieldChanged) {
  if (!playfieldChanged)
    return;

  // Goto beginning of console
  fprintf(stdout, "\033[%d;%dH", 0, 0);
  for (unsigned int x = 0; x < game.grid.x + 2; x ++) {
    fprintf(stdout, "-");
  }
  fprintf(stdout, "\n");
  for (unsigned int y = 0; y < game.grid.y; y++) {
    fprintf(stdout, "|");
    for (unsigned int x = 0; x < game.grid.x; x++) {
      coord const checkTile = {x, y};
      fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
    }
    switch (y) {
      case 0:
        fprintf(stdout, "| Tiles: %10u\n", game.tiles);
        break;
      case 1:
        fprintf(stdout, "| Rows:  %10u\n", game.rows);
        break;
      case 2:
        fprintf(stdout, "| Score: %10u\n", game.score);
        break;
      case 4:
        fprintf(stdout, "| Level: %10u\n", game.level);
        break;
      case 7:
        fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
        break;
    default:
        fprintf(stdout, "|\n");
    }
  }
  for (unsigned int x = 0; x < game.grid.x + 2; x++) {
    fprintf(stdout, "-");
  }
  fflush(stdout);
}


inline unsigned long uSecFromTimespec(struct timespec const ts) {
  return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv) {
  (void) argc;
  (void) argv;
  // This sets the stdin in a special state where each
  // keyboard press is directly flushed to the stdin and additionally
  // not outputted to the stdout
  {
    struct termios ttystate;
    tcgetattr(STDIN_FILENO, &ttystate);
    ttystate.c_lflag &= ~(ICANON | ECHO);
    ttystate.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
  }

  // Allocate the playing field structure
  game.rawPlayfield = (tile *) malloc(game.grid.x * game.grid.y * sizeof(tile));
  game.playfield = (tile**) malloc(game.grid.y * sizeof(tile *));
  if (!game.playfield || !game.rawPlayfield) {
    fprintf(stderr, "ERROR: could not allocate playfield\n");
    return 1;
  }
  for (unsigned int y = 0; y < game.grid.y; y++) {
    game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
  }

  // Reset playfield to make it empty
  resetPlayfield();
  // Start with gameOver
  gameOver();

  if (!initializeSenseHat()) {
    fprintf(stderr, "ERROR: could not initilize sense hat\n");
    return 1;
  };

  // Clear console, render first time
  fprintf(stdout, "\033[H\033[J");
  renderConsole(true);
  renderSenseHatMatrix(true);

  while (true) {
    struct timeval sTv, eTv;
    gettimeofday(&sTv, NULL);

    int key = readSenseHatJoystick();
    if (!key)
      key = readKeyboard();
    if (key == KEY_ENTER)
      break;

    bool playfieldChanged = sTetris(key);
    renderConsole(playfieldChanged);
    renderSenseHatMatrix(playfieldChanged);

    // Wait for next tick
    gettimeofday(&eTv, NULL);
    unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
    if (uSecProcessTime < game.uSecTickTime) {
      usleep(game.uSecTickTime - uSecProcessTime);
    }
    game.tick = (game.tick + 1) % game.nextGameTick;
  }

  freeSenseHat();
  free(game.playfield);
  free(game.rawPlayfield);

  return 0;
}

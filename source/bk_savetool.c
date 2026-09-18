/* bk_savetool.c -- edit the game's save files at boot, from saves.txt.
 *
 * MIT licensed. See README.
 *
 * THE FORMAT
 * ----------
 * Banana Kong stores its profile with Defold's own sys.save, which is a
 * straight serialisation of one Lua table -- no encryption, no obfuscation:
 *
 *     "HDTB"            magic
 *     uint16 version    4
 *     uint16 reserved   0
 *     uint32 count      number of entries in the table
 *     entries:
 *       uint8  key type     4 = string, 3 = number
 *       uint8  value type   3 = number, 1 = boolean, 4 = string, 5 = table
 *       uint32 key length   (string keys)
 *       bytes  key          not NUL terminated
 *       <padding to a 4-byte boundary>
 *       value               number = 8-byte double, boolean = 1 byte,
 *                           string = uint32 length + bytes,
 *                           table = a nested count + entries
 *
 * Every value the profile uses is a number, because Lua has no integers here:
 * a banana count is the double 144.0.
 *
 * WHY EDIT IN PLACE
 * -----------------
 * A named key's 8 bytes are overwritten where they sit, so every other byte of
 * the file stays exactly as the engine wrote it. Reserialising the table would
 * mean guaranteeing this writer round-trips the engine's key order and number
 * spelling, and any difference is a save the engine may reject -- on data the
 * player cannot easily get back.
 *
 * A key the save does not have yet is appended instead: one entry at the end
 * and the count bumped by one. That is exact in this format (entries are
 * order-independent, the table is a Lua table), which is why it is safe here
 * when the same move on a text format would not be.
 *
 * The first time a file is touched, the original is copied to <name>.bak and
 * never overwritten after that. If an edit goes wrong the untouched save is
 * still there.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * It will not create a save that does not exist: the engine writes the profile
 * on first run, and inventing one risks skipping first-run setup. Play once,
 * then edit. It also refuses any file whose header is not the one above, and
 * any key whose current value is not a number.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "bk_savetool.h"
#include "config.h"
#include "log.h"

#define MAX_SAVE   (1024 * 1024)
#define MAX_EDITS  256
#define SAVE_VERSION 4

/* Lua type tags, as the serialiser writes them. */
#define T_BOOLEAN 1
#define T_NUMBER  3
#define T_STRING  4
#define T_TABLE   5

/* Groups only decide how the generated saves.txt is laid out. */
enum { G_CURRENCY, G_RECORD, G_HAT, G_PARACHUTE, G_POWERUP, G_UTILITY, G_IAP, G_TOGGLE, G_STAT };

typedef struct { const char *name; unsigned char group; } Field;

/* Every field of the game's profile, read out of its own
 * modules/save/profile_data.lua (the NAMES list it asserts is 255 long).
 * Any of them can be written; the groups only order the template. */
static const Field FIELDS[] = {
  { "bestDistance", G_RECORD },
  { "interstitialsOff", G_TOGGLE },
  { "removeAds", G_TOGGLE },
  { "soundMuted", G_TOGGLE },
  { "musicMuted", G_TOGGLE },
  { "tutorialEnabled", G_TOGGLE },
  { "cloudEnabled", G_TOGGLE },
  { "distanceTotal", G_STAT },
  { "bananaBest", G_CURRENCY },
  { "bananaTotal", G_CURRENCY },
  { "completedObjectivesCount", G_STAT },
  { "toucanDistanceBest", G_STAT },
  { "toucanDistanceTotal", G_STAT },
  { "boarDistanceBest", G_STAT },
  { "boarDistanceTotal", G_STAT },
  { "caveDistanceBest", G_STAT },
  { "caveDistanceTotal", G_STAT },
  { "treetopDistanceBest", G_STAT },
  { "treetopDistanceTotal", G_STAT },
  { "ropesRodeTotal", G_STAT },
  { "destroyedRocksTotal", G_STAT },
  { "glidedDistanceTotal", G_STAT },
  { "lianaSweetSpotTotal", G_STAT },
  { "timesEnteredTreetopTotal", G_STAT },
  { "timesEnteredCaveTotal", G_STAT },
  { "timesFellLavaPitTotal", G_STAT },
  { "destroyedStalagtitesTotal", G_STAT },
  { "destroyedDinosaurHeadsTotal", G_STAT },
  { "waterBouncesTotal", G_STAT },
  { "simpleBoostUsedTotal", G_STAT },
  { "doubleBoostUsedTotal", G_STAT },
  { "powerDashUsedTotal", G_STAT },
  { "reviveUsedTotal", G_STAT },
  { "rainbowBananaEatenTotal", G_STAT },
  { "magnetBananasTotal", G_STAT },
  { "deathsBySmashingObstacleTotal", G_STAT },
  { "destroyedObstaclesTotal", G_STAT },
  { "destroyedBananaRocksTotal", G_STAT },
  { "giraffeBouncesTotal", G_STAT },
  { "timesRodeToucan", G_STAT },
  { "timesRodeBoar", G_STAT },
  { "jumpedFlowersTotal", G_STAT },
  { "spiderWebTotal", G_STAT },
  { "batsTotal", G_STAT },
  { "barrelTotal", G_STAT },
  { "waterTotal", G_STAT },
  { "deathsByBananaBallTotal", G_STAT },
  { "underwaterAccess", G_TOGGLE },
  { "grabbedTurtles", G_STAT },
  { "treasuresTaken", G_STAT },
  { "streamsTaken", G_STAT },
  { "underwaterBanana", G_STAT },
  { "underwaterDistance", G_STAT },
  { "streamsWithPranha", G_STAT },
  { "underwaterSwipedowns", G_STAT },
  { "electrocutedByEel", G_STAT },
  { "turtleKnocks", G_STAT },
  { "jungleDistance", G_STAT },
  { "crocodileHit", G_STAT },
  { "bananaDash", G_STAT },
  { "underwaterRainbowBanana", G_STAT },
  { "crocodileBite", G_STAT },
  { "piranhaBite", G_STAT },
  { "blowfishHit", G_STAT },
  { "doubleBananaUsed", G_STAT },
  { "distanceWithScuba", G_STAT },
  { "hatsBought", G_STAT },
  { "capesBought", G_STAT },
  { "bananasWithDoubleBanana", G_STAT },
  { "bananasWithTreasure", G_STAT },
  { "hat1", G_HAT },
  { "hat2", G_HAT },
  { "hat3", G_HAT },
  { "hat4", G_HAT },
  { "hat5", G_HAT },
  { "hat6", G_HAT },
  { "hat7", G_HAT },
  { "hat8", G_HAT },
  { "hat9", G_HAT },
  { "hat10", G_HAT },
  { "hat11", G_HAT },
  { "hat12", G_HAT },
  { "hat13", G_HAT },
  { "hat14", G_HAT },
  { "hat15", G_HAT },
  { "hat16", G_HAT },
  { "hat17", G_HAT },
  { "hat18", G_HAT },
  { "hat19", G_HAT },
  { "hat20", G_HAT },
  { "hat21", G_HAT },
  { "hat22", G_HAT },
  { "hat23", G_HAT },
  { "hat24", G_HAT },
  { "hat25", G_HAT },
  { "hat26", G_HAT },
  { "hat27", G_HAT },
  { "hat28", G_HAT },
  { "hat29", G_HAT },
  { "hat30", G_HAT },
  { "hat31", G_HAT },
  { "hat32", G_HAT },
  { "hat33", G_HAT },
  { "hat34", G_HAT },
  { "hat35", G_HAT },
  { "hat36", G_HAT },
  { "hat37", G_HAT },
  { "hat38", G_HAT },
  { "parachute1", G_PARACHUTE },
  { "parachute2", G_PARACHUTE },
  { "parachute3", G_PARACHUTE },
  { "parachute4", G_PARACHUTE },
  { "parachute5", G_PARACHUTE },
  { "parachute6", G_PARACHUTE },
  { "parachute7", G_PARACHUTE },
  { "parachute8", G_PARACHUTE },
  { "parachute9", G_PARACHUTE },
  { "parachute10", G_PARACHUTE },
  { "parachute11", G_PARACHUTE },
  { "parachute12", G_PARACHUTE },
  { "parachute13", G_PARACHUTE },
  { "parachute14", G_PARACHUTE },
  { "parachute15", G_PARACHUTE },
  { "parachute16", G_PARACHUTE },
  { "parachute17", G_PARACHUTE },
  { "bananaRainbowPowerup", G_POWERUP },
  { "toucanPowerup", G_POWERUP },
  { "magnetPowerup", G_POWERUP },
  { "boarPowerup", G_POWERUP },
  { "giraffePowerup", G_POWERUP },
  { "glidePowerup", G_POWERUP },
  { "turtlePowerup", G_POWERUP },
  { "treasurePowerup", G_POWERUP },
  { "snakePowerup", G_POWERUP },
  { "oneUpUtil", G_UTILITY },
  { "simpleBoostUtil", G_UTILITY },
  { "doubleBoostUtil", G_UTILITY },
  { "fullPowerBarUtil", G_UTILITY },
  { "waterBounceUtil", G_UTILITY },
  { "doubleBananasUtil", G_UTILITY },
  { "bananaDashUtil", G_UTILITY },
  { "bananabundle1", G_IAP },
  { "bananabundle2", G_IAP },
  { "bananabundle3", G_IAP },
  { "bananabundle4", G_IAP },
  { "heartbundle1", G_IAP },
  { "heartbundle2", G_IAP },
  { "heartbundle3", G_IAP },
  { "heartbundle4", G_IAP },
  { "megaSaleItem", G_IAP },
  { "luckyCharmItem", G_IAP },
  { "goldenBananaItem", G_IAP },
  { "bananaCurrent", G_CURRENCY },
  { "goldenHeartCurrent", G_CURRENCY },
  { "isMagnetNew", G_TOGGLE },
  { "isRainbowNew", G_TOGGLE },
  { "caveShowMsg", G_TOGGLE },
  { "birdShowMsg", G_TOGGLE },
  { "hogShowMsg", G_TOGGLE },
  { "pipeShowMsg", G_TOGGLE },
  { "turtleShowMsg", G_TOGGLE },
  { "notificationsOn", G_TOGGLE },
  { "alreadyRated", G_TOGGLE },
  { "showICloudMsg", G_TOGGLE },
  { "playingTime", G_STAT },
  { "timesPlayed", G_STAT },
  { "tapjoyShop", G_STAT },
  { "jumpedFrogsTotal", G_STAT },
  { "swipedDownFrogsTotal", G_STAT },
  { "upPassedCrabsTotal", G_STAT },
  { "downPassedCrabsTotal", G_STAT },
  { "sinkedCastlesTotal", G_STAT },
  { "crabsTotal", G_STAT },
  { "coconutsTotal", G_STAT },
  { "snakesTotal", G_STAT },
  { "avoidedBalloonsTotal", G_STAT },
  { "readDataFromOldFileOnlyOnce", G_STAT },
  { "goldenHeartAcquired", G_STAT },
  { "goldenHeartUsed", G_STAT },
  { "goldenHeartFree", G_STAT },
  { "IAPsTotal", G_IAP },
  { "timesLoaded", G_STAT },
  { "promoVideoBanana", G_STAT },
  { "videoPromoBananaLastDate", G_STAT },
  { "videoPromoHeartLastDate", G_STAT },
  { "videoPromoBananaTimesUsed", G_STAT },
  { "removeAdsItem", G_IAP },
  { "bananaBundleFree", G_IAP },
  { "distanceSnake", G_STAT },
  { "beachDistanceBest", G_STAT },
  { "beachDistanceTotal", G_STAT },
  { "totalBalloons", G_STAT },
  { "totalFrogJumps", G_STAT },
  { "totalSnakes", G_STAT },
  { "totalRockets", G_STAT },
  { "totalSandCastles", G_STAT },
  { "totalCanopyPlants", G_STAT },
  { "grabbedMagnets", G_STAT },
  { "glidedDistanceBest", G_STAT },
  { "snakeShowMsg", G_STAT },
  { "deathsAgainstWallTotal", G_STAT },
  { "showVideoAdFirstTime", G_TOGGLE },
  { "coconutDeathsTotal", G_STAT },
  { "avoidStartSignIn", G_TOGGLE },
  { "usedGoldenHeartPromo", G_STAT },
  { "usedVideoPromoHeart", G_STAT },
  { "runsWhenBananaItemBought", G_STAT },
  { "currentPromoIndex", G_STAT },
  { "promoLockedHour", G_STAT },
  { "promoUnlockRun", G_STAT },
  { "beachAccess", G_TOGGLE },
  { "beachBanana", G_STAT },
  { "jungleBanana", G_STAT },
  { "caveBanana", G_STAT },
  { "treetopBanana", G_STAT },
  { "dashedObstacles", G_STAT },
  { "treetopBounces", G_STAT },
  { "cumulativeMissionCounter1", G_STAT },
  { "cumulativeMissionCounter2", G_STAT },
  { "cumulativeMissionCounter3", G_STAT },
  { "cumulativeMissionCounter4", G_STAT },
  { "cumulativeMissionCounter5", G_STAT },
  { "cumulativeMissionCounter6", G_STAT },
  { "cumulativeMissionCounter7", G_STAT },
  { "cumulativeMissionCounter8", G_STAT },
  { "cumulativeMissionCounter9", G_STAT },
  { "cumulativeMissionCounter10", G_STAT },
  { "cumulativeMissionCounter11", G_STAT },
  { "cumulativeMissionCounter12", G_STAT },
  { "cumulativeMissionCounter13", G_STAT },
  { "cumulativeMissionCounter14", G_STAT },
  { "cumulativeMissionCounter15", G_STAT },
  { "cumulativeMissionCounter16", G_STAT },
  { "cumulativeMissionCounter17", G_STAT },
  { "cumulativeMissionCounter18", G_STAT },
  { "cumulativeMissionCounter19", G_STAT },
  { "cumulativeMissionCounter20", G_STAT },
  { "cumulativeMissionCounter21", G_STAT },
  { "cumulativeMissionCounter22", G_STAT },
  { "cumulativeMissionCounter23", G_STAT },
  { "cumulativeMissionCounter24", G_STAT },
  { "cumulativeMissionCounter25", G_STAT },
  { "cumulativeMissionCounter26", G_STAT },
  { "cumulativeMissionCounter27", G_STAT },
  { "cumulativeMissionCounter28", G_STAT },
  { "cumulativeMissionCounter29", G_STAT },
  { "cumulativeMissionCounter30", G_STAT },
  { "cumulativeMissionCounter31", G_STAT },
  { "missionSkipedDateSeconds", G_STAT },
  { "scoreMultiplier", G_RECORD },
  { "completedMissionsSinceReward", G_STAT },
  { "currentRewardIndex", G_STAT },
  { "bestScore", G_RECORD },
  { "cumulativeMissionPointer", G_STAT },
  { "receivedRewards", G_STAT },
};

#define N_FIELDS ((int)(sizeof(FIELDS) / sizeof(*FIELDS)))

typedef struct { char file[32]; char key[64]; double value; } Edit;
static Edit g_edits[MAX_EDITS];
static int  g_nedits;

/* ------------------------------------------------------------------ */
/* saves.txt                                                           */
/* ------------------------------------------------------------------ */

static void list_group(FILE *f, int group, const char *suffix) {
  for (int i = 0; i < N_FIELDS; i++)
    if (FIELDS[i].group == group) fprintf(f, "#%s = %s\n", FIELDS[i].name, suffix);
}

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { LOGE("savetool: cannot create %s", path); return; }

  fputs(
    "# Banana Kong for Switch -- save editor\n"
    "#\n"
    "# Everything here is commented out, so by default this file does nothing.\n"
    "# Uncomment a line and set a value to have it written into your save the\n"
    "# next time the game starts.\n"
    "#\n"
    "# The edit happens ONCE PER BOOT, before the game loads anything. After\n"
    "# that the game owns the value again -- spend bananas and they go down,\n"
    "# and they will not come back until you restart. Leave a line uncommented\n"
    "# and it is re-applied on every launch.\n"
    "#\n"
    "# The original of each file it touches is copied to <name>.bak the first\n"
    "# time, and never overwritten after that.\n"
    "#\n"
    "# Play the game once before editing: the profile has to exist first.\n"
    "#\n"
    "# Every name below is a real field of the game's profile. Lines are\n"
    "# \"key = number\"; on/off/yes/no/true/false also work and mean 1 and 0.\n"
    "# A key your save does not have yet is added to it.\n"
    "\n"
    "# --- currency ------------------------------------------------------\n"
    "# bananaCurrent is what you can spend in the shop. bananaTotal and\n"
    "# bananaBest are the lifetime and single-run records the stats screen\n"
    "# shows, and are not spendable.\n"
    "#bananaCurrent = 99999\n"
    "#goldenHeartCurrent = 25\n"
    "\n"
    "# --- records -------------------------------------------------------\n"
    "# scoreMultiplier is the permanent multiplier the rewards track grants.\n"
    "#bestDistance = 10000\n"
    "#bestScore = 100000\n"
    "#scoreMultiplier = 10\n"
    "\n"
    "# --- items ---------------------------------------------------------\n"
    "# Status values:  0 = not owned,  1 = bought,  2 = bought and equipped.\n"
    "# Equip exactly one hat and one parachute; set the rest to 1.\n"
    "#\n"
    "#   hat1 = 2            wear hat 1\n"
    "#   parachute3 = 1      own parachute 3 without wearing it\n"
    "#\n"
    "# Powerups and utilities are consumables: the number is how many you hold.\n"
    "\n", f);

  fputs("# hats\n", f);
  list_group(f, G_HAT, "1");
  fputs("\n# parachutes\n", f);
  list_group(f, G_PARACHUTE, "1");
  fputs("\n# powerups (counts)\n", f);
  list_group(f, G_POWERUP, "10");
  fputs("\n# utilities (counts)\n", f);
  list_group(f, G_UTILITY, "10");

  fputs(
    "\n# --- purchases -----------------------------------------------------\n"
    "# The in-app purchases, as the game records them. removeAdsItem and\n"
    "# removeAds also silence the ad slots, which on this port never fill\n"
    "# anyway.\n", f);
  list_group(f, G_IAP, "1");

  fputs(
    "\n# --- switches ------------------------------------------------------\n"
    "# 1 = on, 0 = off. tutorialEnabled = 0 skips the tutorial prompts.\n", f);
  list_group(f, G_TOGGLE, "1");

  fputs(
    "\n# --- everything else -----------------------------------------------\n"
    "# The rest of the profile: lifetime counters, per-area distances and the\n"
    "# mission bookkeeping. Listed so you can see what exists; most of it is\n"
    "# only read by the stats and achievement screens.\n", f);
  list_group(f, G_STAT, "0");
  list_group(f, G_RECORD, "0");
  list_group(f, G_CURRENCY, "0");

  fputs(
    "\n# --- another save file ----------------------------------------------\n"
    "# The profile is files/playerprefs and is where everything above lives.\n"
    "# The game keeps a few smaller files beside it in the same format; name\n"
    "# one in brackets to aim the lines that follow at it.\n"
    "#\n"
    "#[rentapower]\n"
    "#someKey = 1\n", f);

  fclose(f);
  LOGI("savetool: wrote template %s", path);
}

static int truthy(const char *v, double *out) {
  if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true"))  { *out = 1.0; return 1; }
  if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false")) { *out = 0.0; return 1; }
  return 0;
}

static void trim(char *s) {
  char *p = s;
  size_t n;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int read_config(void) {
  char path[512], line[256], file[32] = "playerprefs";
  snprintf(path, sizeof(path), "%s/saves.txt", app_game_dir());
  FILE *f = fopen(path, "r");
  if (!f) { write_template(path); return 0; }

  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;                 /* '#' starts a comment anywhere */
    trim(line);
    if (!*line) continue;

    if (line[0] == '[') {                /* [file] aims the lines that follow */
      char *end = strchr(line, ']');
      if (!end) continue;
      *end = 0;
      snprintf(file, sizeof(file), "%.31s", line + 1);
      trim(file);
      continue;
    }
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key);
    trim(val);
    if (!*key || !*val || g_nedits >= MAX_EDITS) continue;

    double v;
    if (!truthy(val, &v)) {
      char *end = NULL;
      v = strtod(val, &end);
      if (end == val) { LOGE("savetool: %s: not a number, ignored", key); continue; }
    }
    snprintf(g_edits[g_nedits].file, sizeof(g_edits[0].file), "%s", file);
    snprintf(g_edits[g_nedits].key, sizeof(g_edits[0].key), "%.63s", key);
    g_edits[g_nedits].value = v;
    g_nedits++;
  }
  fclose(f);
  return g_nedits;
}

/* ------------------------------------------------------------------ */
/* the save file                                                       */
/* ------------------------------------------------------------------ */

static size_t align4(size_t o) { return (o + 3u) & ~(size_t)3u; }

static int rd32(const unsigned char *b, size_t len, size_t o, unsigned *out) {
  if (o + 4 > len) return 0;
  *out = (unsigned)b[o] | ((unsigned)b[o + 1] << 8) | ((unsigned)b[o + 2] << 16) | ((unsigned)b[o + 3] << 24);
  return 1;
}

/* Step over one value, so the walk can reach the next entry. */
static int skip_value(const unsigned char *b, size_t len, size_t *o, unsigned char vt);

static int skip_entries(const unsigned char *b, size_t len, size_t *o) {
  unsigned count;
  if (!rd32(b, len, *o, &count)) return 0;
  *o += 4;
  for (unsigned i = 0; i < count; i++) {
    if (*o + 2 > len) return 0;
    unsigned char kt = b[*o], vt = b[*o + 1];
    *o += 2;
    if (kt == T_STRING) {
      unsigned klen;
      if (!rd32(b, len, *o, &klen)) return 0;
      *o += 4 + klen;
    } else if (kt == T_NUMBER) {
      *o = align4(*o) + 8;
    } else {
      return 0;
    }
    *o = align4(*o);
    if (!skip_value(b, len, o, vt)) return 0;
  }
  return *o <= len;
}

static int skip_value(const unsigned char *b, size_t len, size_t *o, unsigned char vt) {
  unsigned slen;
  switch (vt) {
    case T_NUMBER:  *o += 8; return *o <= len;
    case T_BOOLEAN: *o += 1; return *o <= len;
    case T_STRING:
      if (!rd32(b, len, *o, &slen)) return 0;
      *o += 4 + slen;
      return *o <= len;
    case T_TABLE:   return skip_entries(b, len, o);
    default:        return 0;   /* a type this tool does not know: refuse the file */
  }
}

/* Offset of the value bytes for `key` at the top level, or -1. */
static long find_value(const unsigned char *b, size_t len, const char *key, unsigned char *vtype) {
  size_t o = 8;
  unsigned count;
  if (!rd32(b, len, o, &count)) return -1;
  o += 4;
  const size_t keylen = strlen(key);
  for (unsigned i = 0; i < count; i++) {
    if (o + 2 > len) return -1;
    unsigned char kt = b[o], vt = b[o + 1];
    o += 2;
    int match = 0;
    if (kt == T_STRING) {
      unsigned klen;
      if (!rd32(b, len, o, &klen)) return -1;
      o += 4;
      if (o + klen > len) return -1;
      match = klen == keylen && memcmp(b + o, key, klen) == 0;
      o += klen;
    } else if (kt == T_NUMBER) {
      o = align4(o) + 8;
    } else {
      return -1;
    }
    o = align4(o);
    if (match) { *vtype = vt; return (long)o; }
    if (!skip_value(b, len, &o, vt)) return -1;
  }
  return -1;
}

static void put_double(unsigned char *b, size_t o, double v) { memcpy(b + o, &v, 8); }

static int backup_once(const char *path, const unsigned char *b, size_t len) {
  char bak[640];
  snprintf(bak, sizeof(bak), "%s.bak", path);
  FILE *f = fopen(bak, "rb");
  if (f) { fclose(f); return 1; }        /* already have one: never overwrite */
  f = fopen(bak, "wb");
  if (!f) { LOGE("savetool: cannot write %s", bak); return 0; }
  int ok = fwrite(b, 1, len, f) == len;
  fclose(f);
  if (ok) LOGI("savetool: kept the original as %s", bak);
  return ok;
}

static int write_save(const char *path, const unsigned char *b, size_t len) {
  char tmp[640];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) { LOGE("savetool: cannot write %s", tmp); return 0; }
  int ok = fwrite(b, 1, len, f) == len;
  if (fclose(f) != 0) ok = 0;
  if (!ok) { remove(tmp); return 0; }
  remove(path);                          /* this filesystem will not rename onto a file */
  if (rename(tmp, path) != 0) { LOGE("savetool: cannot replace %s", path); return 0; }
  return 1;
}

static void apply_file(const char *name) {
  char path[600];
  snprintf(path, sizeof(path), "%s/%.64s", app_files_dir(), name);

  FILE *f = fopen(path, "rb");
  if (!f) {
    LOGI("savetool: %s does not exist yet -- play once, then edit", name);
    return;
  }
  unsigned char *b = malloc(MAX_SAVE);
  if (!b) { fclose(f); return; }
  size_t len = fread(b, 1, MAX_SAVE, f);
  fclose(f);

  unsigned version = 0, count = 0;
  if (len < 12 || memcmp(b, "HDTB", 4) != 0) {
    LOGE("savetool: %s is not a Defold save (no HDTB header) -- left alone", name);
    free(b);
    return;
  }
  version = (unsigned)b[4] | ((unsigned)b[5] << 8);
  rd32(b, len, 8, &count);
  if (version != SAVE_VERSION) {
    LOGE("savetool: %s is table version %u, this tool knows %d -- left alone", name, version, SAVE_VERSION);
    free(b);
    return;
  }
  {   /* Walk the whole file first: edit nothing that does not parse cleanly. */
    size_t o = 8;
    if (!skip_entries(b, len, &o) || o != len) {
      LOGE("savetool: %s did not parse cleanly (%u entries, %zu of %zu bytes) -- left alone",
           name, count, o, len);
      free(b);
      return;
    }
  }

  int changed = 0;
  for (int i = 0; i < g_nedits; i++) {
    if (strcmp(g_edits[i].file, name) != 0) continue;
    const char *key = g_edits[i].key;
    unsigned char vt = 0;
    long at = find_value(b, len, key, &vt);

    if (at >= 0) {
      if (vt != T_NUMBER) {
        LOGE("savetool: %s.%s is not a number -- left alone", name, key);
        continue;
      }
      double old;
      memcpy(&old, b + at, 8);
      if (old == g_edits[i].value) continue;
      if (!changed && !backup_once(path, b, len)) break;
      put_double(b, (size_t)at, g_edits[i].value);
      changed = 1;
      LOGI("savetool: %s.%s: %g -> %g", name, key, old, g_edits[i].value);
    } else {
      size_t klen = strlen(key);
      size_t need = align4(len + 6 + klen) + 8;
      if (need > MAX_SAVE) { LOGE("savetool: %s would not fit", name); break; }
      if (!changed && !backup_once(path, b, len)) break;
      size_t o = len;
      b[o++] = T_STRING;
      b[o++] = T_NUMBER;
      b[o++] = (unsigned char)(klen & 0xff);
      b[o++] = (unsigned char)((klen >> 8) & 0xff);
      b[o++] = (unsigned char)((klen >> 16) & 0xff);
      b[o++] = (unsigned char)((klen >> 24) & 0xff);
      memcpy(b + o, key, klen);
      o += klen;
      while (o < align4(o)) b[o++] = 0;
      put_double(b, o, g_edits[i].value);
      o += 8;
      len = o;
      count++;
      b[8] = (unsigned char)(count & 0xff);
      b[9] = (unsigned char)((count >> 8) & 0xff);
      b[10] = (unsigned char)((count >> 16) & 0xff);
      b[11] = (unsigned char)((count >> 24) & 0xff);
      changed = 1;
      LOGI("savetool: %s.%s: added, = %g", name, key, g_edits[i].value);
    }
  }

  if (changed && write_save(path, b, len))
    LOGI("savetool: wrote %s (%u entries)", name, count);
  free(b);
}

void bk_savetool_apply(void) {
  if (!read_config()) return;

  char done[8][32];
  int ndone = 0;
  for (int i = 0; i < g_nedits; i++) {
    int seen = 0;
    for (int j = 0; j < ndone; j++)
      if (!strcmp(done[j], g_edits[i].file)) seen = 1;
    if (seen || ndone == 8) continue;
    snprintf(done[ndone++], 32, "%.31s", g_edits[i].file);
    apply_file(g_edits[i].file);
  }
}

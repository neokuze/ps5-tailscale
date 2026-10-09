/* Remote Play pairing helper for Tailscale on PS5.
 *
 * A small payload the daemon hands to the ELF loader when someone presses
 * "Pair a device" or opens the account list on the status page. It is a
 * separate payload, like the icon helper, so that libSceRemoteplay and the
 * registry libraries never end up in the daemon's process.
 *
 * Two modes, picked by a byte after the marker below (the daemon rewrites it
 * before sending; a payload gets no arguments):
 *
 *   P  pair: make a PIN, print it with the account id, then wait for a
 *      client to register with it. The daemon cancels by closing the
 *      connection; the PIN is then invalidated.
 *   U  users: list the console's users and their account ids.
 *
 * Every answer is a line starting "rp: ", which tsd/remoteplay.go reads:
 *
 *   rp: user <slot> <user id> <account id base64 or -> <signed in 0/1> <name>
 *   rp: end
 *   rp: account <base64>
 *   rp: pin <8 digits> <seconds>
 *   rp: paired | rp: timeout | rp: cancelled | rp: error <text>
 *
 * The Sony calls, registry keys and the gotchas around them (the init
 * return that means "ready", the foreground user that may be -1, the base64
 * padding) are as worked out by ps5upload (phantomptr, GPL-3.0,
 * payload/src/remoteplay.c, measured on FW 13.60); idlesauce's
 * ps5-remoteplay-get-pin found that sceRemoteplayGeneratePinCode works
 * without patching. This is our own implementation of that knowledge.
 *
 * Link with -lSceUserService -lSceRegMgr -lSceSystemService. */

#include <dlfcn.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAIR_SECONDS 300
#define MAX_USERS 16

/* sceRemoteplayInitialize on every call after the first in a process. */
#define RP_ALREADY_INITIALIZED ((int)0x80FC0003)
#define RP_CONFIRM_PAIRED 2u
#define RP_CONFIRM_FAILED 3u
#define RP_CONFIRM_ABORTED 4u
#define RP_ERR_ACCOUNT 0x80FC1040u
#define RP_ERR_PIN 0x80FC1047u
#define RP_ERR_DEVICE_LIMIT 0x80FC1042u

#define REG_RP_ENABLE 1098973184u

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *);
int sceUserServiceGetLoginUserIdList(int *);
int sceUserServiceGetUserName(int, char *, size_t);
int sceRegMgrGetInt(uint32_t, int *);
int sceRegMgrSetInt(uint32_t, int);
int sceRegMgrGetBin(uint32_t, void *, size_t);

static volatile const char mode_marker[] = "RPAIR-MODE=P";
#define MODE (mode_marker[sizeof(mode_marker) - 2])

/* Registry entry numbers of the per-user tables, slots 1..16. */
static uint32_t
key_user_id(uint32_t slot) {
  return (slot - 1) * 65536u + 125829376u;
}

static uint32_t
key_account_id(uint32_t slot) {
  return (slot - 1) * 65536u + 125830400u;
}

static void
base64(const uint8_t *in, size_t len, char *out) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t j = 0;

  for (size_t i = 0; i < len; i += 3) {
    size_t rem = len - i;
    uint32_t v = (uint32_t)in[i] << 16 | (rem > 1 ? in[i + 1] : 0) << 8 | (rem > 2 ? in[i + 2] : 0);
    out[j++] = tab[v >> 18 & 63];
    out[j++] = tab[v >> 12 & 63];
    out[j++] = rem > 1 ? tab[v >> 6 & 63] : '=';
    out[j++] = rem > 2 ? tab[v & 63] : '=';
  }
  out[j] = 0;
}

/* The base64 account id in slot, or 0 when the slot has no activated
 * account (all zeroes: a local-only user, which Remote Play refuses). */
static int
slot_account(uint32_t slot, char out[16]) {
  uint8_t raw[8] = {0};
  int any = 0;

  if (sceRegMgrGetBin(key_account_id(slot), raw, sizeof(raw))) {
    return 0;
  }
  for (int i = 0; i < 8; i++) {
    any |= raw[i];
  }
  if (!any) {
    return 0;
  }
  base64(raw, sizeof(raw), out);
  return 1;
}

static int
slot_for_user(int uid) {
  for (uint32_t slot = 1; uid > 0 && slot <= MAX_USERS; slot++) {
    int v = 0;
    if (!sceRegMgrGetInt(key_user_id(slot), &v) && v == uid) {
      return (int)slot;
    }
  }
  return -1;
}

static int
signed_in(int uid, const int *ids) {
  for (int i = 0; i < MAX_USERS; i++) {
    if (ids[i] == uid) {
      return 1;
    }
  }
  return 0;
}

static int
list_users(void) {
  int ids[MAX_USERS];

  for (int i = 0; i < MAX_USERS; i++) {
    ids[i] = -1;
  }
  sceUserServiceGetLoginUserIdList(ids);

  for (uint32_t slot = 1; slot <= MAX_USERS; slot++) {
    int uid = 0;
    char acct[16] = "-";
    char name[32] = "";

    if (sceRegMgrGetInt(key_user_id(slot), &uid) || uid == 0 || uid == -1) {
      continue;
    }
    slot_account(slot, acct);
    sceUserServiceGetUserName(uid, name, sizeof(name));
    for (char *c = name; *c; c++) {
      if (*c == '\n' || *c == '\r') {
        *c = ' ';
      }
    }
    printf("rp: user %u %d %s %d %s\n", slot, uid, acct, signed_in(uid, ids), name);
  }
  printf("rp: end\n");
  return 0;
}

/* The account a client pairs with: the foreground user's, or else the first
 * signed-in user with an activated account (on the home screen there may be
 * no foreground user at all). */
static int
pairing_account(char out[16]) {
  int uid = -1;
  int ids[MAX_USERS];

  if (!sceUserServiceGetForegroundUser(&uid) && slot_account(slot_for_user(uid), out)) {
    return 1;
  }
  for (int i = 0; i < MAX_USERS; i++) {
    ids[i] = -1;
  }
  if (sceUserServiceGetLoginUserIdList(ids)) {
    return 0;
  }
  for (int i = 0; i < MAX_USERS; i++) {
    if (ids[i] > 0 && slot_account(slot_for_user(ids[i]), out)) {
      return 1;
    }
  }
  return 0;
}

typedef int (*rp_init_fn)(void *, size_t);
typedef int (*rp_pin_fn)(uint32_t *);
typedef int (*rp_confirm_fn)(uint32_t *, uint32_t *);
typedef int (*rp_pin_error_fn)(int);

/* Writes to a connection the daemon has closed must fail, not kill us: that
 * is how a cancel arrives. */
static int
say(const char *fmt, ...) {
  char line[160];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  return write(STDOUT_FILENO, line, n) == n;
}

static int
pair(void) {
  static const char *const paths[] = {
      "/system/common/lib/libSceRemoteplay.sprx",
      "/system_ex/common/lib/libSceRemoteplay.sprx",
      "/system/priv/lib/libSceRemoteplay.sprx",
  };
  void *lib = 0;
  char acct[16];
  uint32_t pin = 0;
  int enabled = 0;
  int rc;

  for (size_t i = 0; !lib && i < sizeof(paths) / sizeof(paths[0]); i++) {
    lib = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
  }
  if (!lib) {
    lib = RTLD_DEFAULT;
  }
  rp_init_fn rp_init = (rp_init_fn)dlsym(lib, "sceRemoteplayInitialize");
  rp_pin_fn rp_pin = (rp_pin_fn)dlsym(lib, "sceRemoteplayGeneratePinCode");
  rp_confirm_fn rp_confirm = (rp_confirm_fn)dlsym(lib, "sceRemoteplayConfirmDeviceRegist");
  rp_pin_error_fn rp_pin_error = (rp_pin_error_fn)dlsym(lib, "sceRemoteplayNotifyPinCodeError");
  if (!rp_init || !rp_pin || !rp_confirm || !rp_pin_error) {
    say("rp: error this firmware does not give payloads the Remote Play library\n");
    return 1;
  }

  if (!pairing_account(acct)) {
    say("rp: error no signed-in user with a PSN-activated account\n");
    return 1;
  }

  /* With Remote Play switched off the PIN is made but registration fails
   * without a reason, so switch it on (Settings > System > Remote Play). */
  if (!sceRegMgrGetInt(REG_RP_ENABLE, &enabled) && enabled != 1) {
    sceRegMgrSetInt(REG_RP_ENABLE, 1);
  }

  rc = rp_init(0, 0);
  if (rc && rc != RP_ALREADY_INITIALIZED) {
    say("rp: error sceRemoteplayInitialize failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }
  rp_pin_error(1); /* drop an earlier PIN, if any */
  if ((rc = rp_pin(&pin))) {
    say("rp: error sceRemoteplayGeneratePinCode failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  say("rp: account %s\n", acct);
  say("rp: pin %08u %d\n", pin, PAIR_SECONDS);

  time_t until = time(0) + PAIR_SECONDS;
  while (time(0) < until) {
    uint32_t status = 0, err = 0;

    sleep(1);
    if (!say("rp: wait\n")) {
      rp_pin_error(1);
      return 0;
    }
    if (rp_confirm(&status, &err)) {
      continue;
    }
    if (status == RP_CONFIRM_PAIRED) {
      say("rp: paired\n");
      return 0;
    }
    if (status == RP_CONFIRM_FAILED) {
      rp_pin_error(1);
      say(err == RP_ERR_PIN ? "rp: error wrong PIN\n"
          : err == RP_ERR_ACCOUNT ? "rp: error the client used another account id\n"
          : err == RP_ERR_DEVICE_LIMIT ? "rp: error too many paired devices; remove one in Settings > System > Remote Play\n"
          : "rp: error registration failed: 0x%08X\n", (unsigned)err);
      return 1;
    }
    if (status == RP_CONFIRM_ABORTED) {
      say("rp: cancelled\n");
      return 1;
    }
  }
  rp_pin_error(1);
  say("rp: timeout\n");
  return 1;
}

int
main(void) {
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stdout, 0, _IONBF, 0);
  sceUserServiceInitialize(0);
  return MODE == 'U' ? list_users() : pair();
}

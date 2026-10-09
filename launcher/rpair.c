/* Saves the Remote Play pairing helper (rpair/) for the daemon.
 *
 * The helper is its own payload so that libSceRemoteplay never loads into
 * the daemon's process; the daemon hands it to the ELF loader each time a
 * PIN is asked for. Build with -DRPAIR_HELPER="path/to/rpair.elf"; without
 * it this file does nothing and the status page says pairing is missing. */

#include "rpair.h"

#ifdef RPAIR_HELPER

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/stat.h>

#define DATA_DIR "/data/tailscale"
#define RPAIR_FILE DATA_DIR "/rpair-helper.elf"

extern const uint8_t rpair_helper[];
extern const uint8_t rpair_helper_end[];

__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global rpair_helper\n"
        "rpair_helper:\n"
        ".incbin \"" RPAIR_HELPER "\"\n"
        ".global rpair_helper_end\n"
        "rpair_helper_end:\n"
        ".text\n");

void
rpair_helper_save(void) {
  size_t size = rpair_helper_end - rpair_helper;
  int fd;

  mkdir(DATA_DIR, 0755);
  /* Always rewrite: a helper left by an older version may differ in content
   * but not in size. */
  if ((fd = open(RPAIR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    return;
  }
  for (size_t done = 0; done < size;) {
    ssize_t n = write(fd, rpair_helper + done, size - done);
    if (n <= 0) {
      break;
    }
    done += n;
  }
  close(fd);
}

#else

void
rpair_helper_save(void) {
}

#endif

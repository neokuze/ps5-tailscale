#pragma once

/* Leave the Remote Play pairing helper in the data directory, where the
 * daemon sends it to the ELF loader from (see tsd/remoteplay.go). */
void rpair_helper_save(void);

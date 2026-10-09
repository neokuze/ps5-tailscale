package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Pairing a Remote Play client (Chiaki, chiaki-ng, PXPlay) from the status
// page, so that someone away from home can register a new device without
// touching the console.
//
// The Sony calls live in a separate helper payload (rpair/main.c) that the
// launcher leaves in the data directory, for the same reason as the icon
// helper: their libraries must not load into this process. The daemon
// switches the helper's mode byte, sends it to the ELF loader and reads its
// "rp: ..." lines back over the same connection. Closing the connection
// cancels a pairing; the helper then invalidates the PIN.

const (
	rpairHelperFile = "rpair-helper.elf"
	rpairModeMarker = "RPAIR-MODE="
	rpairUsersWait  = 15 * time.Second
	rpairPairWait   = 6 * time.Minute // the helper gives up after five
)

// rpUser is one user on the console.
type rpUser struct {
	Slot      int    `json:"slot"`
	UserID    int    `json:"userId"`
	Name      string `json:"name"`
	AccountID string `json:"accountId"` // base64, "" for a user without PSN
	SignedIn  bool   `json:"signedIn"`
}

// rpPairing is what the status page shows about the current pairing.
type rpPairing struct {
	State     string    `json:"state"` // idle, starting, waiting, paired, failed
	PIN       string    `json:"pin,omitempty"`
	AccountID string    `json:"accountId,omitempty"`
	Error     string    `json:"error,omitempty"`
	Expires   time.Time `json:"expires,omitzero"`
}

// remotePlay holds the one pairing that may run at a time.
type remotePlay struct {
	mu     sync.Mutex
	cur    rpPairing
	cancel func()
}

func (rp *remotePlay) snapshot() rpPairing {
	rp.mu.Lock()
	defer rp.mu.Unlock()
	p := rp.cur
	if p.State == "" {
		p.State = "idle"
	}
	return p
}

func (rp *remotePlay) set(f func(p *rpPairing)) {
	rp.mu.Lock()
	f(&rp.cur)
	rp.mu.Unlock()
}

// rpairHelper returns the helper with its mode byte set.
func rpairHelper(mode byte) ([]byte, error) {
	helper, err := os.ReadFile(filepath.Join(dataDir, rpairHelperFile))
	if err != nil {
		return nil, errors.New("the pairing helper is missing; this build was made without it")
	}
	i := bytes.Index(helper, []byte(rpairModeMarker))
	if i < 0 || i+len(rpairModeMarker) >= len(helper) {
		return nil, errors.New("the pairing helper is not one this version understands")
	}
	helper[i+len(rpairModeMarker)] = mode
	return helper, nil
}

// runRpair sends the helper to the ELF loader and returns the connection
// its answers arrive on.
func runRpair(mode byte, wait time.Duration) (net.Conn, error) {
	helper, err := rpairHelper(mode)
	if err != nil {
		return nil, err
	}
	c, err := net.DialTimeout("tcp", elfLoaderAddr, 3*time.Second)
	if err != nil {
		return nil, fmt.Errorf("no ELF loader to run the pairing helper: %w", err)
	}
	c.SetDeadline(time.Now().Add(wait))
	if _, err := c.Write(helper); err != nil {
		c.Close()
		return nil, err
	}
	return c, nil
}

// rpLines calls f with each "rp: " line from r, without the prefix, until f
// returns false or the input ends. Other output (the loader's own) is
// skipped.
func rpLines(r io.Reader, f func(line string) bool) {
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		i := strings.Index(line, "rp: ")
		if i < 0 {
			continue
		}
		if !f(line[i+len("rp: "):]) {
			return
		}
	}
}

// parseRPUser reads "user <slot> <uid> <account|-> <0|1> <name...>".
func parseRPUser(line string) (rpUser, bool) {
	f := strings.SplitN(line, " ", 6)
	if len(f) < 5 || f[0] != "user" {
		return rpUser{}, false
	}
	slot, err1 := strconv.Atoi(f[1])
	uid, err2 := strconv.Atoi(f[2])
	if err1 != nil || err2 != nil {
		return rpUser{}, false
	}
	u := rpUser{Slot: slot, UserID: uid, SignedIn: f[4] == "1"}
	if f[3] != "-" {
		u.AccountID = f[3]
	}
	if len(f) == 6 {
		u.Name = f[5]
	}
	return u, true
}

func listRPUsers() ([]rpUser, error) {
	c, err := runRpair('U', rpairUsersWait)
	if err != nil {
		return nil, err
	}
	defer c.Close()
	users := []rpUser{}
	done := false
	rpLines(c, func(line string) bool {
		if line == "end" {
			done = true
			return false
		}
		if u, ok := parseRPUser(line); ok {
			users = append(users, u)
		}
		return true
	})
	if !done {
		return users, errors.New("the pairing helper did not finish the list")
	}
	return users, nil
}

// applyRPLine updates a pairing from one helper line and reports whether
// the pairing is over.
func applyRPLine(p *rpPairing, line string, now time.Time) bool {
	word, rest, _ := strings.Cut(line, " ")
	switch word {
	case "account":
		p.AccountID = rest
	case "pin":
		pin, secs, _ := strings.Cut(rest, " ")
		if len(pin) == 8 {
			pin = pin[:4] + " " + pin[4:]
		}
		p.PIN = pin
		p.State = "waiting"
		if n, err := strconv.Atoi(secs); err == nil {
			p.Expires = now.Add(time.Duration(n) * time.Second)
		}
	case "paired":
		p.State = "paired"
		return true
	case "timeout":
		p.State, p.Error = "failed", "the PIN expired before a device used it"
		return true
	case "cancelled":
		p.State, p.Error = "failed", "the pairing was cancelled on the console"
		return true
	case "error":
		p.State, p.Error = "failed", rest
		return true
	}
	return false
}

// startPairing makes a new PIN, replacing a pairing already running.
func (d *daemon) startPairing() {
	rp := &d.rp
	rp.mu.Lock()
	if rp.cancel != nil {
		rp.cancel()
	}
	rp.cur = rpPairing{State: "starting"}
	rp.mu.Unlock()

	c, err := runRpair('P', rpairPairWait)
	if err != nil {
		rp.set(func(p *rpPairing) { p.State, p.Error = "failed", err.Error() })
		return
	}
	var once sync.Once
	stop := func() { once.Do(func() { c.Close() }) }
	rp.mu.Lock()
	rp.cancel = stop
	rp.mu.Unlock()

	go func() {
		defer stop()
		over := false
		rpLines(c, func(line string) bool {
			rp.mu.Lock()
			defer rp.mu.Unlock()
			over = applyRPLine(&rp.cur, line, time.Now())
			return !over
		})
		rp.mu.Lock()
		if !over && rp.cur.State != "idle" {
			rp.cur.State, rp.cur.Error = "failed", "the pairing helper stopped answering"
		}
		if rp.cur.State == "paired" {
			d.logf("remote play: a device was paired")
		} else if rp.cur.Error != "" {
			d.logf("remote play: pairing failed: %s", rp.cur.Error)
		}
		rp.mu.Unlock()
	}()
}

func (d *daemon) cancelPairing() {
	rp := &d.rp
	rp.mu.Lock()
	defer rp.mu.Unlock()
	if rp.cancel != nil {
		rp.cancel()
		rp.cancel = nil
	}
	rp.cur = rpPairing{State: "idle"}
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	json.NewEncoder(w).Encode(v)
}

func (d *daemon) handleRPStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, d.rp.snapshot())
}

func (d *daemon) handleRPPair(w http.ResponseWriter, r *http.Request) {
	d.startPairing()
	writeJSON(w, d.rp.snapshot())
}

func (d *daemon) handleRPCancel(w http.ResponseWriter, r *http.Request) {
	d.cancelPairing()
	writeJSON(w, d.rp.snapshot())
}

func (d *daemon) handleRPUsers(w http.ResponseWriter, r *http.Request) {
	users, err := listRPUsers()
	if err != nil && len(users) == 0 {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	writeJSON(w, users)
}

package main

import (
	"strings"
	"testing"
	"time"
)

func TestParseRPUser(t *testing.T) {
	u, ok := parseRPUser("user 2 268435457 AAECAwQFBgc= 1 Ana María")
	if !ok || u.Slot != 2 || u.UserID != 268435457 || u.AccountID != "AAECAwQFBgc=" || !u.SignedIn || u.Name != "Ana María" {
		t.Fatalf("got %+v, %v", u, ok)
	}
	u, ok = parseRPUser("user 3 268435458 - 0 local")
	if !ok || u.AccountID != "" || u.SignedIn {
		t.Fatalf("got %+v, %v", u, ok)
	}
	if _, ok := parseRPUser("user x"); ok {
		t.Fatal("accepted a broken line")
	}
}

func TestRPPairingLines(t *testing.T) {
	now := time.Unix(1000, 0)
	var p rpPairing
	out := "[elfldr] running\nrp: account AAECAwQFBgc=\nrp: pin 01234567 300\nrp: wait\nrp: paired\n"
	over := false
	rpLines(strings.NewReader(out), func(line string) bool {
		over = applyRPLine(&p, line, now)
		return !over
	})
	if !over || p.State != "paired" || p.PIN != "0123 4567" || p.AccountID != "AAECAwQFBgc=" {
		t.Fatalf("got %+v", p)
	}
	if !p.Expires.Equal(now.Add(300 * time.Second)) {
		t.Fatalf("expires %v", p.Expires)
	}

	p = rpPairing{}
	if !applyRPLine(&p, "error wrong PIN", now) || p.State != "failed" || p.Error != "wrong PIN" {
		t.Fatalf("got %+v", p)
	}
}

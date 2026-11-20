package state

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/doubleastandard9093-debug/eve-elite-ogame-design/backend/internal/db"
)

func TestValidate(t *testing.T) {
	cases := []struct {
		ns, key, data string
		ok            bool
	}{
		{NSWorld, "economy", `{"orders":[]}`, true},
		{NSWorld, "territory", ` {"corps":[]} `, true},
		{NSWorld, "bogus", `{}`, false},
		{NSWorld, "economy", `[1,2]`, false},
		{NSWorld, "economy", `{"a":`, false},
		{NSPilot, "p1", `{"credits":5}`, true},
		{NSPilot, "", `{}`, false},
		{NSPilot, strings.Repeat("x", 129), `{}`, false},
		{"other", "k", `{}`, false},
	}
	for _, c := range cases {
		err := Validate(c.ns, c.key, []byte(c.data))
		if (err == nil) != c.ok {
			t.Errorf("Validate(%q,%q,%q) = %v, want ok=%v", c.ns, c.key, c.data, err, c.ok)
		}
		if err != nil && !errors.Is(err, ErrInvalid) {
			t.Errorf("error should wrap ErrInvalid: %v", err)
		}
	}
	big := `{"x":"` + strings.Repeat("a", MaxPilotBytes) + `"}`
	if err := Validate(NSPilot, "p1", []byte(big)); err == nil {
		t.Error("oversized pilot doc accepted")
	}
}

// exercise runs the same behavioural checks against any Store.
func exercise(t *testing.T, s Store, pilot string) {
	t.Helper()
	ctx := context.Background()
	if _, ok, err := s.Get(ctx, NSPilot, pilot); err != nil || ok {
		t.Fatalf("fresh get: ok=%v err=%v", ok, err)
	}
	d1, err := s.Put(ctx, NSPilot, pilot, json.RawMessage(`{"credits":100}`), 0)
	if err != nil || d1.Version != 1 {
		t.Fatalf("create: %+v %v", d1, err)
	}
	if _, err := s.Put(ctx, NSPilot, pilot, json.RawMessage(`{"credits":1}`), 0); !errors.Is(err, ErrConflict) {
		t.Fatalf("create-if-absent on existing doc should conflict, got %v", err)
	}
	d2, err := s.Put(ctx, NSPilot, pilot, json.RawMessage(`{"credits":200}`), 1)
	if err != nil || d2.Version != 2 {
		t.Fatalf("update v1: %+v %v", d2, err)
	}
	_, err = s.Put(ctx, NSPilot, pilot, json.RawMessage(`{"credits":300}`), 1)
	var ce *ConflictError
	if !errors.As(err, &ce) || ce.Current != 2 {
		t.Fatalf("stale write should conflict with current=2, got %v", err)
	}
	d3, err := s.Put(ctx, NSPilot, pilot, json.RawMessage(`{"credits":400}`), AnyVersion)
	if err != nil || d3.Version != 3 {
		t.Fatalf("unconditional put: %+v %v", d3, err)
	}
	got, ok, err := s.Get(ctx, NSPilot, pilot)
	if err != nil || !ok || got.Version != 3 {
		t.Fatalf("get: %+v ok=%v err=%v", got, ok, err)
	}
	var v struct{ Credits int }
	if err := json.Unmarshal(got.Data, &v); err != nil || v.Credits != 400 {
		t.Fatalf("data round trip: %s %v", got.Data, err)
	}
	if _, err := s.Put(ctx, NSPilot, pilot, json.RawMessage(`nope`), AnyVersion); !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid data should be rejected, got %v", err)
	}
	list, err := s.List(ctx, NSPilot)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, d := range list {
		if d.Key == pilot {
			found = true
			if d.Data != nil {
				t.Error("List must not return data")
			}
		}
	}
	if !found {
		t.Fatalf("list missing %s: %+v", pilot, list)
	}
}

func TestMemStore(t *testing.T) {
	exercise(t, NewMemStore(), "pilot-a")
}

func TestMemStoreIsolation(t *testing.T) {
	s := NewMemStore()
	ctx := context.Background()
	raw := json.RawMessage(`{"a":1}`)
	if _, err := s.Put(ctx, NSWorld, "economy", raw, AnyVersion); err != nil {
		t.Fatal(err)
	}
	raw[2] = 'b' // caller mutating its buffer must not change the stored doc
	got, _, _ := s.Get(ctx, NSWorld, "economy")
	if string(got.Data) != `{"a":1}` {
		t.Fatalf("stored doc aliased caller buffer: %s", got.Data)
	}
	if _, ok, _ := s.Get(ctx, NSPilot, "economy"); ok {
		t.Fatal("namespaces must not collide")
	}
}

// TestPGStore runs against a real Postgres when HELIOS_TEST_DATABASE_URL is set (e.g. docker compose up db).
func TestPGStore(t *testing.T) {
	url := os.Getenv("HELIOS_TEST_DATABASE_URL")
	if url == "" {
		t.Skip("HELIOS_TEST_DATABASE_URL not set")
	}
	d, err := db.Open(url)
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	ctx := context.Background()
	if err := db.Migrate(ctx, d); err != nil {
		t.Fatal(err)
	}
	pilot := "test-" + t.Name()
	if _, err := d.ExecContext(ctx, `DELETE FROM state_docs WHERE ns='pilot' AND key=$1`, pilot); err != nil {
		t.Fatal(err)
	}
	exercise(t, PGStore{DB: d}, pilot)
}

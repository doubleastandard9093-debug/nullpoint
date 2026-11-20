package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/doubleastandard9093-debug/eve-elite-ogame-design/backend/internal/ships"
	"github.com/doubleastandard9093-debug/eve-elite-ogame-design/backend/internal/state"
)

type fakeDB struct{ err error }

func (f fakeDB) PingContext(context.Context) error { return f.err }

func do(h http.Handler, method, path, body string, hdr map[string]string) *httptest.ResponseRecorder {
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	h.ServeHTTP(rr, req)
	return rr
}

func router(key string) http.Handler {
	return NewRouter(Config{DB: fakeDB{}, Ships: ships.NewMemStore(), State: state.NewMemStore(), Version: "test", ServerKey: key})
}

func TestHealth(t *testing.T) {
	if c := do(router(""), "GET", "/healthz", "", nil).Code; c != 200 {
		t.Fatalf("healthz %d", c)
	}
}

func TestReady(t *testing.T) {
	if c := do(router(""), "GET", "/readyz", "", nil).Code; c != 200 {
		t.Fatalf("ready ok %d", c)
	}
	down := NewRouter(Config{DB: fakeDB{errors.New("down")}, Ships: ships.NewMemStore()})
	if c := do(down, "GET", "/readyz", "", nil).Code; c != 503 {
		t.Fatalf("ready down %d", c)
	}
}

func TestShipRoundTrip(t *testing.T) {
	h := router("")
	if c := do(h, "GET", "/v1/pilots/p1/ship", "", nil).Code; c != 404 {
		t.Fatalf("want 404, got %d", c)
	}
	body := `{"hull":"skiff","system_id":"sys-0001","location":[1,2,3],"rotation":[0,90,0],"hull_hp":750}`
	if rr := do(h, "PUT", "/v1/pilots/p1/ship", body, nil); rr.Code != 200 {
		t.Fatalf("put %d %s", rr.Code, rr.Body)
	}
	rr := do(h, "GET", "/v1/pilots/p1/ship", "", nil)
	if rr.Code != 200 {
		t.Fatalf("get %d", rr.Code)
	}
	var s ships.Ship
	if err := json.Unmarshal(rr.Body.Bytes(), &s); err != nil {
		t.Fatal(err)
	}
	if s.PilotID != "p1" || s.HullHP != 750 || s.Location != [3]float64{1, 2, 3} {
		t.Fatalf("unexpected ship %+v", s)
	}
}

func TestShipValidation(t *testing.T) {
	h := router("")
	if c := do(h, "PUT", "/v1/pilots/p1/ship", `not json`, nil).Code; c != 400 {
		t.Fatalf("bad json %d", c)
	}
	if c := do(h, "PUT", "/v1/pilots/p1/ship", `{"hull":"","hull_hp":1}`, nil).Code; c != 400 {
		t.Fatalf("missing hull %d", c)
	}
	if c := do(h, "PUT", "/v1/pilots/p1/ship", `{"hull":"skiff","hull_hp":-5}`, nil).Code; c != 400 {
		t.Fatalf("negative hp %d", c)
	}
}

func TestServerKey(t *testing.T) {
	h := router("secret")
	if c := do(h, "GET", "/v1/pilots/p1/ship", "", nil).Code; c != 401 {
		t.Fatalf("want 401, got %d", c)
	}
	if c := do(h, "GET", "/v1/pilots/p1/ship", "", map[string]string{"X-Helios-Server-Key": "secret"}).Code; c != 404 {
		t.Fatalf("want 404 with key, got %d", c)
	}
	if c := do(h, "GET", "/healthz", "", nil).Code; c != 200 {
		t.Fatalf("healthz must not need key, got %d", c)
	}
}

func TestWorldDocRoundTrip(t *testing.T) {
	h := router("")
	if c := do(h, "GET", "/v1/world/territory", "", nil).Code; c != 404 {
		t.Fatalf("fresh world doc: want 404, got %d", c)
	}
	if c := do(h, "GET", "/v1/world/bogus", "", nil).Code; c != 404 {
		t.Fatalf("unknown kind: want 404, got %d", c)
	}
	rr := do(h, "PUT", "/v1/world/territory", `{"corps":[{"id":"test"}],"nextId":3}`, map[string]string{"If-Match": "0"})
	if rr.Code != 200 || rr.Header().Get("ETag") != "1" {
		t.Fatalf("create: %d etag=%q %s", rr.Code, rr.Header().Get("ETag"), rr.Body)
	}
	rr = do(h, "GET", "/v1/world/territory", "", nil)
	if rr.Code != 200 {
		t.Fatalf("get %d", rr.Code)
	}
	var d state.Doc
	if err := json.Unmarshal(rr.Body.Bytes(), &d); err != nil {
		t.Fatal(err)
	}
	var body struct {
		NextID int `json:"nextId"`
	}
	if err := json.Unmarshal(d.Data, &body); err != nil || d.Version != 1 || body.NextID != 3 {
		t.Fatalf("unexpected doc %+v (%v)", d, err)
	}
	// Stale writer is rejected with the current version; a fresh one wins.
	if rr := do(h, "PUT", "/v1/world/territory", `{"nextId":4}`, map[string]string{"If-Match": "0"}); rr.Code != 409 || !strings.Contains(rr.Body.String(), `"version":1`) {
		t.Fatalf("stale create: %d %s", rr.Code, rr.Body)
	}
	if rr := do(h, "PUT", "/v1/world/territory", `{"nextId":4}`, map[string]string{"If-Match": "1"}); rr.Code != 200 || rr.Header().Get("ETag") != "2" {
		t.Fatalf("update: %d %s", rr.Code, rr.Body)
	}
	if rr := do(h, "PUT", "/v1/world/economy", `{"orders":[]}`, nil); rr.Code != 200 {
		t.Fatalf("unconditional put: %d %s", rr.Code, rr.Body)
	}
	rr = do(h, "GET", "/v1/world", "", nil)
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), `"key":"economy"`) || !strings.Contains(rr.Body.String(), `"key":"territory"`) {
		t.Fatalf("list: %d %s", rr.Code, rr.Body)
	}
}

func TestWorldDocValidation(t *testing.T) {
	h := router("")
	if c := do(h, "PUT", "/v1/world/economy", `[1,2,3]`, nil).Code; c != 400 {
		t.Fatalf("array body: want 400, got %d", c)
	}
	if c := do(h, "PUT", "/v1/world/economy", `{"broken":`, nil).Code; c != 400 {
		t.Fatalf("broken json: want 400, got %d", c)
	}
	if c := do(h, "PUT", "/v1/world/bogus", `{}`, nil).Code; c != 400 {
		t.Fatalf("unknown kind: want 400, got %d", c)
	}
	if c := do(h, "PUT", "/v1/world/economy", `{}`, map[string]string{"If-Match": "x"}).Code; c != 400 {
		t.Fatalf("bad If-Match: want 400, got %d", c)
	}
	big := `{"x":"` + strings.Repeat("a", state.MaxPilotBytes+10) + `"}`
	if c := do(h, "PUT", "/v1/pilots/p1/state", big, nil).Code; c != 413 {
		t.Fatalf("oversized pilot state: want 413, got %d", c)
	}
}

func TestPilotState(t *testing.T) {
	h := router("secret")
	key := map[string]string{"X-Helios-Server-Key": "secret"}
	if c := do(h, "PUT", "/v1/pilots/p1/state", `{"credits":5}`, nil).Code; c != 401 {
		t.Fatalf("state needs the server key, got %d", c)
	}
	if c := do(h, "GET", "/v1/pilots/p1/state", "", key).Code; c != 404 {
		t.Fatalf("fresh state: want 404, got %d", c)
	}
	body := `{"credits":123456,"standings":[{"Faction":"accord","Value":1.5}],"fit":{"hull":"lance","modules":["laser_s"]},"cargo":[]}`
	if rr := do(h, "PUT", "/v1/pilots/p1/state", body, key); rr.Code != 200 {
		t.Fatalf("put %d %s", rr.Code, rr.Body)
	}
	rr := do(h, "GET", "/v1/pilots/p1/state", "", key)
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), `"credits":123456`) {
		t.Fatalf("get %d %s", rr.Code, rr.Body)
	}
	if c := do(h, "GET", "/v1/pilots/p2/state", "", key).Code; c != 404 {
		t.Fatalf("other pilot: want 404, got %d", c)
	}
}

func TestStateUnavailable(t *testing.T) {
	h := NewRouter(Config{DB: fakeDB{}, Ships: ships.NewMemStore()})
	if c := do(h, "GET", "/v1/world/economy", "", nil).Code; c != 503 {
		t.Fatalf("no store: want 503, got %d", c)
	}
	if c := do(h, "GET", "/readyz", "", nil).Code; c != 503 {
		t.Fatalf("readyz without a state store: want 503, got %d", c)
	}
}

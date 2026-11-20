// Package api exposes the HTTP surface used by UE dedicated servers and the Holo-Command UI.
package api

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"strconv"
	"time"

	"github.com/doubleastandard9093-debug/eve-elite-ogame-design/backend/internal/ships"
	"github.com/doubleastandard9093-debug/eve-elite-ogame-design/backend/internal/state"
)

// Pinger is satisfied by *sql.DB.
type Pinger interface {
	PingContext(ctx context.Context) error
}

type Config struct {
	DB        Pinger
	Ships     ships.Store
	State     state.Store
	Version   string
	ServerKey string // if set, /v1 requires X-Helios-Server-Key
}

func NewRouter(cfg Config) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "version": cfg.Version})
	})
	mux.HandleFunc("GET /readyz", func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		defer cancel()
		if cfg.DB == nil || cfg.Ships == nil || cfg.State == nil || cfg.DB.PingContext(ctx) != nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"status": "db_unavailable"})
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "ready"})
	})
	mux.HandleFunc("GET /v1/pilots/{id}/ship", cfg.auth(cfg.getShip))
	mux.HandleFunc("PUT /v1/pilots/{id}/ship", cfg.auth(cfg.putShip))
	// M7 world persistence: versioned JSON documents. PUT honours If-Match: <version> (0 = create only).
	mux.HandleFunc("GET /v1/world", cfg.auth(cfg.listWorld))
	mux.HandleFunc("GET /v1/world/{kind}", cfg.auth(cfg.getDoc(state.NSWorld, "kind")))
	mux.HandleFunc("PUT /v1/world/{kind}", cfg.auth(cfg.putDoc(state.NSWorld, "kind", state.MaxWorldBytes)))
	mux.HandleFunc("GET /v1/pilots/{id}/state", cfg.auth(cfg.getDoc(state.NSPilot, "id")))
	mux.HandleFunc("PUT /v1/pilots/{id}/state", cfg.auth(cfg.putDoc(state.NSPilot, "id", state.MaxPilotBytes)))
	return mux
}

func (cfg Config) auth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if cfg.ServerKey != "" && subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Helios-Server-Key")), []byte(cfg.ServerKey)) != 1 {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
			return
		}
		h(w, r)
	}
}

func (cfg Config) getShip(w http.ResponseWriter, r *http.Request) {
	s, ok, err := cfg.Ships.Get(r.Context(), r.PathValue("id"))
	if err != nil {
		log.Printf("get ship %q: %v", r.PathValue("id"), err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
		return
	}
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not_found"})
		return
	}
	writeJSON(w, http.StatusOK, s)
}

func (cfg Config) putShip(w http.ResponseWriter, r *http.Request) {
	var s ships.Ship
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&s); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_json"})
		return
	}
	s.PilotID = r.PathValue("id")
	if err := s.Validate(); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	if err := cfg.Ships.Put(r.Context(), s); err != nil {
		log.Printf("put ship %q: %v", s.PilotID, err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
		return
	}
	writeJSON(w, http.StatusOK, s)
}

func (cfg Config) listWorld(w http.ResponseWriter, r *http.Request) {
	if cfg.State == nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "store"})
		return
	}
	docs, err := cfg.State.List(r.Context(), state.NSWorld)
	if err != nil {
		log.Printf("list world: %v", err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"docs": docs})
}

func (cfg Config) getDoc(ns, param string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if cfg.State == nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "store"})
			return
		}
		key := r.PathValue(param)
		if ns == state.NSWorld && !state.WorldKinds[key] {
			writeJSON(w, http.StatusNotFound, map[string]string{"error": "unknown_kind"})
			return
		}
		d, ok, err := cfg.State.Get(r.Context(), ns, key)
		if err != nil {
			log.Printf("get %s/%q: %v", ns, key, err)
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
			return
		}
		if !ok {
			writeJSON(w, http.StatusNotFound, map[string]string{"error": "not_found"})
			return
		}
		w.Header().Set("ETag", strconv.FormatInt(d.Version, 10))
		writeJSON(w, http.StatusOK, d)
	}
}

func (cfg Config) putDoc(ns, param string, limit int64) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if cfg.State == nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "store"})
			return
		}
		key := r.PathValue(param)
		ifVersion := state.AnyVersion
		if h := r.Header.Get("If-Match"); h != "" {
			v, err := strconv.ParseInt(h, 10, 64)
			if err != nil || v < 0 {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_if_match"})
				return
			}
			ifVersion = v
		}
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, limit))
		if err != nil {
			writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{"error": "too_large"})
			return
		}
		d, err := cfg.State.Put(r.Context(), ns, key, body, ifVersion)
		var ce *state.ConflictError
		switch {
		case errors.As(err, &ce):
			writeJSON(w, http.StatusConflict, map[string]any{"error": "version_conflict", "version": ce.Current})
			return
		case errors.Is(err, state.ErrInvalid):
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		case err != nil:
			log.Printf("put %s/%q: %v", ns, key, err)
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
			return
		}
		w.Header().Set("ETag", strconv.FormatInt(d.Version, 10))
		// Echo metadata only: the caller already has the body.
		d.Data = nil
		writeJSON(w, http.StatusOK, d)
	}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

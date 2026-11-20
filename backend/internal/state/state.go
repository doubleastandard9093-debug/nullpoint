// Package state persists versioned JSON documents: world state written by the UE server
// (economy, territory, rules) and per-pilot state (wallet, standings, storage, hangar, fit, hold).
package state

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"sort"
	"sync"
	"time"
)

// Namespaces.
const (
	NSWorld = "world"
	NSPilot = "pilot"
)

// Size limits per document.
const (
	MaxWorldBytes = 16 << 20
	MaxPilotBytes = 1 << 20
)

// AnyVersion disables the optimistic-concurrency check on Put.
const AnyVersion int64 = -1

// WorldKinds are the world documents a server may write.
var WorldKinds = map[string]bool{"economy": true, "territory": true, "rules": true}

var (
	// ErrConflict is returned when the expected version doesn't match the stored one.
	ErrConflict = errors.New("version conflict")
	// ErrInvalid wraps validation failures.
	ErrInvalid = errors.New("invalid document")
)

// Doc is one stored document.
type Doc struct {
	NS        string          `json:"ns"`
	Key       string          `json:"key"`
	Version   int64           `json:"version"`
	UpdatedAt time.Time       `json:"updated_at"`
	Data      json.RawMessage `json:"data,omitempty"`
}

// ConflictError carries the current version on a conflict.
type ConflictError struct{ Current int64 }

func (e *ConflictError) Error() string { return ErrConflict.Error() }
func (e *ConflictError) Unwrap() error { return ErrConflict }

// Validate checks namespace, key and payload before a write.
func Validate(ns, key string, data []byte) error {
	switch ns {
	case NSWorld:
		if !WorldKinds[key] {
			return errors.Join(ErrInvalid, errors.New("unknown world kind"))
		}
		if len(data) > MaxWorldBytes {
			return errors.Join(ErrInvalid, errors.New("document too large"))
		}
	case NSPilot:
		if key == "" || len(key) > 128 {
			return errors.Join(ErrInvalid, errors.New("pilot id must be 1-128 chars"))
		}
		if len(data) > MaxPilotBytes {
			return errors.Join(ErrInvalid, errors.New("document too large"))
		}
	default:
		return errors.Join(ErrInvalid, errors.New("unknown namespace"))
	}
	trimmed := bytes.TrimSpace(data)
	if len(trimmed) == 0 || trimmed[0] != '{' || !json.Valid(trimmed) {
		return errors.Join(ErrInvalid, errors.New("data must be a JSON object"))
	}
	return nil
}

// Store persists documents.
type Store interface {
	Get(ctx context.Context, ns, key string) (Doc, bool, error)
	// Put writes data. ifVersion: AnyVersion skips the check, 0 requires that the doc doesn't exist,
	// n > 0 requires the stored version to be n. The returned doc has the new version.
	Put(ctx context.Context, ns, key string, data json.RawMessage, ifVersion int64) (Doc, error)
	// List returns document metadata (no data) in a namespace, sorted by key.
	List(ctx context.Context, ns string) ([]Doc, error)
}

func checkVersion(exists bool, current, ifVersion int64) error {
	switch {
	case ifVersion == AnyVersion:
		return nil
	case ifVersion == 0 && exists, ifVersion > 0 && (!exists || current != ifVersion):
		return &ConflictError{Current: current}
	}
	return nil
}

// MemStore is an in-memory Store for tests and local dev.
type MemStore struct {
	mu   sync.RWMutex
	docs map[string]Doc
	now  func() time.Time
}

func NewMemStore() *MemStore { return &MemStore{docs: map[string]Doc{}, now: time.Now} }

func memKey(ns, key string) string { return ns + "\x00" + key }

func (m *MemStore) Get(_ context.Context, ns, key string) (Doc, bool, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	d, ok := m.docs[memKey(ns, key)]
	if ok {
		d.Data = append(json.RawMessage(nil), d.Data...)
	}
	return d, ok, nil
}

func (m *MemStore) Put(_ context.Context, ns, key string, data json.RawMessage, ifVersion int64) (Doc, error) {
	if err := Validate(ns, key, data); err != nil {
		return Doc{}, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	cur, exists := m.docs[memKey(ns, key)]
	if err := checkVersion(exists, cur.Version, ifVersion); err != nil {
		return Doc{}, err
	}
	d := Doc{NS: ns, Key: key, Version: cur.Version + 1, UpdatedAt: m.now().UTC(), Data: append(json.RawMessage(nil), data...)}
	m.docs[memKey(ns, key)] = d
	return d, nil
}

func (m *MemStore) List(_ context.Context, ns string) ([]Doc, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := []Doc{}
	for _, d := range m.docs {
		if d.NS == ns {
			d.Data = nil
			out = append(out, d)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Key < out[j].Key })
	return out, nil
}

// PGStore persists documents in Postgres (state_docs).
type PGStore struct{ DB *sql.DB }

func (p PGStore) Get(ctx context.Context, ns, key string) (Doc, bool, error) {
	d := Doc{NS: ns, Key: key}
	var raw []byte
	err := p.DB.QueryRowContext(ctx, `SELECT version, updated_at, data FROM state_docs WHERE ns=$1 AND key=$2`, ns, key).Scan(&d.Version, &d.UpdatedAt, &raw)
	if errors.Is(err, sql.ErrNoRows) {
		return Doc{}, false, nil
	}
	if err != nil {
		return Doc{}, false, err
	}
	d.Data = raw
	return d, true, nil
}

func (p PGStore) Put(ctx context.Context, ns, key string, data json.RawMessage, ifVersion int64) (Doc, error) {
	if err := Validate(ns, key, data); err != nil {
		return Doc{}, err
	}
	tx, err := p.DB.BeginTx(ctx, nil)
	if err != nil {
		return Doc{}, err
	}
	defer func() { _ = tx.Rollback() }()
	var current int64
	err = tx.QueryRowContext(ctx, `SELECT version FROM state_docs WHERE ns=$1 AND key=$2 FOR UPDATE`, ns, key).Scan(&current)
	exists := err == nil
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return Doc{}, err
	}
	if err := checkVersion(exists, current, ifVersion); err != nil {
		return Doc{}, err
	}
	d := Doc{NS: ns, Key: key}
	if exists {
		err = tx.QueryRowContext(ctx, `UPDATE state_docs SET data=$3, version=version+1, updated_at=now() WHERE ns=$1 AND key=$2 RETURNING version, updated_at`,
			ns, key, []byte(data)).Scan(&d.Version, &d.UpdatedAt)
	} else {
		// A concurrent first insert loses on the primary key; ON CONFLICT turns that into a conflict, not a 500.
		err = tx.QueryRowContext(ctx, `INSERT INTO state_docs (ns, key, data) VALUES ($1, $2, $3) ON CONFLICT (ns, key) DO NOTHING RETURNING version, updated_at`,
			ns, key, []byte(data)).Scan(&d.Version, &d.UpdatedAt)
		if errors.Is(err, sql.ErrNoRows) {
			return Doc{}, &ConflictError{Current: 1}
		}
	}
	if err != nil {
		return Doc{}, err
	}
	if err := tx.Commit(); err != nil {
		return Doc{}, err
	}
	d.Data = data
	return d, nil
}

func (p PGStore) List(ctx context.Context, ns string) ([]Doc, error) {
	rows, err := p.DB.QueryContext(ctx, `SELECT key, version, updated_at FROM state_docs WHERE ns=$1 ORDER BY key`, ns)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Doc{}
	for rows.Next() {
		d := Doc{NS: ns}
		if err := rows.Scan(&d.Key, &d.Version, &d.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}

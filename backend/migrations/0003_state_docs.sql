-- M7: versioned world-state documents (economy, territory, rules) and per-pilot state
-- (wallet, standings, storage, hangar, active fit, hold). One JSONB document per (ns, key),
-- with an optimistic-concurrency version so two servers can't silently clobber each other.
CREATE TABLE IF NOT EXISTS state_docs (
  ns         TEXT        NOT NULL CHECK (ns IN ('world', 'pilot')),
  key        TEXT        NOT NULL,
  version    BIGINT      NOT NULL DEFAULT 1,
  data       JSONB       NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (ns, key)
);

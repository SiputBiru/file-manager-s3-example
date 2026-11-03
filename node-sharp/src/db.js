import Database from "better-sqlite3";
import { mkdirSync } from "node:fs";
import { dirname } from "node:path";

const path = process.env.DB_PATH || "./data/app.db";
mkdirSync(dirname(path), { recursive: true });

export const db = new Database(path);
db.pragma("journal_mode = WAL");

db.exec(`
  CREATE TABLE IF NOT EXISTS files (
    id             TEXT PRIMARY KEY,
    owner_id       TEXT NOT NULL,
    bucket         TEXT NOT NULL,
    original_key   TEXT NOT NULL,
    display_key    TEXT,
    thumb_key      TEXT,
    original_name  TEXT NOT NULL,
    content_type   TEXT NOT NULL,
    size_bytes     INTEGER NOT NULL,
    width          INTEGER,
    height         INTEGER,
    created_at     TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at     TEXT,
    deleted_at     TEXT,
    UNIQUE (bucket, original_key)
  );

  CREATE INDEX IF NOT EXISTS files_owner_idx ON files (owner_id);
`);

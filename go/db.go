package main

import (
	"context"
	"database/sql"
	"errors"
	"os"
	"path/filepath"

	_ "modernc.org/sqlite"
)

func openDB(path string) (*sql.DB, error) {
	if dir := filepath.Dir(path); dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, err
		}
	}

	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}

	db.SetMaxOpenConns(1)

	if _, err := db.Exec("PRAGMA journal_mode = WAL;"); err != nil {
		return nil, err
	}

	schema := `
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
);`

	if _, err := db.Exec(schema); err != nil {
		return nil, err
	}

	if _, err := db.Exec("CREATE INDEX IF NOT EXISTS files_owner_idx ON files (owner_id);"); err != nil {
		return nil, err
	}

	return db, nil
}

const rowColumns = `id, owner_id, bucket, original_key, display_key, thumb_key,
original_name, content_type, size_bytes, width, height, created_at, updated_at, deleted_at`

func scanRow(scanner interface{ Scan(...any) error }) (*FileRow, error) {
	var row FileRow
	err := scanner.Scan(
		&row.ID,
		&row.OwnerID,
		&row.Bucket,
		&row.OriginalKey,
		&row.DisplayKey,
		&row.ThumbKey,
		&row.OriginalName,
		&row.ContentType,
		&row.SizeBytes,
		&row.Width,
		&row.Height,
		&row.CreatedAt,
		&row.UpdatedAt,
		&row.DeletedAt,
	)
	if err != nil {
		return nil, err
	}
	return &row, nil
}

func getOwned(ctx context.Context, db *sql.DB, id, owner string) (*FileRow, error) {
	query := "SELECT " + rowColumns + " FROM files WHERE id = ? AND owner_id = ? AND deleted_at IS NULL"
	row, err := scanRow(db.QueryRowContext(ctx, query, id, owner))
	if errors.Is(err, sql.ErrNoRows) {
		return nil, sql.ErrNoRows
	}
	return row, err
}

func listOwned(ctx context.Context, db *sql.DB, owner string) ([]*FileRow, error) {
	query := "SELECT " + rowColumns + " FROM files WHERE owner_id = ? AND deleted_at IS NULL ORDER BY created_at DESC"
	rows, err := db.QueryContext(ctx, query, owner)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var result []*FileRow
	for rows.Next() {
		row, err := scanRow(rows)
		if err != nil {
			return nil, err
		}
		result = append(result, row)
	}

	return result, rows.Err()
}

package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
)

type server struct {
	db    *sql.DB
	store *storage
}

func ownerOf(r *http.Request) string {
	if owner := r.Header.Get("x-user-id"); owner != "" {
		return owner
	}
	return "demo-user"
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		log.Printf("failed to write json: %v", err)
	}
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

func decodeJSON(w http.ResponseWriter, r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	return json.NewDecoder(r.Body).Decode(dst)
}

func (s *server) item(ctx context.Context, row *FileRow) (FileItem, error) {
	displayKey := row.OriginalKey
	if row.DisplayKey.Valid && row.DisplayKey.String != "" {
		displayKey = row.DisplayKey.String
	}

	thumbKey := displayKey
	if row.ThumbKey.Valid && row.ThumbKey.String != "" {
		thumbKey = row.ThumbKey.String
	}

	displayURL, err := s.store.presignGet(ctx, displayKey)
	if err != nil {
		return FileItem{}, err
	}

	thumbURL, err := s.store.presignGet(ctx, thumbKey)
	if err != nil {
		return FileItem{}, err
	}

	item := FileItem{
		ID:         row.ID,
		Name:       row.OriginalName,
		Size:       row.SizeBytes,
		DisplayURL: displayURL,
		ThumbURL:   thumbURL,
	}

	if row.Width.Valid {
		width := int(row.Width.Int64)
		item.Width = &width
	}
	if row.Height.Valid {
		height := int(row.Height.Int64)
		item.Height = &height
	}

	return item, nil
}

func (s *server) handleUploadURL(w http.ResponseWriter, r *http.Request) {
	var req uploadURLRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "Invalid JSON body")
		return
	}

	if !allowedTypes[req.ContentType] {
		writeError(w, http.StatusUnsupportedMediaType, "Unsupported file type")
		return
	}
	if req.Size <= 0 || req.Size > maxUploadBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "File exceeds the 15 MB limit")
		return
	}

	fileID := newID()
	key := "originals/" + fileID + "/source"

	uploadURL, err := s.store.presignPut(r.Context(), key, req.ContentType)
	if err != nil {
		log.Printf("presign put: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to create upload URL")
		return
	}

	writeJSON(w, http.StatusOK, uploadURLResponse{
		FileID:    fileID,
		Key:       key,
		UploadURL: uploadURL,
		ExpiresIn: 600,
	})
}

func (s *server) handleConfirm(w http.ResponseWriter, r *http.Request) {
	var req confirmRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "Invalid JSON body")
		return
	}

	if req.FileID == "" || req.Key != "originals/"+req.FileID+"/source" {
		writeError(w, http.StatusBadRequest, "Key does not match fileId")
		return
	}

	head, err := s.store.head(r.Context(), req.Key)
	if err != nil {
		writeError(w, http.StatusNotFound, "Object not found")
		return
	}

	actualType := req.ContentType
	if head.ContentType != nil && *head.ContentType != "" {
		actualType = *head.ContentType
	}

	size := int64(0)
	if head.ContentLength != nil {
		size = *head.ContentLength
	}

	if !allowedTypes[actualType] {
		writeError(w, http.StatusUnsupportedMediaType, "Unsupported file type")
		return
	}
	if size > maxUploadBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "File exceeds the 15 MB limit")
		return
	}

	name := req.Name
	if name == "" {
		name = "untitled"
	}

	_, err = s.db.ExecContext(
		r.Context(),
		`INSERT OR IGNORE INTO files
		 (id, owner_id, bucket, original_key, original_name, content_type, size_bytes)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		req.FileID,
		ownerOf(r),
		s.store.bucket,
		req.Key,
		name,
		actualType,
		size,
	)
	if err != nil {
		log.Printf("insert file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to store metadata")
		return
	}

	row, err := getOwned(r.Context(), s.db, req.FileID, ownerOf(r))
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusConflict, "File is already registered to another user")
		return
	}
	if err != nil {
		log.Printf("get file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to read metadata")
		return
	}

	item, err := s.item(r.Context(), row)
	if err != nil {
		log.Printf("presign get: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to create display URL")
		return
	}

	writeJSON(w, http.StatusCreated, item)
}

func (s *server) handleList(w http.ResponseWriter, r *http.Request) {
	rows, err := listOwned(r.Context(), s.db, ownerOf(r))
	if err != nil {
		log.Printf("list files: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to read file list")
		return
	}

	items := make([]FileItem, 0, len(rows))
	for _, row := range rows {
		item, err := s.item(r.Context(), row)
		if err != nil {
			log.Printf("presign get: %v", err)
			writeError(w, http.StatusInternalServerError, "Failed to create display URL")
			return
		}
		items = append(items, item)
	}

	writeJSON(w, http.StatusOK, items)
}

func (s *server) handleEdit(w http.ResponseWriter, r *http.Request) {
	owner := ownerOf(r)
	row, err := getOwned(r.Context(), s.db, r.PathValue("id"), owner)
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusNotFound, "File not found")
		return
	}
	if err != nil {
		log.Printf("get file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to read metadata")
		return
	}

	var req editRequest
	if err := decodeJSON(w, r, &req); err != nil && !errors.Is(err, io.EOF) {
		writeError(w, http.StatusBadRequest, "Invalid JSON body")
		return
	}

	data, err := s.store.getBytes(r.Context(), row.OriginalKey, maxUploadBytes)
	if errors.Is(err, errFileTooLarge) {
		writeError(w, http.StatusRequestEntityTooLarge, "File exceeds the size limit")
		return
	}
	if err != nil {
		log.Printf("get object: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to download object")
		return
	}

	result, err := buildVariants(data, req.Mode, req.Width, req.Size)
	if errors.Is(err, errUnsupportedFormat) {
		writeError(w, http.StatusUnsupportedMediaType, "Unsupported image format")
		return
	}
	if errors.Is(err, errTooManyPixels) {
		writeError(w, http.StatusRequestEntityTooLarge, "Image dimensions are too large")
		return
	}
	if err != nil {
		log.Printf("build variants: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to process image")
		return
	}

	displayKey := "derived/" + row.ID + "/display.jpg"
	thumbKey := "derived/" + row.ID + "/thumb.jpg"

	if err := s.store.putBytes(r.Context(), displayKey, result.display, "image/jpeg"); err != nil {
		log.Printf("put display: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to store edited image")
		return
	}
	if err := s.store.putBytes(r.Context(), thumbKey, result.thumb, "image/jpeg"); err != nil {
		log.Printf("put thumb: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to store thumbnail")
		return
	}

	_, err = s.db.ExecContext(
		r.Context(),
		`UPDATE files
		 SET display_key = ?, thumb_key = ?, width = ?, height = ?, updated_at = datetime('now')
		 WHERE id = ?`,
		displayKey,
		thumbKey,
		result.width,
		result.height,
		row.ID,
	)
	if err != nil {
		log.Printf("update file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to update metadata")
		return
	}

	oldKeys := []string{}
	if row.DisplayKey.Valid {
		oldKeys = append(oldKeys, row.DisplayKey.String)
	}
	if row.ThumbKey.Valid {
		oldKeys = append(oldKeys, row.ThumbKey.String)
	}

	for _, key := range oldKeys {
		if key == "" || key == displayKey || key == thumbKey {
			continue
		}
		if err := s.store.delete(r.Context(), key); err != nil {
			log.Printf("failed to delete old derived object %s: %v", key, err)
		}
	}

	updated, err := getOwned(r.Context(), s.db, row.ID, owner)
	if err != nil {
		log.Printf("get updated file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to read updated metadata")
		return
	}

	item, err := s.item(r.Context(), updated)
	if err != nil {
		log.Printf("presign get: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to create display URL")
		return
	}

	writeJSON(w, http.StatusOK, item)
}

func (s *server) handleDelete(w http.ResponseWriter, r *http.Request) {
	owner := ownerOf(r)
	row, err := getOwned(r.Context(), s.db, r.PathValue("id"), owner)
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusNotFound, "File not found")
		return
	}
	if err != nil {
		log.Printf("get file: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to read metadata")
		return
	}

	if _, err := s.db.ExecContext(
		r.Context(),
		"UPDATE files SET deleted_at = datetime('now') WHERE id = ?",
		row.ID,
	); err != nil {
		log.Printf("soft delete: %v", err)
		writeError(w, http.StatusInternalServerError, "Failed to delete metadata")
		return
	}

	keys := []string{row.OriginalKey}
	if row.DisplayKey.Valid {
		keys = append(keys, row.DisplayKey.String)
	}
	if row.ThumbKey.Valid {
		keys = append(keys, row.ThumbKey.String)
	}

	for _, key := range keys {
		if key == "" {
			continue
		}
		if err := s.store.delete(r.Context(), key); err != nil {
			log.Printf("failed to delete object %s: %v", key, err)
		}
	}

	w.WriteHeader(http.StatusNoContent)
}

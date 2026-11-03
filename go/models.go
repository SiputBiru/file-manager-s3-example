package main

import "database/sql"

type FileRow struct {
	ID           string
	OwnerID      string
	Bucket       string
	OriginalKey  string
	DisplayKey   sql.NullString
	ThumbKey     sql.NullString
	OriginalName string
	ContentType  string
	SizeBytes    int64
	Width        sql.NullInt64
	Height       sql.NullInt64
	CreatedAt    string
	UpdatedAt    sql.NullString
	DeletedAt    sql.NullString
}

type FileItem struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Width      *int   `json:"width"`
	Height     *int   `json:"height"`
	Size       int64  `json:"size"`
	DisplayURL string `json:"displayUrl"`
	ThumbURL   string `json:"thumbUrl"`
}

type uploadURLRequest struct {
	Name        string `json:"name"`
	ContentType string `json:"contentType"`
	Size        int64  `json:"size"`
}

type uploadURLResponse struct {
	FileID    string `json:"fileId"`
	Key       string `json:"key"`
	UploadURL string `json:"uploadUrl"`
	ExpiresIn int    `json:"expiresIn"`
}

type confirmRequest struct {
	FileID      string `json:"fileId"`
	Key         string `json:"key"`
	Name        string `json:"name"`
	ContentType string `json:"contentType"`
}

type editRequest struct {
	Mode  string `json:"mode"`
	Width int    `json:"width"`
	Size  int    `json:"size"`
}

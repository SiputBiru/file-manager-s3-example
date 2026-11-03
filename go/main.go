package main

import (
	"context"
	"log"
	"net/http"
	"time"
)

func main() {
	ctx := context.Background()

	dbPath := env("DB_PATH", "./data/app.db")
	db, err := openDB(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer db.Close()

	store, err := newStorage(ctx)
	if err != nil {
		log.Fatalf("create s3 client: %v", err)
	}

	if err := store.ensureBucket(ctx); err != nil {
		log.Fatalf("prepare bucket: %v", err)
	}

	s := &server{db: db, store: store}

	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/files/upload-url", s.handleUploadURL)
	mux.HandleFunc("POST /api/files/confirm", s.handleConfirm)
	mux.HandleFunc("GET /api/files", s.handleList)
	mux.HandleFunc("POST /api/files/{id}/edit", s.handleEdit)
	mux.HandleFunc("DELETE /api/files/{id}", s.handleDelete)
	mux.Handle("/", http.FileServer(http.Dir("../public")))

	port := env("PORT", "3001")
	srv := &http.Server{
		Addr:              ":" + port,
		Handler:           logRequests(mux),
		ReadHeaderTimeout: 5 * time.Second,
	}

	log.Printf("go running at http://localhost:%s, bucket %s", port, store.bucket)
	log.Fatal(srv.ListenAndServe())
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start))
	})
}

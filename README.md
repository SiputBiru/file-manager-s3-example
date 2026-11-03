# File Manager S3 Example

<img width="800" alt="387734fd4141ed50301e1f21271bbacfe85cc08dcdeb3f9547b679c931e26944" src="https://github.com/user-attachments/assets/d85de864-d83f-4f34-8e11-eedf2938165a" />


One API contract, three implementations: Node.js with sharp, Go, and Rust with Axum. All of them use RustFS as a local S3 server and SQLite for file metadata.

The purpose is comparison after finishing the lab in the roadmap. Pick one language, run it, then compare which parts differ and which parts are identical.

Prerequisites: Node.js 20 or newer, Go 1.26 or newer, a stable Rust toolchain with cargo, and Docker to run RustFS.

## API Contract

| Method | Path | Function |
| :--- | :--- | :--- |
| `POST` | `/api/files/upload-url` | Request a presigned PUT |
| `POST` | `/api/files/confirm` | Verify the object and store metadata |
| `GET` | `/api/files` | List files owned by the current user |
| `POST` | `/api/files/:id/edit` | Resize or square crop |
| `DELETE` | `/api/files/:id` | Soft delete and remove the objects |

Identity header: `x-user-id`. If it is missing, the server falls back to `demo-user`.

## Database Schema

```sql
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
```

## Configuration

| Variable | Default | Description |
| :--- | :--- | :--- |
| `S3_ENDPOINT` | `http://localhost:9000` | RustFS address |
| `S3_REGION` | `us-east-1` | Dummy region for SigV4 |
| `S3_ACCESS_KEY` | `kelasadmin` | Access key |
| `S3_SECRET_KEY` | `kelasrahasia123` | Secret key |
| `S3_BUCKET` | `file-manager` | Bucket used by the app |
| `PORT` | differs per language | HTTP port |
| `DB_PATH` | `./data/app.db` | SQLite file location |

The `env.example` file in each folder is documentation only. The apps read environment variables directly and already have defaults that match the Compose file. To change a value, export the variable before starting the app:

```bash
export S3_BUCKET=another-bucket
export PORT=3010
```

## Running RustFS

From the repository root:

```bash
docker compose up -d
```

If the Compose plugin is not installed, use `docker-compose up -d`.

If another RustFS container already uses port 9000, stop it first:

```bash
docker stop rustfs-server
docker-compose up -d
```

The RustFS console is available at `http://localhost:9001`.

## Running the Implementations

### Node.js

```bash
cd node-sharp
npm install
npm run dev
```

Open `http://localhost:3000`.

### Go

```bash
cd go
go run .
```

Open `http://localhost:3001`.

### Rust and Axum

```bash
cd axum
cargo run
```

Open `http://localhost:3002`.

## Native GUI (raygui)

The repository also includes a native desktop client in `gui-raygui/`, written in C with raylib and raygui. It uses the same five API endpoints.

```bash
cd gui-raygui
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/gui-raygui
```

It supports drag and drop, a native file dialog, thumbnails, resize, square crop, saving the selected image, delete, and switching the user ID. The default backend is `http://localhost:3000`.

For a headless check:

```bash
./build/gui-raygui --selftest /path/to/image.jpg
```

The native client does not need CORS, because it is not a browser. The HTML page still does.

## Browser Uploads and CORS

The browser uploads directly to RustFS on port 9000, so the request is cross-origin. RustFS only sends CORS headers when `RUSTFS_CORS_ALLOWED_ORIGINS` is set. The Compose file includes this value:

```yaml
RUSTFS_CORS_ALLOWED_ORIGINS=http://localhost:3000,http://localhost:3001,http://localhost:3002
```

If you run your own RustFS instance instead of this Compose file, set the same variable and restart RustFS. Without it, the browser preflight `OPTIONS` request fails and you will see:

```text
No 'Access-Control-Allow-Origin' header is present on the requested resource
```

The Node client also disables the SDK default checksum parameters:

```javascript
requestChecksumCalculation: "WHEN_REQUIRED",
responseChecksumValidation: "WHEN_REQUIRED",
```

Without those options, the presigned URL contains `x-amz-checksum-crc32` computed for an empty body, and the browser `PUT` fails even after CORS is configured.

## Implementation Differences

| Aspect | Node.js | Go | Axum |
| :--- | :--- | :--- | :--- |
| HTTP | Express | `net/http` | axum |
| SQLite | better-sqlite3 | modernc.org/sqlite (pure Go) | rusqlite (bundled) |
| SQLite journal mode | WAL | WAL | WAL |
| S3 SDK | `@aws-sdk/client-s3` | `aws-sdk-go-v2` | `aws-sdk-s3` |
| Presigning | `s3-request-presigner` | `PresignClient` | `Presigned` builder |
| EXIF orientation | applied automatically by sharp | not applied | not applied |
| Binary size | requires Node and `node_modules` | single binary | single binary |

The Go and Axum examples do not apply EXIF orientation, so photos from a phone may appear rotated. The Node example handles it through `sharp().rotate()`. For production use, add an EXIF library to Go and Axum, or normalize orientation on the client before upload.

All three use the same table and columns. The SQL differs only in driver style:

- Node uses `ON CONFLICT(id) DO NOTHING`; Go and Axum use `INSERT OR IGNORE`.
- Node and Go use `?` placeholders; Axum uses `?1`, `?2`.
- Node selects an explicit column list, same as Go and Axum.
- Node and Go manage a single SQLite connection; Axum keeps one connection behind a mutex.

## Test Status

All three implementations were tested against RustFS 1.0.4 with the same flow:

1. Request a presigned URL.
2. Upload the object directly to RustFS.
3. Confirm the upload and store metadata.
4. List files, including thumbnails.
5. Resize to a width of 400.
6. Square crop to 300 x 300.
7. Reject access from another user.
8. Delete the file and its objects.

## Notes

- Every implementation serves the UI from the `public/` folder at the repository root.
- Every implementation uses the same bucket, schema, and five endpoints.
- Every implementation runs `ensureBucket` on startup, so the bucket is created automatically.
- The ports differ so all three can run at the same time.
- Images are processed on the server, not in the browser.
- The original object is never overwritten. Every edit creates new derived objects.
- A native raygui client is available in `gui-raygui/`. The HTML page remains available.

## Troubleshooting

| Symptom | Cause and fix |
| :--- | :--- |
| CORS preflight error on upload | `RUSTFS_CORS_ALLOWED_ORIGINS` is missing or does not include the app origin. Update the Compose file and restart RustFS. |
| `x-amz-checksum-crc32` in the presigned URL | The AWS SDK added checksum parameters for an empty body. The Node client disables this with `requestChecksumCalculation: "WHEN_REQUIRED"`. |
| `403 SignatureDoesNotMatch` on upload | The `Content-Type` used to create the presigned URL differs from the one sent with the `PUT`. |
| `favicon.ico` returns `404` | Harmless browser request. `public/index.html` includes an empty data icon to silence it. |
| `413` | The file is larger than 15 MB. |
| `415` | The file is not JPEG, PNG, or WebP. |
| `docker: unknown command: docker compose` | Use `docker-compose` instead. |
| `port is already allocated` | Another RustFS container is using port 9000. Stop it before starting this one. |

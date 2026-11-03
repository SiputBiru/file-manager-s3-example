mod images;

use std::sync::Arc;
use std::time::Duration;

use aws_sdk_s3::config::{Credentials, Region};
use aws_sdk_s3::presigning::PresigningConfig;
use aws_sdk_s3::primitives::ByteStream;
use aws_sdk_s3::Client;
use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{delete, get, post};
use axum::{Json, Router};
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::json;
use tokio::sync::Mutex;
use tower_http::services::ServeDir;
use uuid::Uuid;

use images::{build_variants, ImageError};

const MAX_UPLOAD_BYTES: i64 = 15 * 1024 * 1024;
const PRESIGN_SECONDS: u64 = 600;

const SCHEMA: &str = r#"
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
"#;

const SELECT_COLUMNS: &str =
    "id, original_key, display_key, thumb_key, original_name, size_bytes, width, height";

struct AppState {
    db: Mutex<Connection>,
    s3: Client,
    bucket: String,
}

type Shared = Arc<AppState>;

struct FileRow {
    id: String,
    original_key: String,
    display_key: Option<String>,
    thumb_key: Option<String>,
    original_name: String,
    size_bytes: i64,
    width: Option<i64>,
    height: Option<i64>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct FileItem {
    id: String,
    name: String,
    width: Option<u32>,
    height: Option<u32>,
    size: i64,
    display_url: String,
    thumb_url: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct UploadUrlRequest {
    content_type: String,
    size: i64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct UploadUrlResponse {
    file_id: String,
    key: String,
    upload_url: String,
    expires_in: u64,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ConfirmRequest {
    file_id: String,
    key: String,
    name: Option<String>,
    content_type: String,
}

#[derive(Deserialize)]
struct EditRequest {
    mode: Option<String>,
    width: Option<u32>,
    size: Option<u32>,
}

struct AppError {
    status: StatusCode,
    message: String,
}

impl AppError {
    fn new(status: StatusCode, message: impl Into<String>) -> Self {
        Self {
            status,
            message: message.into(),
        }
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        (self.status, Json(json!({ "error": self.message }))).into_response()
    }
}

fn internal<E: std::fmt::Display>(err: E) -> AppError {
    eprintln!("{err}");
    AppError::new(StatusCode::INTERNAL_SERVER_ERROR, "Internal server error")
}

type AppResult<T> = Result<T, AppError>;

fn env(key: &str, fallback: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| fallback.to_string())
}

fn allowed_type(value: &str) -> bool {
    matches!(value, "image/jpeg" | "image/png" | "image/webp")
}

fn owner_of(headers: &HeaderMap) -> String {
    headers
        .get("x-user-id")
        .and_then(|value| value.to_str().ok())
        .filter(|value| !value.is_empty())
        .unwrap_or("demo-user")
        .to_string()
}

fn map_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<FileRow> {
    Ok(FileRow {
        id: row.get(0)?,
        original_key: row.get(1)?,
        display_key: row.get(2)?,
        thumb_key: row.get(3)?,
        original_name: row.get(4)?,
        size_bytes: row.get(5)?,
        width: row.get(6)?,
        height: row.get(7)?,
    })
}

fn get_owned(conn: &Connection, id: &str, owner: &str) -> rusqlite::Result<Option<FileRow>> {
    let query = format!(
        "SELECT {SELECT_COLUMNS} FROM files WHERE id = ?1 AND owner_id = ?2 AND deleted_at IS NULL"
    );

    conn.query_row(&query, params![id, owner], map_row)
        .optional()
}

fn list_owned(conn: &Connection, owner: &str) -> rusqlite::Result<Vec<FileRow>> {
    let query = format!(
        "SELECT {SELECT_COLUMNS} FROM files WHERE owner_id = ?1 AND deleted_at IS NULL ORDER BY created_at DESC"
    );

    let mut stmt = conn.prepare(&query)?;
    let rows = stmt.query_map(params![owner], map_row)?;

    rows.collect()
}

async fn presign_put(state: &AppState, key: &str, content_type: &str) -> AppResult<String> {
    let config =
        PresigningConfig::expires_in(Duration::from_secs(PRESIGN_SECONDS)).map_err(internal)?;

    let request = state
        .s3
        .put_object()
        .bucket(&state.bucket)
        .key(key)
        .content_type(content_type)
        .presigned(config)
        .await
        .map_err(internal)?;

    Ok(request.uri().to_string())
}

async fn presign_get(state: &AppState, key: &str) -> AppResult<String> {
    let config =
        PresigningConfig::expires_in(Duration::from_secs(PRESIGN_SECONDS)).map_err(internal)?;

    let request = state
        .s3
        .get_object()
        .bucket(&state.bucket)
        .key(key)
        .presigned(config)
        .await
        .map_err(internal)?;

    Ok(request.uri().to_string())
}

async fn to_item(state: &AppState, row: &FileRow) -> AppResult<FileItem> {
    let display_key = row
        .display_key
        .clone()
        .unwrap_or_else(|| row.original_key.clone());

    let thumb_key = row.thumb_key.clone().unwrap_or_else(|| display_key.clone());

    let display_url = presign_get(state, &display_key).await?;
    let thumb_url = presign_get(state, &thumb_key).await?;

    Ok(FileItem {
        id: row.id.clone(),
        name: row.original_name.clone(),
        width: row.width.map(|value| value as u32),
        height: row.height.map(|value| value as u32),
        size: row.size_bytes,
        display_url,
        thumb_url,
    })
}

async fn upload_url(
    State(state): State<Shared>,
    Json(req): Json<UploadUrlRequest>,
) -> AppResult<Json<UploadUrlResponse>> {
    if !allowed_type(&req.content_type) {
        return Err(AppError::new(
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "Unsupported file type",
        ));
    }

    if req.size <= 0 || req.size > MAX_UPLOAD_BYTES {
        return Err(AppError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "File exceeds the 15 MB limit",
        ));
    }

    let file_id = Uuid::new_v4().to_string();
    let key = format!("originals/{file_id}/source");
    let upload_url = presign_put(&state, &key, &req.content_type).await?;

    Ok(Json(UploadUrlResponse {
        file_id,
        key,
        upload_url,
        expires_in: PRESIGN_SECONDS,
    }))
}

async fn confirm(
    State(state): State<Shared>,
    headers: HeaderMap,
    Json(req): Json<ConfirmRequest>,
) -> AppResult<(StatusCode, Json<FileItem>)> {
    let owner = owner_of(&headers);

    if req.file_id.is_empty() || req.key != format!("originals/{}/source", req.file_id) {
        return Err(AppError::new(
            StatusCode::BAD_REQUEST,
            "Key does not match fileId",
        ));
    }

    let head = state
        .s3
        .head_object()
        .bucket(&state.bucket)
        .key(&req.key)
        .send()
        .await
        .map_err(|_| AppError::new(StatusCode::NOT_FOUND, "Object not found"))?;

    let actual_type = head
        .content_type()
        .map(|value| value.to_string())
        .unwrap_or_else(|| req.content_type.clone());

    let size = head.content_length().unwrap_or(0);

    if !allowed_type(&actual_type) {
        return Err(AppError::new(
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "Unsupported file type",
        ));
    }

    if size > MAX_UPLOAD_BYTES {
        return Err(AppError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "File exceeds the 15 MB limit",
        ));
    }

    let name = req
        .name
        .clone()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "untitled".to_string());

    {
        let conn = state.db.lock().await;
        conn.execute(
            "INSERT OR IGNORE INTO files
             (id, owner_id, bucket, original_key, original_name, content_type, size_bytes)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![
                &req.file_id,
                &owner,
                &state.bucket,
                &req.key,
                &name,
                &actual_type,
                size,
            ],
        )
        .map_err(internal)?;
    }

    let row = {
        let conn = state.db.lock().await;
        get_owned(&conn, &req.file_id, &owner).map_err(internal)?
    };

    let row = row.ok_or_else(|| {
        AppError::new(
            StatusCode::CONFLICT,
            "File is already registered to another user",
        )
    })?;

    Ok((StatusCode::CREATED, Json(to_item(&state, &row).await?)))
}

async fn list_files(
    State(state): State<Shared>,
    headers: HeaderMap,
) -> AppResult<Json<Vec<FileItem>>> {
    let owner = owner_of(&headers);

    let rows = {
        let conn = state.db.lock().await;
        list_owned(&conn, &owner).map_err(internal)?
    };

    let mut items = Vec::with_capacity(rows.len());
    for row in &rows {
        items.push(to_item(&state, row).await?);
    }

    Ok(Json(items))
}

async fn edit_file(
    State(state): State<Shared>,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(req): Json<EditRequest>,
) -> AppResult<Json<FileItem>> {
    let owner = owner_of(&headers);

    let row = {
        let conn = state.db.lock().await;
        get_owned(&conn, &id, &owner).map_err(internal)?
    }
    .ok_or_else(|| AppError::new(StatusCode::NOT_FOUND, "File not found"))?;

    let object = state
        .s3
        .get_object()
        .bucket(&state.bucket)
        .key(&row.original_key)
        .send()
        .await
        .map_err(internal)?;

    let data = object.body.collect().await.map_err(internal)?.into_bytes();

    if data.len() as i64 > MAX_UPLOAD_BYTES {
        return Err(AppError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "File exceeds the size limit",
        ));
    }

    let mode = req.mode.as_deref().unwrap_or("resize");

    let variants = match build_variants(&data, mode, req.width, req.size) {
        Ok(value) => value,
        Err(ImageError::Unsupported) | Err(ImageError::Decode(_)) => {
            return Err(AppError::new(
                StatusCode::UNSUPPORTED_MEDIA_TYPE,
                "Unsupported image format",
            ))
        }
        Err(ImageError::TooManyPixels) => {
            return Err(AppError::new(
                StatusCode::PAYLOAD_TOO_LARGE,
                "Image dimensions are too large",
            ))
        }
    };

    let display_key = format!("derived/{}/display.jpg", row.id);
    let thumb_key = format!("derived/{}/thumb.jpg", row.id);

    state
        .s3
        .put_object()
        .bucket(&state.bucket)
        .key(&display_key)
        .content_type("image/jpeg")
        .body(ByteStream::from(variants.display))
        .send()
        .await
        .map_err(internal)?;

    state
        .s3
        .put_object()
        .bucket(&state.bucket)
        .key(&thumb_key)
        .content_type("image/jpeg")
        .body(ByteStream::from(variants.thumb))
        .send()
        .await
        .map_err(internal)?;

    {
        let conn = state.db.lock().await;
        conn.execute(
            "UPDATE files
             SET display_key = ?1, thumb_key = ?2, width = ?3, height = ?4, updated_at = datetime('now')
             WHERE id = ?5",
            params![
                &display_key,
                &thumb_key,
                variants.width as i64,
                variants.height as i64,
                &row.id,
            ],
        )
        .map_err(internal)?;
    }

    let old_keys = [row.display_key.clone(), row.thumb_key.clone()];
    for key in old_keys.into_iter().flatten() {
        if key.is_empty() || key == display_key || key == thumb_key {
            continue;
        }

        if let Err(err) = state
            .s3
            .delete_object()
            .bucket(&state.bucket)
            .key(&key)
            .send()
            .await
        {
            eprintln!("failed to delete old derived object {key}: {err}");
        }
    }

    let updated = {
        let conn = state.db.lock().await;
        get_owned(&conn, &row.id, &owner).map_err(internal)?
    }
    .ok_or_else(|| AppError::new(StatusCode::NOT_FOUND, "File not found"))?;

    Ok(Json(to_item(&state, &updated).await?))
}

async fn delete_file(
    State(state): State<Shared>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> AppResult<StatusCode> {
    let owner = owner_of(&headers);

    let row = {
        let conn = state.db.lock().await;
        get_owned(&conn, &id, &owner).map_err(internal)?
    }
    .ok_or_else(|| AppError::new(StatusCode::NOT_FOUND, "File not found"))?;

    {
        let conn = state.db.lock().await;
        conn.execute(
            "UPDATE files SET deleted_at = datetime('now') WHERE id = ?1",
            params![&row.id],
        )
        .map_err(internal)?;
    }

    let keys = [
        Some(row.original_key.clone()),
        row.display_key.clone(),
        row.thumb_key.clone(),
    ];

    for key in keys.into_iter().flatten() {
        if key.is_empty() {
            continue;
        }

        if let Err(err) = state
            .s3
            .delete_object()
            .bucket(&state.bucket)
            .key(&key)
            .send()
            .await
        {
            eprintln!("failed to delete object {key}: {err}");
        }
    }

    Ok(StatusCode::NO_CONTENT)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let endpoint = env("S3_ENDPOINT", "http://localhost:9000");
    let region = env("S3_REGION", "us-east-1");
    let access_key = env("S3_ACCESS_KEY", "kelasadmin");
    let secret_key = env("S3_SECRET_KEY", "kelasrahasia123");
    let bucket = env("S3_BUCKET", "file-manager");

    let credentials = Credentials::new(access_key, secret_key, None, None, "static");

    let shared_config = aws_config::defaults(aws_config::BehaviorVersion::latest())
        .region(Region::new(region))
        .credentials_provider(credentials)
        .load()
        .await;

    let s3_config = aws_sdk_s3::config::Builder::from(&shared_config)
        .endpoint_url(endpoint)
        .force_path_style(true)
        .build();

    let client = Client::from_conf(s3_config);

    if client.head_bucket().bucket(&bucket).send().await.is_err() {
        client
            .create_bucket()
            .bucket(&bucket)
            .send()
            .await
            .map_err(|err| err.to_string())?;
    }

    let db_path = env("DB_PATH", "./data/app.db");
    if let Some(parent) = std::path::Path::new(&db_path).parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }

    let conn = Connection::open(&db_path)?;
    conn.execute_batch("PRAGMA journal_mode = WAL;")?;
    conn.execute_batch(SCHEMA)?;

    let state = Arc::new(AppState {
        db: Mutex::new(conn),
        s3: client,
        bucket,
    });

    let app = Router::new()
        .route("/api/files/upload-url", post(upload_url))
        .route("/api/files/confirm", post(confirm))
        .route("/api/files", get(list_files))
        .route("/api/files/{id}/edit", post(edit_file))
        .route("/api/files/{id}", delete(delete_file))
        .fallback_service(ServeDir::new("../public"))
        .with_state(state);

    let port: u16 = env("PORT", "3002").parse()?;
    let listener = tokio::net::TcpListener::bind(("0.0.0.0", port)).await?;

    println!("axum running at http://localhost:{port}");
    axum::serve(listener, app).await?;

    Ok(())
}

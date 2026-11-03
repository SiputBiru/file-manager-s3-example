import { Router } from "express";
import { randomUUID } from "node:crypto";
import {
  HeadObjectCommand,
  GetObjectCommand,
  PutObjectCommand,
  DeleteObjectCommand,
} from "@aws-sdk/client-s3";
import { db } from "../db.js";
import { BUCKET, s3, presignGet, presignPut } from "../s3.js";
import {
  ALLOWED_TYPES,
  MAX_UPLOAD_BYTES,
  buildVariants,
  httpError,
} from "../image.js";

export const router = Router();

const wrap = (fn) => (req, res, next) => fn(req, res, next).catch(next);
const ownerOf = (req) => req.header("x-user-id") || "demo-user";

const COLUMNS =
  "id, owner_id, bucket, original_key, display_key, thumb_key, original_name, " +
  "content_type, size_bytes, width, height, created_at, updated_at, deleted_at";

const owned = db.prepare(
  `SELECT ${COLUMNS} FROM files WHERE id = ? AND owner_id = ? AND deleted_at IS NULL`
);

async function item(row) {
  const displayKey = row.display_key || row.original_key;
  const thumbKey = row.thumb_key || displayKey;
  const [displayUrl, thumbUrl] = await Promise.all([
    presignGet(displayKey),
    presignGet(thumbKey),
  ]);

  return {
    id: row.id,
    name: row.original_name,
    width: row.width,
    height: row.height,
    size: row.size_bytes,
    displayUrl,
    thumbUrl,
  };
}

router.post(
  "/upload-url",
  wrap(async (req, res) => {
    const { name, contentType, size } = req.body;

    if (!ALLOWED_TYPES.has(contentType)) {
      throw httpError(415, "Unsupported file type");
    }
    if (!Number.isInteger(size) || size <= 0 || size > MAX_UPLOAD_BYTES) {
      throw httpError(413, "File exceeds the 15 MB limit");
    }

    const fileId = randomUUID();
    const key = `originals/${fileId}/source`;

    res.json({
      fileId,
      key,
      uploadUrl: await presignPut(key, contentType),
      expiresIn: 600,
      name,
    });
  })
);

router.post(
  "/confirm",
  wrap(async (req, res) => {
    const owner = ownerOf(req);
    const { fileId, key, name, contentType } = req.body;

    if (!fileId || key !== `originals/${fileId}/source`) {
      throw httpError(400, "Key does not match fileId");
    }

    const head = await s3.send(new HeadObjectCommand({ Bucket: BUCKET, Key: key }));
    const actualType = head.ContentType || contentType;

    if (!ALLOWED_TYPES.has(actualType)) {
      throw httpError(415, "Unsupported file type");
    }
    if (head.ContentLength > MAX_UPLOAD_BYTES) {
      throw httpError(413, "File exceeds the 15 MB limit");
    }

    db.prepare(
      `INSERT INTO files (id, owner_id, bucket, original_key, original_name, content_type, size_bytes)
       VALUES (?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(id) DO NOTHING`
    ).run(fileId, owner, BUCKET, key, name || "untitled", actualType, head.ContentLength);

    const row = owned.get(fileId, owner);
    if (!row) throw httpError(409, "File is already registered to another user");

    res.status(201).json(await item(row));
  })
);

router.get(
  "/",
  wrap(async (req, res) => {
    const rows = db
      .prepare(
        `SELECT ${COLUMNS} FROM files WHERE owner_id = ? AND deleted_at IS NULL ORDER BY created_at DESC`
      )
      .all(ownerOf(req));

    res.json(await Promise.all(rows.map(item)));
  })
);

router.post(
  "/:id/edit",
  wrap(async (req, res) => {
    const owner = ownerOf(req);
    const row = owned.get(req.params.id, owner);
    if (!row) throw httpError(404, "File not found");

    const obj = await s3.send(
      new GetObjectCommand({ Bucket: BUCKET, Key: row.original_key })
    );

    const chunks = [];
    for await (const chunk of obj.Body) chunks.push(chunk);
    const buf = Buffer.concat(chunks);

    if (buf.length > MAX_UPLOAD_BYTES) {
      throw httpError(413, "File exceeds the size limit");
    }

    const { display, thumb, width, height } = await buildVariants(buf, req.body);

    const displayKey = `derived/${row.id}/display.jpg`;
    const thumbKey = `derived/${row.id}/thumb.jpg`;

    await s3.send(
      new PutObjectCommand({
        Bucket: BUCKET,
        Key: displayKey,
        Body: display,
        ContentType: "image/jpeg",
      })
    );
    await s3.send(
      new PutObjectCommand({
        Bucket: BUCKET,
        Key: thumbKey,
        Body: thumb,
        ContentType: "image/jpeg",
      })
    );

    db.prepare(
      `UPDATE files
       SET display_key = ?, thumb_key = ?, width = ?, height = ?, updated_at = datetime('now')
       WHERE id = ?`
    ).run(displayKey, thumbKey, width, height, row.id);

    const oldKeys = [row.display_key, row.thumb_key].filter(
      (k) => k && k !== displayKey && k !== thumbKey
    );

    for (const oldKey of oldKeys) {
      await s3
        .send(new DeleteObjectCommand({ Bucket: BUCKET, Key: oldKey }))
        .catch((err) => console.error("failed to delete old derived object:", oldKey, err.name));
    }

    const updated = owned.get(row.id, owner);
    res.json(await item(updated));
  })
);

router.delete(
  "/:id",
  wrap(async (req, res) => {
    const owner = ownerOf(req);
    const row = owned.get(req.params.id, owner);
    if (!row) throw httpError(404, "File not found");

    db.prepare("UPDATE files SET deleted_at = datetime('now') WHERE id = ?").run(row.id);

    const keys = [row.original_key, row.display_key, row.thumb_key].filter(Boolean);
    for (const key of keys) {
      await s3
        .send(new DeleteObjectCommand({ Bucket: BUCKET, Key: key }))
        .catch((err) => console.error("failed to delete object:", key, err.name));
    }

    res.status(204).end();
  })
);

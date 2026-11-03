import sharp from "sharp";

export const MAX_UPLOAD_BYTES = 15 * 1024 * 1024;
export const ALLOWED_TYPES = new Set(["image/jpeg", "image/png", "image/webp"]);
const ALLOWED_FORMATS = new Set(["jpeg", "png", "webp"]);
const MAX_PIXELS = 40_000_000;

export function httpError(status, message) {
  const err = new Error(message);
  err.status = status;
  return err;
}

export async function buildVariants(buf, { mode = "resize", width, size } = {}) {
  const meta = await sharp(buf, { limitInputPixels: MAX_PIXELS }).metadata();
  if (!ALLOWED_FORMATS.has(meta.format)) {
    throw httpError(415, "Unsupported image format");
  }

  const target =
    mode === "square"
      ? Math.min(Number(size) || 400, 2000)
      : Math.min(Number(width) || 1600, 4000);

  const displayPipeline = sharp(buf, { limitInputPixels: MAX_PIXELS }).rotate();

  if (mode === "square") {
    displayPipeline.resize({
      width: target,
      height: target,
      fit: "cover",
      position: "centre",
    });
  } else {
    displayPipeline.resize({ width: target, withoutEnlargement: true });
  }

  const display = await displayPipeline.jpeg({ quality: 82 }).toBuffer();

  const thumb = await sharp(buf, { limitInputPixels: MAX_PIXELS })
    .rotate()
    .resize({ width: 400, height: 400, fit: "cover", position: "centre" })
    .jpeg({ quality: 80 })
    .toBuffer();

  const out = await sharp(display).metadata();
  return { display, thumb, width: out.width, height: out.height };
}

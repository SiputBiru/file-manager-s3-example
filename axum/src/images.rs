use std::fmt;
use std::io::Cursor;

use image::imageops::FilterType;
use image::{DynamicImage, ImageFormat, ImageReader};

pub enum ImageError {
    Unsupported,
    TooManyPixels,
    Decode(String),
}

impl fmt::Display for ImageError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImageError::Unsupported => write!(f, "unsupported image format"),
            ImageError::TooManyPixels => write!(f, "image dimensions are too large"),
            ImageError::Decode(message) => write!(f, "failed to read image: {message}"),
        }
    }
}

pub struct Variants {
    pub display: Vec<u8>,
    pub thumb: Vec<u8>,
    pub width: u32,
    pub height: u32,
}

pub fn build_variants(
    data: &[u8],
    mode: &str,
    width: Option<u32>,
    size: Option<u32>,
) -> Result<Variants, ImageError> {
    let reader = ImageReader::new(Cursor::new(data))
        .with_guessed_format()
        .map_err(|err| ImageError::Decode(err.to_string()))?;

    let format = reader.format().ok_or(ImageError::Unsupported)?;

    if !matches!(
        format,
        ImageFormat::Jpeg | ImageFormat::Png | ImageFormat::WebP
    ) {
        return Err(ImageError::Unsupported);
    }

    let img = reader
        .decode()
        .map_err(|err| ImageError::Decode(err.to_string()))?;

    if img.width() as u64 * img.height() as u64 > 40_000_000 {
        return Err(ImageError::TooManyPixels);
    }

    let display = if mode == "square" {
        let target = size.unwrap_or(400).clamp(1, 2000);
        square(&img, target)
    } else {
        let target = width.unwrap_or(1600).clamp(1, 4000).min(img.width());
        img.resize(target, u32::MAX, FilterType::Lanczos3)
    };

    let thumb = square(&img, 400);

    let out_width = display.width();
    let out_height = display.height();

    Ok(Variants {
        display: encode_jpeg(&display, 82)?,
        thumb: encode_jpeg(&thumb, 80)?,
        width: out_width,
        height: out_height,
    })
}

fn square(img: &DynamicImage, target: u32) -> DynamicImage {
    let side = img.width().min(img.height());
    let x = (img.width() - side) / 2;
    let y = (img.height() - side) / 2;

    img.crop_imm(x, y, side, side)
        .resize_exact(target, target, FilterType::Lanczos3)
}

fn encode_jpeg(img: &DynamicImage, quality: u8) -> Result<Vec<u8>, ImageError> {
    let mut out = Vec::new();
    let mut encoder = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, quality);
    encoder
        .encode_image(img)
        .map_err(|err| ImageError::Decode(err.to_string()))?;
    Ok(out)
}

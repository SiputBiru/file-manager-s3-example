package main

import (
	"bytes"
	"errors"
	"image"
	"image/jpeg"
	_ "image/png"
	"math"

	"golang.org/x/image/draw"
	_ "golang.org/x/image/webp"
)

const (
	maxUploadBytes int64 = 15 * 1024 * 1024
	maxPixels            = 40_000_000
)

var (
	errFileTooLarge      = errors.New("file exceeds the size limit")
	errUnsupportedFormat = errors.New("unsupported image format")
	errTooManyPixels     = errors.New("image dimensions are too large")
)

var allowedTypes = map[string]bool{
	"image/jpeg": true,
	"image/png":  true,
	"image/webp": true,
}

var allowedFormats = map[string]bool{
	"jpeg": true,
	"png":  true,
	"webp": true,
}

type variants struct {
	display []byte
	thumb   []byte
	width   int
	height  int
}

func buildVariants(data []byte, mode string, width, size int) (*variants, error) {
	cfg, format, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		return nil, errUnsupportedFormat
	}
	if cfg.Width*cfg.Height > maxPixels {
		return nil, errTooManyPixels
	}
	if !allowedFormats[format] {
		return nil, errUnsupportedFormat
	}

	src, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, errUnsupportedFormat
	}

	var display image.Image
	if mode == "square" {
		target := size
		if target <= 0 {
			target = 400
		}
		if target > 2000 {
			target = 2000
		}
		display = square(src, target)
	} else {
		target := width
		if target <= 0 {
			target = 1600
		}
		if target > 4000 {
			target = 4000
		}
		display = resizeWidth(src, target)
	}

	thumb := square(src, 400)

	displayData, err := encodeJPEG(display, 82)
	if err != nil {
		return nil, err
	}
	thumbData, err := encodeJPEG(thumb, 80)
	if err != nil {
		return nil, err
	}

	bounds := display.Bounds()

	return &variants{
		display: displayData,
		thumb:   thumbData,
		width:   bounds.Dx(),
		height:  bounds.Dy(),
	}, nil
}

func resizeWidth(src image.Image, target int) image.Image {
	b := src.Bounds()
	sw, sh := b.Dx(), b.Dy()

	if target > sw {
		target = sw
	}
	if target < 1 {
		target = 1
	}

	th := int(math.Round(float64(sh) * float64(target) / float64(sw)))
	if th < 1 {
		th = 1
	}

	dst := image.NewRGBA(image.Rect(0, 0, target, th))
	draw.CatmullRom.Scale(dst, dst.Bounds(), src, b, draw.Over, nil)
	return dst
}

func square(src image.Image, target int) image.Image {
	b := src.Bounds()
	side := b.Dx()
	if b.Dy() < side {
		side = b.Dy()
	}

	x0 := b.Min.X + (b.Dx()-side)/2
	y0 := b.Min.Y + (b.Dy()-side)/2

	crop := image.NewRGBA(image.Rect(0, 0, side, side))
	draw.Draw(crop, crop.Bounds(), src, image.Pt(x0, y0), draw.Src)

	dst := image.NewRGBA(image.Rect(0, 0, target, target))
	draw.CatmullRom.Scale(dst, dst.Bounds(), crop, crop.Bounds(), draw.Over, nil)
	return dst
}

func encodeJPEG(img image.Image, quality int) ([]byte, error) {
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: quality}); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

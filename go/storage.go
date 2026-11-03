package main

import (
	"bytes"
	"context"
	"io"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
)

type storage struct {
	client *s3.Client
	bucket string
}

func newStorage(ctx context.Context) (*storage, error) {
	endpoint := env("S3_ENDPOINT", "http://localhost:9000")
	region := env("S3_REGION", "us-east-1")
	accessKey := env("S3_ACCESS_KEY", "kelasadmin")
	secretKey := env("S3_SECRET_KEY", "kelasrahasia123")
	bucket := env("S3_BUCKET", "file-manager")

	cfg, err := config.LoadDefaultConfig(
		ctx,
		config.WithRegion(region),
		config.WithCredentialsProvider(
			credentials.NewStaticCredentialsProvider(accessKey, secretKey, ""),
		),
	)
	if err != nil {
		return nil, err
	}

	client := s3.NewFromConfig(cfg, func(o *s3.Options) {
		o.BaseEndpoint = aws.String(endpoint)
		o.UsePathStyle = true
	})

	return &storage{client: client, bucket: bucket}, nil
}

func (s *storage) ensureBucket(ctx context.Context) error {
	_, err := s.client.HeadBucket(ctx, &s3.HeadBucketInput{
		Bucket: aws.String(s.bucket),
	})
	if err == nil {
		return nil
	}

	_, createErr := s.client.CreateBucket(ctx, &s3.CreateBucketInput{
		Bucket: aws.String(s.bucket),
	})
	return createErr
}

func (s *storage) presignPut(ctx context.Context, key, contentType string) (string, error) {
	client := s3.NewPresignClient(s.client)
	out, err := client.PresignPutObject(ctx, &s3.PutObjectInput{
		Bucket:      aws.String(s.bucket),
		Key:         aws.String(key),
		ContentType: aws.String(contentType),
	}, func(o *s3.PresignOptions) {
		o.Expires = 10 * time.Minute
	})
	if err != nil {
		return "", err
	}
	return out.URL, nil
}

func (s *storage) presignGet(ctx context.Context, key string) (string, error) {
	client := s3.NewPresignClient(s.client)
	out, err := client.PresignGetObject(ctx, &s3.GetObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	}, func(o *s3.PresignOptions) {
		o.Expires = 10 * time.Minute
	})
	if err != nil {
		return "", err
	}
	return out.URL, nil
}

func (s *storage) head(ctx context.Context, key string) (*s3.HeadObjectOutput, error) {
	return s.client.HeadObject(ctx, &s3.HeadObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	})
}

func (s *storage) getBytes(ctx context.Context, key string, max int64) ([]byte, error) {
	out, err := s.client.GetObject(ctx, &s3.GetObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	})
	if err != nil {
		return nil, err
	}
	defer out.Body.Close()

	data, err := io.ReadAll(io.LimitReader(out.Body, max+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > max {
		return nil, errFileTooLarge
	}
	return data, nil
}

func (s *storage) putBytes(ctx context.Context, key string, data []byte, contentType string) error {
	_, err := s.client.PutObject(ctx, &s3.PutObjectInput{
		Bucket:      aws.String(s.bucket),
		Key:         aws.String(key),
		Body:        bytes.NewReader(data),
		ContentType: aws.String(contentType),
	})
	return err
}

func (s *storage) delete(ctx context.Context, key string) error {
	_, err := s.client.DeleteObject(ctx, &s3.DeleteObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	})
	return err
}

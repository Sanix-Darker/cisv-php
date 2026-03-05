# cisv-php

PHP extension distribution for CISV.

## Build extension

```bash
make all
```

## Validate exported API

```bash
php -d extension=bindings/php/modules/cisv.so bindings/php/scripts/verify_api.php
```

## Benchmark Docker

```bash
docker build -t cisv-php-bench -f bindings/php/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-php-bench
```

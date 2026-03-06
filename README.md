# cisv-php

![License](https://img.shields.io/badge/license-MIT-blue)

PHP extension distribution for CISV.

## Features

- Native C-backed CSV parsing in PHP
- Full parse API and low-memory iterator API
- Parallel parse and benchmark/count modes

## Installation

```bash
git clone https://github.com/Sanix-Darker/cisv-php
cd cisv-php
make all
```

## PHP API

### Basic example

```php
$parser = new CisvParser(['delimiter' => ',', 'trim' => true]);
$rows = $parser->parseFile('data.csv');
```

### Detailed example (iterator + early exit)

```php
$parser = new CisvParser(['delimiter' => ',']);
$parser->openIterator('large.csv');
while (($row = $parser->fetchRow()) !== false) {
    if ($row[0] === 'stop') break;
}
$parser->closeIterator();
```

More runnable examples: [`examples/`](./examples)

## Validation

```bash
php -d extension=bindings/php/modules/cisv.so bindings/php/scripts/verify_api.php
```

## Benchmarks

```bash
docker build -t cisv-php-bench -f bindings/php/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-php-bench
```

## Upstream Core

- cisv-core: https://github.com/Sanix-Darker/cisv-core

# cisv-php
[![CI](https://github.com/Sanix-Darker/cisv-php/actions/workflows/ci.yml/badge.svg)](https://github.com/Sanix-Darker/cisv-php/actions/workflows/ci.yml)
[![Packagist Version](https://img.shields.io/packagist/v/sanix-darker/cisv.svg)](https://packagist.org/packages/sanix-darker/cisv)
[Packagist Package](https://packagist.org/packages/sanix-darker/cisv)

![License](https://img.shields.io/badge/license-MIT-blue)

PHP extension distribution for CISV with SIMD-accelerated CSV parsing via native C code.

## Features

- Native extension parser for PHP
- Full parse API (`parseFile`, `parseString`)
- Row-by-row iterator API (`openIterator` / `fetchRow`)
- Fast row counting (`CisvParser::countRows`)
- Better memory behavior with iterator mode on very large files

## Installation

### From source

```bash
git clone https://github.com/Sanix-Darker/cisv-php
cd cisv-php
make -C core all
cd bindings/php
phpize
./configure --enable-cisv
make -j"$(nproc)"
sudo make install
```

Then enable extension in `php.ini`:

```ini
extension=cisv.so
```

## Quick Start

```php
<?php

$parser = new CisvParser(['delimiter' => ',', 'trim' => true]);
$rows = $parser->parseFile('data.csv');
print_r($rows[0]);
```

## API Examples

### Parse file and string

```php
<?php

$parser = new CisvParser(['trim' => true, 'skip_empty' => true]);
$fileRows = $parser->parseFile('data.csv');
$stringRows = $parser->parseString("id,name\n1,alice\n2,bob\n");
```

### Fast row counting

```php
<?php

$total = CisvParser::countRows('large.csv');
echo "Rows: $total\n";
```

### Iterator mode (recommended for huge files)

```php
<?php

$parser = new CisvParser(['delimiter' => ',', 'trim' => true]);
$parser->openIterator('very_large.csv');

while (($row = $parser->fetchRow()) !== false) {
    if (!empty($row) && $row[0] === 'STOP') {
        break;
    }
    // process row
}

$parser->closeIterator();
```

## Examples Directory

Runnable examples are available in [`examples/`](./examples):

- `basic.php`
- `iterator.php`
- `sample.csv`

## Validation

```bash
php -d extension=bindings/php/modules/cisv.so bindings/php/scripts/verify_api.php
```

## Benchmarks

```bash
docker build -t cisv-php-bench -f bindings/php/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-php-bench
```

The benchmark output includes both full parse and iterator paths (including `cisv-iterator`).

## Upstream Core

- cisv-core: https://github.com/Sanix-Darker/cisv-core

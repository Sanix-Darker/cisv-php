<?php

declare(strict_types=1);

if (!extension_loaded('cisv')) {
    fwrite(STDERR, "[cisv][php-api] extension 'cisv' is not loaded\n");
    exit(1);
}

if (!class_exists('CisvParser')) {
    fwrite(STDERR, "[cisv][php-api] class CisvParser is missing\n");
    exit(1);
}

$stubFile = dirname(__DIR__) . '/stubs/cisv.php';

if (!is_file($stubFile)) {
    fwrite(STDERR, "[cisv][php-api] stub file not found: {$stubFile}\n");
    exit(1);
}

$stubContent = file_get_contents($stubFile);
if ($stubContent === false) {
    fwrite(STDERR, "[cisv][php-api] failed to read stub file: {$stubFile}\n");
    exit(1);
}

preg_match_all('/public\\s+(?:static\\s+)?function\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(/', $stubContent, $matches);
$requiredMethods = array_values(array_unique($matches[1] ?? []));

if ($requiredMethods === []) {
    fwrite(STDERR, "[cisv][php-api] no methods detected in stubs\n");
    exit(1);
}

$reflection = new ReflectionClass('CisvParser');
$available = array_map(
    static fn(ReflectionMethod $method): string => $method->getName(),
    $reflection->getMethods()
);

sort($available);
$missing = array_values(array_diff($requiredMethods, $available));

if ($missing !== []) {
    fwrite(STDERR, "[cisv][php-api] missing methods: " . implode(', ', $missing) . "\n");
    fwrite(STDERR, "[cisv][php-api] available methods: " . implode(', ', $available) . "\n");
    exit(1);
}

function cisv_assert_same(mixed $expected, mixed $actual, string $label): void
{
    if ($expected !== $actual) {
        fwrite(STDERR, "[cisv][php-api] {$label}: expected {$expected}, got {$actual}\n");
        exit(1);
    }
}

function cisv_assert_throws(callable $fn, string $label): void
{
    try {
        $fn();
    } catch (Throwable) {
        return;
    }

    fwrite(STDERR, "[cisv][php-api] {$label}: expected exception\n");
    exit(1);
}

$tmpBase = sys_get_temp_dir() . '/cisv_php_count_' . getmypid();

$controlsFile = $tmpBase . '_controls.csv';
file_put_contents($controlsFile, "  #skip\n\nh1,h2\n,,\n\"#keep\",x\n");
cisv_assert_same(3, CisvParser::countRows($controlsFile, [
    'comment' => '#',
    'trim' => true,
    'skip_empty' => true,
]), 'countRows semantic controls');
unlink($controlsFile);

$rangeFile = $tmpBase . '_range.csv';
file_put_contents($rangeFile, "a\nb\nc\nd\n");
cisv_assert_same(2, CisvParser::countRows($rangeFile, [
    'from_line' => 2,
    'to_line' => 3,
]), 'countRows line range');
unlink($rangeFile);

$escapeFile = $tmpBase . '_escape.csv';
file_put_contents($escapeFile, "id,payload\n1,\"line1\\\nline2 with \\\"quote\\\"\"\n");
cisv_assert_same(2, CisvParser::countRows($escapeFile, [
    'escape' => '\\',
]), 'countRows custom escape');
unlink($escapeFile);

$invalidFile = $tmpBase . '_invalid.csv';
file_put_contents($invalidFile, "a,b\n1,2\n");
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['escape' => 'xx']),
    'countRows invalid escape'
);
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['from_line' => 3, 'to_line' => 2]),
    'countRows invalid range'
);
unlink($invalidFile);

echo "[cisv][php-api] OK: all required methods are exported\n";

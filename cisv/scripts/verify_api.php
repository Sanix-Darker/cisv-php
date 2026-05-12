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
        fwrite(
            STDERR,
            "[cisv][php-api] {$label}: expected " . var_export($expected, true)
                . ', got ' . var_export($actual, true) . "\n"
        );
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

function cisv_fetch_all(CisvParser $parser): array
{
    $rows = [];
    while (($row = $parser->fetchRow()) !== false) {
        $rows[] = $row;
    }
    return $rows;
}

$tmpBase = sys_get_temp_dir() . '/cisv_php_count_' . getmypid();

$configuredParser = new CisvParser([
    'escape' => '\\',
    'comment' => '#',
    'trim' => true,
    'from_line' => 1,
    'to_line' => 3,
]);
cisv_assert_same(
    [
        ['id', 'msg'],
        ['1', 'hello "quoted"'],
    ],
    $configuredParser->parseString("  #skip\nid,msg\n1,\"hello \\\"quoted\\\"\"\n2,tail\n"),
    'parseString constructor controls'
);

$skipEmptyParser = new CisvParser(['skip_empty' => true]);
cisv_assert_same(
    [
        ['a', 'b', 'c'],
        ['', '', ''],
        ['1', '2', '3'],
    ],
    $skipEmptyParser->parseString("a,b,c\n\n,,\n1,2,3\n"),
    'parseString skip_empty preserves empty fields'
);

$crOnlyParser = new CisvParser();
cisv_assert_same(
    [
        ['a', 'b'],
        ["line1\rline2", 'c'],
    ],
    $crOnlyParser->parseString("a,b\r\"line1\rline2\",c\r"),
    'parseString CR-only line endings'
);

$parseFile = $tmpBase . '_parse.csv';
file_put_contents($parseFile, "  #skip\nid,msg\n1,\"hello \\\"quoted\\\"\"\n2,tail\n");
cisv_assert_same(
    [
        ['id', 'msg'],
        ['1', 'hello "quoted"'],
    ],
    $configuredParser->parseFile($parseFile),
    'parseFile constructor controls'
);
unlink($parseFile);

$crOnlyFile = $tmpBase . '_cr_only.csv';
file_put_contents($crOnlyFile, "a,b\r\"line1\rline2\",c\r");
cisv_assert_same(
    [
        ['a', 'b'],
        ["line1\rline2", 'c'],
    ],
    $crOnlyParser->parseFile($crOnlyFile),
    'parseFile CR-only line endings'
);

$iteratorFile = $tmpBase . '_iterator.csv';
file_put_contents($iteratorFile, "  #skip\nid,msg\n1,\"hello \\\"quoted\\\"\"\n2,tail\n");
$configuredParser->openIterator($iteratorFile);
cisv_assert_same(
    [
        ['id', 'msg'],
        ['1', 'hello "quoted"'],
    ],
    cisv_fetch_all($configuredParser),
    'iterator constructor controls'
);
$configuredParser->closeIterator();
unlink($iteratorFile);

$crOnlyParser->openIterator($crOnlyFile);
cisv_assert_same(
    [
        ['a', 'b'],
        ["line1\rline2", 'c'],
    ],
    cisv_fetch_all($crOnlyParser),
    'iterator CR-only line endings'
);
$crOnlyParser->closeIterator();

$iteratorEmptyFile = $tmpBase . '_iterator_empty.csv';
file_put_contents($iteratorEmptyFile, "\n\"\"\n#skip\n\"#keep\"\n,,\n");
$iteratorEmptyParser = new CisvParser([
    'comment' => '#',
    'skip_empty' => true,
]);
$iteratorEmptyParser->openIterator($iteratorEmptyFile);
cisv_assert_same(
    [
        [''],
        ['#keep'],
        ['', '', ''],
    ],
    cisv_fetch_all($iteratorEmptyParser),
    'iterator skip_empty preserves quoted empty and quoted comment rows'
);
$iteratorEmptyParser->closeIterator();
unlink($iteratorEmptyFile);

cisv_assert_throws(
    static fn() => (new CisvParser(['max_row_size' => 8]))->parseString("a,b\n123456789,2\n"),
    'parseString max_row_size'
);

$iteratorLimitFile = $tmpBase . '_iterator_limit.csv';
file_put_contents($iteratorLimitFile, "a,b\n123456789,2\n");
cisv_assert_throws(
    static function () use ($iteratorLimitFile): void {
        $parser = new CisvParser(['max_row_size' => 8]);
        $parser->openIterator($iteratorLimitFile);
        try {
            cisv_fetch_all($parser);
        } finally {
            $parser->closeIterator();
        }
    },
    'iterator max_row_size'
);
unlink($iteratorLimitFile);

$iteratorBadAfterQuoteFile = $tmpBase . '_iterator_bad_after_quote.csv';
file_put_contents($iteratorBadAfterQuoteFile, "\"a\"x,b\n");
cisv_assert_throws(
    static function () use ($iteratorBadAfterQuoteFile): void {
        $parser = new CisvParser();
        $parser->openIterator($iteratorBadAfterQuoteFile);
        try {
            cisv_fetch_all($parser);
        } finally {
            $parser->closeIterator();
        }
    },
    'iterator malformed quote'
);
unlink($iteratorBadAfterQuoteFile);

$iteratorBadUnterminatedFile = $tmpBase . '_iterator_bad_unterminated.csv';
file_put_contents($iteratorBadUnterminatedFile, "\"unterminated\n");
cisv_assert_throws(
    static function () use ($iteratorBadUnterminatedFile): void {
        $parser = new CisvParser();
        $parser->openIterator($iteratorBadUnterminatedFile);
        try {
            cisv_fetch_all($parser);
        } finally {
            $parser->closeIterator();
        }
    },
    'iterator unterminated quote'
);
unlink($iteratorBadUnterminatedFile);

cisv_assert_throws(
    static fn() => new CisvParser(['delimiter' => '"']),
    'constructor delimiter quote conflict'
);
cisv_assert_throws(
    static fn() => new CisvParser(['escape' => ',']),
    'constructor escape delimiter conflict'
);
cisv_assert_throws(
    static fn() => new CisvParser(['comment' => ',']),
    'constructor comment delimiter conflict'
);
cisv_assert_throws(
    static fn() => new CisvParser(['from_line' => 3, 'to_line' => 2]),
    'constructor invalid range'
);
cisv_assert_throws(
    static fn() => new CisvParser(['max_row_size' => '8']),
    'constructor max_row_size type'
);
cisv_assert_throws(
    static fn() => (new CisvParser())->setDelimiter('"'),
    'setDelimiter validates quote conflict'
);
cisv_assert_throws(
    static fn() => (new CisvParser())->setQuote(','),
    'setQuote validates delimiter conflict'
);

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

cisv_assert_same(2, CisvParser::countRows($crOnlyFile), 'countRows CR-only line endings');
unlink($crOnlyFile);

$invalidFile = $tmpBase . '_invalid.csv';
file_put_contents($invalidFile, "a,b\n1,2\n");
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['escape' => 'xx']),
    'countRows invalid escape'
);
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['delimiter' => '"']),
    'countRows delimiter quote conflict'
);
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['escape' => ',']),
    'countRows escape delimiter conflict'
);
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['max_row_size' => '8']),
    'countRows max_row_size type'
);
cisv_assert_throws(
    static fn() => CisvParser::countRows($invalidFile, ['from_line' => 3, 'to_line' => 2]),
    'countRows invalid range'
);
unlink($invalidFile);

echo "[cisv][php-api] OK: all required methods are exported\n";

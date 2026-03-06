<?php
$parser = new CisvParser([
    'delimiter' => ',',
    'quote' => '"',
    'trim' => true,
]);

$rows = $parser->parseFile(__DIR__ . '/sample.csv');
print_r($rows);

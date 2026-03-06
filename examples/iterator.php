<?php
$parser = new CisvParser(['delimiter' => ',']);
$parser->openIterator(__DIR__ . '/sample.csv');

while (($row = $parser->fetchRow()) !== false) {
    print_r($row);
    if (isset($row[0]) && $row[0] === '2') {
        break;
    }
}

$parser->closeIterator();

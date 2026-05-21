/*
 * CISV PHP Extension
 * High-performance CSV parser with SIMD optimizations
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include <limits.h>
#include <stdint.h>

#include "cisv/parser.h"
#include "cisv/writer.h"
#include "cisv/transformer.h"

/* Extension information */
#define PHP_CISV_VERSION "0.6.0"
#define PHP_CISV_EXTNAME "cisv"

/* Parser object structure */
typedef struct {
    cisv_config config;
    cisv_transform_pipeline_t *pipeline;  /* Transform pipeline (may be NULL if unused) */
    cisv_iterator_t *iterator;            /* Row-by-row iterator (may be NULL) */
    zend_object std;
} cisv_parser_object;

static zend_class_entry *cisv_parser_ce;
static zend_object_handlers cisv_parser_handlers;

/* Get object from zend_object */
static inline cisv_parser_object *cisv_parser_from_obj(zend_object *obj) {
    return (cisv_parser_object *)((char *)obj - XtOffsetOf(cisv_parser_object, std));
}

#define Z_CISV_PARSER_P(zv) cisv_parser_from_obj(Z_OBJ_P(zv))

static uint32_t php_cisv_array_size_hint(size_t count) {
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

static zend_bool php_cisv_size_to_zend_long(size_t value, const char *name, zend_long *out) {
    if (value > (size_t)ZEND_LONG_MAX) {
        zend_throw_exception_ex(zend_ce_exception, 0, "%s exceeds PHP integer range", name);
        return 0;
    }
    *out = (zend_long)value;
    return 1;
}

static zend_bool php_cisv_add_size_checked(size_t *target, size_t value, const char *name) {
    if (value > SIZE_MAX - *target) {
        zend_throw_exception_ex(zend_ce_exception, 0, "%s overflow", name);
        return 0;
    }
    *target += value;
    return 1;
}

static void php_cisv_result_to_zval_rows(zval *rows, const cisv_result_t *result) {
    array_init_size(rows, result ? php_cisv_array_size_hint(result->row_count) : 0);
    if (!result) {
        return;
    }

    for (size_t i = 0; i < result->row_count; i++) {
        const cisv_row_t *r = &result->rows[i];
        zval row;
        array_init_size(&row, php_cisv_array_size_hint(r->field_count));
        for (size_t j = 0; j < r->field_count; j++) {
            add_next_index_stringl(&row, r->fields[j], r->field_lengths[j]);
        }
        add_next_index_zval(rows, &row);
    }
}

static zend_bool php_cisv_validate_char_option(const char *name, const char *value, size_t value_len) {
    if (value_len != 1) {
        zend_throw_exception_ex(zend_ce_exception, 0, "%s must be exactly 1 character", name);
        return 0;
    }

    if (value[0] == '\0' || value[0] == '\n' || value[0] == '\r') {
        zend_throw_exception_ex(
            zend_ce_exception,
            0,
            "Invalid %s character (NUL, newline, or carriage return not allowed)",
            name
        );
        return 0;
    }

    return 1;
}

static zend_bool php_cisv_apply_optional_char_option(const char *name, zval *val, char *target) {
    if (Z_TYPE_P(val) == IS_NULL) {
        *target = '\0';
        return 1;
    }
    if (Z_TYPE_P(val) != IS_STRING) {
        zend_throw_exception_ex(zend_ce_exception, 0, "%s must be a string or null", name);
        return 0;
    }
    if (Z_STRLEN_P(val) == 0) {
        *target = '\0';
        return 1;
    }
    if (!php_cisv_validate_char_option(name, Z_STRVAL_P(val), Z_STRLEN_P(val))) {
        return 0;
    }
    *target = Z_STRVAL_P(val)[0];
    return 1;
}

static zend_bool php_cisv_apply_long_option(const char *name, zval *val, zend_long *target) {
    if (Z_TYPE_P(val) != IS_LONG) {
        zend_throw_exception_ex(zend_ce_exception, 0, "%s must be an integer", name);
        return 0;
    }
    *target = Z_LVAL_P(val);
    return 1;
}

static zend_bool php_cisv_validate_config(const cisv_config *config) {
    if (config->delimiter == config->quote) {
        zend_throw_exception_ex(zend_ce_exception, 0, "delimiter and quote cannot be the same");
        return 0;
    }
    if (config->escape != '\0' && config->escape == config->delimiter) {
        zend_throw_exception_ex(zend_ce_exception, 0, "escape and delimiter cannot be the same");
        return 0;
    }
    if (config->escape != '\0' && config->escape == config->quote) {
        zend_throw_exception_ex(zend_ce_exception, 0, "escape and quote cannot be the same");
        return 0;
    }
    if (config->comment != '\0' &&
        (config->comment == config->delimiter || config->comment == config->quote || config->comment == config->escape)) {
        zend_throw_exception_ex(zend_ce_exception, 0, "comment cannot conflict with delimiter, quote, or escape");
        return 0;
    }

    int effective_from = config->from_line > 0 ? config->from_line : 1;
    if (config->to_line != 0 && config->to_line < effective_from) {
        zend_throw_exception_ex(zend_ce_exception, 0, "to_line must be >= from_line");
        return 0;
    }

    return 1;
}

static zend_bool php_cisv_apply_options(HashTable *options, cisv_config *config) {
    zval *val;

    if ((val = zend_hash_str_find(options, "delimiter", sizeof("delimiter") - 1)) != NULL) {
        if (Z_TYPE_P(val) != IS_STRING) {
            zend_throw_exception_ex(zend_ce_exception, 0, "Delimiter must be a string");
            return 0;
        }
        if (!php_cisv_validate_char_option("Delimiter", Z_STRVAL_P(val), Z_STRLEN_P(val))) {
            return 0;
        }
        config->delimiter = Z_STRVAL_P(val)[0];
    }

    if ((val = zend_hash_str_find(options, "quote", sizeof("quote") - 1)) != NULL) {
        if (Z_TYPE_P(val) != IS_STRING) {
            zend_throw_exception_ex(zend_ce_exception, 0, "Quote must be a string");
            return 0;
        }
        if (!php_cisv_validate_char_option("Quote", Z_STRVAL_P(val), Z_STRLEN_P(val))) {
            return 0;
        }
        config->quote = Z_STRVAL_P(val)[0];
    }

    if ((val = zend_hash_str_find(options, "escape", sizeof("escape") - 1)) != NULL) {
        if (!php_cisv_apply_optional_char_option("Escape", val, &config->escape)) {
            return 0;
        }
    }

    if ((val = zend_hash_str_find(options, "comment", sizeof("comment") - 1)) != NULL) {
        if (!php_cisv_apply_optional_char_option("Comment", val, &config->comment)) {
            return 0;
        }
    }

    if ((val = zend_hash_str_find(options, "trim", sizeof("trim") - 1)) != NULL) {
        config->trim = zend_is_true(val);
    }

    if ((val = zend_hash_str_find(options, "skip_empty", sizeof("skip_empty") - 1)) != NULL) {
        config->skip_empty_lines = zend_is_true(val);
    }
    if ((val = zend_hash_str_find(options, "skip_empty_lines", sizeof("skip_empty_lines") - 1)) != NULL) {
        config->skip_empty_lines = zend_is_true(val);
    }

    if ((val = zend_hash_str_find(options, "relaxed", sizeof("relaxed") - 1)) != NULL) {
        config->relaxed = zend_is_true(val);
    }

    if ((val = zend_hash_str_find(options, "skip_lines_with_error", sizeof("skip_lines_with_error") - 1)) != NULL) {
        config->skip_lines_with_error = zend_is_true(val);
    }

    if ((val = zend_hash_str_find(options, "max_row_size", sizeof("max_row_size") - 1)) != NULL) {
        zend_long max_row_size;
        if (!php_cisv_apply_long_option("max_row_size", val, &max_row_size)) {
            return 0;
        }
        if (max_row_size < 0) {
            zend_throw_exception_ex(zend_ce_exception, 0, "max_row_size must be >= 0");
            return 0;
        }
        config->max_row_size = (size_t)max_row_size;
    }

    if ((val = zend_hash_str_find(options, "from_line", sizeof("from_line") - 1)) != NULL) {
        zend_long from_line;
        if (!php_cisv_apply_long_option("from_line", val, &from_line)) {
            return 0;
        }
        if (from_line < 0 || from_line > INT_MAX) {
            zend_throw_exception_ex(zend_ce_exception, 0, "from_line is out of range");
            return 0;
        }
        config->from_line = (int)from_line;
    }

    if ((val = zend_hash_str_find(options, "to_line", sizeof("to_line") - 1)) != NULL) {
        zend_long to_line;
        if (!php_cisv_apply_long_option("to_line", val, &to_line)) {
            return 0;
        }
        if (to_line < 0 || to_line > INT_MAX) {
            zend_throw_exception_ex(zend_ce_exception, 0, "to_line is out of range");
            return 0;
        }
        config->to_line = (int)to_line;
    }

    return php_cisv_validate_config(config);
}

/* Create object */
static zend_object *cisv_parser_create_object(zend_class_entry *ce) {
    cisv_parser_object *intern = ecalloc(1, sizeof(cisv_parser_object) + zend_object_properties_size(ce));

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &cisv_parser_handlers;

    /* Initialize config */
    cisv_config_init(&intern->config);

    /* Initialize pipeline to NULL (created lazily if transforms are used) */
    intern->pipeline = NULL;

    /* Initialize iterator to NULL */
    intern->iterator = NULL;

    return &intern->std;
}

/* Free object */
static void cisv_parser_free_object(zend_object *obj) {
    cisv_parser_object *intern = cisv_parser_from_obj(obj);

    /* SECURITY FIX: Free transform pipeline to prevent memory leak */
    if (intern->pipeline) {
        cisv_transform_pipeline_destroy(intern->pipeline);
        intern->pipeline = NULL;
    }

    /* Close iterator if open */
    if (intern->iterator) {
        cisv_iterator_close(intern->iterator);
        intern->iterator = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

/* PHP_METHOD(CisvParser, __construct) */
PHP_METHOD(CisvParser, __construct) {
    zval *options = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    /* Apply options if provided */
    if (options) {
        cisv_config next = intern->config;
        if (!php_cisv_apply_options(Z_ARRVAL_P(options), &next)) {
            return;
        }
        intern->config = next;
    }

    intern->config.field_cb = NULL;
    intern->config.row_cb = NULL;
    intern->config.user = NULL;
}

/* PHP_METHOD(CisvParser, parseFile) */
PHP_METHOD(CisvParser, parseFile) {
    char *filename;
    size_t filename_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(filename, filename_len)
    ZEND_PARSE_PARAMETERS_END();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    cisv_result_t *result = cisv_parse_file_batch(filename, &intern->config);
    if (!result) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to parse file: %s", strerror(errno));
        return;
    }

    if (result->error_code != 0) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Parse error: %s", result->error_message);
        cisv_result_free(result);
        return;
    }

    php_cisv_result_to_zval_rows(return_value, result);
    cisv_result_free(result);
}

/* PHP_METHOD(CisvParser, parseString) */
PHP_METHOD(CisvParser, parseString) {
    char *csv;
    size_t csv_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(csv, csv_len)
    ZEND_PARSE_PARAMETERS_END();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    cisv_result_t *result = cisv_parse_string_batch(csv, csv_len, &intern->config);
    if (!result) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to parse string: %s", strerror(errno));
        return;
    }

    if (result->error_code != 0) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Parse error: %s", result->error_message);
        cisv_result_free(result);
        return;
    }

    php_cisv_result_to_zval_rows(return_value, result);
    cisv_result_free(result);
}

/* PHP_METHOD(CisvParser, countRows) */
PHP_METHOD(CisvParser, countRows) {
    char *filename;
    size_t filename_len;
    zval *options = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(filename, filename_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    cisv_config config;
    cisv_config_init(&config);

    if (options) {
        if (!php_cisv_apply_options(Z_ARRVAL_P(options), &config)) {
            return;
        }
    }

    size_t count = cisv_parser_count_rows_with_config(filename, &config);
    zend_long php_count;
    if (!php_cisv_size_to_zend_long(count, "row count", &php_count)) {
        return;
    }
    RETURN_LONG(php_count);
}

/* PHP_METHOD(CisvParser, setDelimiter) */
PHP_METHOD(CisvParser, setDelimiter) {
    char *delimiter;
    size_t delimiter_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(delimiter, delimiter_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!php_cisv_validate_char_option("Delimiter", delimiter, delimiter_len)) {
        return;
    }

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);
    cisv_config next = intern->config;
    next.delimiter = delimiter[0];
    if (!php_cisv_validate_config(&next)) {
        return;
    }
    intern->config = next;

    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

/* PHP_METHOD(CisvParser, setQuote) */
PHP_METHOD(CisvParser, setQuote) {
    char *quote;
    size_t quote_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(quote, quote_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!php_cisv_validate_char_option("Quote", quote, quote_len)) {
        return;
    }

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);
    cisv_config next = intern->config;
    next.quote = quote[0];
    if (!php_cisv_validate_config(&next)) {
        return;
    }
    intern->config = next;

    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

/* PHP_METHOD(CisvParser, parseFileParallel) - Multi-threaded parsing */
PHP_METHOD(CisvParser, parseFileParallel) {
    char *filename;
    size_t filename_len;
    zend_long num_threads = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(filename, filename_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(num_threads)
    ZEND_PARSE_PARAMETERS_END();

    if (num_threads < 0) {
        zend_throw_exception(zend_ce_exception, "num_threads must be >= 0", 0);
        return;
    }

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    /* Parse file using parallel API */
    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(filename, &intern->config,
                                                        (int)num_threads, &result_count);

    if (!results) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to parse file in parallel: %s", strerror(errno));
        return;
    }

    size_t total_rows = 0;
    for (int chunk = 0; chunk < result_count; chunk++) {
        cisv_result_t *result = results[chunk];
        if (!result) continue;
        if (result->error_code != 0) {
            zend_throw_exception_ex(zend_ce_exception, 0, "Parse error: %s", result->error_message);
            cisv_results_free(results, result_count);
            return;
        }
        if (!php_cisv_add_size_checked(&total_rows, result->row_count, "row count")) {
            cisv_results_free(results, result_count);
            return;
        }
    }

    /* Build result array from all chunks */
    zval rows;
    array_init_size(&rows, php_cisv_array_size_hint(total_rows));

    for (int chunk = 0; chunk < result_count; chunk++) {
        cisv_result_t *result = results[chunk];
        if (!result) continue;

        for (size_t i = 0; i < result->row_count; i++) {
            zval row;
            cisv_row_t *r = &result->rows[i];
            array_init_size(&row, php_cisv_array_size_hint(r->field_count));
            for (size_t j = 0; j < r->field_count; j++) {
                add_next_index_stringl(&row, r->fields[j], r->field_lengths[j]);
            }
            add_next_index_zval(&rows, &row);
        }
    }

    cisv_results_free(results, result_count);
    RETURN_ZVAL(&rows, 0, 1);
}

/* PHP_METHOD(CisvParser, parseFileBenchmark) - Benchmark mode (count only, no data marshaling) */
PHP_METHOD(CisvParser, parseFileBenchmark) {
    char *filename;
    size_t filename_len;
    zend_long num_threads = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(filename, filename_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(num_threads)
    ZEND_PARSE_PARAMETERS_END();

    if (num_threads < 0) {
        zend_throw_exception(zend_ce_exception, "num_threads must be >= 0", 0);
        return;
    }

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    /* Parse file using parallel API */
    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(filename, &intern->config,
                                                        (int)num_threads, &result_count);

    if (!results) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to parse file: %s", strerror(errno));
        return;
    }

    /* Just count totals, don't create PHP arrays */
    size_t total_rows = 0;
    size_t total_fields = 0;

    for (int chunk = 0; chunk < result_count; chunk++) {
        cisv_result_t *result = results[chunk];
        if (!result) continue;

        if (result->error_code != 0) {
            zend_throw_exception_ex(zend_ce_exception, 0, "Parse error: %s", result->error_message);
            cisv_results_free(results, result_count);
            return;
        }

        if (!php_cisv_add_size_checked(&total_rows, result->row_count, "row count") ||
            !php_cisv_add_size_checked(&total_fields, result->total_fields, "field count")) {
            cisv_results_free(results, result_count);
            return;
        }
    }

    cisv_results_free(results, result_count);

    /* Return array with counts */
    zval result_arr;
    zend_long php_rows;
    zend_long php_fields;
    if (!php_cisv_size_to_zend_long(total_rows, "row count", &php_rows) ||
        !php_cisv_size_to_zend_long(total_fields, "field count", &php_fields)) {
        return;
    }
    array_init(&result_arr);
    add_assoc_long(&result_arr, "rows", php_rows);
    add_assoc_long(&result_arr, "fields", php_fields);
    RETURN_ZVAL(&result_arr, 0, 1);
}

/* PHP_METHOD(CisvParser, openIterator) - Open file for row-by-row iteration */
PHP_METHOD(CisvParser, openIterator) {
    char *filename;
    size_t filename_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(filename, filename_len)
    ZEND_PARSE_PARAMETERS_END();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    /* Close existing iterator if any */
    if (intern->iterator) {
        cisv_iterator_close(intern->iterator);
        intern->iterator = NULL;
    }

    /* Open iterator */
    intern->iterator = cisv_iterator_open(filename, &intern->config);
    if (!intern->iterator) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to open file for iteration: %s", strerror(errno));
        return;
    }

    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

/* PHP_METHOD(CisvParser, fetchRow) - Get next row from iterator */
PHP_METHOD(CisvParser, fetchRow) {
    ZEND_PARSE_PARAMETERS_NONE();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    if (!intern->iterator) {
        zend_throw_exception(zend_ce_exception, "No iterator open. Call openIterator() first.", 0);
        return;
    }

    const char **fields;
    const size_t *lengths;
    size_t field_count;

    int result = cisv_iterator_next(intern->iterator, &fields, &lengths, &field_count);

    if (result == CISV_ITER_EOF) {
        RETURN_FALSE;
    }

    if (result == CISV_ITER_ERROR) {
        zend_throw_exception(zend_ce_exception, "Error reading CSV row", 0);
        return;
    }

    /* Build PHP array */
    array_init_size(return_value, php_cisv_array_size_hint(field_count));
    for (size_t i = 0; i < field_count; i++) {
        add_next_index_stringl(return_value, fields[i], lengths[i]);
    }
}

/* PHP_METHOD(CisvParser, closeIterator) - Close the iterator */
PHP_METHOD(CisvParser, closeIterator) {
    ZEND_PARSE_PARAMETERS_NONE();

    cisv_parser_object *intern = Z_CISV_PARSER_P(ZEND_THIS);

    if (intern->iterator) {
        cisv_iterator_close(intern->iterator);
        intern->iterator = NULL;
    }

    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

/* Arginfo definitions for PHP 8+ compatibility */
ZEND_BEGIN_ARG_INFO_EX(arginfo_cisv_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cisv_parseFile, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cisv_parseString, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, csv, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cisv_countRows, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_cisv_setDelimiter, 0, 1, CisvParser, 0)
    ZEND_ARG_TYPE_INFO(0, delimiter, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_cisv_setQuote, 0, 1, CisvParser, 0)
    ZEND_ARG_TYPE_INFO(0, quote, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cisv_parseFileParallel, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, num_threads, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cisv_parseFileBenchmark, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, num_threads, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_cisv_openIterator, 0, 1, CisvParser, 0)
    ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_cisv_fetchRow, 0, 0, MAY_BE_ARRAY|MAY_BE_FALSE)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_cisv_closeIterator, 0, 0, CisvParser, 0)
ZEND_END_ARG_INFO()

/* Method table */
static const zend_function_entry cisv_parser_methods[] = {
    PHP_ME(CisvParser, __construct, arginfo_cisv_construct, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, parseFile, arginfo_cisv_parseFile, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, parseString, arginfo_cisv_parseString, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, countRows, arginfo_cisv_countRows, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(CisvParser, setDelimiter, arginfo_cisv_setDelimiter, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, setQuote, arginfo_cisv_setQuote, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, parseFileParallel, arginfo_cisv_parseFileParallel, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, parseFileBenchmark, arginfo_cisv_parseFileBenchmark, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, openIterator, arginfo_cisv_openIterator, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, fetchRow, arginfo_cisv_fetchRow, ZEND_ACC_PUBLIC)
    PHP_ME(CisvParser, closeIterator, arginfo_cisv_closeIterator, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* Module initialization */
PHP_MINIT_FUNCTION(cisv) {
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "CisvParser", cisv_parser_methods);
    cisv_parser_ce = zend_register_internal_class(&ce);
    cisv_parser_ce->create_object = cisv_parser_create_object;

    memcpy(&cisv_parser_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    cisv_parser_handlers.offset = XtOffsetOf(cisv_parser_object, std);
    cisv_parser_handlers.free_obj = cisv_parser_free_object;

    return SUCCESS;
}

/* Module info */
PHP_MINFO_FUNCTION(cisv) {
    php_info_print_table_start();
    php_info_print_table_header(2, "cisv support", "enabled");
    php_info_print_table_row(2, "Version", PHP_CISV_VERSION);
    php_info_print_table_row(2, "Features", "SIMD-accelerated parsing, zero-copy mmap");
    php_info_print_table_end();
}

/* Module entry */
zend_module_entry cisv_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_CISV_EXTNAME,
    NULL,
    PHP_MINIT(cisv),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(cisv),
    PHP_CISV_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_CISV
ZEND_GET_MODULE(cisv)
#endif

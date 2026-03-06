.PHONY: core php all test clean
all: core php
core:
	$(MAKE) -C core/core all
php: core
	cd cisv && phpize && ./configure --enable-cisv && $(MAKE)
test: php
	php -d extension=cisv/modules/cisv.so cisv/scripts/verify_api.php
clean:
	$(MAKE) -C core/core clean
	cd cisv && $(MAKE) clean || true

.PHONY: core php all clean
all: core php
core:
	$(MAKE) -C core all
php: core
	cd bindings/php && phpize && ./configure --enable-cisv && $(MAKE)
clean:
	$(MAKE) -C core clean
	cd bindings/php && $(MAKE) clean || true

.PHONY: core php all clean
all: core php
core:
	$(MAKE) -C core/core all
php: core
	cd cisv && phpize && ./configure --enable-cisv && $(MAKE)
clean:
	$(MAKE) -C core/core clean
	cd cisv && $(MAKE) clean || true

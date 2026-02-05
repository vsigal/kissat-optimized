all:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto"
kissat:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto" kissat
tissat:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto" tissat
clean:
	rm -f "/home/vsigal/src/kissat"/makefile
	rm -f "/home/vsigal/src/kissat"/src/makefile
	-$(MAKE) -C "/home/vsigal/src/kissat/build_lto" clean
	rm -rf "/home/vsigal/src/kissat/build_lto"
coverage:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto" coverage
format:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto" format
test:
	$(MAKE) -C "/home/vsigal/src/kissat/build_lto" test
.PHONY: all clean coverage format kissat test tissat

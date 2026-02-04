all:
	$(MAKE) -C "/home/vsigal/src/kissat/build"
kissat:
	$(MAKE) -C "/home/vsigal/src/kissat/build" kissat
tissat:
	$(MAKE) -C "/home/vsigal/src/kissat/build" tissat
clean:
	rm -f "/home/vsigal/src/kissat"/makefile
	rm -f "/home/vsigal/src/kissat"/src/makefile
	-$(MAKE) -C "/home/vsigal/src/kissat/build" clean
	rm -rf "/home/vsigal/src/kissat/build"
coverage:
	$(MAKE) -C "/home/vsigal/src/kissat/build" coverage
format:
	$(MAKE) -C "/home/vsigal/src/kissat/build" format
test:
	$(MAKE) -C "/home/vsigal/src/kissat/build" test
.PHONY: all clean coverage format kissat test tissat

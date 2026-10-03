.PHONY: build package verify test release clean

build:
	./scripts/build.sh

package: build
	./scripts/package.sh

verify:
	./scripts/verify.sh

test:
	/usr/bin/python3 tests/run.py

release: package
	/usr/bin/python3 tests/run.py
	./scripts/verify.sh

clean:
	/bin/rm -rf build dist
